// TeamArenaPredictionPC.h
#pragma once
#include "NetcodePlus.h"
#include "UTPlayerController.h"
#include "TeamArenaPredictionPC.generated.h"

/**
 * Enhanced player controller with split prediction system.
 * 
 * Separates visual prediction from hit validation:
 * - Visual Prediction (0ms): No forward extrapolation of enemy movement
 * - Hit Validation (120ms): Forgiving server-side rewind for hitscan
 * 
 * Eliminates the "dual hitbox" bug where enemies appear to have different
 * positions visually vs where server validates hits.
 * 
 * Works in conjunction with TeamArenaCharacter and UTWeaponFixHybrid.
 */
UCLASS()
class NETCODEPLUS_API ATeamArenaPredictionPC : public AUTPlayerController
{
    GENERATED_BODY()

public:
    ATeamArenaPredictionPC(const FObjectInitializer& ObjectInitializer);

    /**
     * Visual prediction time - used for client-side movement extrapolation.
     * Set to 0ms to show enemies exactly where server says they are.
     *
     * @return Prediction time in seconds for visual extrapolation
     */
   // UFUNCTION(BlueprintCallable, Category = "Prediction")
    virtual float GetVisualPredictionTime() const;

    /**
     * Hit validation time - used for server-side hitscan rewind.
     * Set to ~120ms to provide forgiving hit registration.
     *
     * @return Prediction time in seconds for server lag compensation
     */
    //UFUNCTION(BlueprintCallable, Category = "Prediction")
    virtual float GetHitValidationTime() const;




protected:
    /**
     * Desired visual prediction ping in milliseconds.
     * 0ms = no extrapolation (recommended for competitive)
     * 30-60ms = slight smoothing (casual play)
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prediction")
    float DesiredVisualPredictionPing;

    /**
     * Desired hit validation ping in milliseconds.
     * 120ms = forgiving for most players (recommended)
     * 60-90ms = tighter validation (LAN/low-ping)
     * 150-200ms = very forgiving (high-ping players)
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Prediction")
    float DesiredHitValidationPing;

    /**
     * Smoothed visual prediction time (in seconds).
     * Updated from DesiredVisualPredictionPing with smoothing.
     */
    UPROPERTY()
    float CurrentVisualPrediction;

    /**
     * Smoothed hit validation time (in seconds).
     * Updated from DesiredHitValidationPing with smoothing.
     */
    UPROPERTY()
    float CurrentHitValidation;

    /**
     * Smoothing factor for prediction time changes.
     * Prevents jarring changes if ping spikes.
     */
    UPROPERTY(EditAnywhere, Category = "Prediction")
    float PredictionSmoothingFactor_X;

};
