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

	// Skip degenerate rounds (truly empty teams — caller should already have
	// filtered, but be defensive).
	if (Result.WinnerTeam.Num() == 0 || Result.LoserTeam.Num() == 0)
	{
		return;
	}

	// Snapshot which UniqueIds had a real cached rating BEFORE this round.
	// Used after ProcessMatch to gate write-back: only humans (whose ratings
	// were loaded by PostLogin) get their cache entry updated. Bots and any
	// "BOT:<name>" synthetic IDs are absent from this set, so their post-match
	// ratings — present only as transient placeholders below — are discarded.
	TSet<FString> CachedIdsBefore;
	CachedIdsBefore.Reserve(RatingCache.Num());
	for (const TPair<FString, PlayerRating>& Pair : RatingCache)
	{
		CachedIdsBefore.Add(Pair.Key);
	}

	// Build MatchResult. For UniqueIds NOT in cache (bots, late joiners), use a
	// transient default PlayerRating(1400, 350, 0.06). This makes the team-size
	// math correct (e.g. 1 human + 3 bots vs 4 bots is processed as 4v4 with
	// the human's real rating contributing alongside three 1400 placeholders).
	auto BuildTeam = [this](const TArray<FElimPlusPlayerRoundPerf>& Perfs, std::vector<MatchPlayer>& Out)
	{
		Out.reserve(Perfs.Num());
		for (const FElimPlusPlayerRoundPerf& Perf : Perfs)
		{
			const PlayerRating* Cached = RatingCache.Find(Perf.UniqueId);
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

	// Tron's library mutates Match.teamA[i].rating + Match.teamB[i].rating in place.
	TeamGlicko2System::ProcessMatch(Match);

	// Write-back: only update cache for IDs that were already in it (= humans).
	// Bot entries' new ratings are dropped on the floor — they were placeholders
	// for team-size accuracy only.
	auto WriteBack = [this, &CachedIdsBefore](const std::vector<MatchPlayer>& Team, const TArray<FElimPlusPlayerRoundPerf>& Perfs)
	{
		const int32 N = FMath::Min(static_cast<int32>(Team.size()), Perfs.Num());
		for (int32 i = 0; i < N; ++i)
		{
			if (CachedIdsBefore.Contains(Perfs[i].UniqueId))
			{
				RatingCache[Perfs[i].UniqueId] = Team[i].rating;
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

	// Anti-farm cap for solo-vs-bots matches: if only one human was present at
	// match start, clamp their rating delta to ±SoloVsBotsDeltaCap. Lets the
	// solo tester verify the math runs end-to-end while preventing meaningful
	// rating drift from beating the AI. RD/sigma/perf-EMA still update fully
	// so the player's uncertainty bookkeeping stays honest.
	const bool bSoloVsBots = (RatingAtMatchStart.Num() == 1);
	const int32 SoloVsBotsDeltaCap = 5; // tunable

	for (TPair<FString, TeamGlicko2::PlayerRating>& Pair : RatingCache)
	{
		const FString& UniqueId = Pair.Key;
		TeamGlicko2::PlayerRating& PR = Pair.Value;  // mutable: may rewrite Rating below

		const int32 NewEloRaw = FMath::RoundToInt(PR.GetRating());
		const int32 StartElo  = RatingAtMatchStart.FindRef(UniqueId); // 0 if absent (late joiner)
		int32 Delta = (StartElo != 0) ? (NewEloRaw - StartElo) : 0;
		int32 FinalElo = NewEloRaw;

		if (bSoloVsBots && StartElo != 0 && (Delta > SoloVsBotsDeltaCap || Delta < -SoloVsBotsDeltaCap))
		{
			const int32 Clamped = FMath::Clamp(Delta, -SoloVsBotsDeltaCap, SoloVsBotsDeltaCap);
			FinalElo = StartElo + Clamped;
			PR.SetRating(static_cast<double>(FinalElo));  // also rewrite cache so next-match start is consistent with DB
			UE_LOG(LogElimPlusRating, Log, TEXT("Solo-vs-bots clamp: %s delta %d -> %d (Elo %d -> %d)"),
				*UniqueId, Delta, Clamped, NewEloRaw, FinalElo);
			Delta = Clamped;
		}

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

		// Replicator update — only NOW does the HUD ELO change, with the (possibly
		// clamped) match delta. Clients see "1402 +2" or "1395 -5" once at match end.
		if (Replicator)
		{
			Replicator->SetPlayerEloAndDelta(UniqueId, FinalElo, Delta);
		}
	}

	UE_LOG(LogElimPlusRating, Log, TEXT("FlushAtMatchEnd: persisted %d ratings (solo-vs-bots=%s)"),
		RatingCache.Num(), bSoloVsBots ? TEXT("YES") : TEXT("no"));
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
