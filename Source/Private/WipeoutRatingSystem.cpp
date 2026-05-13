// WipeoutRatingSystem.cpp — Mods.db persistence + per-round ProcessMatch glue.
// Tron's TeamGlicko2 headers (and their <vector>/<cmath> baggage) stay confined
// to this translation unit thanks to the Pimpl in the header.

#include "WipeoutRatingSystem.h"
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
	const FString Sql =
		TEXT("CREATE TABLE IF NOT EXISTS NCRatingWipeout (")
		TEXT("  UniqueId       TEXT PRIMARY KEY NOT NULL,")
		TEXT("  Rating         REAL NOT NULL DEFAULT 1400.0,")
		TEXT("  RD             REAL NOT NULL DEFAULT 350.0,")
		TEXT("  Sigma          REAL NOT NULL DEFAULT 0.06,")
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
	const FString Sql = FString::Printf(
		TEXT("SELECT Rating, RD, Sigma, PerfIndexEMA, PerfGames FROM NCRatingWipeout WHERE UniqueId='%s';"),
		*Esc);

	TArray<FDatabaseRow> Rows;
	const bool bOk = WO_ExecSql(World, Sql, Rows);

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
		UE_LOG(LogWipeoutRating, Log, TEXT("Loaded %s: Rating=%.1f RD=%.1f sigma=%.4f"),
			*UniqueId, Rating, RD, Sigma);
	}
	else
	{
		TeamGlicko2::PlayerRating PR(
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility);
		Impl->RatingCache.Add(UniqueId, PR);

		const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
		const FString InsertSql = FString::Printf(
			TEXT("INSERT OR IGNORE INTO NCRatingWipeout (UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, 0.0, 0, %lld);"),
			*Esc,
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility,
			static_cast<long long>(NowUtc));
		WO_ExecSqlNoRows(World, InsertSql);
		UE_LOG(LogWipeoutRating, Log, TEXT("New player %s — cached defaults + INSERT OR IGNORE"), *UniqueId);
	}
}

void FWipeoutRatingSystem::SnapshotMatchStart()
{
	Impl->RatingAtMatchStart.Empty();
	Impl->ActiveHumansThisMatch.Empty();
	Impl->HumansWithHumanOpposition.Empty();

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

	TeamGlicko2System::ProcessMatch(Match);

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
		WO_ExecSqlNoRows(World, Sql);
		++PersistedCount;
	}

	UE_LOG(LogWipeoutRating, Log, TEXT("FlushAtMatchEnd: persisted %d, skipped %d (didn't play), capped %d (no human opposition)"),
		PersistedCount, SkippedCount, CappedCount);
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
	for (int32 i = 0; i < In.Players.Num(); ++i)
	{
		const FNCWipeoutPlayerInput& P = In.Players[i];
		if (P.UniqueId.IsEmpty()) continue;
		if (Impl->RatingCache.Contains(P.UniqueId))
		{
			HumanIndices.Add(i);
		}
	}
	if (HumanIndices.Num() == 0)
	{
		UE_LOG(LogWipeoutRating, Warning,
			TEXT("BuildResultPayload: no human players in cache — skipping upload"));
		return FString();
	}

	FString ServerName;
	if (World)
	{
		if (AUTGameState* GS = World->GetGameState<AUTGameState>())
		{
			ServerName = GS->ServerName;
		}
	}
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
		Writer->WriteObjectEnd();
	}

	Writer->WriteArrayEnd();
	Writer->WriteObjectEnd();
	Writer->Close();

	return Out;
}
