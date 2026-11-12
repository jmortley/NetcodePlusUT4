#pragma once

#include "NetcodePlus.h"
#include "UTDMGameMode.h"
#include "NCAimTrainerPlayerController.h"
#include "NCAimTrainerLocalSession.h"
#include "NCAimTrainerSpawnBalance.h"
#include "NCAimTrainerDrillPolicy.h"
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
    void RestartTraining(ANCAimTrainerPlayerController* PC, bool bUseLightningGun);
    void AbortTraining(ANCAimTrainerPlayerController* PC);
    float RecordTargetHit(ANCAimTrainerTarget* Target, float Damage, const FDamageEvent& Event, AController* Instigator, AActor* Causer);
    double GetTrainerTimeSeconds() const { return TrainerTimeSeconds; }

private:
    UPROPERTY() ANCAimTrainerPlayerController* Trainee = nullptr;
    UPROPERTY() ANCAimTrainerArena* Arena = nullptr;
    UPROPERTY() TArray<ANCAimTrainerTarget*> Targets;
    UPROPERTY() TSubclassOf<AUTWeapon> SniperClass;
    UPROPERTY() TSubclassOf<AUTWeapon> LightningClass;
    UPROPERTY() TSubclassOf<AUTWeapon> SACTFSniperClass;
    UPROPERTY() TSubclassOf<AUTWeapon> InstagibClass;
    UPROPERTY() TSubclassOf<AUTWeapon> LinkClass;
    UPROPERTY() TSubclassOf<AUTWeapon> RocketClass;
    UPROPERTY() AUTWeapon* RunWeapon = nullptr;
    FNCAimTrainerProgress Progress;
    FRandomStream Schedule;
    FNCAimTrainerSpawnBalance AirborneSpawnBalance;
    int32 HeadshotClearedSlots = 0;
    NCAimTrainerDrillPolicy::FAttempt Drill;
    int32 FlakAttemptShot = 0;
    float PendingFlakHitDamage = 0.f;
    double FlakHitFeedbackAt = 0.0;
    FVector FlakHitFeedbackLocation = FVector::ZeroVector;
    TArray<TWeakObjectPtr<AActor>> FlakPellets;
    double NextDrillAction = 0.0;
    bool bDrillDodgeQueued = false;
    FVector ArenaOrigin = FVector(0.f, 0.f, 50000.f);
    double TrainerTimeSeconds = 0.0;
    double PhaseStartedAt = 0.0;
    // Engine projectile CreationTime is float world time; compare it only with
    // stamps in that domain, never with the precise trainer clock above.
    float PhaseStartedWorldAt = 0.f;
    double LastTraceTime = 0.0;
    double NextStatusTime = 0.0;
    double NextDirectionTime = 0.0;
    double NextDodgeTime = 0.0;
    double NextTrackingSlideTime = 0.0;
    double NextPopupTime = 0.0;
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
    TArray<double> NextTargetTime;
    TArray<double> TargetExpiry;
    TArray<double> NextWiggleTime;
    TArray<float> PopupLastMoveHold;
    TArray<double> NextPopupSlideTime;
    TArray<double> NextPopupLongStrafeTime;
    TArray<double> NextPopupDodgeTime;
    TArray<int32> PopupSpawnVariants;
    TArray<int32> PopupDodgeActions;
    TArray<double> NextCrouchTime;
    TArray<double> CrouchEndTime;

    bool EnsureArena();
    bool ConfigurePawn();
    bool IsInsidePracticeLane(const AUTCharacter* Pawn) const;
    bool FailSetup(const TCHAR* Message);
    void BeginActiveRun();
    void FinishRun();
    void PublishProgress();
    void RefreshLeaderboard(bool bAfterSubmit = false);
    void HideAllTargets();
    void ActivateSlot(int32 Index, double Now);
    void ActivateAirborneSlot(int32 Index, double Now);
    void UpdateAirborneTargets(double Now);
    bool LaunchAirborneJumper();
    bool IsAtAirborneHazard(const ANCAimTrainerTarget* Target) const;
    bool IsCurrentRocketDamage(const ANCAimTrainerTarget* Target, const FDamageEvent& Event, AActor* Causer) const;
    void ClearTrainerProjectiles();
    void UpdateDrillShots();
    void UpdateDrillTargets(double Now);
    void ActivateDrillTarget(double Now);
    float RecordDrillHit(ANCAimTrainerTarget* Target, float Damage, const FDamageEvent& Event, AActor* Causer);
    void RetireDrillTarget(bool bSuccess, double Now);
    void PublishDrillStatus();
    void QueueFlakHitFeedback(float Damage, const FVector& HitLocation);
    void FlushFlakHitFeedback();
    float PracticeLaneX() const;
    float PracticeFloorZ() const;
    void UpdateTargets(double Now);
    void UpdatePopupDodger(double Now);
    void ChoosePopupMovement(int32 Index, double Now);
    void UpdateTrackingMovement(double Now);
    void UpdateShotCount();
    bool HasTrackingContact() const;
    bool IsTrackingBeamFiring() const;
    void UpdateTrackingSample(double Now);
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
