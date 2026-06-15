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
	/** Per-player accumulation of client-reported time-on-target samples, keyed
	 *  by AUTPlayerState::PlayerId (stable within a match, survives respawns). Fed
	 *  by AUTWeaponFix::ServerReportFireToT on each claimed hitscan hit in Elim /
	 *  iCTF. REVIEW-ONLY — surfaced at match end, never auto-acts. */
	struct FToTStat
	{
		FString PlayerName;
		int32   Shots;         // claimed hitscan hits sampled
		int32   LowDwellShots; // samples with dwell <= ToTLowDwellMs
		int64   SumMs;         // for the mean
		FToTStat() : Shots(0), LowDwellShots(0), SumMs(0) {}
	};
	TMap<int32, FToTStat> ToTStats;

	/** Dwell at or below this many milliseconds counts as "near-zero" (trigger-bot-
	 *  like). A human FLICK also lands here — the signal is the per-match FRACTION
	 *  of low-dwell shots, never any single shot. fps-independent (dwell is timed). */
	static const int32 ToTLowDwellMs = 16;

	/** Build + emit the per-player ToT review summary (server log + bot POST). */
	void PostToTReport();

public:
	/** Server-side sink for AUTWeaponFix's ToT telemetry (one sample per claimed
	 *  hitscan hit). DwellMs = ms the crosshair rested on the visible target before
	 *  fire. Public so the weapon RPC handler can route into it. */
	void RecordFireToT(class AUTPlayerState* Shooter, uint8 DwellMs);

private:
	// ── Helpers ────────────────────────────────────────────────────────
	FString BuildPlayerListJson() const;
	FString BuildTeamScoresJson() const;
	FString GetMatchId() const;
	float GetTimeSeconds() const;
};
