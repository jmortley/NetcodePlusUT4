// NCPlusHUDPresets - curated layout presets + user-saved customs.
//
// Curated presets are baked into the DLL as static const JSON strings so
// they ship with the plugin without any pak/cook step. Users can also save
// their current GetLive() layout to disk under Saved/NetcodePlus/Presets/
// and re-apply it from the gallery later.
//
// FNCPlusHUDPreset.JsonString is the same payload format the editor's
// clipboard Copy/Paste round-trips - i.e. FNCPlusHUDLayout::FromJsonString
// consumes it directly.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

/** One preset entry as the gallery sees it. JsonString is the inner layout
 *  payload (no metadata wrapper) so callers can pass it straight to
 *  FNCPlusHUDLayout::FromJsonString. */
struct FNCPlusHUDPreset
{
	FString Id;             // "streamer_friendly" | "comp_minimal" | "ql_throwback"
	                        // | filesystem stem for customs ("mysetup")
	FString DisplayName;    // user-facing card title
	FString Description;    // 1-line subtitle for the gallery card
	FString JsonString;     // raw layout JSON payload
	bool    bIsCustom = false;
};

namespace NCPlusHUDPresets
{
	/** Curated presets baked into the DLL. Stable order - index 0 is the
	 *  first-run default (Streamer Friendly), referenced by
	 *  FNCPlusHUDLayout::ReloadLive when no on-disk layout exists. */
	NETCODEPLUS_API const TArray<FNCPlusHUDPreset>& GetCurated();

	/** Scan Saved/NetcodePlus/Presets/*.preset.json. Sorted alphabetically
	 *  by display name. */
	NETCODEPLUS_API TArray<FNCPlusHUDPreset> ScanCustoms();

	/** Curated + customs in display order (curated first). */
	NETCODEPLUS_API TArray<FNCPlusHUDPreset> GetAll();

	/** Save FNCPlusHUDLayout::GetLive() as a custom preset. Sanitizes Name
	 *  to a filesystem slug and writes to <PresetsDir>/<slug>.preset.json
	 *  with name + description embedded in a wrapper object. Returns true
	 *  on success and writes the chosen slug to OutId. */
	NETCODEPLUS_API bool SaveCustom(const FString& Name, const FString& Description,
	                                FString& OutId);

	/** Delete a custom preset by id. Returns false if id is unknown or
	 *  refers to a curated preset. */
	NETCODEPLUS_API bool DeleteCustom(const FString& Id);

	/** Saved/NetcodePlus/Presets directory (created on demand). */
	NETCODEPLUS_API FString GetCustomPresetsDir();
}
