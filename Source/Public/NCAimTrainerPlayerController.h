#pragma once

#include "NetcodePlus.h"
#include "UTPlayerController.h"
#include "NCAimTrainerTypes.h"
#include "NCAimTrainerPlayerController.generated.h"

/** Small, owner-only presentation snapshot. Scores are calculated by the game mode. */
USTRUCT()
struct FNCAimTrainerProgress
{
	GENERATED_BODY()

	UPROPERTY() uint8 Scenario = 0;
	/** 0 picker, 1 countdown, 2 running, 3 results. */
	UPROPERTY() uint8 Phase = 0;
	/** Optional lateral/jump/dodge practice. These runs never enter ranked boards. */
	UPROPERTY() bool bMovementPractice = false;
	UPROPERTY() int32 Score = 0;
	UPROPERTY() int32 Shots = 0;
	UPROPERTY() int32 Hits = 0;
	UPROPERTY() int32 Headshots = 0;
	UPROPERTY() int32 TargetsExpired = 0;
	UPROPERTY() float RemainingSeconds = 60.f;
	UPROPERTY() float Accuracy = 0.f;
	UPROPERTY() float TrackingSeconds = 0.f;
};

/** Assigned only by the opt-in trainer game mode. Does not replace any existing mode's PC. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANCAimTrainerPlayerController : public AUTPlayerController
{
	GENERATED_BODY()

public:
	ANCAimTrainerPlayerController(const FObjectInitializer& ObjectInitializer);
	virtual void ClientRestart_Implementation(APawn* NewPawn) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual bool InputKey(FKey Key, EInputEvent EventType, float AmountDepressed, bool bGamepad) override;
	virtual void OnFire() override;
	virtual void OnAltFire() override;
	virtual void MoveForward(float Value) override;
	virtual void MoveRight(float Value) override;
	virtual void Jump() override;
	virtual void Crouch() override;
	virtual void ToggleCrouch() override;

	const FNCAimTrainerProgress& GetTrainerProgress() const { return TrainerProgress; }
	const TArray<FNCAimTrainerLeaderboardRow>& GetTrainerLeaderboard() const { return Leaderboard; }
	const FString& GetTrainerOnlineStatus() const { return OnlineStatus; }
	bool IsTrainerMenuVisible() const;
	bool HasTrainerInputFocus() const;
	void SelectTrainerScenario(uint8 Scenario);
	void StartTrainerRun();
	void ReturnToTrainerMenu();
	void ToggleTrainerMovementPractice();

	/** Authority-side publishers used by ANCAimTrainerGame and its leaderboard service. */
	void SetTrainerProgress(const FNCAimTrainerProgress& Progress);
	void SetTrainerLeaderboard(const TArray<FNCAimTrainerLeaderboardRow>& Rows);
	void SetTrainerOnlineStatus(const FString& Status);
	/** Called only after the authority accepts a scoring hit on a practice target. */
	void NotifyTrainerHit(float Damage);

	UFUNCTION(Server, Reliable, WithValidation)
	void ServerTrainerSelectScenario(uint8 Scenario);
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerTrainerStart();
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerTrainerAbort();
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerTrainerSetMovementPractice(bool bEnabled);
	UFUNCTION(Client, Reliable)
	void ClientTrainerLeaderboard(const TArray<FNCAimTrainerLeaderboardRow>& Rows);
	UFUNCTION(Client, Reliable)
	void ClientTrainerOnlineStatus(const FString& Status);
	UFUNCTION(Client, Reliable)
	void ClientTrainerConfirmedHit(int32 Damage);

private:
	UPROPERTY(ReplicatedUsing=OnRep_TrainerProgress)
	FNCAimTrainerProgress TrainerProgress;
	UPROPERTY(Transient)
	TArray<FNCAimTrainerLeaderboardRow> Leaderboard;
	UPROPERTY(Transient)
	FString OnlineStatus;

	UFUNCTION()
	void OnRep_TrainerProgress();
	void ApplyTrainerMovementMode();
	/** Bound repeated requests without dropping a quick select-then-start sequence. */
	bool AdmitTrainerRequest(uint8 Action);
	double NextTrainerRequestTime[4] = { 0.0, 0.0, 0.0, 0.0 };
	uint8 LastPresentedPhase = 255;
	bool bLastPresentedMovementPractice = false;
};
