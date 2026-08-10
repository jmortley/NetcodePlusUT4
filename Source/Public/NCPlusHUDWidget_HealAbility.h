// NCPlusHUDWidget_HealAbility — Diabotical-style icon for the player's
// heal/boost ability. Shows the icon + the bound key, greys out once the
// charge has been consumed (RemainingBoosts == 0).
//
// Engages with stock AUTPlayerState::BoostClass + RemainingBoosts so it
// works on any mode that uses the boost system; in NetcodePlus the primary
// consumer is Wipeout (heal/shield ability granted on spawn).
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_HealAbility.generated.h"

class UPlayerInput;

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_HealAbility : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;
	virtual float GetDrawScaleOverride() override;

private:
	/** Input mappings change rarely, so keep the resolved label between draws and
	 *  resample once per second to support live rebinding without render-rate scans. */
	TWeakObjectPtr<UWorld> CachedBindingWorld;
	TWeakObjectPtr<UPlayerInput> CachedBindingInput;
	uint32 CachedBindingLayoutRevision;
	float NextBindingRefreshTime;
	FString CachedBindOverride;
	FText CachedDisplayLabel;
};
