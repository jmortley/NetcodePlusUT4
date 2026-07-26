// UTHitsoundMessage.cpp
// LocalMessage that delivers hitsound events to clients.

#include "UTHitsoundMessage.h"
#include "ClientHitsounds.h"
#include "UTPlayerController.h"
#include "Engine/World.h"
#include "Engine/DemoNetDriver.h"
#include "Kismet/GameplayStatics.h"

UUTHitsoundMessage::UUTHitsoundMessage(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // Audio-only: never announced, never drawn.
    bIsStatusAnnouncement = false;
    bIsPartiallyUnique = false;
    Lifetime = 0.0f;
    MessageArea = FName(TEXT("None"));

    // A killcam re-runs recorded messages; replaying the victim's hitsounds
    // over the kill replay is noise, not feedback.
    bPlayDuringInstantReplay = false;
}

void UUTHitsoundMessage::ClientReceive(const FClientReceiveData& ClientData) const
{
    // Deliberately NOT calling Super: the base implementation drives the
    // announcer and HUD message queue, neither of which applies to an
    // audio-only trigger. That means we must reproduce its demo guard here —
    // without it, seeking or fast-forwarding a demo bursts every hitsound
    // recorded in the skipped span at once.
    if (ClientData.LocalPC == nullptr)
    {
        return;
    }
    const UWorld* World = ClientData.LocalPC->GetWorld();
    if (World && World->DemoNetDriver && World->DemoNetDriver->IsFastForwarding())
    {
        return;
    }

    // OptionalObject resolves to THIS client's replicated copy of the mutator,
    // which carries this client's own config.
    AClientHitsounds* Mutator = Cast<AClientHitsounds>(ClientData.OptionalObject);
    if (Mutator == nullptr)
    {
        return;
    }

    // The client already played a predicted hitsound for this hit.
    if (Mutator->ShouldSuppressServerHitsound())
    {
        return;
    }

    const bool bIsFriendly = (ClientData.MessageIndex & 0x1) != 0;
    const int32 Damage = ClientData.MessageIndex >> 1;

    Mutator->PlayHitsound(Damage, bIsFriendly);
}
