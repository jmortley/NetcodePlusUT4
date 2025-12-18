// UTHitsoundMessage.h
// LocalMessage for delivering hitsound events to clients

#pragma once

#include "NetcodePlus.h"
#include "UTLocalMessage.h"
#include "UTHitsoundMessage.generated.h"

/**
 * Message switches for hitsound types
 */
UENUM(BlueprintType)
enum class EHitsoundMessageType : uint8
{
    Enemy = 0,
    Friendly = 1
};

/**
 * UTHitsoundMessage
 * 
 * Uses UT4's built-in LocalMessage system to deliver hitsound events.
 * Server calls ClientReceiveLocalizedMessage, client receives and plays sound.
 * 
 * Switch: 0 = Enemy hit, 1 = Friendly hit
 * OptionalObject: The MutHitsounds actor (to access config)
 * RelatedPlayerState_1: Attacker (for spectator routing)
 * Value: Damage amount (packed into MessageIndex or use OptionalValue if available)
 */
UCLASS()
class NETCODEPLUS_API UUTHitsoundMessage : public UUTLocalMessage
{
    GENERATED_BODY()

public:
    UUTHitsoundMessage(const FObjectInitializer& ObjectInitializer);

    /**
     * Called on the CLIENT when receiving this message.
     * This is where we play the actual hitsound.
     */
    virtual void ClientReceive(
        const FClientReceiveData& ClientData
    ) const override;



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


    /**
     * Precache the sounds on client
     */
    //virtual void PrecacheAnnouncements(class UUTAnnouncer* Announcer) const override;
};
