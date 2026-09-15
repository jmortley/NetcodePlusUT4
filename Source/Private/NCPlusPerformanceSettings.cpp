// NCPlusPerformanceSettings.cpp - cached, client-local rendering preferences.
#include "NCPlusPerformanceSettings.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

namespace
{
	const float CharacterOverlayDistanceMin = 3000.f;
	const float CharacterOverlayDistanceMax = 6500.f;
	const float CharacterOverlayDistanceDefault = 6500.f;

	bool bCharacterOverlayDistanceLoaded = false;
	float CharacterOverlayDistance = CharacterOverlayDistanceDefault;
	float CharacterOverlayDistanceSquared =
		CharacterOverlayDistanceDefault * CharacterOverlayDistanceDefault;
	// -1 = not loaded, 0 = disabled, 1 = enabled.
	int8 ShowDeathBlood = -1;

	FString GetModIniPath()
	{
		return FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	}

	float SanitizeCharacterOverlayDistance(float Distance)
	{
		return FMath::IsFinite(Distance)
			? FMath::Clamp(Distance, CharacterOverlayDistanceMin, CharacterOverlayDistanceMax)
			: CharacterOverlayDistanceDefault;
	}

	void PublishCharacterOverlayDistance(float Distance)
	{
		CharacterOverlayDistance = SanitizeCharacterOverlayDistance(Distance);
		CharacterOverlayDistanceSquared = CharacterOverlayDistance * CharacterOverlayDistance;
		bCharacterOverlayDistanceLoaded = true;
	}

	void LoadCharacterOverlayDistance()
	{
		if (bCharacterOverlayDistanceLoaded)
		{
			return;
		}

		// A very early caller should receive the initialized default without
		// permanently suppressing the real read once the config cache is ready.
		if (GConfig == nullptr)
		{
			return;
		}

		float StoredDistance = CharacterOverlayDistanceDefault;
		GConfig->GetFloat(
			TEXT("NetcodePlus"),
			TEXT("CharacterOverlayDistance"),
			StoredDistance,
			GetModIniPath());

		PublishCharacterOverlayDistance(StoredDistance);
	}

	void LoadShowDeathBlood()
	{
		if (ShowDeathBlood >= 0 || GConfig == nullptr)
		{
			return;
		}

		bool bShowBlood = true;
		GConfig->GetBool(TEXT("InstagibCTF"), TEXT("bShowDeathBlood"), bShowBlood, GetModIniPath());
		ShowDeathBlood = bShowBlood ? 1 : 0;
	}
}

float NCPlusPerformanceSettings::GetCharacterOverlayDistance()
{
	LoadCharacterOverlayDistance();
	return CharacterOverlayDistance;
}

float NCPlusPerformanceSettings::GetCharacterOverlayDistanceSquared()
{
	LoadCharacterOverlayDistance();
	return CharacterOverlayDistanceSquared;
}

bool NCPlusPerformanceSettings::GetShowDeathBlood()
{
	LoadShowDeathBlood();
	return ShowDeathBlood != 0;
}

void NCPlusPerformanceSettings::SetCharacterOverlayDistance(float Distance)
{
	PublishCharacterOverlayDistance(Distance);

	if (GConfig != nullptr)
	{
		const FString ModIniPath = GetModIniPath();
		GConfig->SetFloat(
			TEXT("NetcodePlus"),
			TEXT("CharacterOverlayDistance"),
			CharacterOverlayDistance,
			ModIniPath);
		GConfig->Flush(false, ModIniPath);
	}
}

void NCPlusPerformanceSettings::Reload()
{
	bCharacterOverlayDistanceLoaded = false;
	ShowDeathBlood = -1;
	LoadCharacterOverlayDistance();
	LoadShowDeathBlood();
}
