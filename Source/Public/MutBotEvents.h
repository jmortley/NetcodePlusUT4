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

/** Bounded, cumulative arrival evidence. Losing an identity makes absence unknown
 * for this server instance; reconnecting never moves a player's first arrival. */
struct FBotArrivalLedger
{
	static const int32 MaxPlayers = 128;
	TMap<FString, double> Joined;
	bool bComplete;
	bool bWarmupSeen;

	FBotArrivalLedger() : bComplete(true), bWarmupSeen(false) {}
	static FString CanonicalId(const FString& Value);
	void RecordJoin(const FString& Id, double FirstSeenSeconds);
	bool QualifyState(FName State);
};

struct FBotArrivalConnection
{
	TWeakObjectPtr<APlayerController> Controller;
	double FirstSeenSeconds;
	FString Id;

	FBotArrivalConnection(APlayerController* InController, double InSeconds)
		: Controller(InController), FirstSeenSeconds(InSeconds) {}
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
	virtual void Init_Implementation(const FString& Options) override;
	virtual void PostPlayerInit_Implementation(AController* C) override;
	virtual void NotifyLogout_Implementation(AController* C) override;
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

	// Optional warning-trial telemetry. Separate from legacy readiness events.
	FString ArrivalLaunchId;
	FString ArrivalInstanceId;
	double ArrivalStartedAt;
	double ArrivalRequestStartedAt;
	int32 ArrivalSequence;
	bool bArrivalStopped;
	FBotArrivalLedger ArrivalLedger;
	TArray<FBotArrivalConnection> ArrivalConnections;
	FTimerHandle ArrivalTimer;
	FHttpRequestPtr ArrivalRequest;
	void ObserveArrival(APlayerController* PC, bool bFromLogin);
	void ResolveArrival(FBotArrivalConnection& Connection);
	void PollArrivals();
	void PostArrivals(bool bFinal, bool bImmediate = false);
	void StopArrivalPolling();

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

private:
	// ── Helpers ────────────────────────────────────────────────────────
	FString BuildPlayerListJson() const;
	FString BuildTeamScoresJson() const;
	FString GetMatchId() const;
	float GetTimeSeconds() const;
};
