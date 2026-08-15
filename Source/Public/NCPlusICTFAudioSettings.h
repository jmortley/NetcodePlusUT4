// NCPlusICTFAudioSettings.h - cached, client-local iCTF audio preferences.
#pragma once

#include "NetcodePlus.h"

namespace NCPlusICTFAudioSettings
{
	/**
	 * Whether the local iCTF flag carrier hears the stock looping cloth/flap sound.
	 * Loaded lazily from [InstagibCTF] bPlayFlagCarrierSound in Mod.ini and defaults
	 * to true, preserving stock behavior for clients that have never saved the option.
	 */
	NETCODEPLUS_API bool GetPlayFlagCarrierSound();

	/** Discard the cached preference and immediately re-read Mod.ini. */
	NETCODEPLUS_API void Reload();
}
