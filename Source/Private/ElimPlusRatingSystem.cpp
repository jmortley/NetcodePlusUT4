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
// Recentre guard.
//
// The per-round blend is not zero-sum. The [0,1] clip on the blended score, and
// newcomers being placed at 10-20x a regular's weight, take points out of the
// pool every round. Measured on the stored round history of four hub databases
// (2026-11, 124k rounds): 0.2-0.3 points per player per round, month after month.
// Regulars lost ~100 points a month, and anyone who stopped playing was left
// stranded above the players still playing, so the ordering degraded as well
// (rank correlation of rating with points-per-round 0.64-0.77 on the older tables).
//
// The rule: at match end every rated human gets, for each round they played, one
// small step toward a pool average of kDefaultRating. The pool is the established
// players (kElimPlusRecentreEstablishedRounds or more) seen in the last
// kElimPlusRecentreActiveDays, read from Mods.db so it spans every instance of the
// hub. The step is the same for everyone in the match and scales with rounds
// played: it offsets each player's own leak, leaves players who are not playing
// where they are, and cannot be farmed by joining for the last round. Replayed on
// the same real rounds it held the active pool at ~1395 with rank correlation
// 0.94-0.96.
//
// The step alone must not be left to chase a pool that had already sunk hundreds
// of points when this build arrived: on the real rounds that handed the missing
// points to whoever plays most (the busiest players ended about 100 points above
// the rest of the established pool). Such a hub first gets a one-time BASELINE:
// one whole-number shift, measured once from its established pool and recorded
// in NCRatingElimPlusBaseline (ResolveElimPlusBaseline).
//
// The shift is added when a rating is read and taken off again when it is
// written (ElimPlusLiveFromStored / ElimPlusStoredFromLive). The stored Rating
// column is never rewritten, and that is what lets the hub do this by itself.
// A match instance that has players in it outlives a hub restart
// (AUTGameMode::DefaultTimer only exits an EMPTY orphan), so an older build can
// still be finishing a match, on ratings it loaded before the update, when the
// first instance of this build starts. Had the rows been rewritten, that match
// would save its players hundreds of points low for good, and a later match on
// the same old instance would be saved correctly, with nothing in the table to
// tell the two apart. A stored value means the same thing to every build, so
// there is nothing to go stale: an older build simply shows the stored value.
// NCRatingElimPlusLive is a view of the ratings as this build shows them, for
// admins who read Mods.db.
// =============================================================================
static TAutoConsoleVariable<int32> CVarElimPlusRecentre(
	TEXT("ncp.ElimPlusRecentre"), 1,
	TEXT("Server-side ElimPlus rating recentre guard. 1 (default): at match end each rated player gets a small per-round step that holds the hub's active pool at the 1400 seed, and a hub whose ladder had already sunk gets a one-time baseline shift (recorded in Mods.db and applied when ratings are read; stored ratings are not rewritten). 0: no step and no new baseline; a baseline already recorded still applies (see SERVER-ADMINS)."),
	ECVF_Default);

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

	/** InitDatabase's schema probe result. FlushAtMatchEnd selects its INSERT
	 *  shape from THIS — never from a runtime write failure: treating a transient
	 *  SQLITE_BUSY as "unmigrated table" once let the v2 fallback REPLACE rewrite
	 *  a veteran's row without TotalDamage/PlayerName on a real v3 table.
	 *  Tri-state because a FAILED probe proves nothing: the v2 shape may only be
	 *  used when a probe POSITIVELY established a v2 table (core columns readable,
	 *  v3 columns absent). Unknown fails closed to the v3 shape — on a real v3
	 *  table that is simply correct, and on an unreadable db the write fails and
	 *  is COUNTED rather than downgraded into a column-wiping v2 REPLACE. */
	enum class EDbSchema : uint8 { Unknown, V2, V3 };
	EDbSchema Schema = EDbSchema::Unknown;

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

	// ---- Recentre guard ----
	/** This map load read the hub's baseline from Mods.db: a row, or positively no
	 *  row. False when that read (or the write that sets a first baseline) failed:
	 *  the map then runs on the stored ratings as they are, with the guard off.
	 *  Settled before the first rating is loaded and not changed after it. */
	bool bBaselineKnown = false;

	/** The hub has a baseline row (bBaselineKnown, and the read found one). The
	 *  recentre step only runs on such a hub: a guard switched on in the middle of
	 *  a map, on a hub that has not been measured yet, waits for the next map load
	 *  instead of chasing a pool that may have sunk. */
	bool bHasBaseline = false;

	/** The hub's baseline in whole rating points: live rating = stored rating +
	 *  this, for every row that has played a round. 0 while the hub has none.
	 *  Fixed for the map load, so a match never loads under one shift and saves
	 *  under another. */
	int32 BaselineShift = 0;

	/** Recentre points added to each player by this match's flush, and the per-round
	 *  step they came from. Emitted by BuildResultPayload so ut4stats can reconcile
	 *  its round-by-round replay with the hub's match delta. Reset each
	 *  SnapshotMatchStart and at the top of each flush. */
	TMap<FString, double> RecentreThisMatch;
	double RecentreStepThisMatch = 0.0;
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

	// ---- Recentre guard tunables (rationale in the block comment at the top of this file).
	// ut4stats mirrors them in browse/elimplus_drift.py (RECENTRE_*) for its backtest: keep both in sync.
	const double kElimPlusRecentreGainPerRound      = 0.03;  // share of the gap to the seed closed per rated round
	const double kElimPlusRecentreCapPerRound       = 0.3;   // rating points per rated round, either direction
	const int32  kElimPlusRecentreMinPoolPlayers    = 10;    // no step until the pool is at least this many players
	const int32  kElimPlusRecentreEstablishedRounds = 20;    // rounds before a player counts toward the pool
	const int32  kElimPlusRecentreActiveDays        = 30;    // ...and how recently they must have played

	/** Rating points to add per rated round, from the hub's active-pool mean and size. */
	double ElimPlusRecentreStepPerRound(double PoolMean, int32 PoolPlayers)
	{
		if (PoolPlayers < kElimPlusRecentreMinPoolPlayers)
		{
			return 0.0;
		}
		const double Step = kElimPlusRecentreGainPerRound * (TeamGlicko2::kDefaultRating - PoolMean);
		return FMath::Clamp(Step, -kElimPlusRecentreCapPerRound, kElimPlusRecentreCapPerRound);
	}

	/** The rating the game works with, from the Rating column of a row. A row that
	 *  has never played a round is the untouched 1400 seed and takes no shift. */
	double ElimPlusLiveFromStored(double StoredRating, int32 RoundsPlayed, int32 BaselineShift)
	{
		return (RoundsPlayed > 0) ? StoredRating + static_cast<double>(BaselineShift) : StoredRating;
	}

	/** The inverse, for the row a flush writes. RoundsPlayed must be the value
	 *  written in that same row, so that the next load takes the same branch. */
	double ElimPlusStoredFromLive(double LiveRating, int32 RoundsPlayed, int32 BaselineShift)
	{
		return (RoundsPlayed > 0) ? LiveRating - static_cast<double>(BaselineShift) : LiveRating;
	}

	/** The hub's baseline row. Returns false when the read itself failed; a hub
	 *  without a baseline is a successful read with bOutHasRow false. COUNT(*)
	 *  leads because the engine wrapper drops NULL columns, and the MAX()s are
	 *  NULL when there is no row. */
	bool ReadElimPlusBaseline(UWorld* World, bool& bOutHasRow, int32& OutShift, int32& OutPoolPlayers, double& OutPoolMean)
	{
		bOutHasRow     = false;
		OutShift       = 0;
		OutPoolPlayers = 0;
		OutPoolMean    = 0.0;

		const FString ReadSql =
			TEXT("SELECT COUNT(*), MAX(Shift), MAX(PoolPlayers), MAX(PoolMean) FROM NCRatingElimPlusBaseline WHERE Id = 1;");
		TArray<FDatabaseRow> Rows;
		if (!ExecSql(World, ReadSql, Rows) || Rows.Num() == 0 || Rows[0].Text.Num() < 1)
		{
			return false;
		}
		if (FCString::Atoi(*Rows[0].Text[0]) <= 0)
		{
			return true;
		}
		if (Rows[0].Text.Num() < 4)
		{
			return false;   // a row with an empty column: not one this build wrote, so not one to trust
		}
		bOutHasRow     = true;
		OutShift       = FCString::Atoi(*Rows[0].Text[1]);
		OutPoolPlayers = FCString::Atoi(*Rows[0].Text[2]);
		OutPoolMean    = FCString::Atod(*Rows[0].Text[3]);
		return true;
	}

	/** Settle this map load's baseline (rationale in the block comment at the top
	 *  of this file). Sets the hub's baseline when it has none and the guard is
	 *  enabled; otherwise reads what is there. Each statement is a single atomic
	 *  one (the engine wrapper runs exactly one), so two instances doing this at
	 *  the same moment agree: INSERT OR IGNORE lets one of them set the row and
	 *  both read that row back. Must run before the first rating of the map load
	 *  is read. */
	void ResolveElimPlusBaseline(UWorld* World, FElimPlusRatingSystemImpl& Impl)
	{
		Impl.bBaselineKnown = false;
		Impl.bHasBaseline   = false;
		Impl.BaselineShift  = 0;

		ExecSqlNoRows(World,
			TEXT("CREATE TABLE IF NOT EXISTS NCRatingElimPlusBaseline (")
			TEXT("  Id           INTEGER PRIMARY KEY NOT NULL,")
			TEXT("  Shift        INTEGER NOT NULL,")
			TEXT("  PoolPlayers  INTEGER NOT NULL,")
			TEXT("  PoolMean     REAL NOT NULL,")
			TEXT("  CreatedUtc   INTEGER NOT NULL")
			TEXT(");"));

		const bool bGuardEnabled = CVarElimPlusRecentre.GetValueOnGameThread() != 0;
		bool   bHasRow     = false;
		int32  Shift       = 0;
		int32  PoolPlayers = 0;
		double PoolMean    = 0.0;
		bool bRead   = ReadElimPlusBaseline(World, bHasRow, Shift, PoolPlayers, PoolMean);
		bool bSetNow = false;
		if (bRead && !bHasRow && bGuardEnabled)
		{
			// Measured ONCE, the first time the guard meets this hub, on the pool the
			// guard steps on (established, seen recently); a hub too quiet for that
			// falls back to every established player. A row is written even when
			// there are too few to measure (new or reset ladder, tiny hub), with a
			// shift of 0, so the question is never asked again: a baseline measured
			// later, on the first ten players to become established, would move
			// everyone by the noise of ten ratings. The guard holds the level from
			// here, and an admin can delete the row to have it measured afresh.
			// The shift is a whole number so that every instance, and SQL, read back
			// exactly the same value (the wrapper hands REAL columns back as floats).
			const int64 NowUtc = FDateTime::UtcNow().ToUnixTimestamp();
			const FString SetSql = FString::Printf(
				TEXT("INSERT OR IGNORE INTO NCRatingElimPlusBaseline (Id, Shift, PoolPlayers, PoolMean, CreatedUtc) ")
				TEXT("SELECT 1, CASE WHEN PoolPlayers >= %d THEN CAST(ROUND(%.1f - PoolMean) AS INTEGER) ELSE 0 END, ")
				TEXT("PoolPlayers, COALESCE(PoolMean, 0.0), %lld FROM (")
				TEXT("SELECT CASE WHEN Recent.N >= %d THEN Recent.N ELSE Everyone.N END AS PoolPlayers, ")
				TEXT("CASE WHEN Recent.N >= %d THEN Recent.M ELSE Everyone.M END AS PoolMean FROM ")
				TEXT("(SELECT COUNT(*) AS N, AVG(Rating) AS M FROM NCRatingElimPlus WHERE RoundsPlayed >= %d AND LastSeenUtc >= %lld) AS Recent, ")
				TEXT("(SELECT COUNT(*) AS N, AVG(Rating) AS M FROM NCRatingElimPlus WHERE RoundsPlayed >= %d) AS Everyone);"),
				kElimPlusRecentreMinPoolPlayers,
				TeamGlicko2::kDefaultRating,
				static_cast<long long>(NowUtc),
				kElimPlusRecentreMinPoolPlayers,
				kElimPlusRecentreMinPoolPlayers,
				kElimPlusRecentreEstablishedRounds,
				static_cast<long long>(NowUtc - static_cast<int64>(kElimPlusRecentreActiveDays) * 86400),
				kElimPlusRecentreEstablishedRounds);
			// A failed write proves nothing about the row, so the read-back only
			// counts after a write that ran.
			bRead   = ExecSqlNoRows(World, SetSql) && ReadElimPlusBaseline(World, bHasRow, Shift, PoolPlayers, PoolMean);
			bSetNow = bRead && bHasRow;
		}

		if (!bRead)
		{
			UE_LOG(LogElimPlusRating, Warning,
				TEXT("Rating baseline unreadable (db busy?): this map runs on the stored ratings as they are, recentre guard off"));
			return;
		}

		Impl.bBaselineKnown = true;
		Impl.bHasBaseline   = bHasRow;
		Impl.BaselineShift  = bHasRow ? Shift : 0;
		const TCHAR* GuardState = bGuardEnabled ? TEXT("recentre guard ON") : TEXT("recentre guard OFF (ncp.ElimPlusRecentre 0)");
		if (!bHasRow)
		{
			// Only reachable with the guard switched off: an enabled guard always leaves a row.
			UE_LOG(LogElimPlusRating, Log, TEXT("No rating baseline for this hub; %s"), GuardState);
		}
		else if (PoolPlayers < kElimPlusRecentreMinPoolPlayers)
		{
			UE_LOG(LogElimPlusRating, Log,
				TEXT("Rating baseline %+d (%s when the hub had %d established players, too few to measure one); %s"),
				Shift, bSetNow ? TEXT("set just now,") : TEXT("set"), PoolPlayers, GuardState);
		}
		else if (bSetNow)
		{
			UE_LOG(LogElimPlusRating, Display,
				TEXT("Rating baseline set for this hub: %+d, measured on %d established players averaging %.1f. The game now shows each stored rating %+d; the stored Rating column is not rewritten (NCRatingElimPlusLive shows the live values); %s"),
				Shift, PoolPlayers, PoolMean, Shift, GuardState);
		}
		else
		{
			UE_LOG(LogElimPlusRating, Log,
				TEXT("Rating baseline %+d (measured on %d established players averaging %.1f); %s"),
				Shift, PoolPlayers, PoolMean, GuardState);
		}
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
	// The engine opens the shared Mods.db handle with NO busy_timeout, so any lock
	// contention (concurrent instance flushing at its own match end) surfaces as an
	// instant SQLITE_BUSY failure — which is exactly the "load FAILED / zero rows"
	// class that seeds veterans at 1400/RD200. 5s of patience collapses most of
	// those transients into ordinary successful reads. Connection-wide: every
	// rating system sharing the process handle benefits. NB the PRAGMA returns a
	// result row, which the no-rows exec helper reports as failure — use the rows
	// variant and ignore the output; the pragma takes effect regardless.
	{
		TArray<FDatabaseRow> PragmaRows;
		ExecSql(World, TEXT("PRAGMA busy_timeout=5000;"), PragmaRows);
	}

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
		// The probe result is CACHED and drives the flush's INSERT shape for the whole
		// session (see FlushAtMatchEnd). ONE statement decides it: sequential probes
		// race — a v3 SELECT can time out on a lock that clears before a follow-up
		// core SELECT, misclassifying a healthy v3 table as v2, whose shorter REPLACE
		// shape then wipes TotalDamage/PlayerName at every flush. table_info succeeds
		// or fails atomically: on success the column list is a consistent snapshot,
		// so the ABSENCE of TotalDamage positively establishes v2; on failure we know
		// nothing -> Unknown, and the flush fails closed to the v3 shape.
		TArray<FDatabaseRow> SchemaRows;
		if (ExecSql(World, TEXT("PRAGMA table_info(NCRatingElimPlus);"), SchemaRows) && SchemaRows.Num() > 0)
		{
			// The v3 INSERT writes BOTH columns and the two ALTERs above are
			// independent, so a partial migration (exactly one landed) is reachable:
			// classify V3 only when both exist, V2 only when neither does, and fail
			// a partial closed as Unknown — the ALTERs retry next session.
			bool bHasTotalDamage = false;
			bool bHasPlayerName  = false;
			for (const FDatabaseRow& Row : SchemaRows)
			{
				bHasTotalDamage = bHasTotalDamage || Row.Text.Contains(TEXT("TotalDamage"));
				bHasPlayerName  = bHasPlayerName  || Row.Text.Contains(TEXT("PlayerName"));
			}
			if (bHasTotalDamage && bHasPlayerName)
			{
				Impl->Schema = FElimPlusRatingSystemImpl::EDbSchema::V3;
			}
			else if (!bHasTotalDamage && !bHasPlayerName)
			{
				Impl->Schema = FElimPlusRatingSystemImpl::EDbSchema::V2;
				UE_LOG(LogElimPlusRating, Warning,
					TEXT("InitDatabase: v3 columns (TotalDamage/PlayerName) missing after ALTER — v2-shape writes this session, damage/name capture deferred"));
			}
			else
			{
				Impl->Schema = FElimPlusRatingSystemImpl::EDbSchema::Unknown;
				UE_LOG(LogElimPlusRating, Warning,
					TEXT("InitDatabase: PARTIAL v3 migration (one of TotalDamage/PlayerName missing) — failing closed to v3-shape writes; ALTERs retry next session"));
			}
		}
		else
		{
			Impl->Schema = FElimPlusRatingSystemImpl::EDbSchema::Unknown;
			UE_LOG(LogElimPlusRating, Warning,
				TEXT("InitDatabase: schema probe unreadable (db busy or table missing?) — failing closed to v3-shape writes; flushes may fail-and-count until the db recovers"));
		}
	}

	// Recentre guard: settle the hub's baseline for this map load, before any
	// rating is read (rationale in the block comment at the top of this file).
	ResolveElimPlusBaseline(World, *Impl);

	// For admins who read Mods.db: the ratings as the game shows them. The Rating
	// column of NCRatingElimPlus itself stays as stored (StoredRating here). The
	// view names v3 columns, so it waits for a table the probe confirmed as v3. To
	// change its definition in a later build, DROP VIEW first: IF NOT EXISTS keeps
	// whatever is there.
	if (Impl->Schema == FElimPlusRatingSystemImpl::EDbSchema::V3)
	{
		ExecSqlNoRows(World,
			TEXT("CREATE VIEW IF NOT EXISTS NCRatingElimPlusLive AS ")
			TEXT("SELECT UniqueId, PlayerName, ")
			TEXT("Rating + CASE WHEN RoundsPlayed > 0 THEN COALESCE((SELECT Shift FROM NCRatingElimPlusBaseline WHERE Id = 1), 0) ELSE 0 END AS Rating, ")
			TEXT("RD, Sigma, RoundsPlayed, TotalPoints, TotalDamage, LastSeenUtc, PerfIndexEMA, PerfGames, Rating AS StoredRating ")
			TEXT("FROM NCRatingElimPlus;"));
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

	// InitDatabase could not settle the baseline (db busy at map load). Nobody has
	// been loaded yet, so it can still be settled now; once a rating is in the
	// cache the map keeps the shift it was read with.
	if (!Impl->bBaselineKnown && Impl->RatingCache.Num() == 0)
	{
		ResolveElimPlusBaseline(World, *Impl);
	}

	const FString Esc = SqlEscape(UniqueId);

	// One load path, used twice (see below). Core columns FIRST, v2 set only — this
	// SELECT is valid on ANY schema version, so a failed/blocked v3 migration can never
	// make a veteran look like a new player (whose defaults-seeded row would then
	// INSERT OR REPLACE over the real rating at flush). Returns: 0=loaded (caches
	// populated), 1=no row, 2=query failed.
	auto TryLoadRow = [&]() -> int32
	{
		const FString Sql = FString::Printf(
			TEXT("SELECT Rating, RD, Sigma, PerfIndexEMA, PerfGames, TotalPoints, RoundsPlayed FROM NCRatingElimPlus WHERE UniqueId='%s';"),
			*Esc);

		TArray<FDatabaseRow> Rows;
		if (!ExecSql(World, Sql, Rows))
		{
			return 2;
		}
		if (Rows.Num() == 0 || Rows[0].Text.Num() < 7)
		{
			return 1;
		}

		const double StoredRating = FCString::Atod(*Rows[0].Text[0]);
		const double RD           = FCString::Atod(*Rows[0].Text[1]);
		const double Sigma        = FCString::Atod(*Rows[0].Text[2]);
		const double PerfIndexEMA = FCString::Atod(*Rows[0].Text[3]);
		const int32  PerfGames    = FCString::Atoi(*Rows[0].Text[4]);
		const double TotalPoints  = FCString::Atod(*Rows[0].Text[5]);
		const int32  RoundsPlayed = FCString::Atoi(*Rows[0].Text[6]);
		// Everything from here on (cache, scoreboard, balancer, upload) is the live
		// rating; the flush takes the hub's baseline off again when it writes the row.
		const double Rating       = ElimPlusLiveFromStored(StoredRating, RoundsPlayed, Impl->BaselineShift);

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
		UE_LOG(LogElimPlusRating, Log, TEXT("Loaded %s: Rating=%.1f (stored %.1f) RD=%.1f sigma=%.4f LifetimePPR=%.2f (%d rounds)"),
			*UniqueId, Rating, StoredRating, RD, Sigma,
			(RoundsPlayed > 0) ? float(TotalPoints / RoundsPlayed) : 0.f, RoundsPlayed);
		return 0;
	};

	auto SeedDefaultsGuarded = [&](const TCHAR* Why)
	{
		// Cache defaults so the match can proceed, but guard the flush: we cannot
		// confirm what the DB really holds for this player, and persisting a
		// defaults-seeded row via INSERT OR REPLACE would overwrite a real rating.
		TeamGlicko2::PlayerRating PR(
			TeamGlicko2::kDefaultRating, TeamGlicko2::kDefaultRD, TeamGlicko2::kDefaultVolatility);
		Impl->RatingCache.Add(UniqueId, PR);
		Impl->TotalPointsCache.Add(UniqueId, 0.0);
		Impl->RoundsPlayedCache.Add(UniqueId, 0);
		Impl->TotalDamageCache.Add(UniqueId, 0.0);
		Impl->LoadFailedGuard.Add(UniqueId);
		UE_LOG(LogElimPlusRating, Warning,
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

	// Zero rows. This is EITHER a genuinely new player OR a veteran whose row was
	// invisible to this read (fresh per-instance Mods.db is fine; a stale/partial
	// read against a shared file is the destructive case — the old code seeded
	// defaults, played the match at RD=200 with ~9x swings, then INSERT OR
	// REPLACE'd the real row away at flush; the J@r0d -193 incident). Resolve it
	// with a positive-existence check: INSERT OR IGNORE the default row, then load
	// WHATEVER the row now holds. A truly new player reads back the defaults we
	// just wrote; a conflicting pre-existing row survives the IGNORE and reads
	// back its REAL values — the veteran is rescued instead of destroyed. If the
	// row still can't be read back, treat it as a failed load and guard the flush.
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

	const int32 SecondTry = TryLoadRow();
	if (SecondTry == 0)
	{
		// Distinguish for the log only — both paths loaded whatever the DB truly
		// holds, so the flush is safe either way.
		const TeamGlicko2::PlayerRating* PR = Impl->RatingCache.Find(UniqueId);
		const bool bLooksNew = PR
			&& FMath::IsNearlyEqual(PR->GetRating(), TeamGlicko2::kDefaultRating, 0.01)
			&& Impl->RoundsPlayedCache.FindRef(UniqueId) == 0;
		if (bLooksNew)
		{
			UE_LOG(LogElimPlusRating, Log, TEXT("New player %s — row created at defaults"), *UniqueId);
		}
		else
		{
			UE_LOG(LogElimPlusRating, Warning,
				TEXT("RESCUED %s: row was invisible on first read but exists (Rating=%.1f) — loaded real values instead of default-seeding"),
				*UniqueId, PR ? PR->GetRating() : 0.0);
		}
		return;
	}
	SeedDefaultsGuarded(TEXT("row unconfirmable after INSERT OR IGNORE"));
}

void FElimPlusRatingSystem::SnapshotMatchStart()
{
	Impl->RatingAtMatchStart.Empty();
	Impl->ActiveHumansThisMatch.Empty();
	Impl->HumansWithHumanOpposition.Empty();
	Impl->RoundLog.Empty();
	Impl->RecentreThisMatch.Empty();
	Impl->RecentreStepThisMatch = 0.0;
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
	int32 FailedCount    = 0;
	int32 CappedCount    = 0;

	// ---- Recentre guard (rationale in the block comment at the top of this file) ----
	// The step needs the switch, a hub whose baseline this map load found in place,
	// and a match that went through SnapshotMatchStart: bMatchActive makes a flush
	// with no such match (or a second flush of the same one) step-free, because the
	// step must never be applied twice.
	Impl->RecentreThisMatch.Empty();
	Impl->RecentreStepThisMatch = 0.0;
	const bool bRecentre = Impl->bHasBaseline && Impl->bMatchActive && CVarElimPlusRecentre.GetValueOnGameThread() != 0;
	TMap<FString, int32> RoundsThisMatch;
	if (bRecentre)
	{
		// The hub's active pool as Mods.db holds it right now: every established
		// player's row as last flushed, across all instances. This match's players
		// are still at their pre-match values, which is fine for a pool average.
		// COUNT(*) leads because the engine wrapper drops NULL columns and AVG() is
		// NULL on an empty pool. The rows are stored ratings and every one of them
		// has played, so the live pool mean is the stored one plus the baseline.
		const FString PoolSql = FString::Printf(
			TEXT("SELECT COUNT(*), AVG(Rating) FROM NCRatingElimPlus WHERE RoundsPlayed >= %d AND LastSeenUtc >= %lld;"),
			kElimPlusRecentreEstablishedRounds,
			static_cast<long long>(NowUtc - static_cast<int64>(kElimPlusRecentreActiveDays) * 86400));
		TArray<FDatabaseRow> PoolRows;
		if (ExecSql(World, PoolSql, PoolRows) && PoolRows.Num() > 0 && PoolRows[0].Text.Num() >= 2)
		{
			const int32  PoolPlayers = FCString::Atoi(*PoolRows[0].Text[0]);
			const double PoolMean    = FCString::Atod(*PoolRows[0].Text[1]) + static_cast<double>(Impl->BaselineShift);
			Impl->RecentreStepThisMatch = ElimPlusRecentreStepPerRound(PoolMean, PoolPlayers);
			UE_LOG(LogElimPlusRating, Log,
				TEXT("Recentre: pool mean %.1f over %d established players seen in the last %d days -> %+.3f per round"),
				PoolMean, PoolPlayers, kElimPlusRecentreActiveDays, Impl->RecentreStepThisMatch);
		}
		else
		{
			UE_LOG(LogElimPlusRating, Log, TEXT("Recentre: no active pool to read (empty, or db busy) — no step this match"));
		}

		if (Impl->RecentreStepThisMatch != 0.0)
		{
			// Rounds each human was rated in this match: the round log holds exactly those.
			for (const FElimPlusRoundRecord& Rec : Impl->RoundLog)
			{
				for (const FElimPlusRoundPlayerRecord& RoundPlayer : Rec.Players)
				{
					RoundsThisMatch.FindOrAdd(RoundPlayer.UniqueId) += 1;
				}
			}
		}
	}

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

		// Recentre step, before anything below reads the rating, so the scoreboard
		// delta, the persisted row and the upload all carry it. Rated humans only: a
		// player who never faced a human this match is on the bot-match path below
		// (delta capped at +/-5) and gets none, or the step could be farmed on bots.
		if (bRecentre && Impl->RecentreStepThisMatch != 0.0 && Impl->HumansWithHumanOpposition.Contains(UniqueId))
		{
			const double RecentreAdd = Impl->RecentreStepThisMatch * static_cast<double>(RoundsThisMatch.FindRef(UniqueId));
			if (RecentreAdd != 0.0)
			{
				PR.SetRating(PR.GetRating() + RecentreAdd);
				Impl->RecentreThisMatch.Add(UniqueId, RecentreAdd);
			}
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

		// The row holds the STORED rating: the live one with the hub's baseline taken
		// off again, by the same rule LoadPlayerFromDB put it on (keyed on the
		// RoundsPlayed written in this very row). Everything above this line, and the
		// replicator and upload below, work on the live rating.
		const double StoredRating = ElimPlusStoredFromLive(PR.GetRating(), RoundsPlayed, Impl->BaselineShift);

		const FString Esc = SqlEscape(UniqueId);
		const FString Sql = FString::Printf(
			TEXT("INSERT OR REPLACE INTO NCRatingElimPlus ")
			TEXT("(UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, TotalPoints, RoundsPlayed, TotalDamage, PlayerName, SchemaVersion) ")
			TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.6f, %d, %lld, %.6f, %d, %.6f, '%s', 3);"),
			*Esc,
			StoredRating, PR.GetRD(), PR.GetSigma(),
			PR.GetPerfIndexEMA(), PR.GetPerfGames(),
			static_cast<long long>(NowUtc),
			TotalPoints, RoundsPlayed,
			TotalDamage, *SqlEscape(PlayerName));
		// INSERT shape comes from the InitDatabase schema probe, NEVER from a write
		// failure: a failed v3 write here is contention/IO, not "unmigrated table",
		// and falling back to the shorter v2 REPLACE on error could rewrite the row
		// without TotalDamage/PlayerName the moment the lock cleared. Unknown fails
		// CLOSED to the v3 shape: correct on a v3 table, and on an unreadable db the
		// write fails and is counted — never downgraded.
		bool bPersisted = false;
		if (Impl->Schema != FElimPlusRatingSystemImpl::EDbSchema::V2)
		{
			bPersisted = ExecSqlNoRows(World, Sql);
		}
		else
		{
			// POSITIVELY-confirmed v2 table: persist the core row; damage/name
			// wait for the migration (InitDatabase warned).
			const FString V2Sql = FString::Printf(
				TEXT("INSERT OR REPLACE INTO NCRatingElimPlus ")
				TEXT("(UniqueId, Rating, RD, Sigma, PerfIndexEMA, PerfGames, LastSeenUtc, TotalPoints, RoundsPlayed, SchemaVersion) ")
				TEXT("VALUES ('%s', %.6f, %.6f, %.6f, %.6f, %d, %lld, %.6f, %d, 2);"),
				*Esc,
				StoredRating, PR.GetRD(), PR.GetSigma(),
				PR.GetPerfIndexEMA(), PR.GetPerfGames(),
				static_cast<long long>(NowUtc),
				TotalPoints, RoundsPlayed);
			bPersisted = ExecSqlNoRows(World, V2Sql);
		}
		if (!bPersisted)
		{
			++FailedCount;
			UE_LOG(LogElimPlusRating, Warning,
				TEXT("Flush FAILED for %s: rating write did not persist (db busy/locked?) — stored row is now one match stale"),
				*UniqueId);
		}

		if (Replicator)
		{
			Replicator->SetPlayerEloAndDelta(UniqueId, FinalElo, Delta);
			// Refresh the leaderboard rank now that the row above holds the new
			// rating, so the end-of-match scoreboard reflects updated standings.
			Replicator->SetPlayerGlobalRank(UniqueId, GetPlayerGlobalRank(World, UniqueId));
		}
		if (bPersisted)
		{
			++PersistedCount;
		}
	}

	UE_LOG(LogElimPlusRating, Log, TEXT("FlushAtMatchEnd: persisted %d, failed %d, skipped %d (didn't play), capped %d (no human opposition), recentred %d (%+.3f per round)"),
		PersistedCount, FailedCount, SkippedCount, CappedCount, Impl->RecentreThisMatch.Num(), Impl->RecentreStepThisMatch);

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
	// call only at match start/end, never per-tick. Compared as LIVE ratings: the
	// hub's baseline goes onto every row that has played a round, exactly as
	// ElimPlusLiveFromStored does, so a never-played 1400 seed row is not ranked
	// against stored values. With no baseline this is the plain comparison.
	const FString Esc = SqlEscape(UniqueId);
	const FString Sql = FString::Printf(
		TEXT("SELECT (SELECT COUNT(*) FROM NCRatingElimPlus other ")
		TEXT("WHERE other.Rating + (CASE WHEN other.RoundsPlayed > 0 THEN %d ELSE 0 END) ")
		TEXT("> me.Rating + (CASE WHEN me.RoundsPlayed > 0 THEN %d ELSE 0 END)) + 1 ")
		TEXT("FROM NCRatingElimPlus me WHERE me.UniqueId='%s';"),
		Impl->BaselineShift, Impl->BaselineShift,
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
	TSet<FString> RosterIds;
	for (int32 i = 0; i < In.Players.Num(); ++i)
	{
		const FNCElimPlusPlayerInput& P = In.Players[i];
		if (P.UniqueId.IsEmpty()) continue;
		if (Impl->RatingCache.Contains(P.UniqueId))
		{
			HumanIndices.Add(i);
			RosterIds.Add(P.UniqueId);
		}
	}

	// LEAVER FIX (2026-07-17): In.Players is built from PlayerArray at match end, so
	// anyone who left mid-match is missing — their rating moved locally (the flush
	// persists it) but their delta never reached ut4stats: no audit row, no games
	// increment, hub/site ladders diverge, and the site's round-by-round replay
	// default-seeds them at 1400/RD200. Prod enumeration (J@r0d): 5 of 5 missing
	// matches were exactly this; one match's push carried only 6 of 10 players.
	// Union in every cached human who played a round but is gone from the roster.
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
	// Recentre guard (additive keys): the per-round step used by this match's flush,
	// and per player below the points it added. They are already inside post/delta;
	// ut4stats needs them apart to reconcile its round-by-round replay, which does
	// not include the step. 0 when the guard was off; absent on older builds.
	Writer->WriteValue(TEXT("recentre_per_round"), Impl->RecentreStepThisMatch);
	// The hub's baseline this map (additive key; whole points, 0 when it has none,
	// absent on older builds). pre/post/delta are live ratings and already include
	// it: the hub's stored rating is post minus this. A player's first match after
	// the baseline is set shows pre jumping by it from their previous post.
	Writer->WriteValue(TEXT("baseline_shift"), Impl->BaselineShift);
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
		Writer->WriteValue(TEXT("recentre"),       Impl->RecentreThisMatch.FindRef(P.UniqueId));
		if (Impl->LoadFailedGuard.Contains(P.UniqueId))
		{
			// This player's session ran on unconfirmed default-seeded state (DB load
			// guard) — the numbers are not trustworthy. Additive key: old Django
			// ignores it; new Django records the row without applying the delta.
			Writer->WriteValue(TEXT("provisional"), true);
		}
		Writer->WriteObjectEnd();
	}

	// Leavers: roster entry is gone, so team + this-match box score come from the
	// per-round log instead (their per-round lines were captured live while they
	// played). Rating fields come from the same caches as everyone else.
	int32 LeaversEmitted = 0;
	for (const FString& Id : LeaverIds)
	{
		const PlayerRating* PR = Impl->RatingCache.Find(Id);
		if (!PR) continue;

		int32  TeamIndex = -1;
		int32  Kills = 0, Deaths = 0;
		double Damage = 0.0;
		for (const FElimPlusRoundRecord& Rec : Impl->RoundLog)
		{
			for (const FElimPlusRoundPlayerRecord& RP : Rec.Players)
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
			// Played (ActiveHumans) but never landed in the round log — can't
			// attribute a team/result; skip rather than fabricate.
			UE_LOG(LogElimPlusRating, Warning,
				TEXT("BuildResultPayload: leaver %s has no round-log lines — omitted from upload"), *Id);
			continue;
		}

		const int32 PreElo  = Impl->RatingAtMatchStart.FindRef(Id);
		const int32 PostElo = FMath::RoundToInt(PR->GetRating());
		const int32 Delta   = (PreElo != 0) ? (PostElo - PreElo) : 0;

		const TCHAR* Result;
		if (In.WinnerTeamIndex < 0)      { Result = TEXT("draw"); }
		else                             { Result = (TeamIndex == In.WinnerTeamIndex) ? TEXT("win") : TEXT("loss"); }

		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("id"),             Id);
		Writer->WriteValue(TEXT("name"),           Impl->PlayerNameCache.FindRef(Id));
		Writer->WriteValue(TEXT("team"),           TeamIndex);
		Writer->WriteValue(TEXT("result"),         FString(Result));
		Writer->WriteValue(TEXT("kills"),          Kills);
		Writer->WriteValue(TEXT("deaths"),         Deaths);
		Writer->WriteValue(TEXT("damage"),         Damage);
		Writer->WriteValue(TEXT("pre"),            static_cast<double>(PreElo));
		Writer->WriteValue(TEXT("post"),           PR->GetRating());
		Writer->WriteValue(TEXT("delta"),          Delta);
		Writer->WriteValue(TEXT("rd"),             PR->GetRD());
		Writer->WriteValue(TEXT("sigma"),          PR->GetSigma());
		Writer->WriteValue(TEXT("total_points"),   Impl->TotalPointsCache.FindRef(Id));
		Writer->WriteValue(TEXT("rounds_played"),  Impl->RoundsPlayedCache.FindRef(Id));
		Writer->WriteValue(TEXT("faced_humans"),   Impl->HumansWithHumanOpposition.Contains(Id));
		Writer->WriteValue(TEXT("recentre"),       Impl->RecentreThisMatch.FindRef(Id));
		Writer->WriteValue(TEXT("left"),           true);
		if (Impl->LoadFailedGuard.Contains(Id))
		{
			Writer->WriteValue(TEXT("provisional"), true);
		}
		Writer->WriteObjectEnd();
		++LeaversEmitted;
	}
	if (LeaversEmitted > 0)
	{
		UE_LOG(LogElimPlusRating, Log,
			TEXT("BuildResultPayload: included %d mid-match leaver(s) alongside %d rostered player(s)"),
			LeaversEmitted, HumanIndices.Num());
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

	// Server-authored clutch telemetry (additive keys, 2026-07-20). ut4stats
	// consumes these as source="server_v1", cross-checked against the rounds[]
	// manifest + KillLog before acceptance. clutch_version is emitted even when
	// the array is empty so a zero-attempt match is distinguishable from an
	// old-build hub that never sent the keys. Bots/invalid IDs are filtered at
	// capture (attempt open requires a valid UniqueId) — the emptiness check
	// here is belt-and-suspenders only.
	Writer->WriteValue(TEXT("clutch_version"), 1);
	Writer->WriteArrayStart(TEXT("clutches"));
	for (const FNCElimPlusClutchInput& C : In.Clutches)
	{
		if (C.UniqueId.IsEmpty()) continue;
		Writer->WriteObjectStart();
		Writer->WriteValue(TEXT("round_index"),       C.RoundIndex);
		Writer->WriteValue(TEXT("id"),                C.UniqueId);
		Writer->WriteValue(TEXT("team"),              C.TeamIndex);
		Writer->WriteValue(TEXT("enemies_at_start"),  C.EnemiesAtStart);
		Writer->WriteValue(TEXT("result"),            C.Result);
		Writer->WriteValue(TEXT("direct_kills"),      C.DirectKills);
		Writer->WriteValue(TEXT("clean_finish"),      C.bCleanFinish);
		Writer->WriteValue(TEXT("start_event_index"), C.StartEventIndex);
		Writer->WriteValue(TEXT("end_event_index"),   C.EndEventIndex);
		Writer->WriteValue(TEXT("start_time"),        static_cast<double>(C.StartTime));
		Writer->WriteValue(TEXT("end_time"),          static_cast<double>(C.EndTime));
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
		if (Players[i].StrengthOverride >= 0.f)
		{
			// Caller supplied an explicit strength (6-0 shuffle: current-match
			// PPR x100). Uniform default RD/sigma so the balancer's top-player
			// handling ranks purely on the supplied values.
			PR = PlayerRating(static_cast<double>(Players[i].StrengthOverride), kDefaultRD, kDefaultVolatility);
		}
		else if (PlayerRating* Cached = Impl->RatingCache.Find(Uid))
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
