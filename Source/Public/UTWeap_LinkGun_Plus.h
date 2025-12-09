// UTWeap_LinkGun_Plus.h
#pragma once
#include "CoreMinimal.h"
#include "UTWeaponFix.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "UTWeap_LinkGun_Plus.generated.h"

class UUTWeaponStateFiringLinkBeamPlus;
class AUTProjectile;
class UUTHUDWidget;
class UAnimMontage;
class UParticleSystem;
class USoundBase;

UCLASS(abstract)
class NETCODEPLUS_API AUTWeap_LinkGun_Plus : public AUTWeaponFix
{
    GENERATED_UCLASS_BODY()

public:

    // ===========================================
    // CSHD / NetcodePlus Configuration
    // ===========================================
    virtual void FireInstantHit(bool bDealDamage, FHitResult* OutHit = nullptr) override;
    /** Ping threshold below which we trust client hits (CSHD mode) */
    UPROPERTY(EditDefaultsOnly, Category = "NetcodePlus")
    float LowPingThreshold;

    /** Buffer zone for ping mode switching to prevent oscillation */
    UPROPERTY(EditDefaultsOnly, Category = "NetcodePlus")
    float HysteresisBuffer;

    /** Maximum distance tolerance for hit validation */
    UPROPERTY(EditDefaultsOnly, Category = "NetcodePlus")
    float MaxHitDistanceTolerance;

    /** Extra beam width added for high-ping rewind validation */
    UPROPERTY(EditDefaultsOnly, Category = "NetcodePlus")
    float HighPingBeamWidthPadding;

    /** Minimum damage to accumulate before sending to server */
    UPROPERTY(EditDefaultsOnly, Category = "NetcodePlus")
    int32 ClientDamageBatchSize;

    /** Tracks if we're currently in high ping validation mode */
    bool bHighPingMode;

    // ===========================================
    // Link Gun Beam State
    // ===========================================

    /** Current actor being hit by the beam */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    AActor* CurrentLinkedTarget;

    /** Time when we started linking to current target */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    float LinkStartTime;

    /** True if beam is currently impacting something */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    bool bLinkBeamImpacting;

    /** True if beam is dealing damage (valid target) */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    bool bLinkCausingDamage;

    /** True if held on target long enough to pull */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    bool bReadyToPull;

    // ===========================================
    // Link Pull / Pulse System
    // ===========================================

    /** Time required to hold beam on target before pull is ready */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Pull")
    float PullWarmupTime;

    /** Damage dealt by the link pull */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Pull")
    int32 LinkPullDamage;

    /** Momentum applied during beam pulse */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Pull")
    float BeamPulseMomentum;

    /** Ammo cost for beam pulse */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Pull")
    int32 BeamPulseAmmoCost;

    /** Duration of the pulse effect */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Pull")
    float BeamPulseInterval;

    /** Last time a beam pulse occurred */
    float LastBeamPulseTime;

    /** Current pulse target actor */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    AActor* PulseTarget;

    /** Location for pulse beam endpoint */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    FVector PulseLoc;

    /** Offset for missed pulse visual */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Pull")
    FVector MissedPulseOffset;

    /** Damage type for beam pulse */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Pull")
    TSubclassOf<UDamageType> BeamPulseDamageType;

    /** Crosshair color when ready to pull */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Pull")
    FLinearColor ReadyToPullColor;

    UPROPERTY(BlueprintReadWrite, Category = Mesh)
    float LastClientKillTime;

    virtual void NotifyKillWhileHolding_Implementation(TSubclassOf<UDamageType> DmgType) override
    {
        LastClientKillTime = GetWorld()->TimeSeconds;
    }

    // ===========================================
    // Overheat System
    // ===========================================

    /** Current overheat level (0-1+) */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    float OverheatFactor;

    /** True if weapon is in cooldown from overheat */
    UPROPERTY(BlueprintReadOnly, Category = "LinkGun")
    bool bIsInCoolDown;

    /** Sound played during overheat */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Audio")
    USoundBase* OverheatSound;

    /** Last time plasma was fired (for overheat calc) */
    float LastFiredPlasmaTime;

    // ===========================================
    // Visual Effects
    // ===========================================

