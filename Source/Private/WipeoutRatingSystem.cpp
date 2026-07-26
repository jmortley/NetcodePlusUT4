// WipeoutRatingSystem.cpp — Mods.db persistence + per-round ProcessMatch glue.
// Tron's TeamGlicko2 headers (and their <vector>/<cmath> baggage) stay confined
// to this translation unit thanks to the Pimpl in the header.

#include "WipeoutRatingSystem.h"
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

DEFINE_LOG_CATEGORY_STATIC(LogWipeoutRating, Log, All);

// =============================================================================
// Pimpl: the actual cache. Defined here where TeamGlicko2 types are visible.
// =============================================================================
struct FWipeoutRatingSystemImpl
{
	/** UniqueId -> mutable PlayerRating. Updated each ProcessRound. */
	TMap<FString, TeamGlicko2::PlayerRating> RatingCache;

	/** UniqueId -> rating-rounded-int captured by SnapshotMatchStart. */
	TMap<FString, int32> RatingAtMatchStart;

	/** UniqueIds of cached humans who appeared in at least one round's
	 *  MatchResult during this match. Used to skip flushing rows for cached-
	 *  but-never-played ghosts (kicked-at-login, AFK spectators). */
	TSet<FString> ActiveHumansThisMatch;

	/** Subset of ActiveHumansThisMatch — humans who faced at least one human
	 *  opponent. Anyone not in this set at flush gets bot-match delta clamping.
	 *  Cleared each SnapshotMatchStart. */
	TSet<FString> HumansWithHumanOpposition;

	/** Players whose DB state could not be confirmed at load (query failed, or the
	 *  row was unconfirmable even after INSERT OR IGNORE + re-read). They play the
	 *  session on default-seeded 1400/RD200 state; FlushAtMatchEnd never persists
	 *  them (an INSERT OR REPLACE from defaults would destroy a real stored row —
	 *  the J@r0d -193 class) and BuildResultPayload marks them "provisional". */
	TSet<FString> LoadFailedGuard;

	/** Last-seen display name per UniqueId, fed from the per-round perf records.
	 *  Used to name mid-match leavers in the upload (their PlayerState — the
	 *  normal name source — is gone by match end). */
	TMap<FString, FString> PlayerNameCache;

	/** Per-round human box-score log for the ut4stats upload's rounds[] array —
	 *  same shape as ElimPlus's. This is what lets Django rate Wipeout
	 *  authoritatively (its live handler derives perf from rounds[]; without the
	 *  array it falls back to trusting the hub delta). Cleared each
	 *  SnapshotMatchStart. */
	struct FRoundPlayerRecord
	{
		FString UniqueId;
		int32   TeamIndex = 0;
		bool    bWinner   = false;
		int32   Kills     = 0;
		int32   Deaths    = 0;
		float   Damage    = 0.f;
	};
	struct FRoundRecord
	{
		int32 WinnerTeamIndex = -1;   // -1 = draw
		bool  bIsDraw         = false;
		TArray<FRoundPlayerRecord> Players;
	};
	TArray<FRoundRecord> RoundLog;
};

// Unity-build aware: anonymous-namespace symbols can collide across .cpp files
// bundled into one TU. Use file-scoped prefixed names so these stay distinct
// from ElimPlusRatingSystem.cpp's / NCDuelRatingSystem.cpp's identical helpers.
namespace
{
	FString WO_SqlEscape(const FString& In)
	{
		return In.Replace(TEXT("'"), TEXT("''"));
	}

	bool WO_ExecSql(UWorld* World, const FString& Sql, TArray<FDatabaseRow>& OutRows)
	{
		if (!World) return false;
		UUTGameInstance* GI = Cast<UUTGameInstance>(World->GetGameInstance());
		if (!GI) return false;
		return GI->ExecDatabaseCommand(Sql, OutRows);
	}

	bool WO_ExecSqlNoRows(UWorld* World, const FString& Sql)
	{
		TArray<FDatabaseRow> Discard;
		return WO_ExecSql(World, Sql, Discard);
	}
}

// =============================================================================
// FWipeoutRatingSystem
// =============================================================================

FWipeoutRatingSystem::FWipeoutRatingSystem()
	: Impl(MakeUnique<FWipeoutRatingSystemImpl>())
{}

