// Client-local aim trainer controls, saved independently of normal match binds.
#pragma once

#include "NetcodePlus.h"
#include "InputCoreTypes.h"

namespace NCAimTrainerInputSettings
{
	/** Mod.ini [NCAimTrainer] RestartKey. Missing/invalid values use R; None unbinds.
	 *  If R is configured as a console key, the fallback is unbound instead. */
	NETCODEPLUS_API FKey GetRestartKey();

	/** Allow a single key/button or EKeys::Invalid (unbound), retaining menu/fire keys. */
	NETCODEPLUS_API bool IsAllowedRestartKey(FKey Key);

	/** Persist an allowed key locally. Returns false without changing an invalid bind. */
	NETCODEPLUS_API bool SetRestartKey(FKey Key);
}
