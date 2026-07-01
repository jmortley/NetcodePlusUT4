// ElimPlusRatingSystem.cpp — Mods.db persistence + ProcessMatch glue.
// Tron's TeamGlicko2 headers (and their <iostream>/<vector>/<cmath> bagage)
// stay confined to this translation unit thanks to the Pimpl in the header.

#include "ElimPlusRatingSystem.h"
#include "NCEloUploader.h"             // FNCEloUploader::ResolveServerName
#include "UnrealTournament.h"
#include "UTGameInstance.h"            // FDatabaseRow + ExecDatabaseCommand
#include "UTGameState.h"               // ServerName for BuildResultPayload
#include "ElimPlusStatsReplicator.h"
#include "Engine/World.h"
#include "Misc/DateTime.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

// Vendored TeamGlicko2 — included only here, never via the .h.
#include "TeamGlickoRating.h"
#include "TeamGlicko2System.h"
#include "TeamGlicko2Config.h"
#include "TeamBalancer.h"

DEFINE_LOG_CATEGORY_STATIC(LogElimPlusRating, Log, All);

// =============================================================================
// Per-round capture for the upload's rounds[] array. ut4stats persists these and
// re-runs a Python port of Tron's TeamGlicko2 over the real round history with
// tuned params (kCarryWeight / kPerfSlope / kDefaultRD / ...) — backtesting the
// rating without waiting to gather new data. Humans only: synthetic "BOT:" keys
// and empty ids are skipped, matching the rest of the upload path. Internal to
// this TU (never crosses the .h), so adding fields here is ABI-irrelevant.
// =============================================================================
struct FElimPlusRoundPlayerRecord
{
	FString UniqueId;
	int32   TeamIndex = 0;        // 0 = red, 1 = blue
	int32   Kills     = 0;
	int32   Deaths    = 0;
	double  Damage    = 0.0;
	bool    bWinner   = false;    // bWinner=false + bIsDraw=false => loss
};

