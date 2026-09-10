// NPPlayerController.cpp
#include "NPPlayerController.h"
#include "UnrealTournament.h"
#include "UTPlayerCameraManager.h"
#include "UTGameState.h"
#include "UTGameMessage.h"
#include "UTCTFGameMessage.h"
#include "NCPlusXTDMGameMode.h"
#include "NCPlusXTDMHUD.h"
#include "NCPlusXTDMMessage.h"

ANPPlayerController::ANPPlayerController(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Redundant with AUTPlayerController's own constructor (audit-verified),
	// kept as harmless insurance: an unset camera manager class fails PIE
	// spawn loudly.
	PlayerCameraManagerClass = AUTPlayerCameraManager::StaticClass();
}

void ANPPlayerController::ClientReceiveLocalizedMessage_Implementation(
	TSubclassOf<ULocalMessage> Message, int32 Switch, APlayerState* RelatedPlayerState_1,
	APlayerState* RelatedPlayerState_2, UObject* OptionalObject)
{
	const AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	const bool bXTDM = Cast<ANCPlusXTDMHUD>(MyHUD) != nullptr
		|| (GS && GS->GameModeClass && GS->GameModeClass->IsChildOf(ANCPlusXTDMGameMode::StaticClass()));
	if (bXTDM && Message)
	{
		// Stock TDM reuses two-team CTF dominance messages. Those can call a
		// Green/Yellow winner Blue; xTDM renders its own four-team status.
		if (Message->IsChildOf(UUTCTFGameMessage::StaticClass())) { return; }
		if (Message == UUTGameMessage::StaticClass() && (Switch == 9 || Switch == 10))
		{
			// Do this before ClientReceive: changing only HUD text is too late
			// to correct the stock console text and red/blue announcer audio.
			Message = UNCPlusXTDMGameMessage::StaticClass();
		}
	}
	Super::ClientReceiveLocalizedMessage_Implementation(Message, Switch,
		RelatedPlayerState_1, RelatedPlayerState_2, OptionalObject);
}
