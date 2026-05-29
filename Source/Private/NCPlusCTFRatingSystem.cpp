// NCPlusCTFRatingSystem.cpp — Mods.db persistence + match-end ProcessMatch glue
// for CTF/iCTF. Tron's TeamGlicko2 headers stay confined to this TU.

#include "NCPlusCTFRatingSystem.h"
#include "NCEloUploader.h"             // FNCEloUploader::ResolveServerName
#include "UnrealTournament.h"
#include "UTGameInstance.h"            // FDatabaseRow + ExecDatabaseCommand
#include "UTGameState.h"               // ServerName for BuildResultPayload
#include "Engine/World.h"
#include "Misc/DateTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Vendored TeamGlicko2 — included only here, never via the .h.
#include "TeamGlickoRating.h"
#include "TeamGlicko2System.h"
#include "TeamGlicko2Config.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCPlusCTFRating, Log, All);

// =============================================================================
// Pimpl: cache + per-instance flag. TeamGlicko2 types only visible here.
// =============================================================================
struct FNCPlusCTFRatingSystemImpl
{
	bool bIsInstagib = false;

	/** UniqueId -> mutable PlayerRating. Updated by ProcessMatch. */
	TMap<FString, TeamGlicko2::PlayerRating> RatingCache;

	/** UniqueId -> rating-rounded-int captured by SnapshotMatchStart. */
	TMap<FString, int32> RatingAtMatchStart;

	/** Humans who faced at least one human opponent (= both teams had >=1
	 *  cached human in ProcessMatch). Anyone NOT in this set at flush time
	 *  gets their delta clamped to ±BotMatchDeltaCap. Single-shot for CTF
	 *  since there's no per-round loop — populated by ProcessMatch. */
	TSet<FString> HumansWithHumanOpposition;
};

// Unity-build aware: file-scoped prefixed names to avoid collisions with
// other rating systems bundled into the same TU.
namespace
{
	FString CTFR_SqlEscape(const FString& In)
	{
		return In.Replace(TEXT("'"), TEXT("''"));
	}

	bool CTFR_ExecSql(UWorld* World, const FString& Sql, TArray<FDatabaseRow>& OutRows)
	{
		if (!World) return false;
		UUTGameInstance* GI = Cast<UUTGameInstance>(World->GetGameInstance());
		if (!GI) return false;
		return GI->ExecDatabaseCommand(Sql, OutRows);
	}

	bool CTFR_ExecSqlNoRows(UWorld* World, const FString& Sql)
	{
		TArray<FDatabaseRow> Discard;
		return CTFR_ExecSql(World, Sql, Discard);
	}

	const TCHAR* TableForMode(bool bInstagib)
	{
		return bInstagib ? TEXT("NCRatingICTF") : TEXT("NCRatingCTF");
	}

	const TCHAR* JsonModeKey(bool bInstagib)
	{
		return bInstagib ? TEXT("iCTF") : TEXT("CTF");
	}

	// Legacy performance score: cumulative K/D/damage. Used for the live delta
	// while bShadow is set, and as the fallback when bEnabled is false.
	double CTFR_LegacyPerf(const FNCPlusCTFPlayerInput& P)
	{
		return double(P.Kills) - double(P.Deaths) + double(P.Damage) / 220.0;
	}

