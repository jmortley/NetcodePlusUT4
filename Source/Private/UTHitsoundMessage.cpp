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
    if (World && World->DemoNetDriver)
    {
        if (World->DemoNetDriver->IsFastForwarding())
        {
            return;
        }

        // POV gate for replay and live-watch playback. The recorded stream
        // deliberately carries EVERY attacker's hits (NotifyDamage's
        // demo-recorder leg) so any POV is watchable later; at playback a
        // hitsound is the WATCHED player's feedback, so only the current view
        // target's hits play. IsPlaying() keeps this off live clients, whose
        // DemoNetDriver is the constantly-recording instant-replay driver;
        // in-server spectators are already filtered server-side by view target.
        if (World->DemoNetDriver->IsPlaying())
        {
            const APawn* ViewedPawn = Cast<APawn>(ClientData.LocalPC->GetViewTarget());
            if (ViewedPawn == nullptr || ViewedPawn->PlayerState == nullptr
                || ViewedPawn->PlayerState != ClientData.RelatedPlayerState_1)
            {
                return;
            }
        }
    }

    // OptionalObject resolves to THIS client's replicated copy of the mutator,
    // which carries this client's own config.
    AClientHitsounds* Mutator = Cast<AClientHitsounds>(ClientData.OptionalObject);
    if (Mutator == nullptr)
    {
        return;
    }

    const bool bIsFriendly = (ClientData.MessageIndex & 0x1) != 0;
    const int32 Damage = ClientData.MessageIndex >> 1;

    // The client already played a predicted hitsound for this hit AND the
    // authoritative pair resolves to the same tier — a pure duplicate. A
    // different tier plays through as the misprediction correction.
    if (Mutator->ShouldSuppressServerHitsound(Damage, bIsFriendly))
    {
        return;
    }

    Mutator->PlayHitsound(Damage, bIsFriendly);
}