    /** Kickback amount for beam firing */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Effects")
    float FiringBeamKickbackY;

    /** Kickback amount for link pull */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Effects")
    float LinkPullKickbackY;

    /** Animation for pulse/pull */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Animation")
    UAnimMontage* PulseAnim;

    /** Hands animation for pulse/pull */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Animation")
    UAnimMontage* PulseAnimHands;

    /** Particle effect for successful pulse */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Effects")
    UParticleSystem* PulseSuccessEffect;

    /** Particle effect for failed pulse */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Effects")
    UParticleSystem* PulseFailEffect;

    /** Sound for successful pull */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Audio")
    USoundBase* PullSucceeded;

    /** Sound for failed pull */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Audio")
    USoundBase* PullFailed;

    // ===========================================
    // Screen Display
    // ===========================================

    /** Material ID for main screen */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Screen")
    int32 ScreenMaterialID;

    /** Material ID for side screen */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Screen")
    int32 SideScreenMaterialID;

    /** Dynamic material instance for screen */
    UPROPERTY()
    UMaterialInstanceDynamic* ScreenMI;

    /** Dynamic material instance for side screen */
    UPROPERTY()
    UMaterialInstanceDynamic* SideScreenMI;

    /** Render target for screen texture */
    UPROPERTY()
    UCanvasRenderTarget2D* ScreenTexture;

    /** Font used on screen display */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Screen")
    UFont* ScreenFont;

    /** Texture shown on kill */
    UPROPERTY(EditDefaultsOnly, Category = "LinkGun|Screen")
    UTexture2D* ScreenKillNotifyTexture;

    /** Word wrapper for screen text */
    TSharedPtr<FCanvasWordWrapper> WordWrapper;

    // ===========================================
    // Core Functions
    // ===========================================

    virtual void FireShot() override;
    virtual void StartFire(uint8 FireModeNum) override;
    virtual AUTProjectile* FireProjectile() override;
    virtual void Tick(float DeltaTime) override;
    virtual void PlayWeaponAnim(UAnimMontage* WeaponAnim, UAnimMontage* HandsAnim, float RateOverride = 0.0f) override;

    // ===========================================
    // CSHD Hit Processing
    // ===========================================

    /** Called by beam state to process a client-side hit */
    void ProcessClientSideHit(float DeltaTime, AActor* HitActor, FVector HitLoc, const FInstantHitDamageInfo& DamageInfo);

    /** Server RPC to validate and apply beam damage */
    UFUNCTION(Server, WithValidation, Reliable)
    void ServerProcessBeamHit(AActor* HitActor, FVector_NetQuantize HitLocation, int32 DamageAmount);

    // ===========================================
    // Link Pull System
    // ===========================================

    /** Start the link pull on current target */
    void StartLinkPull();

    /** Check if target is valid for linking */
    bool IsValidLinkTarget(AActor* InTarget);

    /** Returns true if currently in pulse animation */
    bool IsLinkPulsing();

    /** Server RPC to set pulse target */
    UFUNCTION(Server, WithValidation, Reliable)
    void ServerSetPulseTarget(AActor* InTarget);

    // ===========================================
    // State & Lifecycle
    // ===========================================

    virtual void AttachToOwner_Implementation() override;
    virtual void Removed() override;
    virtual void ClientRemoved() override;
    virtual void StateChanged() override;

    /** AI check for bot pulse firing */
    UFUNCTION()
    void CheckBotPulseFire();

    // ===========================================
    // Visual Updates
    // ===========================================

    /** Update the screen render target */
    UFUNCTION()
    void UpdateScreenTexture(UCanvas* C, int32 Width, int32 Height);

    /** Called when flash extra is replicated (for pulse effects) */
    UFUNCTION(BlueprintNativeEvent)
    void FiringExtraUpdated(uint8 NewFlashExtra, uint8 InFireMode);

    /** Draw weapon crosshair with overheat/pull indicators */
    UFUNCTION(BlueprintNativeEvent)
    void DrawWeaponCrosshair(UUTHUDWidget* WeaponHudWidget, float RenderDelta);



    //virtual float GetHitValidationPredictionTime() const;
};