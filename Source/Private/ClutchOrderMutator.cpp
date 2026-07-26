#include "ClutchOrderMutator.h"
#include "ClutchGameMode.h"
#include "UnrealTournament.h"


AClutchOrderMutator::AClutchOrderMutator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	GroupNames.Add(FName(TEXT("ClutchOrder")));
}


void AClutchOrderMutator::Mutate_Implementation(
	const FString& MutateString, APlayerController* Sender)
{
	FString Command = MutateString;
	Command.Trim();
	Command.TrimTrailing();
	const FString Prefix(TEXT("ClutchOrder"));
	const bool bIsOrderCommand = Command.Equals(Prefix, ESearchCase::IgnoreCase)
		|| Command.StartsWith(Prefix + TEXT(" "), ESearchCase::IgnoreCase);
	if (bIsOrderCommand && Sender)
	{
		FString Arguments = Command.RightChop(Prefix.Len());
		Arguments.Trim();
		Arguments.TrimTrailing();
		FString NormalizedArguments = Arguments.Replace(TEXT(","), TEXT(" "));
		TArray<FString> Tokens;
		NormalizedArguments.ParseIntoArray(Tokens, TEXT(" "), true);

		TArray<int32> OrderedSlots;
		bool bAllNumeric = Tokens.Num() > 0;
		for (const FString& Token : Tokens)
		{
			if (!Token.IsNumeric())
			{
				bAllNumeric = false;
				break;
			}
			OrderedSlots.Add(FCString::Atoi(*Token));
		}

		AClutchGameMode* GameMode = GetWorld()
			? Cast<AClutchGameMode>(GetWorld()->GetAuthGameMode())
			: nullptr;
		if (!bAllNumeric)
		{
			Sender->ClientMessage(TEXT("Invalid Clutch attack-order selection."));
		}
		else if (!GameMode)
		{
			Sender->ClientMessage(TEXT("Clutch attack ordering is unavailable."));
		}
		else
		{
			GameMode->SubmitAttackOrder(Sender, OrderedSlots);
		}
	}

	// Preserve every other mutator command in the chain.
	Super::Mutate_Implementation(MutateString, Sender);
}
