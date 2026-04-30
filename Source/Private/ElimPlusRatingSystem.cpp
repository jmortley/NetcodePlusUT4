// ElimPlusRatingSystem.cpp — Mods.db persistence + ProcessMatch glue.

#include "ElimPlusRatingSystem.h"
#include "UnrealTournament.h"
#include "UTGameInstance.h"            // FDatabaseRow + ExecDatabaseCommand
#include "ElimPlusStatsReplicator.h"
#include "TeamGlicko2System.h"         // Vendored: ProcessMatch
#include "TeamGlicko2Config.h"         // Vendored: kDefault* constants
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogElimPlusRating, Log, All);

namespace
{
	/** SQL-escape a string by doubling single quotes. UniqueIds are typically
	 *  hex/alphanumeric so this is belt-and-suspenders, but cheap to apply. */
	FString SqlEscape(const FString& In)
	{
		return In.Replace(TEXT("'"), TEXT("''"));
	}

	/** Run a SQL command via the engine's UTGameInstance wrapper. Returns true
	 *  if the call succeeded (regardless of row count). Server-only path:
	 *  ExecDatabaseCommand is no-op without USE_SQLITE on the target. */
	bool ExecSql(UWorld* World, const FString& Sql, TArray<FDatabaseRow>& OutRows)
	{
		if (!World) return false;
		UUTGameInstance* GI = Cast<UUTGameInstance>(World->GetGameInstance());
		if (!GI) return false;
		return GI->ExecDatabaseCommand(Sql, OutRows);
	}

	bool ExecSqlNoRows(UWorld* World, const FString& Sql)
	{
		TArray<FDatabaseRow> Discard;
		return ExecSql(World, Sql, Discard);
	}
}

FElimPlusRatingSystem::FElimPlusRatingSystem() {}

bool FElimPlusRatingSystem::InitDatabase(UWorld* World)
{
	// One CREATE per call. Use IF NOT EXISTS so it's safe to call every BeginPlay.
	const FString Sql =
		TEXT("CREATE TABLE IF NOT EXISTS NCRatingElimPlus (")
		TEXT("  UniqueId       TEXT PRIMARY KEY NOT NULL,")
		TEXT("  Rating         REAL NOT NULL DEFAULT 1400.0,")
		TEXT("  RD             REAL NOT NULL DEFAULT 350.0,")
		TEXT("  Sigma          REAL NOT NULL DEFAULT 0.06,")
		TEXT("  PerfIndexEMA   REAL NOT NULL DEFAULT 0.0,")
		TEXT("  PerfGames      INTEGER NOT NULL DEFAULT 0,")
		TEXT("  LastSeenUtc    INTEGER NOT NULL DEFAULT 0,")
		TEXT("  SchemaVersion  INTEGER NOT NULL DEFAULT 1")
		TEXT(");");

	const bool bOk = ExecSqlNoRows(World, Sql);
	UE_LOG(LogElimPlusRating, Log, TEXT("InitDatabase: NCRatingElimPlus %s"),
		bOk ? TEXT("ready") : TEXT("FAILED (USE_SQLITE off?)"));
	return bOk;
}

void FElimPlusRatingSystem::LoadPlayerFromDB(UWorld* World, const FString& UniqueId)
{
	if (UniqueId.IsEmpty()) return;
	if (RatingCache.Contains(UniqueId)) return; // already loaded this match

	const FString Esc = SqlEscape(UniqueId);
	const FString Sql = FString::Printf(
		TEXT("SELECT Rating, RD, Sigma, PerfIndexEMA, PerfGames FROM NCRatingElimPlus WHERE UniqueId='%s';"),
		*Esc);

	TArray<FDatabaseRow> Rows;
	const bool bOk = ExecSql(World, Sql, Rows);

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
		RatingCache.Add(UniqueId, PR);
		UE_LOG(LogElimPlusRating, Log, TEXT("Loaded %s: Rating=%.1f RD=%.1f σ=%.4f"),
			*UniqueId, Rating, RD, Sigma);
	}
	else
	{
		// New player — insert default and cache.
		TeamGlicko2::PlayerRating PR(
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility);
		RatingCache.Add(UniqueId, PR);

		const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
		const FString InsertSql = FString::Printf(
			TEXT("INSERT OR IGNORE INTO NCRatingElimPlus (UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, 0.0, 0, %lld);"),
			*Esc,
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility,
			static_cast<long long>(NowUtc));
		ExecSqlNoRows(World, InsertSql);
		UE_LOG(LogElimPlusRating, Log, TEXT("New player %s — cached defaults + INSERT OR IGNORE"), *UniqueId);
	}
}

