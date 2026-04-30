#include "ElimPlusVictoryMessage.h"
#include "UnrealTournament.h"
#include "Sound/SoundBase.h"

UElimPlusVictoryMessage::UElimPlusVictoryMessage(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsUnique = true;
	bIsStatusAnnouncement = true;
	Lifetime = 3.0f;
	MessageArea = FName(TEXT("Announcements"));
	MessageSlot = FName(TEXT("GameMessages"));

	BlueTeamWinsRoundText = NSLOCTEXT("TeamArena", "BlueWinsRound", "Blue Team Wins the Round!");
	RedTeamWinsRoundText = NSLOCTEXT("TeamArena", "RedWinsRound", "Red Team Wins the Round!");
	RoundDrawText = NSLOCTEXT("TeamArena", "RoundDraw", "Round Draw!");
	BlueTeamDominatingText = NSLOCTEXT("TeamArena", "BlueDominating", "Blue Team is Dominating!");
	RedTeamDominatingText = NSLOCTEXT("TeamArena", "RedDominating", "Red Team is Dominating!");
	BlueTeamTakesLeadText = NSLOCTEXT("TeamArena", "BlueTakesLead", "Blue Team Takes the Lead!");
	RedTeamTakesLeadText = NSLOCTEXT("TeamArena", "RedTakesLead", "Red Team Takes the Lead!");

	// Initialize sounds to nullptr - will be set in Blueprint
	RedTeamVictorySound = nullptr;
	BlueTeamVictorySound = nullptr;
	DrawSound = nullptr;
	RedTeamDominatingSound = nullptr;
	BlueTeamDominatingSound = nullptr;
	RedTeamTakesLeadSound = nullptr;
	BlueTeamTakesLeadSound = nullptr;
}

void UElimPlusVictoryMessage::ClientReceive(const FClientReceiveData& ClientData) const
{
	Super::ClientReceive(ClientData);

	AUTPlayerController* PC = Cast<AUTPlayerController>(ClientData.LocalPC);
	if (PC)
	{
		USoundBase* SoundToPlay = nullptr;

		switch (ClientData.MessageIndex)
		{
			case 0: // Red team wins
				SoundToPlay = RedTeamVictorySound;
				break;
			case 1: // Blue team wins
				SoundToPlay = BlueTeamVictorySound;
				break;
			case 2: // Draw
				SoundToPlay = DrawSound;
				break;
			case 3: // Red team dominating
				SoundToPlay = RedTeamDominatingSound;
				break;
			case 4: // Blue team dominating
				SoundToPlay = BlueTeamDominatingSound;
				break;
			case 5: // Red team takes lead
				SoundToPlay = RedTeamTakesLeadSound;
				break;
			case 6: // Blue team takes lead
				SoundToPlay = BlueTeamTakesLeadSound;
				break;
		}

		if (SoundToPlay)
		{
			PC->UTClientPlaySound(SoundToPlay);
		}
	}
}

FText UElimPlusVictoryMessage::GetText(int32 Switch, bool bTargetsPlayerState1, class APlayerState* RelatedPlayerState_1, class APlayerState* RelatedPlayerState_2, class UObject* OptionalObject) const
{
	switch (Switch)
	{
		case 0:
			return RedTeamWinsRoundText;
		case 1:
			return BlueTeamWinsRoundText;
		case 2:
			return RoundDrawText;
		case 3:
			return RedTeamDominatingText;
		case 4:
			return BlueTeamDominatingText;
		case 5:
			return RedTeamTakesLeadText;
		case 6:
			return BlueTeamTakesLeadText;
		case 10:
			return RoundDrawText;
		default:
			return FText();
	}
}

FLinearColor UElimPlusVictoryMessage::GetMessageColor_Implementation(int32 MessageIndex) const
{
	switch (MessageIndex)
	{
		case 0: // Red team wins
		case 3: // Red team dominating
		case 5: // Red team takes lead
			return FLinearColor::Red;
		case 1: // Blue team wins
		case 4: // Blue team dominating
		case 6: // Blue team takes lead
			return FLinearColor::Blue;
		default:
			return FLinearColor::White;
	}
}

FName UElimPlusVictoryMessage::GetAnnouncementName_Implementation(int32 Switch, const UObject* OptionalObject, const class APlayerState* RelatedPlayerState_1, const class APlayerState* RelatedPlayerState_2) const
{
	switch (Switch)
	{
		case 0:
			return FName(TEXT("RedTeamWinsRound"));
		case 1:
			return FName(TEXT("BlueTeamWinsRound"));
		case 2:
			return FName(TEXT("RoundDraw"));
		case 3:
			return FName(TEXT("RedTeamDominating"));
		case 4:
			return FName(TEXT("BlueTeamDominating"));
		case 5:
			return FName(TEXT("RedTeamTakesLead"));
		case 6:
			return FName(TEXT("BlueTeamTakesLead"));
		default:
			return FName(TEXT("GameMessage"));
	}
}
