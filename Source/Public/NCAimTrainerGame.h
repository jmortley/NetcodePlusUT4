#pragma once

#include "NetcodePlus.h"
#include "UTDMGameMode.h"
#include "NCAimTrainerPlayerController.h"
#include "NCAimTrainerLocalSession.h"
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
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void PostLogin(APlayerController* NewPlayer) override;
    virtual void Logout(AController* Exiting) override;
    virtual void RestartPlayer(AController* Player) override;
    virtual UClass* GetDefaultPawnClassForController_Implementation(AController* InController) override;
    virtual bool ReadyToStartMatch_Implementation() override;
    virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;
    virtual void SetPlayerDefaults(APawn* Pawn) override;
    virtual bool CheckRelevance_Implementation(AActor* Other) override;
    virtual bool AllowPausing(APlayerController* PC) override;

    void SelectScenario(ANCAimTrainerPlayerController* PC, uint8 Scenario, bool bUseLightningGun);
    void SetMovementPractice(ANCAimTrainerPlayerController* PC, bool bEnabled);
    void StartTraining(ANCAimTrainerPlayerController* PC, bool bUseLightningGun);
    void AbortTraining(ANCAimTrainerPlayerController* PC);
    float RecordTargetHit(ANCAimTrainerTarget* Target, float Damage, const FDamageEvent& Event, AController* Instigator, AActor* Causer);

private:
    UPROPERTY() ANCAimTrainerPlayerController* Trainee = nullptr;
    UPROPERTY() ANCAimTrainerArena* Arena = nullptr;
    UPROPERTY() TArray<ANCAimTrainerTarget*> Targets;
    UPROPERTY() TSubclassOf<AUTWeapon> SniperClass;
    UPROPERTY() TSubclassOf<AUTWeapon> LightningClass;
    UPROPERTY() TSubclassOf<AUTWeapon> SACTFSniperClass;
    UPROPERTY() TSubclassOf<AUTWeapon> InstagibClass;
    UPROPERTY() TSubclassOf<AUTWeapon> LinkClass;
    UPROPERTY() AUTWeapon* RunWeapon = nullptr;
    FNCAimTrainerProgress Progress;
    FRandomStream Schedule;
    FVector ArenaOrigin = FVector(0.f, 0.f, 50000.f);
    float PhaseStartedAt = 0.f;
    float LastTraceTime = 0.f;
    float NextStatusTime = 0.f;
    float NextDirectionTime = 0.f;
    float NextDodgeTime = 0.f;
    float NextTrackingSlideTime = 0.f;
    float NextPopupTime = 0.f;
    float PopupRefireSeconds = 1.f;
    float ShotStatBaseline = 0.f;
    double TrackedSeconds = 0.0;
    double FiredSeconds = 0.0;
    bool bPreviousContact = false;
    bool bPreviousFiring = false;
    bool bRankedRun = false;
    FString UnrankedReason;
    FString SetupError;
    FString RunId;
    TSharedPtr<FNCAimTrainerLocalSession> LocalSession;
    TArray<FNCAimTrainerLocalEvent> LocalEvents;
    int32 LocalAppearances[6] = {};
    int32 LocalNextCheckpoint = 1;
    int32 LocalLastShotCount = 0;
    bool bLocalAuthPending = false;
    bool bLocalRecording = false;
    TArray<float> NextTargetTime;
    TArray<float> TargetExpiry;
    TArray<float> NextWiggleTime;
    TArray<float> NextPopupSlideTime;
    TArray<float> NextPopupLongStrafeTime;
    TArray<float> NextCrouchTime;
    TArray<float> CrouchEndTime;

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
    void UpdateTargets(float Now);
    void UpdatePopupDodger(float Now);
    void UpdateTrackingMovement(float Now);
    void UpdateShotCount();
    bool HasTrackingContact() const;
    bool IsTrackingBeamFiring() const;
    void UpdateTrackingSample(float Now);
    bool IsTrainee(const ANCAimTrainerPlayerController* PC) const;
    bool IsStandardPreset() const;
    void ResetLocalSession();
    void StartLocalSession();
    bool PrepareLocalRecording();
    void BeginLocalRecording();
    void InvalidateLocalRun();
    void RecordLocalShotCount();
    void RecordLocalTarget(int32 Slot, bool bHit, bool bHeadshot = false);
    void RecordLocalSample(bool bFiring, bool bContact);
    void AddLocalEvent(FNCAimTrainerLocalEvent Event);
    void FlushLocalCheckpoint(bool bFinal = false);
};
