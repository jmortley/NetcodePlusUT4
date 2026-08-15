// NCWeaponColorSettings.cpp - one lazy Mod.ini read shared by local weapon effects.
#include "NCWeaponColorSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

namespace
{
	const FLinearColor DefaultHitscanColor(1.f, 0.55f, 0.f, 1.f);
	const FLinearColor DefaultShockColor(0.2f, 0.85f, 1.f, 1.f);
	// Matches NCPLinkGun's existing Blueprint LinearColor default.
	const FLinearColor DefaultLinkColor(0.061f, 0.66f, 0.f, 1.f);

	FNCWeaponColorSnapshot GWeaponColors;
	bool bWeaponColorsLoaded = false;

	bool TryValidateColor(const FLinearColor& Color, const FLinearColor& Fallback,
		FLinearColor& OutColor)
	{
		// Preserve finite HDR RGB values instead of forcing the color-picker's
		// display range. A malformed tuple falls back atomically so effects do not
		// combine unrelated valid and invalid channels.
		if (!FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G)
			|| !FMath::IsFinite(Color.B) || !FMath::IsFinite(Color.A))
		{
			OutColor = Fallback;
			return false;
		}

		OutColor = Color;
		OutColor.A = FMath::Clamp(Color.A, 0.f, 1.f);
		return true;
	}

	FLinearColor ValidateColor(const FLinearColor& Color, const FLinearColor& Fallback)
	{
		FLinearColor Validated;
		TryValidateColor(Color, Fallback, Validated);
		return Validated;
	}

	bool ReadColor(const TCHAR* Key, const FLinearColor& Fallback,
		const FString& ModIniPath, FLinearColor& OutColor)
	{
		OutColor = Fallback;
		FString StoredColor;
		if (GConfig && GConfig->GetString(TEXT("WeaponSkinsPlus"), Key, StoredColor, ModIniPath)
			&& !StoredColor.IsEmpty())
		{
			FLinearColor ParsedColor;
			if (ParsedColor.InitFromString(StoredColor))
			{
				return TryValidateColor(ParsedColor, Fallback, OutColor);
			}
		}

		return false;
	}

	void LoadWeaponColorsOnce()
	{
		if (bWeaponColorsLoaded)
		{
			return;
		}

		const FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
		GWeaponColors.bHasHitscanColor = ReadColor(TEXT("LGColor"), DefaultHitscanColor,
			ModIniPath, GWeaponColors.HitscanColor);
		GWeaponColors.bHasShockColor = ReadColor(TEXT("ShockBeam"), DefaultShockColor,
			ModIniPath, GWeaponColors.ShockColor);
		GWeaponColors.bHasLinkColor = ReadColor(TEXT("LinkBeam"), DefaultLinkColor,
			ModIniPath, GWeaponColors.LinkColor);
		GWeaponColors.bCustomShock = false;
		if (GConfig)
		{
			GConfig->GetBool(TEXT("WeaponSkinsPlus"), TEXT("CustomShockBeam"),
				GWeaponColors.bCustomShock, ModIniPath);
		}
		GWeaponColors.Generation = 1;
		bWeaponColorsLoaded = true;
	}
}

const FNCWeaponColorSnapshot& NCWeaponColors::GetSnapshot()
{
	LoadWeaponColorsOnce();
	return GWeaponColors;
}

void NCWeaponColors::SetFromSelector(const FLinearColor& HitscanColor,
	const FLinearColor& ShockColor, const FLinearColor& LinkColor,
	bool bCustomShock)
{
	LoadWeaponColorsOnce();
	GWeaponColors.HitscanColor = ValidateColor(HitscanColor, DefaultHitscanColor);
	GWeaponColors.ShockColor = ValidateColor(ShockColor, DefaultShockColor);
	GWeaponColors.LinkColor = ValidateColor(LinkColor, DefaultLinkColor);
	GWeaponColors.bCustomShock = bCustomShock;
	GWeaponColors.bHasHitscanColor = true;
	GWeaponColors.bHasShockColor = true;
	GWeaponColors.bHasLinkColor = true;

	// Zero is reserved as the weapon-side "never applied" sentinel.
	++GWeaponColors.Generation;
	if (GWeaponColors.Generation == 0)
	{
		GWeaponColors.Generation = 1;
	}
}
