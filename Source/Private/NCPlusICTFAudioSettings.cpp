// NCPlusICTFAudioSettings.cpp - cached, client-local iCTF audio preferences.
#include "NCPlusICTFAudioSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

namespace
{
	// -1 = not loaded, 0 = disabled, 1 = enabled.
	int8 GPlayFlagCarrierSound = -1;

	FString GetICTFAudioModIniPath()
	{
		return FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	}

	void LoadPlayFlagCarrierSound()
	{
		if (GPlayFlagCarrierSound >= 0 || GConfig == nullptr)
		{
			return;
		}

		bool bPlaySound = true;
		GConfig->GetBool(
			TEXT("InstagibCTF"),
			TEXT("bPlayFlagCarrierSound"),
			bPlaySound,
			GetICTFAudioModIniPath());
		GPlayFlagCarrierSound = bPlaySound ? 1 : 0;
	}
}

bool NCPlusICTFAudioSettings::GetPlayFlagCarrierSound()
{
	LoadPlayFlagCarrierSound();
	return GPlayFlagCarrierSound != 0;
}

void NCPlusICTFAudioSettings::Reload()
{
	GPlayFlagCarrierSound = -1;
	LoadPlayFlagCarrierSound();
}
