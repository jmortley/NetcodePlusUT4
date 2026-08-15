#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTMutator.h"
#include "ClutchOrderMutator.generated.h"

/**
 * Invisible transport for the Clutch HUD's attack-order buttons.
 *
 * The stock AUTPlayerController already exposes a reliable ServerMutate RPC,
 * so this receiver avoids introducing a custom controller solely for one UI
 * action. Players interact with Canvas buttons and never type the command.
 */
UCLASS()
class NETCODEPLUS_API AClutchOrderMutator : public AUTMutator
{
	GENERATED_UCLASS_BODY()

public:
	virtual void Mutate_Implementation(
		const FString& MutateString, APlayerController* Sender) override;
};
