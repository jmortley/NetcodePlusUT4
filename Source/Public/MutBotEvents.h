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

	// ── Player Readiness Polling ──────────────────────────────────────
	FTimerHandle ReadyCheckTimer;
	void PollPlayerReadiness();
	void StopReadyPolling();

	// ── Helpers ────────────────────────────────────────────────────────
	FString BuildPlayerListJson() const;
	FString BuildTeamScoresJson() const;
	FString GetMatchId() const;
	float GetTimeSeconds() const;
};
