// NCPlusWeaponPresentation.h - client-only weapon HUD drawing shared by NetcodePlus
// weapons that cannot share a native parent (AUTWeaponFix and AUTWeap_Enforcer_Plus).
#pragma once

#include "NetcodePlus.h"

class AUTWeapon;
class UUTHUDWidget;

namespace NCPlusWeaponPresentation
{
	/**
	 * AUTWeapon::DrawWeaponCrosshair_Implementation (UTWeapon.cpp) with only the
	 * teammate "forbidden" sign removed. Firing-state DrawHUD calls, the active or
	 * fallback crosshair, and the single virtual ShouldDrawFFIndicator call are kept
	 * in stock order. On a friendly result nothing is drawn and UpdateCrosshairTarget
	 * is still skipped, so no teammate name appears under the crosshair.
	 *
	 * Used only while NCPlusDisplaySettings::GetHideFriendlyCrosshairSign() is true;
	 * otherwise callers delegate to their Super implementation.
	 */
	void DrawWeaponCrosshairWithoutFriendlySign(AUTWeapon* Weapon, UUTHUDWidget* WeaponHudWidget, float RenderDelta);
}
