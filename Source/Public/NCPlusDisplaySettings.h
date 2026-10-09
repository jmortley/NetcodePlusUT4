// NCPlusDisplaySettings.h - cached, client-local HUD display preferences.
#pragma once

#include "NetcodePlus.h"

/**
 * Opt-in HUD presentation toggles stored in [NetcodePlus] in Mod.ini. Every value
 * defaults to false, which is exactly the stock presentation.
 *
 * Mod.ini is read only by Reload() (client module startup) and written by the
 * setters (F5 menu Save, nchud editor). Render and message paths only read the
 * cached bools, never the config cache. If the config cache is unavailable the
 * values stay false.
 */
namespace NCPlusDisplaySettings
{
	/** Hide the green "forbidden" sign drawn on the crosshair while aiming at a
	 *  teammate. Enemy names under the crosshair are unaffected. */
	NETCODEPLUS_API bool GetHideFriendlyCrosshairSign();

	/** Hide the overhead beacon (name, health/armor bars, combat indicator) that
	 *  stock draws above visible teammates while playing. */
	NETCODEPLUS_API bool GetHideTeammateOverheadTags();

	/** List each victim once in a combined "You killed ..." announcement. */
	NETCODEPLUS_API bool GetCollapseRepeatedKillNames();

	/** Cache, persist and flush one value. Takes effect on the next frame/message. */
	NETCODEPLUS_API void SetHideFriendlyCrosshairSign(bool bHide);
	NETCODEPLUS_API void SetHideTeammateOverheadTags(bool bHide);
	NETCODEPLUS_API void SetCollapseRepeatedKillNames(bool bCollapse);

	/** Discard the cached values and re-read Mod.ini. */
	NETCODEPLUS_API void Reload();
}