FWipeoutRatingSystem::~FWipeoutRatingSystem() = default;

bool FWipeoutRatingSystem::InitDatabase(UWorld* World)
{
	// No busy_timeout on the engine's shared Mods.db handle means concurrent-
	// instance lock contention fails reads INSTANTLY — the class of transient
	// failure that seeds veterans at 1400/RD200. 5s of patience, connection-wide
	// (idempotent with the ElimPlus InitDatabase doing the same; whichever runs
	// first sets it). The PRAGMA returns a result row; WO_ExecSqlNoRows discards it.
	WO_ExecSqlNoRows(World, TEXT("PRAGMA busy_timeout=5000;"));

	const FString Sql =
		TEXT("CREATE TABLE IF NOT EXISTS NCRatingWipeout (")
		TEXT("  UniqueId       TEXT PRIMARY KEY NOT NULL,")
		TEXT("  Rating         REAL NOT NULL DEFAULT 1400.0,")
		TEXT("  RD             REAL NOT NULL DEFAULT 350.0,")
		TEXT("  Sigma          REAL NOT NULL DEFAULT 0.03,")
		TEXT("  PerfIndexEMA   REAL NOT NULL DEFAULT 0.0,")
		TEXT("  PerfGames      INTEGER NOT NULL DEFAULT 0,")
		TEXT("  LastSeenUtc    INTEGER NOT NULL DEFAULT 0,")
		TEXT("  SchemaVersion  INTEGER NOT NULL DEFAULT 1")
		TEXT(");");

	const bool bOk = WO_ExecSqlNoRows(World, Sql);
	UE_LOG(LogWipeoutRating, Log, TEXT("InitDatabase: NCRatingWipeout %s"),
		bOk ? TEXT("ready") : TEXT("FAILED (USE_SQLITE off?)"));
	return bOk;
}

void FWipeoutRatingSystem::LoadPlayerFromDB(UWorld* World, const FString& UniqueId)
{
	if (UniqueId.IsEmpty()) return;
	if (Impl->RatingCache.Contains(UniqueId)) return;

	const FString Esc = WO_SqlEscape(UniqueId);

	// One load path, used twice. Returns: 0=loaded (cache populated), 1=no row,
	// 2=query failed. Mirrors the ElimPlus load hardening (2026-07-17).
	auto TryLoadRow = [&]() -> int32
	{
		const FString Sql = FString::Printf(
			TEXT("SELECT Rating, RD, Sigma, PerfIndexEMA, PerfGames FROM NCRatingWipeout WHERE UniqueId='%s';"),
			*Esc);

		TArray<FDatabaseRow> Rows;
		if (!WO_ExecSql(World, Sql, Rows))
		{
			return 2;
		}
		if (Rows.Num() == 0 || Rows[0].Text.Num() < 5)
		{
			return 1;
		}

		const double Rating       = FCString::Atod(*Rows[0].Text[0]);
		const double RD           = FCString::Atod(*Rows[0].Text[1]);
		const double Sigma        = FCString::Atod(*Rows[0].Text[2]);
		const double PerfIndexEMA = FCString::Atod(*Rows[0].Text[3]);
		const int32  PerfGames    = FCString::Atoi(*Rows[0].Text[4]);

		TeamGlicko2::PlayerRating PR(Rating, RD, Sigma);
		PR.SetPerfIndexEMA(PerfIndexEMA);
		PR.SetPerfGames(PerfGames);
		Impl->RatingCache.Add(UniqueId, PR);
		UE_LOG(LogWipeoutRating, Log, TEXT("Loaded %s: Rating=%.1f RD=%.1f sigma=%.4f"),
			*UniqueId, Rating, RD, Sigma);
		return 0;
	};

	auto SeedDefaultsGuarded = [&](const TCHAR* Why)
	{
		// Cache defaults so the match can proceed, but the flush never persists
		// this player (a defaults-seeded INSERT OR REPLACE would destroy a real
		// stored row) and the upload marks them provisional.
		TeamGlicko2::PlayerRating PR(
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->LoadFailedGuard.Add(UniqueId);
		UE_LOG(LogWipeoutRating, Warning,
			TEXT("Load guard for %s (%s) — playing on defaults, flush persistence DISABLED for this player"),
			*UniqueId, Why);
	};

	const int32 FirstTry = TryLoadRow();
	if (FirstTry == 0)
	{
		return;
	}
	if (FirstTry == 2)
	{
		SeedDefaultsGuarded(TEXT("db busy/unavailable"));
		return;
	}

	// Zero rows: genuinely-new player OR a veteran whose row was invisible to this
	// read. Positive-existence check: INSERT OR IGNORE the default row, then load
	// whatever the row now holds — a new player reads back the defaults we just
	// wrote; a conflicting pre-existing row survives the IGNORE and reads back its
	// REAL values (veteran rescued instead of default-seeded + destroyed at flush).
	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
	const FString InsertSql = FString::Printf(
		TEXT("INSERT OR IGNORE INTO NCRatingWipeout (UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc) ")
		TEXT("VALUES ('%s', %.6f, %.6f, %.6f, 0.0, 0, %lld);"),
		*Esc,
		TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility,
		static_cast<long long>(NowUtc));
	WO_ExecSqlNoRows(World, InsertSql);

	const int32 SecondTry = TryLoadRow();
	if (SecondTry == 0)
	{
		const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId);
		const bool bLooksNew = PR
			&& FMath::IsNearlyEqual(PR->GetRating(), TeamGlicko2::kDefaultRating, 0.01)
			&& PR->GetPerfGames() == 0;
		if (bLooksNew)
		{
			UE_LOG(LogWipeoutRating, Log, TEXT("New player %s — row created at defaults"), *UniqueId);
		}
		else
		{
			UE_LOG(LogWipeoutRating, Warning,
				TEXT("RESCUED %s: row was invisible on first read but exists (Rating=%.1f) — loaded real values instead of default-seeding"),
				*UniqueId, PR ? PR->GetRating() : 0.0);
		}
		return;
	}
	SeedDefaultsGuarded(TEXT("row unconfirmable after INSERT OR IGNORE"));
}