	// CTF-aware performance score. Only the WITHIN-TEAM relative spread of this
	// value matters (TeamGlicko2 turns it into a teammate-relative z-score), so
	// the weights are chosen as ratios anchored to UT4's own per-event scoring
	// (UTCTFScoring.cpp: FlagCapPoints=12, FlagSupportAssist=5, FC-kill~4,
	// FlagReturnPoints~3, support-kill~2, BaseKillScore=1). Each signal is
	// capped per game so a blowout can't explode one player's z and crush the
	// rest of the team. iCTF drops damage (degenerate — every hit is a kill)
	// and halves combat so objective play, not fragging, drives the ranking.
	double CTFR_CTFPerf(const FNCPlusCTFPlayerInput& P, const FNCPlusCTFPerfConfig& Cfg, bool bInstagib)
	{
		auto Cap = [](int32 V, int32 Hi) -> double { return double(FMath::Clamp(V, 0, Hi)); };

		const int32  KillCap = bInstagib ? 40 : 30;
		const double Kills   = Cap(P.Kills,  KillCap);
		const double Deaths  = Cap(P.Deaths, KillCap);

		double Combat;
		if (bInstagib)
		{
			Combat = 0.5 * Kills - 0.5 * Deaths;            // damage ≡ kills in IG — omit
		}
		else
		{
			const double Damage = FMath::Min(double(P.Damage), double(KillCap) * 220.0);
			Combat = Kills - Deaths + Damage / 220.0;
		}

		// FCKills / SupportKills are bonuses layered on the base kill already
		// counted above — matching UT4, where an FC kill = base kill + bonus.
		const double Objective =
			  12.0 * Cap(P.Caps,         5)
			+  5.0 * Cap(P.Assists,      8)
			+  4.0 * Cap(P.FCKills,      6)
			+  3.0 * Cap(P.Returns,      8)
			+  2.0 * Cap(P.SupportKills, 8);

		// Anti-pad: grabs that advanced nothing (not a cap, not a carry assist).
		const double Feeder = Cap(FMath::Max(0, P.Grabs - P.Caps - P.CarryAssists), 6);

		return Combat
			 + Cfg.ObjectiveWeight * Objective
			 - Cfg.FeederPenalty   * Feeder;
	}

	// The score fed to the LIVE Glicko update: new CTF perf only once enabled
	// AND out of shadow; legacy K/D otherwise.
	double CTFR_LivePerf(const FNCPlusCTFPlayerInput& P, const FNCPlusCTFPerfConfig& Cfg, bool bInstagib)
	{
		return (Cfg.bEnabled && !Cfg.bShadow)
			? CTFR_CTFPerf(P, Cfg, bInstagib)
			: CTFR_LegacyPerf(P);
	}
}

// =============================================================================
// FNCPlusCTFRatingSystem
// =============================================================================

FNCPlusCTFRatingSystem::FNCPlusCTFRatingSystem(bool bInIsInstagib)
	: Impl(MakeUnique<FNCPlusCTFRatingSystemImpl>())
{
	Impl->bIsInstagib = bInIsInstagib;
}

FNCPlusCTFRatingSystem::~FNCPlusCTFRatingSystem() = default;

bool FNCPlusCTFRatingSystem::IsInstagibMode() const
{
	return Impl->bIsInstagib;
}

bool FNCPlusCTFRatingSystem::InitDatabase(UWorld* World)
{
	// Create BOTH tables so the static InitDatabase call from BeginPlay covers
	// either path (regular CTF or instagib CTF) regardless of when bIsInstagib
	// is determined. The unused table will sit empty — that's fine, schema
	// creation is idempotent and cheap.
	bool bAllOk = true;

	for (int32 Variant = 0; Variant < 2; ++Variant)
	{
		const bool bInstagib = (Variant == 1);
		const FString Sql = FString::Printf(
			TEXT("CREATE TABLE IF NOT EXISTS %s (")
			TEXT("  UniqueId       TEXT PRIMARY KEY NOT NULL,")
			TEXT("  Rating         REAL NOT NULL DEFAULT 1400.0,")
			TEXT("  RD             REAL NOT NULL DEFAULT 350.0,")
			TEXT("  Sigma          REAL NOT NULL DEFAULT 0.03,")
			TEXT("  PerfIndexEMA   REAL NOT NULL DEFAULT 0.0,")
			TEXT("  PerfGames      INTEGER NOT NULL DEFAULT 0,")
			TEXT("  LastSeenUtc    INTEGER NOT NULL DEFAULT 0,")
			TEXT("  SchemaVersion  INTEGER NOT NULL DEFAULT 1")
			TEXT(");"), TableForMode(bInstagib));
		const bool bOk = CTFR_ExecSqlNoRows(World, Sql);
		if (!bOk) bAllOk = false;
	}

	UE_LOG(LogNCPlusCTFRating, Log, TEXT("InitDatabase: NCRatingCTF + NCRatingICTF %s"),
		bAllOk ? TEXT("ready") : TEXT("FAILED (USE_SQLITE off?)"));
	return bAllOk;
}

