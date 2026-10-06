#pragma once

#include "NetcodePlus.h"
#include "UTDMGameMode.h"
#include "NCAimTrainerPlayerController.h"
#include "NCAimTrainerGame.generated.h"

class ANCAimTrainerTarget;
class ANCAimTrainerArena;
class AUTWeapon;

/** Fixed-preset, one-trainee aim practice. Explicitly selected game mode only. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANCAimTrainerGame : public AUTDMGameMode
{
    GENERATED_BODY()
public:
    ANCAimTrainerGame(const FObjectInitializer& ObjectInitializer);
    virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void PostLogin(APlayerController* NewPlayer) override;
    virtual void Logout(AController* Exiting) override;
    virtual void RestartPlayer(AController* Player) override;
    virtual bool ReadyToStartMatch_Implementation() override;
    virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;
    virtual void SetPlayerDefaults(APawn* Pawn) override;
    virtual bool CheckRelevance_Implementation(AActor* Other) override;
    virtual bool AllowPausing(APlayerController* PC) override;

    void SelectScenario(ANCAimTrainerPlayerController* PC, uint8 Scenario);
    void SetMovementPractice(ANCAimTrainerPlayerController* PC, bool bEnabled);
    void StartTraining(ANCAimTrainerPlayerController* PC);
    void AbortTraining(ANCAimTrainerPlayerController* PC);
    float RecordTargetHit(ANCAimTrainerTarget* Target, float Damage, const FDamageEvent& Event, AController* Instigator, AActor* Causer);

private:
    UPROPERTY() ANCAimTrainerPlayerController* Trainee = nullptr;
    UPROPERTY() ANCAimTrainerArena* Arena = nullptr;
    UPROPERTY() TArray<ANCAimTrainerTarget*> Targets;
    UPROPERTY() TSubclassOf<AUTWeapon> SniperClass;
    UPROPERTY() TSubclassOf<AUTWeapon> InstagibClass;
    UPROPERTY() AUTWeapon* RunWeapon = nullptr;
    FNCAimTrainerProgress Progress;
    FRandomStream Schedule;
    FVector ArenaOrigin = FVector(0.f, 0.f, 50000.f);
    float PhaseStartedAt = 0.f;
    float LastTraceTime = 0.f;
    float NextStatusTime = 0.f;
    float NextDirectionTime = 0.f;
    float ShotStatBaseline = 0.f;
    double TrackedSeconds = 0.0;
    bool bPreviousContact = false;
    bool bRankedRun = false;
    FString UnrankedReason;
    FString SetupError;
    FString RunId;
    float NextTargetTime[3] = { 0.f, 0.f, 0.f };
    float TargetExpiry[3] = { 0.f, 0.f, 0.f };
    double NextLeaderboardFetch[3] = { 0.0, 0.0, 0.0 };
    bool LeaderboardInFlight[3] = { false, false, false };
    TArray<FNCAimTrainerLeaderboardRow> LeaderboardCache[3];

    bool EnsureArena();
    bool ConfigurePawn();
    bool IsInsidePracticeLane(const AUTCharacter* Pawn) const;
    bool FailSetup(const TCHAR* Message);
    void BeginActiveRun();
    void FinishRun();
    void PublishProgress();
    void RefreshLeaderboard(bool bAfterSubmit = false);
    void HideAllTargets();
    void ActivateSlot(int32 Index, float Now);
    void UpdateShotCount();
    bool IsTrainee(const ANCAimTrainerPlayerController* PC) const;
};
