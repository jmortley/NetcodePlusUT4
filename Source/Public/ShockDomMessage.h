// ShockDomMessage — Capture announcement messages for Domination
#pragma once
#include "NetcodePlus.h"
#include "CoreMinimal.h"
#include "UTLocalMessage.h"
#include "ShockDomMessage.generated.h"

/**
 * Message indices:
 *   0 = "RED/BLUE captured Point X"
 *   1 = "RED/BLUE controls all points!"
 */
UCLASS()
class NETCODEPLUS_API UShockDomMessage : public UUTLocalMessage
{
	GENERATED_UCLASS_BODY()

public:
	virtual FText GetText(int32 Switch, bool bTargetsPlayerState1, APlayerState* RelatedPlayerState_1,
		APlayerState* RelatedPlayerState_2, UObject* OptionalObject) const override;

	virtual FLinearColor GetMessageColor_Implementation(int32 MessageIndex) const override;
};
