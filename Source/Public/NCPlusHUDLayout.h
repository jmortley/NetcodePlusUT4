// NCPlusHUDLayout — JSON-driven HUD element registry for NetcodePlus.
//
// Phase 1 MVP:
//   - 9-anchor grid (TopLeft .. BottomRight)
//   - per-element offset (in 1080p design pixels), scale, hidden
//   - alias map: short names ("weapon_bar") → UClass paths
//   - apply-pass that mutates loaded UUTHUDWidget instances post-BeginPlay
//
// Reserved for Phase 2:
//   - per-element style block (fonts, colors, thresholds)
//   - layout consult from C++-drawn pieces (portraits, scorebar)
//   - Slate editor with live preview
//
// Not a UStruct — plain runtime config. JSON parse/serialize via FJsonSerializer.
#pragma once

#include "NetcodePlus.h"
#include "CoreMinimal.h"

/** 9-anchor screen grid. Anchor coords are normalized (0..1, 0..1). */
enum class ENCPlusHUDAnchor : uint8
{
	TopLeft = 0, TopCenter, TopRight,
	CenterLeft,  Center,    CenterRight,
	BottomLeft,  BottomCenter, BottomRight,
};

/** Layout overrides for a single HUD element. */
struct FNCPlusHUDElement
{
	ENCPlusHUDAnchor Anchor = ENCPlusHUDAnchor::Center;
	FVector2D Offset = FVector2D::ZeroVector;  // 1080p design pixels
	float Scale = 1.0f;
	bool bHidden = false;
	// Style block intentionally omitted in v1 — see header comment.
};

/** Whole-HUD layout config. Keyed by alias (e.g. "hp_armor", "weapon_bar"). */
struct FNCPlusHUDLayout
{
	static const int32 SchemaVersion = 1;

	int32 Version = SchemaVersion;
	TMap<FName, FNCPlusHUDElement> Elements;

	const FNCPlusHUDElement* Find(FName ElementId) const;

	/** Load from a JSON file. Returns empty layout if file missing or parse fails. */
	static FNCPlusHUDLayout LoadFromFile(const FString& Path);

	/** Write layout to JSON file. Returns true on success. */
	bool SaveToFile(const FString& Path) const;

	/** Default location: <ProjectSaved>/NetcodePlus/ElimPlusHUDLayout.json */
	static FString GetDefaultElimPlusPath();

	/** Convert anchor enum to normalized screen coords (0..1, 0..1). */
	static FVector2D AnchorToScreenCoords(ENCPlusHUDAnchor Anchor);

	/** Parse anchor name (case-insensitive). Returns Center on failure. */
	static ENCPlusHUDAnchor ParseAnchor(const FString& Name);

	/** Stringify anchor for JSON serialization. */
	static FString AnchorToString(ENCPlusHUDAnchor Anchor);

	// -------------------------------------------------------------------------
	// Phase 2: in-memory singleton for live editing.
	//
	// The HUD re-applies GetLive() every frame, so any mutation here is visible
	// next render tick. The Slate editor binds to GetLive() and mutates in place.
	// Disk file is read into the singleton on game/PIE start, written on Save.
	// -------------------------------------------------------------------------

	/** The runtime live layout — mutable, shared across the process. */
	static FNCPlusHUDLayout& GetLive();

	/** Re-read the layout file from disk into GetLive(). Called on PIE start. */
	static void ReloadLive();

	/** Persist GetLive() to disk. Returns true on success. */
	static bool SaveLive();

	/** Wipe GetLive() back to defaults (empty map = no overrides anywhere). */
	static void ResetLive();

	/** Slate editor calls this after any mutation; HUD apply-pass uses it to no-op when clean. */
	static void MarkLiveDirty();

	/** True if GetLive() has been mutated since the last ApplyLayoutToWidgets() call. */
	static bool IsLiveDirty();

	/** Cleared automatically by ApplyLayoutToWidgets after a successful apply. */
	static void ClearLiveDirty();
};

/**
 * Alias system — lets JSON keys (short names) map to UClass paths.
 *
 * Why: writing "/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponBar.bpHW_WeaponBar_C"
 * in user-facing JSON is hostile. Aliases are friendlier and let us renumber the
 * underlying class paths without breaking existing layout files.
 */
namespace NCPlusHUDAliases
{
	/** Look up the UClass path for an alias. Empty string if unknown. */
	NETCODEPLUS_API FString GetClassPath(FName Alias);

	/** Reverse lookup: find the alias for a widget instance's class. NAME_None if unknown. */
	NETCODEPLUS_API FName GetAliasForClass(UClass* Cls);

	/** All known aliases, in stable display order (for editor list). */
	NETCODEPLUS_API TArray<FName> GetAllAliases();

	/** Pretty display name for an alias (e.g. "hp_armor" → "Health & Armor"). */
	NETCODEPLUS_API FText GetDisplayName(FName Alias);
}

/**
 * Apply a layout to every widget in HUD->HudWidgets[].
 *
 * For each widget, looks up its class in the alias table; if a matching entry
 * exists in `Layout.Elements`, mutates the widget's ScreenPosition / Position /
 * bHidden accordingly. Widgets without a layout entry are left untouched.
 *
 * Origin is intentionally NOT touched — each widget knows its preferred pivot.
 * Users adjust offset to compensate.
 */
NETCODEPLUS_API void ApplyLayoutToWidgets(class AUTHUD* HUD, const FNCPlusHUDLayout& Layout);
