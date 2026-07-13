#pragma once

#include "NetcodePlus.h"
#include "WipeoutHUD.h"
#include "ClutchHUD.generated.h"

class AClutchRoundState;
class AUTGameState;
class AUTPlayerState;
class UTexture2D;

/**
 * Native Clutch HUD.
 *
 * Reuses NetcodePlus's Wipeout widget stack (crosshair, messages, weapon bar,
 * spectator support and layout integration) while replacing its round display
 * with Clutch role, armor, timer and capture information.
 */
UCLASS()
class NETCODEPLUS_API AClutchHUD : public AWipeoutHUD
{
	GENERATED_UCLASS_BODY()

public:
	virtual void DrawHUD() override;
	virtual void DrawTeamScoreBar(AUTGameState* GameState) override;

protected:
	AClutchRoundState* ResolveClutchState();
	AUTPlayerState* ResolveDisplayedPlayerState(AClutchRoundState* State) const;
	void LoadRecoveredHUDTextures();
	void DrawRolePanel(AClutchRoundState* State);
	void DrawCapturePanel(AClutchRoundState* State);

	TWeakObjectPtr<AClutchRoundState> CachedClutchState;
	bool bTriedLoadRecoveredHUDTextures;

	UPROPERTY()
	UTexture2D* RecoveredBaseTexture;

	UPROPERTY()
	UTexture2D* RecoveredKnobTexture;

	UPROPERTY()
	UTexture2D* RecoveredRocketTexture;

	UPROPERTY()
	UTexture2D* WeaponIconAtlas;
};