struct FElimPlusRoundRecord
{
	int32 WinnerTeamIndex = -1;   // 0/1; -1 == draw
	bool  bIsDraw         = false;
	TArray<FElimPlusRoundPlayerRecord> Players;
};

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

	/** Lifetime damage accumulator (overkill-inclusive — the same PlayerRoundDamage feed
	 *  the PPR term uses): sum of every round's damage. DPR = TotalDamage / RoundsPlayed. */
	TMap<FString, double> TotalDamageCache;

	/** Last-seen display name per UniqueId — refreshed at PostLogin (LoadPlayerFromDB)
	 *  and every RecordRoundPPR, persisted at flush so Mods.db rows are human-readable. */
	TMap<FString, FString> PlayerNameCache;

	/** UniqueIds whose DB load FAILED outright (locked/unavailable db — NOT "no row").
	 *  They play the session on cached defaults, but FlushAtMatchEnd must NOT persist
	 *  them: an INSERT OR REPLACE seeded from those defaults would overwrite the real
	 *  stored rating. Cleared per-player on Forget. */
	TSet<FString> LoadFailedGuard;

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

	/** True between SnapshotMatchStart and FlushAtMatchEnd. Lets a mid-match
	 *  joiner pick up a match-start baseline at LoadPlayerFromDB time. */
	bool bMatchActive = false;

	// ---- Testing: random bot ELO assignment ----
	/** When true, GetOrAssignBotElo returns a random rating in [BotEloMin, BotEloMax]
	 *  on first lookup of a synthetic bot key, cached for the session. */
	bool bRandomBotElo = false;
	int32 BotEloMin = 1400;
	int32 BotEloMax = 1600;

	/** UniqueId (synthetic bot key like "BOT:Foo") -> assigned random ELO. Persists
	 *  across rounds within a session so the same bot keeps the same ELO. Never
	 *  written to Mods.db — bots are transient. */
	TMap<FString, int32> BotEloCache;

	/** Per-round perf log captured each ProcessRound (humans only), emitted as the
	 *  rounds[] array in BuildResultPayload. Cleared each SnapshotMatchStart so it
	 *  holds exactly this match's rounds. */
	TArray<FElimPlusRoundRecord> RoundLog;
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
		TEXT("  Sigma          REAL NOT NULL DEFAULT 0.03,")
		TEXT("  PerfIndexEMA   REAL NOT NULL DEFAULT 0.0,")
		TEXT("  PerfGames      INTEGER NOT NULL DEFAULT 0,")
		TEXT("  LastSeenUtc    INTEGER NOT NULL DEFAULT 0,")
		TEXT("  TotalPoints    REAL NOT NULL DEFAULT 0.0,")
		TEXT("  RoundsPlayed   INTEGER NOT NULL DEFAULT 0,")
		TEXT("  TotalDamage    REAL NOT NULL DEFAULT 0.0,")
		TEXT("  PlayerName     TEXT NOT NULL DEFAULT '',")
		TEXT("  SchemaVersion  INTEGER NOT NULL DEFAULT 3")
		TEXT(");");

	const bool bOk = ExecSqlNoRows(World, Sql);
	UE_LOG(LogElimPlusRating, Log, TEXT("InitDatabase: NCRatingElimPlus %s"),
		bOk ? TEXT("ready") : TEXT("FAILED (USE_SQLITE off?)"));

	// Schema migrations: v1 -> v2 added TotalPoints + RoundsPlayed, v2 -> v3 adds
	// TotalDamage + PlayerName. ALTER errors if the column already exists; we ignore
	// the bool return since the steady-state outcome is the same either way (column
	// present).
	ExecSqlNoRows(World, TEXT("ALTER TABLE NCRatingElimPlus ADD COLUMN TotalPoints REAL NOT NULL DEFAULT 0.0;"));
	ExecSqlNoRows(World, TEXT("ALTER TABLE NCRatingElimPlus ADD COLUMN RoundsPlayed INTEGER NOT NULL DEFAULT 0;"));
	ExecSqlNoRows(World, TEXT("ALTER TABLE NCRatingElimPlus ADD COLUMN TotalDamage REAL NOT NULL DEFAULT 0.0;"));
	ExecSqlNoRows(World, TEXT("ALTER TABLE NCRatingElimPlus ADD COLUMN PlayerName TEXT NOT NULL DEFAULT '';"));

	// Loud breadcrumb if the v3 migration didn't land (ALTERs drop silently on e.g.
	// SQLITE_BUSY — hub instances share one Mods.db and the engine sets no busy_timeout).
	// LoadPlayerFromDB degrades gracefully either way; this just explains the session.
	{
		TArray<FDatabaseRow> ProbeRows;
		if (!ExecSql(World, TEXT("SELECT TotalDamage, PlayerName FROM NCRatingElimPlus LIMIT 1;"), ProbeRows))
		{
			UE_LOG(LogElimPlusRating, Warning,
				TEXT("InitDatabase: v3 columns (TotalDamage/PlayerName) missing after ALTER (db busy?) — damage/name capture degraded this session"));
		}
	}

	return bOk;
}

