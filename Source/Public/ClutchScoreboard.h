#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTTeamScoreboard.h"
#include "ClutchScoreboard.generated.h"

class AClutchRoundState;

/** Team scoreboard with Clutch role/status and attacker armor columns. */
UCLASS()
class NETCODEPLUS_API UClutchScoreboard : public UUTTeamScoreboard
{
	GENERATED_UCLASS_BODY()

public:
	virtual FLinearColor GetPlayerColorFor(AUTPlayerState* PlayerState) const override;

protected:
	virtual void DrawTeamPanel(float RenderDelta, float& YOffset) override;
	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset,
		float YOffset, float Width, FLinearColor DrawColor) override;
	virtual void DrawReadyText(AUTPlayerState* PlayerState, float XOffset,
		float YOffset, float Width) override;

	AClutchRoundState* ResolveClutchState() const;

private:
	UPROPERTY(Transient)
	mutable TWeakObjectPtr<AClutchRoundState> CachedClutchState;
	mutable TWeakObjectPtr<UWorld> CachedClutchStateWorld;
	mutable float NextClutchStateSearchTime = 0.f;
};
