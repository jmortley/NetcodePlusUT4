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
	virtual void NotifyHitBoxClick(FName BoxName) override;
	virtual void Destroyed() override;

	/** Frees/shows the mouse cursor during the attack-order picker. The base
	 *  AWipeoutHUD forces EIM_GameOnly for the whole InProgress match, which captures
	 *  the mouse and hides the cursor the picker's click hitboxes need. */
	virtual EInputMode::Type GetInputMode_Implementation() const override;

protected:
	AClutchRoundState* ResolveClutchState();
	AUTPlayerState* ResolveDisplayedPlayerState(AClutchRoundState* State) const;
	void LoadRecoveredHUDTextures();
	void DrawClutchPortraits(AClutchRoundState* State);
	void DrawRolePanel(AClutchRoundState* State);
	void DrawDefenderAttackerPanel(AClutchRoundState* State);
	void DrawCapturePanel(AClutchRoundState* State);
	void DrawAttackOrderPanel(AClutchRoundState* State);
	void UpdateAttackOrderInput(AClutchRoundState* State, bool bPanelVisible);
	void RestoreAttackOrderInput();
	void ResetAttackOrderDraft(AClutchRoundState* State, uint8 TeamIndex);
	void PickAttackOrderSlot(uint8 RosterSlot);
	void SubmitAttackOrderDraft();
	void UpdateEliminatedCameraRestriction(AClutchRoundState* State);
	void RestoreSpectatorCameraPreference();

	TWeakObjectPtr<AClutchRoundState> CachedClutchState;
	bool bTriedLoadRecoveredHUDTextures;
	bool bAttackOrderInputActive;
	bool bSavedShowMouseCursor;
	bool bSavedEnableClickEvents;
	bool bSavedEnableMouseOverEvents;
	bool bAttackOrderSubmitted;
	bool bEliminatedCameraForced;
	bool bSavedSpectateBehindView;
	/** When the confirm was sent. The submit latch is provisional: if the replicated
	 *  team lock has not appeared shortly after, the server rejected the pick (stale
	 *  lock, roster change) and the picker re-opens instead of dying silently. */
	float AttackOrderSubmitTime;
	uint8 DraftAttackOrderTeam;
	FString DraftAttackOrderRosterKey;
	TArray<uint8> DraftAttackOrderSlots;

	UPROPERTY()
	UTexture2D* LegacyCircleTexture;

	UPROPERTY()
	UTexture2D* LegacyInnerCircleTexture;

	UPROPERTY()
	UTexture2D* LegacyAttackingTexture;

	UPROPERTY()
	UTexture2D* LegacyLeftGadgetTexture;

	UPROPERTY()
	UTexture2D* LegacyRightGadgetTexture;

	UPROPERTY()
	UTexture2D* LegacyShieldTexture;

	UPROPERTY()
	UTexture2D* LegacyInstaTexture;

	UPROPERTY()
	UTexture2D* LegacyRocketTexture;

	UPROPERTY()
	UTexture2D* LegacyAttackRailBackTexture;

	UPROPERTY()
	UTexture2D* LegacyAttackRailFrontTexture;

	UPROPERTY()
	UTexture2D* LegacyAttackProgressTexture;

	UPROPERTY()
	UTexture2D* LegacyDefendRailBackTexture;

	UPROPERTY()
	UTexture2D* LegacyDefendRailFrontTexture;

	UPROPERTY()
	UTexture2D* LegacyDefendProgressTexture;

	/** Exact 2560x1440 transparent header recovered from ClutchMini. */
	UPROPERTY()
	UTexture2D* LegacyTopFrameTexture;
};
