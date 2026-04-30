// ElimPlusRatingSystem.cpp — Mods.db persistence + ProcessMatch glue.
// Tron's TeamGlicko2 headers (and their <iostream>/<vector>/<cmath> bagage)
// stay confined to this translation unit thanks to the Pimpl in the header.

#include "ElimPlusRatingSystem.h"
#include "UnrealTournament.h"
#include "UTGameInstance.h"            // FDatabaseRow + ExecDatabaseCommand
#include "ElimPlusStatsReplicator.h"
#include "Engine/World.h"

// Vendored TeamGlicko2 — included only here, never via the .h.
#include "TeamGlickoRating.h"
#include "TeamGlicko2System.h"
#include "TeamGlicko2Config.h"

DEFINE_LOG_CATEGORY_STATIC(LogElimPlusRating, Log, All);

// =============================================================================
// Pimpl: the actual cache. Defined here where TeamGlicko2 types are visible.
// =============================================================================
struct FElimPlusRatingSystemImpl
{
	/** UniqueId -> mutable PlayerRating. Updated each ProcessRound. */
	TMap<FString, TeamGlicko2::PlayerRating> RatingCache;

	/** UniqueId -> rating-rounded-int captured by SnapshotMatchStart.
	 *  Used at FlushAtMatchEnd to compute (NewElo - StartElo) delta. */
	TMap<FString, int32> RatingAtMatchStart;

	/** Lifetime PPR accumulator: sum of every round's PPR across all matches the
	 *  player has played. Lifetime mean = TotalPoints / max(1, RoundsPlayed). */
	TMap<FString, double> TotalPointsCache;

	/** Lifetime round count: incremented once per round completed by this player. */
	TMap<FString, int32> RoundsPlayedCache;

	/** UniqueIds of cached humans who appeared in at least one round's
	 *  MatchResult during this match. Disconnected-before-spawn / kicked-at-login
	 *  players (e.g. plugin-mismatch) get cached by PostLogin but never end up
	 *  here, so FlushAtMatchEnd can skip them entirely. Cleared each
	 *  SnapshotMatchStart. */
	TSet<FString> ActiveHumansThisMatch;

	/** Subset of ActiveHumansThisMatch — humans who faced at least one human
	 *  opponent (= round had cached players on BOTH teams). Used at flush to
	 *  decide whether the rating change is "real" or should be clamped to the
	 *  bot-match cap. Cleared each SnapshotMatchStart. */
	TSet<FString> HumansWithHumanOpposition;
};

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

// =============================================================================
// FElimPlusRatingSystem
// =============================================================================

FElimPlusRatingSystem::FElimPlusRatingSystem()
	: Impl(MakeUnique<FElimPlusRatingSystemImpl>())
{}

FElimPlusRatingSystem::~FElimPlusRatingSystem() = default;

bool FElimPlusRatingSystem::InitDatabase(UWorld* World)
{
	const FString Sql =
		TEXT("CREATE TABLE IF NOT EXISTS NCRatingElimPlus (")
		TEXT("  UniqueId       TEXT PRIMARY KEY NOT NULL,")
		TEXT("  Rating         REAL NOT NULL DEFAULT 1400.0,")
		TEXT("  RD             REAL NOT NULL DEFAULT 350.0,")
		TEXT("  Sigma          REAL NOT NULL DEFAULT 0.06,")
		TEXT("  PerfIndexEMA   REAL NOT NULL DEFAULT 0.0,")
		TEXT("  PerfGames      INTEGER NOT NULL DEFAULT 0,")
		TEXT("  LastSeenUtc    INTEGER NOT NULL DEFAULT 0,")
		TEXT("  TotalPoints    REAL NOT NULL DEFAULT 0.0,")
		TEXT("  RoundsPlayed   INTEGER NOT NULL DEFAULT 0,")
		TEXT("  SchemaVersion  INTEGER NOT NULL DEFAULT 2")
		TEXT(");");

	const bool bOk = ExecSqlNoRows(World, Sql);
	UE_LOG(LogElimPlusRating, Log, TEXT("InitDatabase: NCRatingElimPlus %s"),
		bOk ? TEXT("ready") : TEXT("FAILED (USE_SQLITE off?)"));

	// Schema migration v1 -> v2: add TotalPoints + RoundsPlayed columns to existing
	// tables. ALTER errors if the column already exists; we ignore the bool return
	// since the steady-state outcome is the same either way (column present).
	ExecSqlNoRows(World, TEXT("ALTER TABLE NCRatingElimPlus ADD COLUMN TotalPoints REAL NOT NULL DEFAULT 0.0;"));
	ExecSqlNoRows(World, TEXT("ALTER TABLE NCRatingElimPlus ADD COLUMN RoundsPlayed INTEGER NOT NULL DEFAULT 0;"));

	return bOk;
}