void FElimPlusRatingSystem::SnapshotMatchStart()
{
	RatingAtMatchStart.Empty();
	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : RatingCache)
	{
		const int32 RoundedElo = FMath::RoundToInt(Pair.Value.GetRating());
		RatingAtMatchStart.Add(Pair.Key, RoundedElo);
	}
	UE_LOG(LogElimPlusRating, Log, TEXT("Snapshot %d ratings at match start"), RatingAtMatchStart.Num());
}

void FElimPlusRatingSystem::ProcessRound(const FElimPlusRoundResult& Result)
{
	using namespace TeamGlicko2;

	// Skip degenerate rounds (e.g. 0v0, 1v0).
	if (Result.WinnerTeam.Num() == 0 || Result.LoserTeam.Num() == 0)
	{
		return;
	}

	// Build MatchResult from cached ratings + per-round perf scores.
	// Note: Tron's MatchResult uses std::vector (not UE TArray) — STL methods.
	MatchResult Match;
	Match.teamA.reserve(Result.WinnerTeam.Num());
	Match.teamB.reserve(Result.LoserTeam.Num());

	for (const FElimPlusPlayerRoundPerf& Perf : Result.WinnerTeam)
	{
		PlayerRating* Cached = RatingCache.Find(Perf.UniqueId);
		if (!Cached) continue;
		Match.teamA.push_back(MatchPlayer(*Cached, Perf.ToPerfScore()));
	}
	for (const FElimPlusPlayerRoundPerf& Perf : Result.LoserTeam)
	{
		PlayerRating* Cached = RatingCache.Find(Perf.UniqueId);
		if (!Cached) continue;
		Match.teamB.push_back(MatchPlayer(*Cached, Perf.ToPerfScore()));
	}

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

	// Tron's library mutates Match.teamA[i].rating + Match.teamB[i].rating in place.
	TeamGlicko2System::ProcessMatch(Match);

	// Copy mutated ratings back to our cache by UniqueId.
	// std::vector on the Tron side, TArray on our side.
	auto WriteBack = [this](const std::vector<MatchPlayer>& Team, const TArray<FElimPlusPlayerRoundPerf>& Perfs)
	{
		const int32 N = FMath::Min(static_cast<int32>(Team.size()), Perfs.Num());
		for (int32 i = 0; i < N; ++i)
		{
			RatingCache.FindOrAdd(Perfs[i].UniqueId) = Team[i].rating;
		}
	};
	WriteBack(Match.teamA, Result.WinnerTeam);
	WriteBack(Match.teamB, Result.LoserTeam);
}

void FElimPlusRatingSystem::FlushAtMatchEnd(UWorld* World, AElimPlusStatsReplicator* Replicator)
{
	if (!World)
	{
		return;
	}

	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();

	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : RatingCache)
	{
		const FString& UniqueId = Pair.Key;
		const TeamGlicko2::PlayerRating& PR = Pair.Value;
		const FString Esc = SqlEscape(UniqueId);

		const FString Sql = FString::Printf(
			TEXT("INSERT OR REPLACE INTO NCRatingElimPlus ")
			TEXT("(UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, SchemaVersion) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.6f, %d, %lld, 1);"),
			*Esc,
			PR.GetRating(), PR.GetRD(), PR.GetSigma(),
			PR.GetPerfIndexEMA(), PR.GetPerfGames(),
			static_cast<long long>(NowUtc));
		ExecSqlNoRows(World, Sql);

		// Replicator update — only NOW does the HUD ELO change, with a delta vs
		// match-start. Clients see "1402 +2" or "1395 -5" etc. once at match end.
		if (Replicator)
		{
			const int32 NewElo  = FMath::RoundToInt(PR.GetRating());
			const int32 StartElo = RatingAtMatchStart.FindRef(UniqueId); // 0 if absent => delta = NewElo
			const int32 Delta = (StartElo != 0) ? (NewElo - StartElo) : 0;
			Replicator->SetPlayerEloAndDelta(UniqueId, NewElo, Delta);
		}
	}

	UE_LOG(LogElimPlusRating, Log, TEXT("FlushAtMatchEnd: persisted %d ratings"), RatingCache.Num());
}

int32 FElimPlusRatingSystem::GetCachedElo(const FString& UniqueId) const
{
	if (const TeamGlicko2::PlayerRating* PR = RatingCache.Find(UniqueId))
	{
		return FMath::RoundToInt(PR->GetRating());
	}
	return FMath::RoundToInt(TeamGlicko2::kDefaultRating);
}

void FElimPlusRatingSystem::Forget(const FString& UniqueId)
{
	RatingCache.Remove(UniqueId);
	RatingAtMatchStart.Remove(UniqueId);
}

const TeamGlicko2::PlayerRating* FElimPlusRatingSystem::FindRating(const FString& UniqueId) const
{
	return RatingCache.Find(UniqueId);
}

bool FElimPlusRatingSystem::IsPlayerActive(const FString& UniqueId) const
{
	return RatingAtMatchStart.Contains(UniqueId);
}
