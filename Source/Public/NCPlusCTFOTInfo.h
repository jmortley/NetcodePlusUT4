// NCPlusCTFOTInfo.h — tiny replicator carrying the elapsed-time snapshot at
// which CTF entered overtime. Engine's AUTCTFGameState::OvertimeStartTime is
// BlueprintReadOnly but not Replicated, so clients can't read it directly.
// This actor pushes the value once on the authority when OT begins; the HUD
// reads it via TActorIterator to render an OT count-up timer.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCPlusCTFOTInfo.generated.h"

UCLASS(NotPlaceable)
class NETCODEPLUS_API ANCPlusCTFOTInfo : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	/** GS->ElapsedTime captured the moment the gamemode entered OT.
	 *  -1 = OT hasn't started yet (or replication hasn't reached this client). */
	UPROPERTY(Replicated, Transient)
	int32 OvertimeStartElapsed;

	/** GS->ElapsedTime captured the moment EnterAdvantage() fired.
	 *  -1 = not currently in advantage. Cleared on EndOfHalf / intermission /
	 *  OT transition so the HUD doesn't show a stale timer. */
	UPROPERTY(Replicated, Transient)
	int32 AdvantageStartElapsed;

	/** Mirror of the gamemode's bHasHalftime decision (false for 3v3+ etc.).
	 *  Lets the HUD suppress the "1st/2nd Half" label entirely for single-
	 *  period games — without this, clients can't tell halftime is disabled. */
	UPROPERTY(Replicated, Transient)
	bool bHasHalftime;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
};