void FWipeoutRatingSystem::SnapshotMatchStart()
{
	Impl->RatingAtMatchStart.Empty();
	Impl->ActiveHumansThisMatch.Empty();
	Impl->HumansWithHumanOpposition.Empty();
	Impl->RoundLog.Empty();

	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const int32 RoundedElo = FMath::RoundToInt(Pair.Value.GetRating());
		Impl->RatingAtMatchStart.Add(Pair.Key, RoundedElo);
	}
	UE_LOG(LogWipeoutRating, Log, TEXT("Snapshot %d ratings at match start"), Impl->RatingAtMatchStart.Num());
}

void FWipeoutRatingSystem::ProcessRound(const FWipeoutRoundResult& Result)
{
	using namespace TeamGlicko2;

	if (Result.WinnerTeam.Num() == 0 || Result.LoserTeam.Num() == 0)
	{
		return;
	}

	// Per-round box-score log for the upload's rounds[] array (humans only —
	// synthetic BOT: keys skipped) + last-seen name capture for leaver naming.
	// Team attribution: on a draw the call site fills WinnerTeam with team 0 and
	// LoserTeam with team 1; otherwise WinnerTeamIndex names the winner's team.
	{
		const int32 WinTeamIdx = Result.bIsDraw ? 0 : Result.WinnerTeamIndex;
		if (WinTeamIdx == 0 || WinTeamIdx == 1)
		{
			FWipeoutRatingSystemImpl::FRoundRecord Rec;
			Rec.bIsDraw         = Result.bIsDraw;
			Rec.WinnerTeamIndex = Result.bIsDraw ? -1 : Result.WinnerTeamIndex;

			auto LogSide = [&](const TArray<FWipeoutPlayerRoundPerf>& Side, int32 TeamIdx, bool bWinner)
			{
				for (const FWipeoutPlayerRoundPerf& P : Side)
				{
					if (P.UniqueId.IsEmpty() || P.UniqueId.StartsWith(TEXT("BOT:"))) continue;
					if (!P.PlayerName.IsEmpty()) { Impl->PlayerNameCache.Add(P.UniqueId, P.PlayerName); }
					FWipeoutRatingSystemImpl::FRoundPlayerRecord RP;
					RP.UniqueId  = P.UniqueId;
					RP.TeamIndex = TeamIdx;
					RP.bWinner   = bWinner && !Result.bIsDraw;
					RP.Kills     = P.Kills;
					RP.Deaths    = P.Deaths;
					RP.Damage    = P.Damage;
					Rec.Players.Add(MoveTemp(RP));
				}
			};
			LogSide(Result.WinnerTeam, WinTeamIdx,     true);
			LogSide(Result.LoserTeam,  1 - WinTeamIdx, false);

			if (Rec.Players.Num() > 0)
			{
				Impl->RoundLog.Add(MoveTemp(Rec));
			}
		}
		else
		{
			UE_LOG(LogWipeoutRating, Warning,
				TEXT("ProcessRound: non-draw round with invalid WinnerTeamIndex=%d — round omitted from the upload log"),
				Result.WinnerTeamIndex);
		}
	}

	// Snapshot which UniqueIds had a real cached rating BEFORE this round.
	// Used post-ProcessMatch to gate write-back: only humans (loaded by PostLogin)
	// get cache updates. Bots / synthetic IDs are absent so their post-match
	// ratings — present only as transient placeholders below — are discarded.
	TSet<FString> CachedIdsBefore;
	CachedIdsBefore.Reserve(Impl->RatingCache.Num());
	for (const TPair<FString, PlayerRating>& Pair : Impl->RatingCache)
	{
		CachedIdsBefore.Add(Pair.Key);
	}

	auto BuildTeam = [this](const TArray<FWipeoutPlayerRoundPerf>& Perfs, std::vector<MatchPlayer>& Out)
	{
		Out.reserve(Perfs.Num());
		for (const FWipeoutPlayerRoundPerf& Perf : Perfs)
		{
			const PlayerRating* Cached = Impl->RatingCache.Find(Perf.UniqueId);
			PlayerRating PR;
			if (Cached)
			{
				PR = *Cached;
			}
			else
			{
				PR = PlayerRating(kDefaultRating, kDefaultRD, kDefaultVolatility);
			}
			Out.push_back(MatchPlayer(PR, Perf.ToPerfScore()));
		}
	};

	MatchResult Match;
	BuildTeam(Result.WinnerTeam, Match.teamA);
	BuildTeam(Result.LoserTeam,  Match.teamB);

	if (Match.teamA.empty() || Match.teamB.empty())
	{
		return;
	}

	if (Result.bIsDraw)
	{
		Match.scoreA = 0.5;
		Match.scoreB = 0.5;
	}
	else
	{
		Match.scoreA = 1.0;
		Match.scoreB = 0.0;
	}

	int32 HumansOnWinner = 0;
	int32 HumansOnLoser  = 0;
	for (const FWipeoutPlayerRoundPerf& Perf : Result.WinnerTeam)
	{
		if (CachedIdsBefore.Contains(Perf.UniqueId))
		{
			Impl->ActiveHumansThisMatch.Add(Perf.UniqueId);
			++HumansOnWinner;
		}
	}
	for (const FWipeoutPlayerRoundPerf& Perf : Result.LoserTeam)
	{
		if (CachedIdsBefore.Contains(Perf.UniqueId))
		{
			Impl->ActiveHumansThisMatch.Add(Perf.UniqueId);
			++HumansOnLoser;
		}
	}
	if (HumansOnWinner > 0 && HumansOnLoser > 0)
	{
		for (const FWipeoutPlayerRoundPerf& Perf : Result.WinnerTeam)
		{
			if (CachedIdsBefore.Contains(Perf.UniqueId))
			{
				Impl->HumansWithHumanOpposition.Add(Perf.UniqueId);
			}
		}
		for (const FWipeoutPlayerRoundPerf& Perf : Result.LoserTeam)
		{
			if (CachedIdsBefore.Contains(Perf.UniqueId))
			{
				Impl->HumansWithHumanOpposition.Add(Perf.UniqueId);
			}
		}
	}

	TeamGlicko2System::ProcessMatch(Match, /*bLobbyImpactBlend=*/true);

	auto WriteBack = [this, &CachedIdsBefore](const std::vector<MatchPlayer>& Team, const TArray<FWipeoutPlayerRoundPerf>& Perfs)
	{
		const int32 N = FMath::Min(static_cast<int32>(Team.size()), Perfs.Num());
		for (int32 i = 0; i < N; ++i)
		{
			if (CachedIdsBefore.Contains(Perfs[i].UniqueId))
			{
				Impl->RatingCache[Perfs[i].UniqueId] = Team[i].rating;
			}
		}
	};
	WriteBack(Match.teamA, Result.WinnerTeam);
	WriteBack(Match.teamB, Result.LoserTeam);
}

