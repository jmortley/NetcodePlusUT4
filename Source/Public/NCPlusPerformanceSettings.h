// NCPlusPerformanceSettings.h - cached, client-local rendering preferences.
#pragma once

#include "NetcodePlus.h"

namespace NCPlusPerformanceSettings
{
	/**
	 * Maximum distance at which character overlay meshes (armor, shield and other
	 * stock character overlays) are allowed to render for this client.
	 *
	 * Loaded lazily from [NetcodePlus] CharacterOverlayDistance in Mod.ini.
	 * The persisted value is clamped to 3000..6500; an absent value defaults to
	 * 6500, which preserves the 328 behavior before this setting was exposed.
	 */
	NETCODEPLUS_API float GetCharacterOverlayDistance();

	/** Cached squared form for the character tick's distance comparison. */
	NETCODEPLUS_API float GetCharacterOverlayDistanceSquared();

	/**
	 * Clamp, cache and persist a new client-local overlay distance. The cached
	 * value takes effect immediately; no map restart or config read is required.
	 */
	NETCODEPLUS_API void SetCharacterOverlayDistance(float Distance);

	/** Discard the cache and immediately re-read Mod.ini. */
	NETCODEPLUS_API void Reload();
}