void FElimPlusRatingSystem::LoadPlayerFromDB(UWorld* World, const FString& UniqueId, const FString& PlayerName)
{
	if (UniqueId.IsEmpty()) return;
	// Refresh the last-seen name even for an already-cached player (rejoin /
	// mid-session rename) — it's persisted at the next flush.
	if (!PlayerName.IsEmpty()) { Impl->PlayerNameCache.Add(UniqueId, PlayerName); }
	if (Impl->RatingCache.Contains(UniqueId)) return;

	const FString Esc = SqlEscape(UniqueId);

	// Core columns FIRST, v2 set only — this SELECT is valid on ANY schema version, so a
	// failed/blocked v3 migration can never make a veteran look like a new player (whose
	// defaults-seeded row would then INSERT OR REPLACE over the real rating at flush).
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

		// v3 extras in a SEPARATE query so a missing migration degrades to defaults for
		// damage/name instead of failing the whole load (InitDatabase warns when so).
		double  TotalDamage = 0.0;
		FString DbName;
		{
			TArray<FDatabaseRow> ExtraRows;
			const FString ExtraSql = FString::Printf(
				TEXT("SELECT TotalDamage, PlayerName FROM NCRatingElimPlus WHERE UniqueId='%s';"), *Esc);
			if (ExecSql(World, ExtraSql, ExtraRows) && ExtraRows.Num() > 0 && ExtraRows[0].Text.Num() >= 2)
			{
				TotalDamage = FCString::Atod(*ExtraRows[0].Text[0]);
				DbName      = ExtraRows[0].Text[1];
			}
		}

		TeamGlicko2::PlayerRating PR(Rating, RD, Sigma);
		PR.SetPerfIndexEMA(PerfIndexEMA);
		PR.SetPerfGames(PerfGames);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->TotalPointsCache.Add(UniqueId, TotalPoints);
		Impl->RoundsPlayedCache.Add(UniqueId, RoundsPlayed);
		Impl->TotalDamageCache.Add(UniqueId, TotalDamage);
		if (PlayerName.IsEmpty() && !DbName.IsEmpty()) { Impl->PlayerNameCache.Add(UniqueId, DbName); }
		UE_LOG(LogElimPlusRating, Log, TEXT("Loaded %s: Rating=%.1f RD=%.1f sigma=%.4f LifetimePPR=%.2f (%d rounds)"),
			*UniqueId, Rating, RD, Sigma,
			(RoundsPlayed > 0) ? float(TotalPoints / RoundsPlayed) : 0.f, RoundsPlayed);
	}
	else if (!bOk)
	{
		// Query FAILED (db locked/unavailable — not "no row"). Cache defaults so the match
		// can proceed, but guard the flush: we cannot know whether a real row exists, and
		// persisting a defaults-seeded row would overwrite it. (This hazard predates v3 —
		// the old code fell through to the new-player branch here.)
		TeamGlicko2::PlayerRating PR(
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->TotalPointsCache.Add(UniqueId, 0.0);
		Impl->RoundsPlayedCache.Add(UniqueId, 0);
		Impl->TotalDamageCache.Add(UniqueId, 0.0);
		Impl->LoadFailedGuard.Add(UniqueId);
		UE_LOG(LogElimPlusRating, Warning,
			TEXT("Load FAILED for %s (db busy/unavailable) — playing on defaults, flush persistence DISABLED for this player"),
			*UniqueId);
	}
	else
	{
		TeamGlicko2::PlayerRating PR(
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->TotalPointsCache.Add(UniqueId, 0.0);
		Impl->RoundsPlayedCache.Add(UniqueId, 0);
		Impl->TotalDamageCache.Add(UniqueId, 0.0);

		const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
		const FString InsertSql = FString::Printf(
			TEXT("INSERT OR IGNORE INTO NCRatingElimPlus (UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, TotalPoints, RoundsPlayed, TotalDamage, PlayerName) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, 0.0, 0, %lld, 0.0, 0, 0.0, '%s');"),
			*Esc,
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility,
			static_cast<long long>(NowUtc),
			*SqlEscape(PlayerName));
		if (!ExecSqlNoRows(World, InsertSql))
		{
			// Unmigrated (v2) table: retry with the v2 column set so the player still
			// gets a row (damage/name wait for the migration; InitDatabase warned).
			const FString V2InsertSql = FString::Printf(
				TEXT("INSERT OR IGNORE INTO NCRatingElimPlus (UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, TotalPoints, RoundsPlayed) ")
				TEXT("VALUES ('%s', %.6f, %.6f, %.6f, 0.0, 0, %lld, 0.0, 0);"),
				*Esc,
				TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility,
				static_cast<long long>(NowUtc));
			ExecSqlNoRows(World, V2InsertSql);
		}
		UE_LOG(LogElimPlusRating, Log, TEXT("New player %s — cached defaults + INSERT OR IGNORE"), *UniqueId);
	}
}

void FElimPlusRatingSystem::SnapshotMatchStart()
{
	Impl->RatingAtMatchStart.Empty();
	Impl->ActiveHumansThisMatch.Empty();
	Impl->HumansWithHumanOpposition.Empty();
	Impl->RoundLog.Empty();
	Impl->bMatchActive = true;

	for (const TPair<FString, TeamGlicko2::PlayerRating>& Pair : Impl->RatingCache)
	{
		const int32 RoundedElo = FMath::RoundToInt(Pair.Value.GetRating());
		Impl->RatingAtMatchStart.Add(Pair.Key, RoundedElo);
	}
	UE_LOG(LogElimPlusRating, Log, TEXT("Snapshot %d ratings at match start"), Impl->RatingAtMatchStart.Num());
}

void FElimPlusRatingSystem::CaptureLateJoinBaseline(const FString& UniqueId)
{
	// Mid-match joiner: no SnapshotMatchStart entry exists, so the end-of-match
	// delta would render blank even though their rating still moves for the
	// rounds they play. Baseline them at their current (just-loaded) rating so
	// the scoreboard shows a change-since-join delta.
	if (!Impl->bMatchActive) return;
	if (Impl->RatingAtMatchStart.Contains(UniqueId)) return; // present at start / reconnect
	const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId);
	if (!PR) return;
	Impl->RatingAtMatchStart.Add(UniqueId, FMath::RoundToInt(PR->GetRating()));
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
	// transient placeholder PlayerRating so team-size math is honest. If bot ELO
	// randomization is on, use the per-bot assigned ELO (so balancer + Glicko
	// math agree on team strengths); else default 1400.
	auto BuildTeam = [this](const TArray<FElimPlusPlayerRoundPerf>& Perfs, std::vector<MatchPlayer>& Out)
	{
		Out.reserve(Perfs.Num());
		for (const FElimPlusPlayerRoundPerf& Perf : Perfs)
		{
			const PlayerRating* Cached = Impl->RatingCache.Find(Perf.UniqueId);
			PlayerRating PR;
			if (Cached)
			{
				PR = *Cached;
			}
			else
			{
				const int32 BotElo = GetOrAssignBotElo(Perf.UniqueId);
				PR = PlayerRating(static_cast<double>(BotElo), kDefaultRD, kDefaultVolatility);
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

	TeamGlicko2System::ProcessMatch(Match, /*bLobbyImpactBlend=*/true);

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

	// Capture this round for the upload's rounds[] (ut4stats TeamGlicko2 backtest).
	// Humans only — skip synthetic "BOT:" keys + empty ids so the stored history
	// matches the human ladder. WinnerTeam/LoserTeam already carry per-player team
	// indices (set by the gamemode's BuildPerf), so the round's winner index is
	// just the winning side's team. Recorded post-write-back; logging only, never
	// affects the rating math above.
	{
		FElimPlusRoundRecord Rec;
		Rec.bIsDraw = Result.bIsDraw;
		auto AddSide = [&Rec](const TArray<FElimPlusPlayerRoundPerf>& Side, bool bWinnerSide)
		{
			for (const FElimPlusPlayerRoundPerf& Perf : Side)
			{
				if (Perf.UniqueId.IsEmpty() || Perf.UniqueId.StartsWith(TEXT("BOT:")))
				{
					continue;
				}
				FElimPlusRoundPlayerRecord PR;
				PR.UniqueId  = Perf.UniqueId;
				PR.TeamIndex = Perf.TeamIndex;
				PR.Kills     = Perf.Kills;
				PR.Deaths    = Perf.Deaths;
				PR.Damage    = static_cast<double>(Perf.Damage);
				PR.bWinner   = bWinnerSide;
				Rec.Players.Add(MoveTemp(PR));
			}
		};
		AddSide(Result.WinnerTeam, /*bWinnerSide=*/ !Result.bIsDraw);
		AddSide(Result.LoserTeam,  /*bWinnerSide=*/ false);
		if (!Result.bIsDraw && Result.WinnerTeam.Num() > 0)
		{
			Rec.WinnerTeamIndex = Result.WinnerTeam[0].TeamIndex;
		}
		if (Rec.Players.Num() > 0)
		{
			Impl->RoundLog.Add(MoveTemp(Rec));
		}
	}
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

		// Never persist a player whose DB load FAILED (locked/unavailable db at login):
		// their cache was seeded from defaults, and an INSERT OR REPLACE from those
		// values would overwrite whatever real row the db holds.
		if (Impl->LoadFailedGuard.Contains(UniqueId))
		{
			UE_LOG(LogElimPlusRating, Warning,
				TEXT("Flush skip %s: DB load failed at login — not persisting over the stored row"), *UniqueId);
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
		const double TotalDamage  = Impl->TotalDamageCache.FindRef(UniqueId);
		const FString PlayerName  = Impl->PlayerNameCache.FindRef(UniqueId);

		const FString Esc = SqlEscape(UniqueId);
		const FString Sql = FString::Printf(
			TEXT("INSERT OR REPLACE INTO NCRatingElimPlus ")
			TEXT("(UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, TotalPoints, RoundsPlayed, TotalDamage, PlayerName, SchemaVersion) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.6f, %d, %lld, %.6f, %d, %.6f, '%s', 3);"),
			*Esc,
			PR.GetRating(), PR.GetRD(), PR.GetSigma(),
			PR.GetPerfIndexEMA(), PR.GetPerfGames(),
			static_cast<long long>(NowUtc),
			TotalPoints, RoundsPlayed,
			TotalDamage, *SqlEscape(PlayerName));
		if (!ExecSqlNoRows(World, Sql))
		{
			// Unmigrated (v2) table: never lose the session's rating update — persist the
			// core row; damage/name wait for the migration (InitDatabase warned).
			const FString V2Sql = FString::Printf(
				TEXT("INSERT OR REPLACE INTO NCRatingElimPlus ")
				TEXT("(UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, TotalPoints, RoundsPlayed, SchemaVersion) ")
				TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.6f, %d, %lld, %.6f, %d, 2);"),
				*Esc,
				PR.GetRating(), PR.GetRD(), PR.GetSigma(),
				PR.GetPerfIndexEMA(), PR.GetPerfGames(),
				static_cast<long long>(NowUtc),
				TotalPoints, RoundsPlayed);
			ExecSqlNoRows(World, V2Sql);
		}

		if (Replicator)
		{
			Replicator->SetPlayerEloAndDelta(UniqueId, FinalElo, Delta);
			// Refresh the leaderboard rank now that the row above holds the new
			// rating, so the end-of-match scoreboard reflects updated standings.
			Replicator->SetPlayerGlobalRank(UniqueId, GetPlayerGlobalRank(World, UniqueId));
		}
		++PersistedCount;
	}

	UE_LOG(LogElimPlusRating, Log, TEXT("FlushAtMatchEnd: persisted %d, skipped %d (didn't play), capped %d (no human opposition)"),
		PersistedCount, SkippedCount, CappedCount);

	Impl->bMatchActive = false;
}

int32 FElimPlusRatingSystem::GetPlayerGlobalRank(UWorld* World, const FString& UniqueId) const
{
	if (!World || UniqueId.IsEmpty())
	{
		return 0;
	}

	// 1-based rank = 1 + count of rated players with a strictly higher Rating.
	// Returns exactly one row when this player has a rating row; zero rows -> 0
	// (unranked, e.g. a bot or a brand-new id). Ties share a rank. Live query —
	// call only at match start/end, never per-tick.
	const FString Esc = SqlEscape(UniqueId);
	const FString Sql = FString::Printf(
		TEXT("SELECT (SELECT COUNT(*) FROM NCRatingElimPlus WHERE Rating > me.Rating) + 1 ")
		TEXT("FROM NCRatingElimPlus me WHERE me.UniqueId='%s';"),
		*Esc);

	TArray<FDatabaseRow> Rows;
	if (!ExecSql(World, Sql, Rows) || Rows.Num() == 0 || Rows[0].Text.Num() < 1)
	{
		return 0;
	}
	return FCString::Atoi(*Rows[0].Text[0]);
}

int32 FElimPlusRatingSystem::GetCachedElo(const FString& UniqueId) const
{
	if (const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId))
	{
		return FMath::RoundToInt(PR->GetRating());
	}
	return FMath::RoundToInt(TeamGlicko2::kDefaultRating);
}

void FElimPlusRatingSystem::RecordRoundPPR(const FString& UniqueId, float RoundPPR, float RoundDamage, const FString& PlayerName)
{
	if (UniqueId.IsEmpty()) return;
	// Bots/unrated synthetic IDs (e.g. "BOT:Foo") aren't loaded by LoadPlayerFromDB
	// and never enter the rating cache. Gate on RatingCache membership so we don't
	// accidentally accumulate lifetime stats for transient placeholders.
	if (!Impl->RatingCache.Contains(UniqueId)) return;

	Impl->TotalPointsCache.FindOrAdd(UniqueId) += static_cast<double>(RoundPPR);
	Impl->RoundsPlayedCache.FindOrAdd(UniqueId) += 1;
	Impl->TotalDamageCache.FindOrAdd(UniqueId) += static_cast<double>(RoundDamage);
	if (!PlayerName.IsEmpty()) { Impl->PlayerNameCache.Add(UniqueId, PlayerName); }
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
	Impl->TotalDamageCache.Remove(UniqueId);
	Impl->PlayerNameCache.Remove(UniqueId);
	Impl->LoadFailedGuard.Remove(UniqueId);
}

void FElimPlusRatingSystem::SetBotRandomEloRange(bool bEnabled, int32 Min, int32 Max)
{
	Impl->bRandomBotElo = bEnabled;
	Impl->BotEloMin = FMath::Min(Min, Max);
	Impl->BotEloMax = FMath::Max(Min, Max);
	UE_LOG(LogElimPlusRating, Log, TEXT("Bot random ELO: %s [%d, %d]"),
		bEnabled ? TEXT("enabled") : TEXT("disabled"),
		Impl->BotEloMin, Impl->BotEloMax);
}

int32 FElimPlusRatingSystem::GetOrAssignBotElo(const FString& UniqueId)
{
	if (!Impl->bRandomBotElo)
	{
		return FMath::RoundToInt(TeamGlicko2::kDefaultRating);
	}
	if (const int32* Existing = Impl->BotEloCache.Find(UniqueId))
	{
		return *Existing;
	}
	const int32 NewElo = FMath::RandRange(Impl->BotEloMin, Impl->BotEloMax);
	Impl->BotEloCache.Add(UniqueId, NewElo);
	UE_LOG(LogElimPlusRating, Verbose, TEXT("Assigned bot ELO %d to %s"), NewElo, *UniqueId);
	return NewElo;
}

FString FElimPlusRatingSystem::BuildResultPayload(UWorld* World, const FNCElimPlusMatchInput& In) const
{
	using namespace TeamGlicko2;

	// Filter to humans (= cache entries that exist). Bots have either no cache
	// entry (their MatchResult contributions used transient placeholder ratings)
	// or a synthetic "BOT:..." key that LoadPlayerFromDB never seeds. Either way
	// they're absent from RatingCache and don't appear in RatingAtMatchStart.
	TArray<int32> HumanIndices;
	HumanIndices.Reserve(In.Players.Num());
	for (int32 i = 0; i < In.Players.Num(); ++i)
	{
		const FNCElimPlusPlayerInput& P = In.Players[i];
		if (P.UniqueId.IsEmpty()) continue;
		if (Impl->RatingCache.Contains(P.UniqueId))
		{
			HumanIndices.Add(i);
		}
	}
	if (HumanIndices.Num() == 0)
	{
		UE_LOG(LogElimPlusRating, Warning,
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
	Writer->WriteValue(TEXT("mode"),          FString(TEXT("ElimPlus")));
	Writer->WriteValue(TEXT("server"),        ServerName);
	Writer->WriteValue(TEXT("map"),           MapName);
	Writer->WriteValue(TEXT("played_at_utc"), PlayedAtUtc);
	Writer->WriteValue(TEXT("winner_team"),   In.WinnerTeamIndex);
	Writer->WriteValue(TEXT("red_score"),     In.RedScore);
	Writer->WriteValue(TEXT("blue_score"),    In.BlueScore);
	Writer->WriteArrayStart(TEXT("players"));

	for (int32 Idx : HumanIndices)
	{
		const FNCElimPlusPlayerInput& P = In.Players[Idx];
		const PlayerRating* PR = Impl->RatingCache.Find(P.UniqueId);
		if (!PR) continue; // shouldn't happen — we filtered above

		const int32 PreElo = Impl->RatingAtMatchStart.FindRef(P.UniqueId);
		const int32 PostElo = FMath::RoundToInt(PR->GetRating());
		const int32 Delta = (PreElo != 0) ? (PostElo - PreElo) : 0;

		const TCHAR* Result;
		if (In.WinnerTeamIndex < 0)
		{
			Result = TEXT("draw");
		}
		else
		{
			Result = (P.TeamIndex == In.WinnerTeamIndex) ? TEXT("win") : TEXT("loss");
		}

		const double TotalPoints  = Impl->TotalPointsCache.FindRef(P.UniqueId);
		const int32  RoundsPlayed = Impl->RoundsPlayedCache.FindRef(P.UniqueId);
		const bool   bFacedHumans = Impl->HumansWithHumanOpposition.Contains(P.UniqueId);

		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"),             P.UniqueId);
		Writer->WriteValue(TEXT("name"),           P.PlayerName);
		Writer->WriteValue(TEXT("team"),           P.TeamIndex);
		Writer->WriteValue(TEXT("result"),         FString(Result));
		Writer->WriteValue(TEXT("kills"),          P.Kills);
		Writer->WriteValue(TEXT("deaths"),         P.Deaths);
		Writer->WriteValue(TEXT("damage"),         P.Damage);
		Writer->WriteValue(TEXT("pre"),            static_cast<double>(PreElo));
		Writer->WriteValue(TEXT("post"),           PR->GetRating());
		Writer->WriteValue(TEXT("delta"),          Delta);
		Writer->WriteValue(TEXT("rd"),             PR->GetRD());
		Writer->WriteValue(TEXT("sigma"),          PR->GetSigma());
		Writer->WriteValue(TEXT("total_points"),   TotalPoints);
		Writer->WriteValue(TEXT("rounds_played"),  RoundsPlayed);
		Writer->WriteValue(TEXT("faced_humans"),   bFacedHumans);
		Writer->WriteObjectEnd();
	}

	Writer->WriteArrayEnd();

	// Per-round perf log (rounds[]) for the ut4stats TeamGlicko2 backtest. Lets the
	// site re-run the team-Glicko over real round history with tuned params
	// (kCarryWeight / kPerfSlope / kDefaultRD / ...) without waiting to gather new
	// data. Backward-compatible: hubs on the old build simply omit this key and
	// Django falls back to the match-aggregate path. Humans only (BOT keys already
	// filtered out at capture). winner_team is 0/1, or -1 when draw.
	Writer->WriteArrayStart(TEXT("rounds"));
	for (int32 r = 0; r < Impl->RoundLog.Num(); ++r)
	{
		const FElimPlusRoundRecord& Rec = Impl->RoundLog[r];
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("index"),       r);
		Writer->WriteValue(TEXT("winner_team"), Rec.WinnerTeamIndex);
		Writer->WriteValue(TEXT("draw"),        Rec.bIsDraw);
		Writer->WriteArrayStart(TEXT("players"));
		for (const FElimPlusRoundPlayerRecord& PR : Rec.Players)
		{
			const TCHAR* RResult = Rec.bIsDraw ? TEXT("draw") : (PR.bWinner ? TEXT("win") : TEXT("loss"));
			Writer->WriteObjectStart();
			Writer->WriteValue(TEXT("id"),     PR.UniqueId);
			Writer->WriteValue(TEXT("team"),   PR.TeamIndex);
			Writer->WriteValue(TEXT("kills"),  PR.Kills);
			Writer->WriteValue(TEXT("deaths"), PR.Deaths);
			Writer->WriteValue(TEXT("damage"), PR.Damage);
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

FElimPlusBalanceResult FElimPlusRatingSystem::ComputeBalancedTeams(const TArray<FElimPlusBalanceInput>& Players)
{
	using namespace TeamGlicko2;

	FElimPlusBalanceResult Result;
	if (Players.Num() < 2)
	{
		return Result;  // bValid=false
	}

	// Build PlayerInfo list. playerId is the input index — caller uses it to map
	// back to their parallel controller array.
	std::vector<PlayerInfo> PInfos;
	PInfos.reserve(Players.Num());
	for (int32 i = 0; i < Players.Num(); ++i)
	{
		const FString& Uid = Players[i].UniqueId;
		PlayerRating PR;
		if (PlayerRating* Cached = Impl->RatingCache.Find(Uid))
		{
			PR = *Cached;
		}
		else
		{
			// Bot or unrated. GetOrAssignBotElo returns the cached random value
			// when randomization is on, else 1400. Either way we use default
			// RD/sigma since these are transient placeholders.
			const int32 BotElo = GetOrAssignBotElo(Uid);
			PR = PlayerRating(static_cast<double>(BotElo), kDefaultRD, kDefaultVolatility);
		}
		PInfos.push_back(PlayerInfo(i, PR));
	}

	BalancerConfig Config;  // defaults: lambda=kLambda, separateTopPlayers=true,
	                        // putTopPlayerInSmallerTeam=true, maxCombinations=10000
	TeamAssignment TA = TeamBalancer::BalanceTeams(PInfos, Config);

	Result.Team0Indices.Reserve(TA.team0PlayerIds.size());
	for (int Id : TA.team0PlayerIds) Result.Team0Indices.Add(Id);
	Result.Team1Indices.Reserve(TA.team1PlayerIds.size());
	for (int Id : TA.team1PlayerIds) Result.Team1Indices.Add(Id);
	Result.Team0Strength = static_cast<float>(TA.team0Strength);
	Result.Team1Strength = static_cast<float>(TA.team1Strength);
	Result.StrengthDifference = static_cast<float>(TA.strengthDifference);
	Result.bValid = (Result.Team0Indices.Num() + Result.Team1Indices.Num()) == Players.Num();
	return Result;
}