void FWipeoutRatingSystem::FlushAtMatchEnd(UWorld* World)
{
	if (!World) return;

	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
	const int32 BotMatchDeltaCap = 5;

	int32 PersistedCount = 0;
	int32 SkippedCount   = 0;
	int32 FailedCount    = 0;
	int32 CappedCount    = 0;

	for (TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const FString& UniqueId = Pair.Key;
		TeamGlicko2::PlayerRating& PR = Pair.Value;

		if (!Impl->ActiveHumansThisMatch.Contains(UniqueId))
		{
			++SkippedCount;
			continue;
		}

		// Never persist a player whose DB state was unconfirmable at load: their
		// cache was seeded from defaults, and an INSERT OR REPLACE from those
		// values would overwrite whatever real row the db holds.
		if (Impl->LoadFailedGuard.Contains(UniqueId))
		{
			UE_LOG(LogWipeoutRating, Warning,
				TEXT("Flush skip %s: DB load unconfirmed at login — not persisting over the stored row"), *UniqueId);
			++SkippedCount;
			continue;
		}

		const int32 NewEloRaw = FMath::RoundToInt(PR.GetRating());
		const int32 StartElo  = Impl->RatingAtMatchStart.FindRef(UniqueId);
		int32 Delta = (StartElo != 0) ? (NewEloRaw - StartElo) : 0;
		int32 FinalElo = NewEloRaw;

		const bool bFacedHumans = Impl->HumansWithHumanOpposition.Contains(UniqueId);
		if (!bFacedHumans && StartElo != 0 && (Delta > BotMatchDeltaCap || Delta < -BotMatchDeltaCap))
		{
			const int32 Clamped = FMath::Clamp(Delta, -BotMatchDeltaCap, BotMatchDeltaCap);
			FinalElo = StartElo + Clamped;
			PR.SetRating(static_cast<double>(FinalElo));
			UE_LOG(LogWipeoutRating, Log, TEXT("Bot-match clamp: %s delta %d -> %d (Elo %d -> %d, no human opposition)"),
				*UniqueId, Delta, Clamped, NewEloRaw, FinalElo);
			Delta = Clamped;
			++CappedCount;
		}

		const FString Esc = WO_SqlEscape(UniqueId);
		const FString Sql = FString::Printf(
			TEXT("INSERT OR REPLACE INTO NCRatingWipeout ")
			TEXT("(UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, SchemaVersion) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.6f, %d, %lld, 1);"),
			*Esc,
			PR.GetRating(), PR.GetRD(), PR.GetSigma(),
			PR.GetPerfIndexEMA(), PR.GetPerfGames(),
			static_cast<long long>(NowUtc));
		// Count what actually landed — an unchecked failure here used to report
		// "persisted" while the stored row silently went one match stale.
		if (WO_ExecSqlNoRows(World, Sql))
		{
			++PersistedCount;
		}
		else
		{
			++FailedCount;
			UE_LOG(LogWipeoutRating, Warning,
				TEXT("Flush FAILED for %s: rating write did not persist (db busy/locked?) — stored row is now one match stale"),
				*UniqueId);
		}
	}

	UE_LOG(LogWipeoutRating, Log, TEXT("FlushAtMatchEnd: persisted %d, failed %d, skipped %d (didn't play), capped %d (no human opposition)"),
		PersistedCount, FailedCount, SkippedCount, CappedCount);
}

