// UTHitsoundMessage.h
// LocalMessage that delivers hitsound events to clients.

#pragma once

#include "NetcodePlus.h"
#include "UTLocalMessage.h"
#include "UTHitsoundMessage.generated.h"

/**
 * UUTHitsoundMessage
 *
 * Carries one hit from server to client over UT's localized-message channel.
 *
 * Wire format — everything the client needs is packed into the message switch:
 *     MessageIndex = (Damage << 1) | (bFriendly ? 1 : 0)
 * Damage is clamped to >= 0 before packing so the friendly bit can never be
 * corrupted by a sign-extended shift.
 *
 * OptionalObject is the AClientHitsounds actor itself. Because that actor
 * replicates and is always relevant, it resolves on the receiving client to
 * THAT client's own copy, which holds THAT client's personal config — so no
 * sound preference ever crosses the wire, and custom user sound packs work
 * without the server knowing anything about them.
 *
 * RelatedPlayerState_1 is the attacker, for spectator/demo routing.
 *
 * Nothing here draws to the HUD or announces: Lifetime is 0 and MessageArea is
 * None, so the message exists purely as an audio trigger.
 */
UCLASS()
class NETCODEPLUS_API UUTHitsoundMessage : public UUTLocalMessage
{
    GENERATED_BODY()

public:
    UUTHitsoundMessage(const FObjectInitializer& ObjectInitializer);

    /** Pack a hit into the message switch. */
    static int32 PackSwitch(int32 Damage, bool bFriendly)
    {
        return (FMath::Max(0, Damage) << 1) | (bFriendly ? 1 : 0);
    }

    /** Called on the CLIENT — resolves the cue from local config and plays it. */
    virtual void ClientReceive(const FClientReceiveData& ClientData) const override;

    virtual FText GetText(
        int32 Switch,
        bool bTargetsPlayerState1,
        APlayerState* RelatedPlayerState_1,
        APlayerState* RelatedPlayerState_2,
        UObject* OptionalObject
    ) const override
    {
        return FText::GetEmpty();
    }
};
