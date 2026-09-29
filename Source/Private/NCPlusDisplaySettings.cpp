// NCPlusDisplaySettings.cpp - cached, client-local HUD display preferences.
#include "NCPlusDisplaySettings.h"

#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

namespace
{
	// Distinct names: this file can share a unity translation unit with the other
	// Mod.ini settings helpers.
	const TCHAR* const NCPDisplaySettingsSection = TEXT("NetcodePlus");
	const TCHAR* const NCPHideFriendlyCrosshairSignKey = TEXT("HideFriendlyCrosshairSign");
	const TCHAR* const NCPHideTeammateOverheadTagsKey = TEXT("HideTeammateOverheadTags");
	const TCHAR* const NCPCollapseRepeatedKillNamesKey = TEXT("CollapseRepeatedKillNames");

	// Stock presentation until Reload() reads an explicit True.
	bool GNCPHideFriendlyCrosshairSign = false;
	bool GNCPHideTeammateOverheadTags = false;
	bool GNCPCollapseRepeatedKillNames = false;

	FString GetDisplaySettingsModIniPath()
	{
		return FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	}

	bool ReadDisplaySetting(const TCHAR* Key)
	{
		bool bValue = false;
		if (GConfig != nullptr)
		{
			GConfig->GetBool(NCPDisplaySettingsSection, Key, bValue, GetDisplaySettingsModIniPath());
		}
		return bValue;
	}

	void WriteDisplaySetting(const TCHAR* Key, bool bValue)
	{
		if (GConfig != nullptr)
		{
			const FString ModIniPath = GetDisplaySettingsModIniPath();
			GConfig->SetBool(NCPDisplaySettingsSection, Key, bValue, ModIniPath);
			GConfig->Flush(false, ModIniPath);
		}
	}
}

bool NCPlusDisplaySettings::GetHideFriendlyCrosshairSign()
{
	return GNCPHideFriendlyCrosshairSign;
}

bool NCPlusDisplaySettings::GetHideTeammateOverheadTags()
{
	return GNCPHideTeammateOverheadTags;
}

bool NCPlusDisplaySettings::GetCollapseRepeatedKillNames()
{
	return GNCPCollapseRepeatedKillNames;
}

void NCPlusDisplaySettings::SetHideFriendlyCrosshairSign(bool bHide)
{
	GNCPHideFriendlyCrosshairSign = bHide;
	WriteDisplaySetting(NCPHideFriendlyCrosshairSignKey, bHide);
}

void NCPlusDisplaySettings::SetHideTeammateOverheadTags(bool bHide)
{
	GNCPHideTeammateOverheadTags = bHide;
	WriteDisplaySetting(NCPHideTeammateOverheadTagsKey, bHide);
}

void NCPlusDisplaySettings::SetCollapseRepeatedKillNames(bool bCollapse)
{
	GNCPCollapseRepeatedKillNames = bCollapse;
	WriteDisplaySetting(NCPCollapseRepeatedKillNamesKey, bCollapse);
}

void NCPlusDisplaySettings::Reload()
{
	GNCPHideFriendlyCrosshairSign = ReadDisplaySetting(NCPHideFriendlyCrosshairSignKey);
	GNCPHideTeammateOverheadTags = ReadDisplaySetting(NCPHideTeammateOverheadTagsKey);
	GNCPCollapseRepeatedKillNames = ReadDisplaySetting(NCPCollapseRepeatedKillNamesKey);
}
