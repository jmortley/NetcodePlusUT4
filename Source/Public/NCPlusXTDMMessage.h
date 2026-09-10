#pragma once

#include "NetcodePlus.h"
#include "UTGameMessage.h"
#include "UTVictoryMessage.h"
#include "NCPlusXTDMMessage.generated.h"

UCLASS()
class NETCODEPLUS_API UNCPlusXTDMGameMessage : public UUTGameMessage
{
	GENERATED_UCLASS_BODY()
public:
	virtual void GetEmphasisText(FText& Prefix, FText& Emphasis, FText& Postfix,
		FLinearColor& Color, int32 Switch, APlayerState* Player1, APlayerState* Player2,
		UObject* OptionalObject) const override;
	virtual FName GetAnnouncementName_Implementation(int32 Switch, const UObject* OptionalObject,
		const APlayerState* Player1, const APlayerState* Player2) const override;
};

UCLASS()
class NETCODEPLUS_API UNCPlusXTDMVictoryMessage : public UUTVictoryMessage
{
	GENERATED_UCLASS_BODY()
public:
	virtual void GetEmphasisText(FText& Prefix, FText& Emphasis, FText& Postfix,
		FLinearColor& Color, int32 Switch, APlayerState* Player1, APlayerState* Player2,
		UObject* OptionalObject) const override;
	virtual FText GetText(int32 Switch, bool bTargetsPlayerState1, APlayerState* Player1,
		APlayerState* Player2, UObject* OptionalObject) const override;
	virtual FName GetAnnouncementName_Implementation(int32 Switch, const UObject* OptionalObject,
		const APlayerState* Player1, const APlayerState* Player2) const override;
};
