// TeamArenaPredictionPC.cpp
#include "TeamArenaPredictionPC.h"
#include "UnrealTournament.h"
#include "TeamArenaCharacter.h"
#include "UTWeaponFix.h"
#include "UTPlayerState.h"


ATeamArenaPredictionPC::ATeamArenaPredictionPC(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Fix: Explicitly set the CameraManager to prevent "SpawnActor failed" crashes
    // if the Blueprint Class Default Object (CDO) loses its reference.
    //PlayerCameraManagerClass = UTPlayerCameraManager::StaticClass();

    // Split prediction configuration
    DesiredVisualPredictionPing = 0.0f;      // No client-side extrapolation
    DesiredHitValidationPing = 120.0f;       // Forgiving server-side rewind

    CurrentVisualPrediction = 0.0f;
    CurrentHitValidation = 0.12f;            // 120ms in seconds
    PredictionSmoothingFactor_X = 0.1f;        // Smooth over ~10 frames


}





float ATeamArenaPredictionPC::GetVisualPredictionTime() const
{
    // SERVER ONLY: Calculates how far to rewind hitboxes to match the 
    // "Predict 0" view of the client.
    if (!HasAuthority() || !PlayerState)
    {
        return 0.12f;
    }

    AUTPlayerState* SafePS = Cast<AUTPlayerState>(PlayerState);
    if (!SafePS)
    {
        return 0.12f;
    }

    if (HasAuthority())
    {
        // Safe Cast
        AUTPlayerState* SafePS = Cast<AUTPlayerState>(PlayerState);

        // 1. Get the player's actual exact ping
        float ActualPingMs = (SafePS) ? SafePS->ExactPing : 0.0f;

        // 2. Add fudge factor (Matches your Linear Smoothing settings)
        //float CalculatedRewind = ActualPingMs + (PredictionFudgeFactor * 0.001f);
        float CalculatedRewindMs = ActualPingMs + PredictionFudgeFactor;
        // 3. Cap it at your desired limit (e.g. 120ms)
        //return FMath::Clamp(CalculatedRewind, 0.0f, DesiredHitValidationPing);
        float ClampedMs = FMath::Clamp(CalculatedRewindMs, 0.0f, DesiredHitValidationPing);
        return ClampedMs * 0.001f;
    }

    // Client doesn't need this anymore because Movement uses the Character override.
    return 0.0f;
}



float ATeamArenaPredictionPC::GetHitValidationTime() const
{
    // Return current hit validation time (smoothed from desired)
    return CurrentHitValidation;
}

/*float ATeamArenaPredictionPC::GetPredictionTime()
{
    // Redirect to hit validation time for weapon systems
    // This makes Epic's GetRewindLocation() use the correct value
    return GetHitValidationTime();
}*/