void FNCPlusCTFRatingSystem::LoadPlayerFromDB(UWorld* World, const FString& UniqueId)
{
	if (UniqueId.IsEmpty()) return;
	if (Impl->RatingCache.Contains(UniqueId)) return;

	const FString Esc = CTFR_SqlEscape(UniqueId);
	const TCHAR* Table = TableForMode(Impl->bIsInstagib);
	const FString Sql = FString::Printf(
		TEXT("SELECT Rating, RD, Sigma, PerfIndexEMA, PerfGames FROM %s WHERE UniqueId='%s';"),
		Table, *Esc);

	TArray<FDatabaseRow> Rows;
	const bool bOk = CTFR_ExecSql(World, Sql, Rows);

	if (bOk && Rows.Num() > 0 && Rows[0].Text.Num() >= 5)
	{
		const double Rating       = FCString::Atod(*Rows[0].Text[0]);
		const double RD           = FCString::Atod(*Rows[0].Text[1]);
		const double Sigma        = FCString::Atod(*Rows[0].Text[2]);
		const double PerfIndexEMA = FCString::Atod(*Rows[0].Text[3]);
		const int32  PerfGames    = FCString::Atoi(*Rows[0].Text[4]);

		TeamGlicko2::PlayerRating PR(Rating, RD, Sigma);
		PR.SetPerfIndexEMA(PerfIndexEMA);
		PR.SetPerfGames(PerfGames);
		Impl->RatingCache.Add(UniqueId, PR);
		UE_LOG(LogNCPlusCTFRating, Log, TEXT("[%s] Loaded %s: Rating=%.1f RD=%.1f"),
			Table, *UniqueId, Rating, RD);
	}
	else
	{
		TeamGlicko2::PlayerRating PR(
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility);
		Impl->RatingCache.Add(UniqueId, PR);

		const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
		const FString InsertSql = FString::Printf(
			TEXT("INSERT OR IGNORE INTO %s (UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, 0.0, 0, %lld);"),
			Table,
			*Esc,
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility,
			static_cast<long long>(NowUtc));
		CTFR_ExecSqlNoRows(World, InsertSql);
		UE_LOG(LogNCPlusCTFRating, Log, TEXT("[%s] New player %s — defaults + INSERT OR IGNORE"), Table, *UniqueId);
	}
}

void FNCPlusCTFRatingSystem::SnapshotMatchStart()
{
	Impl->RatingAtMatchStart.Empty();
	Impl->HumansWithHumanOpposition.Empty();
	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		Impl->RatingAtMatchStart.Add(Pair.Key, FMath::RoundToInt(Pair.Value.GetRating()));
	}
	UE_LOG(LogNCPlusCTFRating, Log, TEXT("[%s] Snapshot %d ratings at match start"),
		TableForMode(Impl->bIsInstagib), Impl->RatingAtMatchStart.Num());
}

