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
	/** /reward POST — unified endpoint for kill-streak highlights (multikill
	 *  rungs above Monster, sprees Dominating+). Type = "monster"|"spree";
	 *  Level = the engine's MultiKillLevel (1..4) or Spree/5 (1..5). */
	void PostReward(AUTPlayerState* Scorer, const FString& Type, int32 Level);

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

	// ── Helpers ────────────────────────────────────────────────────────
	FString BuildPlayerListJson() const;
	FString BuildTeamScoresJson() const;
	FString GetMatchId() const;
	float GetTimeSeconds() const;
};
