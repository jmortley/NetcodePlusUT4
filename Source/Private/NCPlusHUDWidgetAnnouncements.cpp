// NCPlusHUDWidgetAnnouncements.cpp - see header.
#include "NCPlusHUDWidgetAnnouncements.h"

#include "NCPlusDisplaySettings.h"
#include "UTKillerMessage.h"
#include "UTLocalMessage.h"

UNCPlusHUDWidgetAnnouncements::UNCPlusHUDWidgetAnnouncements(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UNCPlusHUDWidgetAnnouncements::AddMessage(int32 QueueIndex, TSubclassOf<class UUTLocalMessage> MessageClass, uint32 MessageIndex, FText LocalMessageText, int32 MessageCount, APlayerState* RelatedPlayerState_1, APlayerState* RelatedPlayerState_2, UObject* OptionalObject)
{
	// Partially-unique classes can be re-slotted after ReceiveLocalMessage captured the
	// combined text, which would break the continuation test below: leave them stock.
	// (Stock UUTKillerMessage is plain bIsUnique.)
	const UClass* KillerClass = MessageClass;
	if (KillerClass == nullptr || !KillerClass->IsChildOf(UUTKillerMessage::StaticClass())
		|| GetDefault<UUTLocalMessage>(MessageClass)->bIsPartiallyUnique)
	{
		Super::AddMessage(QueueIndex, MessageClass, MessageIndex, LocalMessageText, MessageCount, RelatedPlayerState_1, RelatedPlayerState_2, OptionalObject);
		return;
	}

	// ReceiveLocalMessage sets CombinedEmphasisText to the live entry's EmphasisText
	// just before this call when the message continues an entry of this exact class,
	// and clears it otherwise. That is the same test stock AddMessage combines on.
	const bool bContinuation = !CombinedEmphasisText.IsEmpty();
	if (!bContinuation)
	{
		KillChain = FNCPlusKillChain();
		KillChain.MessageClass = KillerClass;
		KillChain.bTracked = true;
	}
	else if (!KillChain.bTracked || KillChain.MessageClass != KillerClass
		|| !CombinedEmphasisText.ToString().Equals(KillChain.LastEmphasis, ESearchCase::CaseSensitive))
	{
		// Continuing an entry this record did not build: its names are unknown, so it
		// stays stock until the entry expires (no killer message for its lifetime) and
		// a fresh chain starts. Kills that keep refreshing it keep it stock.
		KillChain.bTracked = false;
	}

	Super::AddMessage(QueueIndex, MessageClass, MessageIndex, LocalMessageText, MessageCount, RelatedPlayerState_1, RelatedPlayerState_2, OptionalObject);

	if (!KillChain.bTracked)
	{
		return;
	}
	// Stock returns without writing the entry when it has no HUD owner; a continuation's
	// old entry would still match on class, so test the owner itself. A missing victim
	// makes stock combine an empty name into the entry, which the record can't represent.
	if (UTHUDOwner == nullptr || !MessageQueue.IsValidIndex(QueueIndex)
		|| MessageQueue[QueueIndex].MessageClass != MessageClass || RelatedPlayerState_2 == nullptr)
	{
		KillChain.bTracked = false;
		return;
	}

	const bool bKnownVictim = KillChain.Victims.ContainsByPredicate([RelatedPlayerState_2](const FNCPlusKillVictim& Victim)
	{
		return Victim.PlayerState.Get() == RelatedPlayerState_2;
	});
	if (!bKnownVictim)
	{
		FNCPlusKillVictim& Victim = KillChain.Victims[KillChain.Victims.AddDefaulted()];
		Victim.PlayerState = RelatedPlayerState_2;
		Victim.PlayerName = RelatedPlayerState_2->GetPlayerName();
	}

	FLocalizedMessageData& Entry = MessageQueue[QueueIndex];
	if (bContinuation && NCPlusDisplaySettings::GetCollapseRepeatedKillNames())
	{
		// Same fold stock performs one kill at a time, over unique victims only.
		// Stock's prefix (CombinePrefixText) and postfix stay as Super left them.
		const UUTLocalMessage* DefaultMessage = GetDefault<UUTLocalMessage>(MessageClass);
		FText Emphasis = FText::FromString(KillChain.Victims[0].PlayerName);
		for (int32 i = 1; i < KillChain.Victims.Num(); i++)
		{
			Emphasis = DefaultMessage->CombineEmphasisText(CombinedMessageIndex, Emphasis, FText::FromString(KillChain.Victims[i].PlayerName));
		}
		Entry.EmphasisText = Emphasis;
		// DrawMessage centres on the width of Text but draws prefix + emphasis +
		// postfix; stock's CombineText over-measures. Measure what is drawn.
		Entry.Text = FText::FromString(Entry.PrefixText.ToString() + Entry.EmphasisText.ToString() + Entry.PostfixText.ToString());
	}
	KillChain.LastEmphasis = Entry.EmphasisText.ToString();
}
