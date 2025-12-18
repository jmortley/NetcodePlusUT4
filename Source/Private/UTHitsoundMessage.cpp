// UTHitsoundMessage.cpp
// LocalMessage for delivering hitsound events to clients

#include "UTHitsoundMessage.h"
#include "MutHitsounds.h"
#include "UTPlayerController.h"
#include "UTAnnouncer.h"
#include "Kismet/GameplayStatics.h"

UUTHitsoundMessage::UUTHitsoundMessage(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // This message is handled entirely in code, no announcer
    bIsStatusAnnouncement = false;
    bIsPartiallyUnique = false;
    Lifetime = 0.0f; // Don't display on HUD
    MessageArea = FName(TEXT("None"));
}

void UUTHitsoundMessage::ClientReceive(const FClientReceiveData& ClientData) const
{
    // ClientData.MessageIndex = Switch (0 = Enemy, 1 = Friendly)
    // ClientData.OptionalObject = MutHitsounds actor
    // ClientData.RelatedPlayerState_1 = Attacker PlayerState
    // We pack damage into the upper bits of MessageIndex or use a custom approach
    
    // Get the mutator from OptionalObject
    AMutHitsounds* Mutator = Cast<AMutHitsounds>(ClientData.OptionalObject);
    if (!Mutator)
    {
        return;
    }

    // Unpack the message
    // Switch 0 = Enemy, Switch 1 = Friendly
    bool bIsFriendly = (ClientData.MessageIndex & 0x1) != 0;
    
    // Damage is packed in upper bits (shift by 1)
    int32 Damage = ClientData.MessageIndex >> 1;
    
    // Get the appropriate hitsound preset
    const FHitsoundsConfig& Config = Mutator->GetConfig();
    const FHitsound& HitsoundPreset = bIsFriendly ? Config.Friendly : Config.Enemy;

    // Check if we should play zero damage friendly hits
    if (bIsFriendly && Damage == 0 && !Config.bPlayZeroFriendly)
    {
        return;
    }

    // Select sound based on damage
    USoundBase* SoundToPlay = Mutator->SelectSoundForDamage(HitsoundPreset, Damage);
    if (SoundToPlay)
    {
        float FinalVolume = HitsoundPreset.Volume * Config.UserMultiplier;
        UGameplayStatics::PlaySound2D(Mutator, SoundToPlay, FinalVolume, HitsoundPreset.Pitch);
    }
}