void FNCPlusCTFRatingSystem::ProcessMatch(const FNCPlusCTFMatchInput& In)
{
	using namespace TeamGlicko2;

	// Partition players by team. Cumulative K/D/damage drives the performance
	// score. Bots (no cache entry) get a transient default placeholder so
	// team size math reflects reality, but their post-match ratings get
	// discarded at write-back.
	TSet<FString> CachedIdsBefore;
	CachedIdsBefore.Reserve(Impl->RatingCache.Num());
	for (const TPair<FString, PlayerRating>& Pair : Impl->RatingCache)
	{
		CachedIdsBefore.Add(Pair.Key);
	}

	auto BuildTeam = [this, &In](const TArray<FNCPlusCTFPlayerInput>& Inputs, std::vector<MatchPlayer>& Out)
	{
		Out.reserve(Inputs.Num());
		for (const FNCPlusCTFPlayerInput& P : Inputs)
		{
			const PlayerRating* Cached = Impl->RatingCache.Find(P.UniqueId);
			PlayerRating PR;
			if (Cached)
			{
				PR = *Cached;
			}
			else
			{
				PR = PlayerRating(kDefaultRating, kDefaultRD, kDefaultVolatility);
			}
			const double Perf = CTFR_LivePerf(P, In.Perf, Impl->bIsInstagib);
			Out.push_back(MatchPlayer(PR, Perf));
		}
	};

	TArray<FNCPlusCTFPlayerInput> RedTeam, BlueTeam;
	int32 HumansRed = 0, HumansBlue = 0;
	for (const FNCPlusCTFPlayerInput& P : In.Players)
	{
		const bool bIsCachedHuman = CachedIdsBefore.Contains(P.UniqueId);
		if (P.TeamIndex == 0)
		{
			RedTeam.Add(P);
			if (bIsCachedHuman) ++HumansRed;
		}
		else if (P.TeamIndex == 1)
		{
			BlueTeam.Add(P);
			if (bIsCachedHuman) ++HumansBlue;
		}
	}

	if (RedTeam.Num() == 0 || BlueTeam.Num() == 0)
	{
		UE_LOG(LogNCPlusCTFRating, Warning,
			TEXT("ProcessMatch: empty team (Red=%d Blue=%d) — skipping rating update"),
			RedTeam.Num(), BlueTeam.Num());
		return;
	}

	// Bot-match clamp setup: only humans on BOTH teams qualify as "had human
	// opposition". Anyone outside this set at flush time gets a delta clamp
	// to prevent solo-vs-bots farming. Single-shot — CTF has no rounds, so
	// ProcessMatch fires exactly once per match.
	if (HumansRed > 0 && HumansBlue > 0)
	{
		for (const FNCPlusCTFPlayerInput& P : In.Players)
		{
			if (CachedIdsBefore.Contains(P.UniqueId))
			{
				Impl->HumansWithHumanOpposition.Add(P.UniqueId);
			}
		}
	}

	MatchResult Match;
	BuildTeam(RedTeam,  Match.teamA);
	BuildTeam(BlueTeam, Match.teamB);

	if (In.WinnerTeamIndex < 0)
	{
		Match.scoreA = 0.5;
		Match.scoreB = 0.5;
	}
	else if (In.WinnerTeamIndex == 0)
	{
		Match.scoreA = 1.0;
		Match.scoreB = 0.0;
	}
	else
	{
		Match.scoreA = 0.0;
		Match.scoreB = 1.0;
	}

	TeamGlicko2System::ProcessMatch(Match);

	// Write back: only humans (cache entries that existed pre-call).
	auto WriteBack = [this, &CachedIdsBefore](const std::vector<MatchPlayer>& Team, const TArray<FNCPlusCTFPlayerInput>& Players)
	{
		const int32 N = FMath::Min(static_cast<int32>(Team.size()), Players.Num());
		for (int32 i = 0; i < N; ++i)
		{
			if (CachedIdsBefore.Contains(Players[i].UniqueId))
			{
				Impl->RatingCache[Players[i].UniqueId] = Team[i].rating;
			}
		}
	};
	WriteBack(Match.teamA, RedTeam);
	WriteBack(Match.teamB, BlueTeam);
}

