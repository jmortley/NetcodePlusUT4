// NCPlusHUDWidget_Accuracy — live shaft accuracy readout. Used by NCShaftArena
// (and trivially droppable into other modes via the nchud editor under the
// "accuracy" alias). Draws "84%" big and centered; reads NAME_LinkHits /
// NAME_LinkShots off the local AUTPlayerState every frame.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_Accuracy.generated.h"

class ANCAccuracyStatsReplicator;

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_Accuracy : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;

private:
	/** TActorIterator-cached lookup of the per-weapon stats replicator. Weak ptr
	 *  so it survives match restarts (replicator gets re-spawned, weak ref goes
	 *  stale, next Draw() walks the iterator again and re-caches). */
	UPROPERTY(Transient)
	mutable TWeakObjectPtr<ANCAccuracyStatsReplicator> CachedAccuracyReplicator;

	ANCAccuracyStatsReplicator* GetAccuracyReplicator() const;
};