int32 FWipeoutRatingSystem::GetCachedElo(const FString& UniqueId) const
{
	if (const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId))
	{
		return FMath::RoundToInt(PR->GetRating());
	}
	return FMath::RoundToInt(TeamGlicko2::kDefaultRating);
}

void FWipeoutRatingSystem::Forget(const FString& UniqueId)
{
	Impl->RatingCache.Remove(UniqueId);
	Impl->RatingAtMatchStart.Remove(UniqueId);
}

FString FWipeoutRatingSystem::BuildResultPayload(UWorld* World, const FNCWipeoutMatchInput& In) const
{
	using namespace TeamGlicko2;

	TArray<int32> HumanIndices;
	HumanIndices.Reserve(In.Players.Num());
	TSet<FString> RosterIds;
	for (int32 i = 0; i < In.Players.Num(); ++i)
	{
		const FNCWipeoutPlayerInput& P = In.Players[i];
		if (P.UniqueId.IsEmpty()) continue;
		if (Impl->RatingCache.Contains(P.UniqueId))
		{
			HumanIndices.Add(i);
			RosterIds.Add(P.UniqueId);
		}
	}

	// LEAVER FIX (2026-07-17): In.Players comes from PlayerArray at match end, so
	// mid-match leavers are missing — their delta never reached ut4stats and the
	// site's ladder diverged from the hub's. Union in every cached human who
	// played a round but is gone from the roster (same fix as ElimPlus; see the
	// J@r0d 5-of-5 enumeration).
	TArray<FString> LeaverIds;
	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const FString& Id = Pair.Key;
		if (RosterIds.Contains(Id)) continue;
		if (!Impl->ActiveHumansThisMatch.Contains(Id)) continue; // login ghosts / pure spectators
		LeaverIds.Add(Id);
	}

	if (HumanIndices.Num() == 0 && LeaverIds.Num() == 0)
	{
		UE_LOG(LogWipeoutRating, Warning,
			TEXT("BuildResultPayload: no human players in cache — skipping upload"));
		return FString();
	}

	// Server name via NCEloUploader helper — matches StatSQL's Mod.ini value
	// plus the per-instance suffix; correlation substring-matches it.
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
	Writer->WriteValue(TEXT("mode"),          FString(TEXT("Wipeout")));
	Writer->WriteValue(TEXT("server"),        ServerName);
	Writer->WriteValue(TEXT("map"),           MapName);
	Writer->WriteValue(TEXT("played_at_utc"), PlayedAtUtc);
	Writer->WriteValue(TEXT("winner_team"),   In.WinnerTeamIndex);
	Writer->WriteValue(TEXT("red_score"),     In.RedScore);
	Writer->WriteValue(TEXT("blue_score"),    In.BlueScore);
	Writer->WriteArrayStart(TEXT("players"));

	for (int32 Idx : HumanIndices)
	{
		const FNCWipeoutPlayerInput& P = In.Players[Idx];
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
		Writer->WriteValue(TEXT("id"),            P.UniqueId);
		Writer->WriteValue(TEXT("name"),          P.PlayerName);
		Writer->WriteValue(TEXT("team"),          P.TeamIndex);
		Writer->WriteValue(TEXT("result"),        FString(Result));
		Writer->WriteValue(TEXT("kills"),         P.Kills);
		Writer->WriteValue(TEXT("deaths"),        P.Deaths);
		Writer->WriteValue(TEXT("damage"),        P.Damage);
		Writer->WriteValue(TEXT("pre"),           static_cast<double>(PreElo));
		Writer->WriteValue(TEXT("post"),          PR->GetRating());
		Writer->WriteValue(TEXT("delta"),         Delta);
		Writer->WriteValue(TEXT("rd"),            PR->GetRD());
		Writer->WriteValue(TEXT("sigma"),         PR->GetSigma());
		Writer->WriteValue(TEXT("faced_humans"),  bFacedHumans);
		if (Impl->LoadFailedGuard.Contains(P.UniqueId))
		{
			// Session ran on unconfirmed default-seeded state — numbers are not
			// trustworthy. Additive key: old Django ignores it; new Django records
			// the row without applying the delta.
			Writer->WriteValue(TEXT("provisional"), true);
		}
		Writer->WriteObjectEnd();
	}

	// Leavers: roster entry is gone, so team + this-match box score come from the
	// per-round log; name from the round-capture name cache.
	int32 LeaversEmitted = 0;
	for (const FString& Id : LeaverIds)
	{
		const PlayerRating* PR = Impl->RatingCache.Find(Id);
		if (!PR) continue;

		int32  TeamIndex = -1;
		int32  Kills = 0, Deaths = 0;
		double Damage = 0.0;
		for (const FWipeoutRatingSystemImpl::FRoundRecord& Rec : Impl->RoundLog)
		{
			for (const FWipeoutRatingSystemImpl::FRoundPlayerRecord& RP : Rec.Players)
			{
				if (RP.UniqueId == Id)
				{
					TeamIndex = RP.TeamIndex;
					Kills  += RP.Kills;
					Deaths += RP.Deaths;
					Damage += RP.Damage;
				}
			}
		}
		if (TeamIndex < 0)
		{
			UE_LOG(LogWipeoutRating, Warning,
				TEXT("BuildResultPayload: leaver %s has no round-log lines — omitted from upload"), *Id);
			continue;
		}

		const int32 PreElo  = Impl->RatingAtMatchStart.FindRef(Id);
		const int32 PostElo = FMath::RoundToInt(PR->GetRating());
		const int32 Delta   = (PreElo != 0) ? (PostElo - PreElo) : 0;

		const TCHAR* Result;
		if (In.WinnerTeamIndex < 0) { Result = TEXT("draw"); }
		else                        { Result = (TeamIndex == In.WinnerTeamIndex) ? TEXT("win") : TEXT("loss"); }

		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"),            Id);
		Writer->WriteValue(TEXT("name"),          Impl->PlayerNameCache.FindRef(Id));
		Writer->WriteValue(TEXT("team"),          TeamIndex);
		Writer->WriteValue(TEXT("result"),        FString(Result));
		Writer->WriteValue(TEXT("kills"),         Kills);
		Writer->WriteValue(TEXT("deaths"),        Deaths);
		Writer->WriteValue(TEXT("damage"),        Damage);
		Writer->WriteValue(TEXT("pre"),           static_cast<double>(PreElo));
		Writer->WriteValue(TEXT("post"),          PR->GetRating());
		Writer->WriteValue(TEXT("delta"),         Delta);
		Writer->WriteValue(TEXT("rd"),            PR->GetRD());
		Writer->WriteValue(TEXT("sigma"),         PR->GetSigma());
		Writer->WriteValue(TEXT("faced_humans"),  Impl->HumansWithHumanOpposition.Contains(Id));
		Writer->WriteValue(TEXT("left"),          true);
		if (Impl->LoadFailedGuard.Contains(Id))
		{
			Writer->WriteValue(TEXT("provisional"), true);
		}
		Writer->WriteObjectEnd();
		++LeaversEmitted;
	}
	if (LeaversEmitted > 0)
	{
		UE_LOG(LogWipeoutRating, Log,
			TEXT("BuildResultPayload: included %d mid-match leaver(s) alongside %d rostered player(s)"),
			LeaversEmitted, HumanIndices.Num());
	}

	Writer->WriteArrayEnd();

	// Per-round box-score log (rounds[]) — same shape as the ElimPlus payload.
	// This is the array the ut4stats Django-authoritative Wipeout handler derives
	// perf from (2026-07-17); payloads WITHOUT it fall back to the legacy
	// hub-delta accumulator there. Additive: old Django ignores it.
	Writer->WriteArrayStart(TEXT("rounds"));
	for (int32 r = 0; r < Impl->RoundLog.Num(); ++r)
	{
		const FWipeoutRatingSystemImpl::FRoundRecord& Rec = Impl->RoundLog[r];
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("index"),       r);
		Writer->WriteValue(TEXT("winner_team"), Rec.WinnerTeamIndex);
		Writer->WriteValue(TEXT("draw"),        Rec.bIsDraw);
		Writer->WriteArrayStart(TEXT("players"));
		for (const FWipeoutRatingSystemImpl::FRoundPlayerRecord& RP : Rec.Players)
		{
			const TCHAR* RResult = Rec.bIsDraw ? TEXT("draw") : (RP.bWinner ? TEXT("win") : TEXT("loss"));
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("id"),     RP.UniqueId);
			Writer->WriteValue(TEXT("team"),   RP.TeamIndex);
			Writer->WriteValue(TEXT("kills"),  RP.Kills);
			Writer->WriteValue(TEXT("deaths"), RP.Deaths);
			Writer->WriteValue(TEXT("damage"), RP.Damage);
			Writer->WriteValue(TEXT("result"), FString(RResult));
			Writer->WriteObjectEnd();
		}
		Writer->WriteArrayEnd();
		Writer->WriteObjectEnd();
	}
	Writer->WriteArrayEnd();

	Writer->WriteObjectEnd();
	Writer->Close();

	return Out;
}
