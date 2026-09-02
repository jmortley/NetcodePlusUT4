// NCPlusHUDWidget_Accuracy — live shaft accuracy readout. Used by NCShaftArena
// (and trivially droppable into other modes via the nchud editor under the
// "accuracy" alias). Draws "84%" big and centered; reads NAME_LinkHits /
// NAME_LinkShots off the local AUTPlayerState. Formatting and stat lookup are
// cached; rendering still occurs each frame so the Canvas content persists.
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
	uint32 CachedLayoutRevision;
	bool bCachedLayoutPresent;
	bool bCachedLayoutHidden;
	bool bCachedPinnedMode;
	float CachedElementScale;
	float CachedOpacity;
	FName CachedPinnedHitsStat;
	FName CachedPinnedShotsStat;
	FText CachedSmallLabel;
	FString CachedSmallLabelString;

	UPROPERTY(Transient)
	class UFont* CachedBigFont;
	UPROPERTY(Transient)
	class UFont* CachedSmallFont;

	TWeakObjectPtr<class AUTWeapon> CachedSourceWeapon;
	TWeakObjectPtr<class AUTPlayerState> CachedPlayerState;
	FName CachedHitsStat;
	FName CachedShotsStat;
	FString CachedPlayerId;
	bool bCachedPlayerIdIsUnique;
	float NextStatsRefreshTime;
	int32 CachedHits;
	int32 CachedShots;
	bool bCachedDisplayValid;
	FText CachedPercentText;
	FText CachedSubText;
	FString CachedPercentString;
	FString CachedSubString;
	FLinearColor CachedPercentColor;
	struct FTextMeasureCache
	{
		class UFont* Font = nullptr;
		FVector2D Size = FVector2D::ZeroVector;
	};
	FTextMeasureCache CachedLabelMeasure;
	FTextMeasureCache CachedPercentMeasure;
	FTextMeasureCache CachedSubMeasure;

	/** TActorIterator-cached lookup of the per-weapon stats replicator. Weak ptr
	 *  so it survives match restarts (replicator gets re-spawned, weak ref goes
	 *  stale, next Draw() walks the iterator again and re-caches). */
	UPROPERTY(Transient)
	mutable TWeakObjectPtr<ANCAccuracyStatsReplicator> CachedAccuracyReplicator;

	void RefreshLayoutCache();
	void RefreshDisplayCache(class AUTPlayerState* PS, FName HitsStat, FName ShotsStat, float Now);
	void DrawCachedText(const FText& Text, const FString& String, FTextMeasureCache& Measure,
		float X, float Y, class UFont* Font, float TextScale, float DrawOpacity,
		const FLinearColor& DrawColor);
	ANCAccuracyStatsReplicator* GetAccuracyReplicator() const;
};