void FNCPlusCTFRatingSystem::Flush(UWorld* World)
{
	if (!World) return;

	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
	const TCHAR* Table = TableForMode(Impl->bIsInstagib);

	// Bot-match cap: anyone whose match didn't include a human opponent gets
	// their delta clamped (prevents farming inflated ratings vs all-bot
	// scrimmages). Mirrors the ElimPlus pattern.
	const int32 BotMatchDeltaCap = 5;

	int32 PersistedCount = 0;
	int32 CappedCount    = 0;

	for (TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const FString& UniqueId = Pair.Key;
		TeamGlicko2::PlayerRating& PR = Pair.Value;

		const int32 NewEloRaw = FMath::RoundToInt(PR.GetRating());
		const int32 StartElo  = Impl->RatingAtMatchStart.FindRef(UniqueId);
		const int32 Delta     = (StartElo != 0) ? (NewEloRaw - StartElo) : 0;

		const bool bFacedHumans = Impl->HumansWithHumanOpposition.Contains(UniqueId);
		if (!bFacedHumans && StartElo != 0 && (Delta > BotMatchDeltaCap || Delta < -BotMatchDeltaCap))
		{
			const int32 Clamped  = FMath::Clamp(Delta, -BotMatchDeltaCap, BotMatchDeltaCap);
			const int32 FinalElo = StartElo + Clamped;
			PR.SetRating(static_cast<double>(FinalElo));
			UE_LOG(LogNCPlusCTFRating, Log,
				TEXT("[%s] Bot-match clamp: %s delta %d -> %d (Elo %d -> %d, no human opposition)"),
				Table, *UniqueId, Delta, Clamped, NewEloRaw, FinalElo);
			++CappedCount;
		}

		const FString Esc = CTFR_SqlEscape(UniqueId);
		const FString Sql = FString::Printf(
			TEXT("INSERT OR REPLACE INTO %s ")
			TEXT("(UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, SchemaVersion) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.6f, %d, %lld, 1);"),
			Table,
			*Esc,
			PR.GetRating(), PR.GetRD(), PR.GetSigma(),
			PR.GetPerfIndexEMA(), PR.GetPerfGames(),
			static_cast<long long>(NowUtc));
		CTFR_ExecSqlNoRows(World, Sql);
		++PersistedCount;
	}
	UE_LOG(LogNCPlusCTFRating, Log, TEXT("[%s] Flush: persisted %d ratings, capped %d (no human opposition)"),
		Table, PersistedCount, CappedCount);
}

int32 FNCPlusCTFRatingSystem::GetCachedElo(const FString& UniqueId) const
{
	if (const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId))
	{
		return FMath::RoundToInt(PR->GetRating());
	}
	return FMath::RoundToInt(TeamGlicko2::kDefaultRating);
}

void FNCPlusCTFRatingSystem::Forget(const FString& UniqueId)
{
	Impl->RatingCache.Remove(UniqueId);
	Impl->RatingAtMatchStart.Remove(UniqueId);
}

