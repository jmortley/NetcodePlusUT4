// NPPlayerController.cpp
#include "NPPlayerController.h"
#include "UnrealTournament.h"
#include "UTPlayerCameraManager.h"

ANPPlayerController::ANPPlayerController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Redundant with AUTPlayerController's own constructor (audit-verified),
	// kept as harmless insurance: an unset camera manager class fails PIE
	// spawn loudly.
	PlayerCameraManagerClass = AUTPlayerCameraManager::StaticClass();
}
