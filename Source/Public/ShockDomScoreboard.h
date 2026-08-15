// ShockDomScoreboard — DOM-specific scoreboard with Captures column
#pragma once
#include "NetcodePlus.h"
#include "WipeoutScoreboard.h"
#include "ShockDomScoreboard.generated.h"

class AShockDomReplicator;

UCLASS()
class NETCODEPLUS_API UShockDomScoreboard : public UWipeoutScoreboard
{
	GENERATED_UCLASS_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scoreboard")
	FText CH_Captures;

protected:
	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor) override;

private:
	UPROPERTY(Transient)
	TWeakObjectPtr<AShockDomReplicator> CachedDomReplicator;
	TWeakObjectPtr<UWorld> CachedDomReplicatorWorld;
	float NextDomReplicatorSearchTime = 0.f;

	AShockDomReplicator* FindDomReplicator();
};
