// NCWeaponColorSettings.h - process-local weapon effect color snapshot.
#pragma once

#include "NetcodePlus.h"

/**
 * Validated client-side weapon colors loaded from GeneratedConfig/Mod.ini.
 * Generation changes only when the F5 selector applies a new snapshot, so
 * weapon effects can cheaply skip work until the local selection changes.
 */
struct NETCODEPLUS_API FNCWeaponColorSnapshot
{
	FLinearColor HitscanColor;
	FLinearColor ShockColor;
	FLinearColor LinkColor;
	bool bCustomShock;
	/** False keeps fresh installs on the stock effect until the legacy key is saved. */
	bool bHasHitscanColor;
	bool bHasShockColor;
	bool bHasLinkColor;
	uint32 Generation;
};

namespace NCWeaponColors
{
	/** Lazily load and validate the process-local snapshot once. */
	NETCODEPLUS_API const FNCWeaponColorSnapshot& GetSnapshot();

	/**
	 * Publish colors saved by the local F5 selector without re-reading Mod.ini.
	 * Values are validated identically to the lazy load and Generation advances.
	 */
	NETCODEPLUS_API void SetFromSelector(const FLinearColor& HitscanColor,
		const FLinearColor& ShockColor, const FLinearColor& LinkColor,
		bool bCustomShock);
}
