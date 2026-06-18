#pragma once

#include "CoreMinimal.h"

/**
 * Standalone, mutator-INDEPENDENT time-on-target collector.
 *
 * The weapon's ServerReportFireToT RPC records straight into this static singleton,
 * so ToT works on ANY NetcodePlus server — including NA autopug, which does NOT load
 * MutBotEvents (the old host). The per-match report (server log "[ToT]" lines + the
 * Saved/Logs/ToT_*.csv timeline) fires from the NCPlus gamemode at match end, or on
 * demand via the `ncp.ToTDump` console command. Review-only; never affects gameplay.
 */
class NETCODEPLUS_API FNCToTCollector
{
public:
	static FNCToTCollector& Get();

	/** One client-reported sample (already server-clamped). World supplies the match clock. */
	void Record(class UWorld* World, class AUTPlayerState* Shooter, int32 DwellMs, uint8 FrameMs, bool bClaimedHit);

	/** Compute + log "[ToT]" lines + write the per-sample CSV. No-op if no samples.
	 *  Non-destructive — safe to call repeatedly (used by the `ncp.ToTDump` command). */
	void Report(class UWorld* World) const;

	/** Like Report, but fires at most once per match — call from the gamemode's match
	 *  end (the engine routes HandleMatchHasEnded twice on some paths). */
	void ReportOnce(class UWorld* World);

	/** Clear accumulated samples + arm the next report — call at match start. */
	void Reset();

private:
	struct FSample
	{
		int32 DwellMs;
		float ServerTime;   // GetServerWorldTimeSeconds() at receipt
		uint8 FrameMs;      // client mean frame time, ms (0 = unknown)
		bool  bClaimedHit;  // client believed this shot connected (vs on-target miss)
	};
	struct FStat
	{
		FString PlayerName;            // latest display name for this identity
		TArray<FSample> Samples;
	};
	struct FSummary
	{
		int32 N, HitN;
		float MeanMs, StdDevMs, CV;
		int32 P10, P50, P90, MinMs, MaxMs;
		float LowDwellPct, FirstFramePct, MeanFrameMs;
		int32 OutlierCount;
		int32 Hist[8];
		bool  bSuspect;
	};

	FSummary ComputeSummary(const FStat& S) const;
	void     WriteCsv(class UWorld* World) const;

	TMap<FString, FStat> Stats;   // key = stable identity (id:<StatsID> | net:<UniqueId> | name:<name>)
	bool bReportedSinceReset = false;
};
