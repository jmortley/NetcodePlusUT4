#pragma once
#include "NetcodePlus.h"
#include "UTLocalMessage.h"
#include "ElimPlusVictoryMessage.generated.h"

UCLASS()
class NETCODEPLUS_API UElimPlusVictoryMessage : public UUTLocalMessage
{
	GENERATED_BODY()

public:
	// Constructor
	UElimPlusVictoryMessage(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	FText BlueTeamWinsRoundText;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	FText RedTeamWinsRoundText;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	FText RoundDrawText;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	FText BlueTeamDominatingText;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	FText RedTeamDominatingText;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	FText BlueTeamTakesLeadText;

	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	FText RedTeamTakesLeadText;

	/** Sound played when red team wins a round */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	USoundBase* RedTeamVictorySound;

	/** Sound played when blue team wins a round */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	USoundBase* BlueTeamVictorySound;

	/** Sound played when round is a draw */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	USoundBase* DrawSound;

	/** Sound played when red team is dominating (5+ round lead) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	USoundBase* RedTeamDominatingSound;

	/** Sound played when blue team is dominating (5+ round lead) */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	USoundBase* BlueTeamDominatingSound;

	/** Sound played when red team takes the lead */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	USoundBase* RedTeamTakesLeadSound;

	/** Sound played when blue team takes the lead */
	UPROPERTY(EditDefaultsOnly, BlueprintReadWrite, Category = Message)
	USoundBase* BlueTeamTakesLeadSound;

	virtual FText GetText(int32 Switch, bool bTargetsPlayerState1, class APlayerState* RelatedPlayerState_1, class APlayerState* RelatedPlayerState_2, class UObject* OptionalObject) const override;
	virtual FLinearColor GetMessageColor_Implementation(int32 MessageIndex) const override;
	virtual void ClientReceive(const FClientReceiveData& ClientData) const override;
	virtual FName GetAnnouncementName_Implementation(int32 Switch, const UObject* OptionalObject, const class APlayerState* RelatedPlayerState_1, const class APlayerState* RelatedPlayerState_2) const override;
};