void FElimPlusRatingSystem::LoadPlayerFromDB(UWorld* World, const FString& UniqueId)
{
	if (UniqueId.IsEmpty()) return;
	if (Impl->RatingCache.Contains(UniqueId)) return;

	const FString Esc = SqlEscape(UniqueId);
	const FString Sql = FString::Printf(
		TEXT("SELECT Rating, RD, Sigma, PerfIndexEMA, PerfGames, TotalPoints, RoundsPlayed FROM NCRatingElimPlus WHERE UniqueId='%s';"),
		*Esc);

	TArray<FDatabaseRow> Rows;
	const bool bOk = ExecSql(World, Sql, Rows);

	if (bOk && Rows.Num() > 0 && Rows[0].Text.Num() >= 7)
	{
		const double Rating       = FCString::Atod(*Rows[0].Text[0]);
		const double RD           = FCString::Atod(*Rows[0].Text[1]);
		const double Sigma        = FCString::Atod(*Rows[0].Text[2]);
		const double PerfIndexEMA = FCString::Atod(*Rows[0].Text[3]);
		const int32  PerfGames    = FCString::Atoi(*Rows[0].Text[4]);
		const double TotalPoints  = FCString::Atod(*Rows[0].Text[5]);
		const int32  RoundsPlayed = FCString::Atoi(*Rows[0].Text[6]);

		TeamGlicko2::PlayerRating PR(Rating, RD, Sigma);
		PR.SetPerfIndexEMA(PerfIndexEMA);
		PR.SetPerfGames(PerfGames);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->TotalPointsCache.Add(UniqueId, TotalPoints);
		Impl->RoundsPlayedCache.Add(UniqueId, RoundsPlayed);
		UE_LOG(LogElimPlusRating, Log, TEXT("Loaded %s: Rating=%.1f RD=%.1f sigma=%.4f LifetimePPR=%.2f (%d rounds)"),
			*UniqueId, Rating, RD, Sigma,
			(RoundsPlayed > 0) ? float(TotalPoints / RoundsPlayed) : 0.f, RoundsPlayed);
	}
	else
	{
		TeamGlicko2::PlayerRating PR(
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->TotalPointsCache.Add(UniqueId, 0.0);
		Impl->RoundsPlayedCache.Add(UniqueId, 0);

		const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
		const FString InsertSql = FString::Printf(
			TEXT("INSERT OR IGNORE INTO NCRatingElimPlus (UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, TotalPoints, RoundsPlayed) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, 0.0, 0, %lld, 0.0, 0);"),
			*Esc,
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility,
			static_cast<long long>(NowUtc));
		ExecSqlNoRows(World, InsertSql);
		UE_LOG(LogElimPlusRating, Log, TEXT("New player %s — cached defaults + INSERT OR IGNORE"), *UniqueId);
	}
}

void FElimPlusRatingSystem::SnapshotMatchStart()
{
	Impl->RatingAtMatchStart.Empty();
	Impl->ActiveHumansThisMatch.Empty();
	Impl->HumansWithHumanOpposition.Empty();

	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const int32 RoundedElo = FMath::RoundToInt(Pair.Value.GetRating());
		Impl->RatingAtMatchStart.Add(Pair.Key, RoundedElo);
	}
	UE_LOG(LogElimPlusRating, Log, TEXT("Snapshot %d ratings at match start"), Impl->RatingAtMatchStart.Num());
}

