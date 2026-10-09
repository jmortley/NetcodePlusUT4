// NCPlusWeaponPresentation.cpp - client-only weapon HUD drawing shared by NetcodePlus weapons.
#include "NCPlusWeaponPresentation.h"

#include "UTWeapon.h"
#include "UTWeaponStateFiring.h"
#include "UTHUD.h"
#include "UTHUDWidget.h"
#include "UTCrosshair.h"
#include "UTPlayerState.h"

void NCPlusWeaponPresentation::DrawWeaponCrosshairWithoutFriendlySign(AUTWeapon* Weapon, UUTHUDWidget* WeaponHudWidget, float RenderDelta)
{
	// Mirrors AUTWeapon::DrawWeaponCrosshair_Implementation line for line; keep the
	// two in sync if the stock body ever changes.
	bool bDrawCrosshair = true;
	for (int32 i = 0; i < Weapon->FiringState.Num(); i++)
	{
		bDrawCrosshair = Weapon->FiringState[i]->DrawHUD(WeaponHudWidget) && bDrawCrosshair;
	}

	if (bDrawCrosshair)
	{
		Weapon->ActiveCrosshair = WeaponHudWidget->UTHUDOwner->GetCrosshairForWeapon(Weapon->WeaponCustomizationTag, Weapon->ActiveCrosshairCustomizationInfo);
		if (Weapon->ActiveCrosshair != nullptr)
		{
			Weapon->ActiveCrosshair->NativeDrawCrosshair(WeaponHudWidget->UTHUDOwner, WeaponHudWidget->GetCanvas(), Weapon, RenderDelta, Weapon->ActiveCrosshairCustomizationInfo);
		}
		else
		{
			// fall back crosshair
			UTexture2D* CrosshairTexture = WeaponHudWidget->UTHUDOwner->DefaultCrosshairTex;
			if (CrosshairTexture != NULL)
			{
				float W = CrosshairTexture->GetSurfaceWidth();
				float H = CrosshairTexture->GetSurfaceHeight();
				float CrosshairScale = WeaponHudWidget->UTHUDOwner->GetCrosshairScale();

				WeaponHudWidget->DrawTexture(CrosshairTexture, 0, 0, W * CrosshairScale, H * CrosshairScale, 0.0, 0.0, 16, 16, 1.0, WeaponHudWidget->UTHUDOwner->GetCrosshairColor(FLinearColor::White), FVector2D(0.5f, 0.5f));
			}
		}

		// Friendly target: stock draws the HUDAtlas sign here. Draw nothing, and still
		// skip UpdateCrosshairTarget exactly as stock does, so the crosshair name stays
		// enemy-only.
		AUTPlayerState* PS = nullptr;
		if (!Weapon->ShouldDrawFFIndicator(WeaponHudWidget->UTHUDOwner->PlayerOwner, PS))
		{
			Weapon->UpdateCrosshairTarget(PS, WeaponHudWidget, RenderDelta);
		}
	}
}