FString FNCPlusCTFRatingSystem::BuildResultPayload(UWorld* World, const FNCPlusCTFMatchInput& In) const
{
	using namespace TeamGlicko2;

	TArray<int32> HumanIndices;
	HumanIndices.Reserve(In.Players.Num());
	for (int32 i = 0; i < In.Players.Num(); ++i)
	{
		const FNCPlusCTFPlayerInput& P = In.Players[i];
		if (P.UniqueId.IsEmpty()) continue;
		if (Impl->RatingCache.Contains(P.UniqueId))
		{
			HumanIndices.Add(i);
		}
	}
	if (HumanIndices.Num() == 0)
	{
		UE_LOG(LogNCPlusCTFRating, Warning,
			TEXT("BuildResultPayload: no human players in cache — skipping upload"));
		return FString();
	}

	// Resolve server name via NCEloUploader so it matches StatSQL's recorded
	// value (Mod.ini [UTPUGS_STATS] ServerName) + the per-instance suffix —
	// lets Django correlation substring-match without losing PUG identity.
	const FString ServerName = FNCEloUploader::ResolveServerName(World);
	FString MapName;
	if (World)
	{
		MapName = World->GetMapName();
		MapName.RemoveFromStart(World->StreamingLevelsPrefix);
	}
	const FString PlayedAtUtc = FDateTime::UtcNow().ToIso8601();

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);

	Writer->WriteObjectStart();
	Writer->WriteValue(TEXT("request"),       FString(TEXT("elo_match_result")));
	Writer->WriteValue(TEXT("mode"),          FString(JsonModeKey(Impl->bIsInstagib)));
	Writer->WriteValue(TEXT("server"),        ServerName);
	Writer->WriteValue(TEXT("map"),           MapName);
	Writer->WriteValue(TEXT("played_at_utc"), PlayedAtUtc);
	Writer->WriteValue(TEXT("winner_team"),   In.WinnerTeamIndex);
	Writer->WriteValue(TEXT("red_score"),     In.RedScore);
	Writer->WriteValue(TEXT("blue_score"),    In.BlueScore);
	// perf_enabled: the CTF-aware perf machinery is on (raw flag stats + per-player
	// `perf` are present below). perf_shadow: the live `delta` was computed from
	// LEGACY K/D perf for observation only — the new `perf` did NOT move it. Django
	// uses these to decide whether to trust `delta` or just store the new perf.
	Writer->WriteValue(TEXT("perf_enabled"),  In.Perf.bEnabled);
	Writer->WriteValue(TEXT("perf_shadow"),   In.Perf.bShadow);
	Writer->WriteArrayStart(TEXT("players"));

	for (int32 Idx : HumanIndices)
	{
		const FNCPlusCTFPlayerInput& P = In.Players[Idx];
		const PlayerRating* PR = Impl->RatingCache.Find(P.UniqueId);
		if (!PR) continue;

		const int32 PreElo  = Impl->RatingAtMatchStart.FindRef(P.UniqueId);
		const int32 PostElo = FMath::RoundToInt(PR->GetRating());
		const int32 Delta   = (PreElo != 0) ? (PostElo - PreElo) : 0;

		const TCHAR* Result;
		if (In.WinnerTeamIndex < 0)
		{
			Result = TEXT("draw");
		}
		else
		{
			Result = (P.TeamIndex == In.WinnerTeamIndex) ? TEXT("win") : TEXT("loss");
		}

		const bool bFacedHumans = Impl->HumansWithHumanOpposition.Contains(P.UniqueId);

		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"),           P.UniqueId);
		Writer->WriteValue(TEXT("name"),         P.PlayerName);
		Writer->WriteValue(TEXT("team"),         P.TeamIndex);
		Writer->WriteValue(TEXT("result"),       FString(Result));
		Writer->WriteValue(TEXT("kills"),        P.Kills);
		Writer->WriteValue(TEXT("deaths"),       P.Deaths);
		Writer->WriteValue(TEXT("damage"),       P.Damage);
		Writer->WriteValue(TEXT("pre"),          static_cast<double>(PreElo));
		Writer->WriteValue(TEXT("post"),         PR->GetRating());
		Writer->WriteValue(TEXT("delta"),        Delta);
		Writer->WriteValue(TEXT("rd"),           PR->GetRD());
		Writer->WriteValue(TEXT("sigma"),        PR->GetSigma());
		Writer->WriteValue(TEXT("faced_humans"), bFacedHumans);

		// CTF-aware perf scalar + the raw flag signals it derives from. Sent
		// whenever the machinery is enabled (even in shadow) so Django can store
		// them and reconstruct/observe the ordering without re-running matches.
		if (In.Perf.bEnabled)
		{
			Writer->WriteValue(TEXT("perf"),           CTFR_CTFPerf(P, In.Perf, Impl->bIsInstagib));
			Writer->WriteValue(TEXT("caps"),           P.Caps);
			Writer->WriteValue(TEXT("returns"),        P.Returns);
			Writer->WriteValue(TEXT("assists"),        P.Assists);
			Writer->WriteValue(TEXT("fc_kills"),       P.FCKills);
			Writer->WriteValue(TEXT("support_kills"),  P.SupportKills);
			Writer->WriteValue(TEXT("grabs"),          P.Grabs);
			Writer->WriteValue(TEXT("carry_assists"),  P.CarryAssists);
			Writer->WriteValue(TEXT("enemy_fc_damage"),P.EnemyFCDamage);
		}
		Writer->WriteObjectEnd();
	}

	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	return Out;
}
