// NCPlusHUDWidget_CTFFlagStatus - nchud-controllable subclass of the engine
// CTF flag status widget.
//
// Overrides the engine's CTF indicator draws so they consult FNCPlusHUDLayout:
//
//   1. DrawFlagWorld + DrawFlagBaseWorld
//      One Hide toggle controls both world-projected flag indicators: the icon
//      over the carrier and the missing-flag exclamation at the base. The legacy
//      alias key `ctf_carrier_indicator` is retained so existing layouts migrate
//      automatically. Scale, offset and opacity continue to affect the carrier
//      indicator only.
//
//   2. DrawStatusMessage
//      Controls TWO independent banner texts:
//        * `ctf_you_have_flag`   yellow pulse, "You have the flag!" (engine
//                                already drew this; we just make it nchud-positionable)
//        * `ctf_enemy_has_flag`  red pulse,    "The enemy has your flag,
//                                recover it!" (NEW - engine has the FText defined
//                                but the draw was missing; UT99-style alarm for
//                                audio-off players)
//      Per banner: scale, offset_x/y, hidden, opacity, color_text override.
//      First-appear scale-pop-in (1.5x to 1.0x over 200ms, quad ease-out).
#pragma once

#include "NetcodePlus.h"
#include "UTHUDWidget_CTFFlagStatus.h"
#include "NCPlusHUDWidget_CTFFlagStatus.generated.h"

UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_CTFFlagStatus : public UUTHUDWidget_CTFFlagStatus
{
	GENERATED_UCLASS_BODY()

	/** Render the world-projected carrier indicator. Re-implements the parent's
	 *  body with layout overrides applied (parent path is non-trivial to delegate
	 *  to without sacrificing per-element control). */
	virtual void DrawFlagWorld(class AUTCTFGameState* GameState, FVector PlayerViewPoint,
		FRotator PlayerViewRotation, uint8 TeamNum, class AUTCTFFlagBase* FlagBase,
		class AUTFlag* Flag, class AUTPlayerState* FlagHolder) override;

	/** Gate the missing-flag/base exclamation on the same legacy alias as the
	 *  carrier indicator, giving nchud one "CTF World Indicators" Hide toggle. */
	virtual void DrawFlagBaseWorld(class AUTCTFGameState* GameState, FVector PlayerViewPoint,
		FRotator PlayerViewRotation, uint8 TeamNum, class AUTCTFFlagBase* FlagBase,
		class AUTFlag* Flag, class AUTPlayerState* FlagHolder) override;

	/** Replaces the engine version that only ever rendered YouHaveFlagText.
	 *  Adds the missing EnemyHasFlagText branch and routes each through its
	 *  own nchud alias. */
	virtual void DrawStatusMessage(float DeltaTime) override;

	/** Top-of-screen flag silhouettes. Overridden so the `ctf_flag_status` alias
	 *  hides ONLY the silhouettes — the carrier indicator and banners live on the
	 *  same widget and must keep drawing when the silhouettes are hidden. */
	virtual void DrawFlagStatus(class AUTCTFGameState* GameState, FVector PlayerViewPoint,
		FRotator PlayerViewRotation, uint8 TeamNum, FVector2D IndicatorPosition,
		class AUTCTFFlagBase* FlagBase, class AUTFlag* Flag, class AUTPlayerState* FlagHolder) override;

protected:
	/** Pulse alpha for the red enemy-has-flag banner. Tracked separately from
	 *  the parent's AnimationAlpha (which drives the yellow banner) so the
	 *  two banners can be on screen at the same time without phase-locking. */
	UPROPERTY()
	float EnemyAnimationAlpha;

	/** GetTimeSeconds() at which each banner first appeared in the current
	 *  uninterrupted "on" cycle. -1 when the banner is off. Drives the pop-in
	 *  animation; resets to -1 when the banner condition goes false so the
	 *  pop fires again next time. */
	UPROPERTY()
	float YouHaveFlagAppearedAt;

	UPROPERTY()
	float EnemyHasFlagAppearedAt;

	/** Compute the pop-in scale multiplier given the appeared-at timestamp.
	 *  Returns 1.5 at t=AppearedAt, eases to 1.0 over 200ms (quad ease-out),
	 *  stays at 1.0 thereafter. Returns 1.0 if AppearedAt < 0. */
	float ComputePopInScale(float AppearedAt) const;

	/** Internal helper used by DrawStatusMessage to render one banner with
	 *  layout-driven position/scale/color/opacity. Caller picks the alias,
	 *  text, color, and pulse alpha; this handles bScaleByDesignedResolution
	 *  toggling, font-size sizing, position-anchor math, and save/restore of
	 *  the inherited FlagStatusText state. */
	void DrawOneBanner(FName Alias, const FText& Text, const FLinearColor& DefaultColor,
		float PulseAlpha, float PopInScale);
};