void FElimPlusRatingSystem::ProcessRound(const FElimPlusRoundResult& Result)
{
	using namespace TeamGlicko2;

	if (Result.WinnerTeam.Num() == 0 || Result.LoserTeam.Num() == 0)
	{
		return;
	}

	// Snapshot which UniqueIds had a real cached rating BEFORE this round.
	// Used post-ProcessMatch to gate write-back: only humans (loaded by PostLogin)
	// get cache updates. Bots / "BOT:<name>" synthetic IDs are absent from this
	// set, so their post-match ratings — present only as transient placeholders
	// below — are discarded.
	TSet<FString> CachedIdsBefore;
	CachedIdsBefore.Reserve(Impl->RatingCache.Num());
	for (const TPair<FString, PlayerRating>& Pair : Impl->RatingCache)
	{
		CachedIdsBefore.Add(Pair.Key);
	}

	// Build MatchResult. For UniqueIds NOT in cache (bots, late joiners), use a
	// transient default PlayerRating(1400, 350, 0.06) so team-size math is honest.
	auto BuildTeam = [this](const TArray<FElimPlusPlayerRoundPerf>& Perfs, std::vector<MatchPlayer>& Out)
	{
		Out.reserve(Perfs.Num());
		for (const FElimPlusPlayerRoundPerf& Perf : Perfs)
		{
			const PlayerRating* Cached = Impl->RatingCache.Find(Perf.UniqueId);
			const PlayerRating PR = Cached
				? *Cached
				: PlayerRating(kDefaultRating, kDefaultRD, kDefaultVolatility);
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

	// Activity tracking — count cached humans on each side BEFORE we mutate the
	// cache via ProcessMatch. Two purposes:
	//  1. ActiveHumansThisMatch: who actually played a round (vs PostLogin-cached
	//     ghosts like plugin-mismatch kicks who never spawned).
	//  2. HumansWithHumanOpposition: who faced at least one human in any round.
	//     Anyone NOT in this set at match-end gets the bot-match delta cap.
	int32 HumansOnWinner = 0;
	int32 HumansOnLoser  = 0;
	for (const FElimPlusPlayerRoundPerf& Perf : Result.WinnerTeam)
	{
		if (CachedIdsBefore.Contains(Perf.UniqueId))
		{
			Impl->ActiveHumansThisMatch.Add(Perf.UniqueId);
			++HumansOnWinner;
		}
	}
	for (const FElimPlusPlayerRoundPerf& Perf : Result.LoserTeam)
	{
		if (CachedIdsBefore.Contains(Perf.UniqueId))
		{
			Impl->ActiveHumansThisMatch.Add(Perf.UniqueId);
			++HumansOnLoser;
		}
	}
	if (HumansOnWinner > 0 && HumansOnLoser > 0)
	{
		for (const FElimPlusPlayerRoundPerf& Perf : Result.WinnerTeam)
		{
			if (CachedIdsBefore.Contains(Perf.UniqueId))
			{
				Impl->HumansWithHumanOpposition.Add(Perf.UniqueId);
			}
		}
		for (const FElimPlusPlayerRoundPerf& Perf : Result.LoserTeam)
		{
			if (CachedIdsBefore.Contains(Perf.UniqueId))
			{
				Impl->HumansWithHumanOpposition.Add(Perf.UniqueId);
			}
		}
	}

	TeamGlicko2System::ProcessMatch(Match);

	// Write-back: only update cache for IDs that were already in it (= humans).
	auto WriteBack = [this, &CachedIdsBefore](const std::vector<MatchPlayer>& Team, const TArray<FElimPlusPlayerRoundPerf>& Perfs)
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

void FElimPlusRatingSystem::FlushAtMatchEnd(UWorld* World, AElimPlusStatsReplicator* Replicator)
{
	if (!World)
	{
		return;
	}

	const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();

	// Bot-match cap: any cached human who never faced human opposition during
	// the match (= every round was vs all-bots) gets their rating delta clamped
	// to ±BotMatchDeltaCap. Replaces the old "RatingAtMatchStart.Num() == 1"
	// solo check, which mis-counted ghosts (PostLogin-cached players who got
	// kicked for plugin mismatch and never spawned).
	const int32 BotMatchDeltaCap = 5; // tunable

	int32 PersistedCount = 0;
	int32 SkippedCount   = 0;
	int32 CappedCount    = 0;

	for (TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const FString& UniqueId = Pair.Key;
		TeamGlicko2::PlayerRating& PR = Pair.Value;

		// Skip cached humans who never appeared in any round's MatchResult
		// (kicked at login, AFK spectators, joined-then-left before round 1).
		// They have no real activity to persist; their PostLogin INSERT OR
		// IGNORE row in the DB already represents them at default 1400.
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
			UE_LOG(LogElimPlusRating, Log, TEXT("Bot-match clamp: %s delta %d -> %d (Elo %d -> %d, no human opposition)"),
				*UniqueId, Delta, Clamped, NewEloRaw, FinalElo);
			Delta = Clamped;
			++CappedCount;
		}

		const double TotalPoints  = Impl->TotalPointsCache.FindRef(UniqueId);
		const int32  RoundsPlayed = Impl->RoundsPlayedCache.FindRef(UniqueId);

		const FString Esc = SqlEscape(UniqueId);
		const FString Sql = FString::Printf(
			TEXT("INSERT OR REPLACE INTO NCRatingElimPlus ")
			TEXT("(UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, TotalPoints, RoundsPlayed, SchemaVersion) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.6f, %d, %lld, %.6f, %d, 2);"),
			*Esc,
			PR.GetRating(), PR.GetRD(), PR.GetSigma(),
			PR.GetPerfIndexEMA(), PR.GetPerfGames(),
			static_cast<long long>(NowUtc),
			TotalPoints, RoundsPlayed);
		ExecSqlNoRows(World, Sql);

		if (Replicator)
		{
			Replicator->SetPlayerEloAndDelta(UniqueId, FinalElo, Delta);
		}
		++PersistedCount;
	}

	UE_LOG(LogElimPlusRating, Log, TEXT("FlushAtMatchEnd: persisted %d, skipped %d (didn't play), capped %d (no human opposition)"),
		PersistedCount, SkippedCount, CappedCount);
}

int32 FElimPlusRatingSystem::GetCachedElo(const FString& UniqueId) const
{
	if (const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId))
	{
		return FMath::RoundToInt(PR->GetRating());
	}
	return FMath::RoundToInt(TeamGlicko2::kDefaultRating);
}

void FElimPlusRatingSystem::RecordRoundPPR(const FString& UniqueId, float RoundPPR)
{
	if (UniqueId.IsEmpty()) return;
	// Bots/unrated synthetic IDs (e.g. "BOT:Foo") aren't loaded by LoadPlayerFromDB
	// and never enter the rating cache. Gate on RatingCache membership so we don't
	// accidentally accumulate lifetime stats for transient placeholders.
	if (!Impl->RatingCache.Contains(UniqueId)) return;

	Impl->TotalPointsCache.FindOrAdd(UniqueId) += static_cast<double>(RoundPPR);
	Impl->RoundsPlayedCache.FindOrAdd(UniqueId) += 1;
}

float FElimPlusRatingSystem::GetCachedLifetimePPR(const FString& UniqueId) const
{
	const int32 Rounds = Impl->RoundsPlayedCache.FindRef(UniqueId);
	if (Rounds <= 0) return 0.f;
	const double Total = Impl->TotalPointsCache.FindRef(UniqueId);
	return static_cast<float>(Total / double(Rounds));
}

void FElimPlusRatingSystem::Forget(const FString& UniqueId)
{
	Impl->RatingCache.Remove(UniqueId);
	Impl->RatingAtMatchStart.Remove(UniqueId);
	Impl->TotalPointsCache.Remove(UniqueId);
	Impl->RoundsPlayedCache.Remove(UniqueId);
}
