// MutBotEvents.h - HTTP event poster for UT4IGBot Discord bot
// Posts match state changes and flag captures to the bot's FastAPI server.
// Replaces the Blueprint HttpPostEvents mutator.
//
// Config: URL options on server launch command:
//   ?PugId=42&BotApiUrl=http://bot:9999&BotApiToken=secret
//
// Or Mod.ini [BOT_EVENTS] section:
//   BotApiUrl=http://bot:9999
//   BotApiToken=secret

#pragma once

#include "NetcodePlus.h"
#include "UTMutator.h"
#include "Http.h"
#include "MutBotEvents.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogBotEvents, Log, All);

/** Cover kills accrued by a team while it carries the enemy flag. Indexed per
 *  carrier team; snapshotted into the FlagCapture POST when that team scores. */
struct FCoverCarryWindow
{
	bool bOpen;
	FString CarrierName;
	TArray<FString> CoverKills;

	FCoverCarryWindow() : bOpen(false) {}
};

UCLASS()
class NETCODEPLUS_API AMutBotEvents : public AUTMutator
{
	GENERATED_BODY()

public:
	AMutBotEvents(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// ── Mutator Hooks ────────────────────────────────────────────────
	virtual void NotifyMatchStateChange_Implementation(FName NewState) override;
	virtual void ScoreObject_Implementation(AUTCarriedObject* GameObject, AUTCharacter* HolderPawn,
		AUTPlayerState* Holder, FName Reason) override;
	virtual void ScoreKill_Implementation(AController* Killer, AController* Other,
		TSubclassOf<UDamageType> DamageType) override;
	virtual void Mutate_Implementation(const FString& MutateString, APlayerController* Sender) override;

private:
	// ── Config ────────────────────────────────────────────────────────
	FString BotApiUrl;
	FString BotApiToken;
	int32 PugId;

	// ── HTTP ──────────────────────────────────────────────────────────
	void SendPost(const FString& Endpoint, const FString& JsonBody, int32 RetryCount = 0);

	static const int32 MaxRetries = 3;

	// ── Event Senders ─────────────────────────────────────────────────
	void PostStateChange(const FString& State);
	void PostStateChangeWithPlayers(const FString& State);
	void PostFlagCapture(AUTPlayerState* Scorer);
	void PostMatchEnded();
	/** /reward POST — unified endpoint for kill-streak highlights.
	 *  Type      = "monster" | "spree".
	 *  Level     = raw engine value (MultiKillLevel for monster, Spree/5 for spree).
	 *  Multiplier = display multiplier for the bot. Monster: 1 = first Monster
	 *               (5 frags), 2 = next kill in window (6 frags), etc. — bot
	 *               edits the existing embed instead of posting a new one when
	 *               Multiplier > 1. Spree: always 1 (each spree milestone is a
	 *               distinct event). */
	void PostReward(AUTPlayerState* Scorer, const FString& Type, int32 Level, int32 Multiplier);

	// ── Player Readiness Polling ──────────────────────────────────────
	FTimerHandle ReadyCheckTimer;
	void PollPlayerReadiness();
	void StopReadyPolling();

	// ── Cover-Kill Tracking ───────────────────────────────────────────
	FCoverCarryWindow CarryWindows[2]; // indexed by carrier team (0=Red, 1=Blue)
	bool bFlagEventsBound;

	/** Bind the flag holder-changed delegates (once, when flags exist). */
	void TryBindFlagEvents();

	/** Open/close a carry window as a flag is grabbed or dropped/returned. */
	UFUNCTION()
	void OnFlagHolderChanged(AUTCarriedObject* Flag);

	// ── Kill-streak highlights ───────────────────────────────────────
	/** Called from ScoreKill_Implementation after Super:: has updated the
	 *  killer's MultiKillLevel / Spree. Posts /reward for Monster Kill (exact
	 *  threshold) and for Spree levels 3..5 (Dominating, Unstoppable, Godlike). */
	void ScoreKill_PostHighlights(AUTPlayerState* KillerPS);

	// ── Trigger-bot review: time-on-target at fire (ToT) ──────────────
	/** One client-reported sample: dwell-at-fire (ms), WHEN it happened (server
	 *  match clock — scrub a server demo to this to see the shot), and the client's
	 *  smoothed frame time (ms) so the frame-quantization bias is visible in review.
	 *  Dwell is kept full-range (int32 ms), not capped at a byte, so heavy trackers
	 *  no longer saturate the mean. */
	struct FToTSample
	{
		int32 DwellMs;
		float ServerTime;   // GetServerWorldTimeSeconds() at receipt
		uint8 FrameMs;      // client mean frame time, ms (0 = unknown)
		bool  bClaimedHit;  // client believed this shot connected (vs on-target miss)
		FToTSample() : DwellMs(0), ServerTime(0.f), FrameMs(0), bClaimedHit(false) {}
		FToTSample(int32 InDwell, float InTime, uint8 InFrame, bool bInHit)
			: DwellMs(InDwell), ServerTime(InTime), FrameMs(InFrame), bClaimedHit(bInHit) {}
	};

	/** Per-player accumulation, keyed by a STABLE identity (StatsID when the player
	 *  is logged in, else name) so a disconnect/reconnect — which mints a new
	 *  PlayerState, hence a new PlayerId — does NOT split a player's stats. Raw
	 *  samples (tens per match) are retained so percentiles, variance and a
	 *  histogram are computed at report time. Fed by AUTWeaponFix::ServerReportFireToT
	 *  on each claimed hitscan hit in Elim / iCTF. REVIEW-ONLY — never auto-acts. */
	struct FToTStat
	{
		FString PlayerName;            // latest display name for this identity
		TArray<FToTSample> Samples;
	};
	TMap<FString, FToTStat> ToTStats;  // key = stable identity (id:<StatsID> | name:<name>)

	/** Derived per-player summary computed from the raw samples at report time. */
	struct FToTSummary
	{
		int32 N;                       // on-target shots sampled (hits + on-target misses)
		int32 HitN;                    // subset the client claimed connected
		float MeanMs, StdDevMs, CV;    // CV = StdDev/Mean; LOW cv = "weirdly consistent"
		int32 P10, P50, P90, MinMs, MaxMs;
		float LowDwellPct;             // share of samples <= ToTLowDwellMs (fixed ms)
		float FirstFramePct;           // share fired within ~1 of the player's OWN frames
		                               // (dwell <= frame_ms) — fps-FAIR low-dwell measure
		float MeanFrameMs;             // fps context (so the quantization bias is visible)
		int32 OutlierCount;            // samples beyond mean +/- 3 sd (extreme outliers)
		int32 Hist[8];                 // dwell histogram (see ToTHistBucket in the .cpp)
		bool  bSuspectConsistent;      // review hint: tight + low cluster over enough shots
	};

	/** Dwell at or below this many milliseconds counts as "near-zero" (trigger-bot-
	 *  like). A human FLICK also lands here — the signal is the per-match FRACTION
	 *  of low-dwell shots (and the variance/consistency), never any single shot. */
	static const int32 ToTLowDwellMs = 16;

	/** Below this sample count, never raise the "consistent" review hint — small
	 *  samples are noisy and a tight cluster can be coincidence. */
	static const int32 ToTMinSamplesForFlag = 12;

	/** Collapse a player's raw samples into the derived summary above. */
	FToTSummary ComputeToTSummary(const FToTStat& S) const;

	/** Build + emit the per-player ToT review summary (server log + bot POST). */
	void PostToTReport();

	/** Write every sample as one time-sorted CSV timeline (Saved/Logs/ToT_*.csv)
	 *  so an admin reviewing a server demo can scrub to server_time and see the
	 *  shot. Always written (even with no bot URL configured). */
	void WriteToTTimelineCsv() const;

public:
	/** Server-side sink for AUTWeaponFix's ToT telemetry (one sample per claimed
	 *  hitscan hit). DwellMs = ms the crosshair rested on the visible target before
	 *  fire; FrameMs = client mean frame time (fps context). Public so the weapon
	 *  RPC handler can route into it. */
	void RecordFireToT(class AUTPlayerState* Shooter, int32 DwellMs, uint8 FrameMs, bool bClaimedHit);

private:
	// ── Helpers ────────────────────────────────────────────────────────
	FString BuildPlayerListJson() const;
	FString BuildTeamScoresJson() const;
	FString GetMatchId() const;
	float GetTimeSeconds() const;
};
