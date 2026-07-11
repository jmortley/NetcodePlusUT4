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

	/** Per-element free-form settings (style, color, etc.). Only certain
	 *  widgets honor specific keys — e.g. hp_armor reads "style" for its
	 *  visual variant. Unknown keys are silently preserved by the loader. */
	TMap<FName, FString> Extras;

	/** Read an extras key as string (returns empty if missing). */
	FString GetExtra(FName Key) const
	{
		const FString* V = Extras.Find(Key);
		return V ? *V : FString();
	}

	/** Read an extras key as a color (parses #RRGGBB or #RRGGBBAA).
	 *  Returns Fallback if the key is missing or the string is malformed. */
	FLinearColor GetExtraColor(FName Key, const FLinearColor& Fallback) const;

	/** Read an extras key as a float (e.g. opacity multiplier).
	 *  Returns Fallback if the key is missing or unparsable. */
	float GetExtraFloat(FName Key, float Fallback) const;

	/** Read an extras key as a bool. Accepts "true"/"false"/"1"/"0" (case-insensitive). */
	bool GetExtraBool(FName Key, bool Fallback) const;

private:
	// Extras are editable strings, but their typed values only change when the
	// live-layout revision changes. Malformed/missing values deliberately are not
	// cached because their caller-provided fallback can differ by use site.
	mutable uint32 ExtrasCacheRevision = 0;
	mutable TMap<FName, FLinearColor> ExtraColorCache;
	mutable TMap<FName, float> ExtraFloatCache;
	mutable TMap<FName, bool> ExtraBoolCache;
public:
};

/** Hex ↔ FLinearColor utilities for the HUD layout's per-element color overrides. */
namespace NCPlusHUDColor
{
	/** Parse "#RRGGBB", "#RRGGBBAA", or with no leading hash. Returns true on success. */
	NETCODEPLUS_API bool TryParse(const FString& Hex, FLinearColor& Out);

	/** Convert a color to "#RRGGBBAA" (or "#RRGGBB" if bIncludeAlpha=false). */
	NETCODEPLUS_API FString ToHexString(const FLinearColor& Color, bool bIncludeAlpha = true);
}

/** Visual variants for the HP/Armor widget (mirrors Docs/HudMockups SVGs). */
enum class ENCPlusHPArmorStyle : uint8
{
	MinimalTypography = 0,  // Mockup 03 — current default
	SegmentedBars,          // Mockup 01 — 5 segments + overflow
	RadialArcs,             // Mockup 02 — concentric arcs around crosshair
	HexChevrons,            // Mockup 04 — sci-fi angled segments
	VerticalPills,          // Mockup 05 — paired tall meters
};

namespace NCPlusHPArmorStyle
{
	NETCODEPLUS_API ENCPlusHPArmorStyle Parse(const FString& Name);
	NETCODEPLUS_API FString ToString(ENCPlusHPArmorStyle Style);
	NETCODEPLUS_API TArray<TSharedPtr<FString>> GetChoices();
}

/** Visual variants for the ammo counter widget (Phase 3.7). */
enum class ENCPlusAmmoStyle : uint8
{
	BigNumber = 0,    // Large number with thin accent underline + "/Max" subtext.
	IconAndCount,     // Weapon icon left, count right, ammo bar underline.
	VerticalGauge,    // Stacked: icon top, vertical fill gauge, number below.
};

namespace NCPlusAmmoStyle
{
	NETCODEPLUS_API ENCPlusAmmoStyle Parse(const FString& Name);
	NETCODEPLUS_API FString ToString(ENCPlusAmmoStyle Style);
	NETCODEPLUS_API TArray<TSharedPtr<FString>> GetChoices();
}

/**
 * Drag overlay activation flag (Phase 4.0a/b). The overlay flips this on
 * Construct and clears on ClosePanel; each NetcodePlus HUD's
 * GetInputMode_Implementation reads it and returns EIM_GameAndUI early when
 * set, so Slate mouse events route to the overlay instead of being eaten by
 * the in-match camera-capture / pre-match scoreboard.
 */
namespace NCPlusHUDDragMode
{
	NETCODEPLUS_API bool IsActive();
	NETCODEPLUS_API void SetActive(bool bActive);
	NETCODEPLUS_API void Reset();   // hard-clear the refcount (level-transition backstop)
}

/**
 * Per-element font override (Phase 3.8). Layout JSON stores a display-name
 * string in Extras["font"]; widgets call Resolve() at draw time to convert
 * that to a real UFont*. Tier A fonts (Tiny/Small/Medium/Large/Huge/Number/
 * Chat) come from AUTHUD which already loads them on construction — zero
 * extra cost. Tier B fonts (Exo2 Bold, Rajdhani, Ambex, Positec, Extreme)
 * are lazy-loaded on first use and cached for the rest of the session.
 *
 * "Default" / empty string in Extras means "no override" — Resolve returns
 * the Fallback parameter (whatever the widget's call-site already uses).
 */
namespace NCPlusHUDFonts
{
	/** Resolve a font override for an alias. Returns Fallback if no override
	 *  (or if the override fails to load). HUD must be non-null for Tier A
	 *  fonts to resolve; pass it through from your widget's UTHUDOwner. */
	NETCODEPLUS_API class UFont* Resolve(FName Alias, class AUTHUD* HUD, class UFont* Fallback);

	/** Resolve a font scale multiplier. Default 1.0 — applied on top of the
	 *  widget's existing per-call scale so users can compensate for the
	 *  glyph-height difference between font families. */
	NETCODEPLUS_API float ResolveScale(FName Alias, float Default = 1.f);

	/** Display-name dropdown choices ("Default", "Tiny", ..., "Rajdhani Bold"). */
	NETCODEPLUS_API TArray<TSharedPtr<FString>> GetChoices();
}

/** Whole-HUD layout config. Keyed by alias (e.g. "hp_armor", "weapon_bar_left"). */
struct FNCPlusHUDLayout
{
	static const int32 SchemaVersion = 1;

	int32 Version = SchemaVersion;
	TMap<FName, FNCPlusHUDElement> Elements;

	/** Weapon class short name (e.g. "UTWeap_Sniper") → "left" or "right".
	 *  Drives the custom split WeaponBar (see NCPlusHUDWidget_WeaponBar).
	 *  Unassigned weapons fall back to GetDefaultWeaponSide(). */
	TMap<FName, FName> WeaponGroupAssignments;

	const FNCPlusHUDElement* Find(FName ElementId) const;

	/** Return "left" or "right" for a weapon class.
	 *  Honors WeaponGroupAssignments first; falls back to a built-in heuristic
	 *  (hitscan-ish names → left; everything else → right). */
	FName GetWeaponSide(UClass* WeaponClass) const;

	/** Built-in default for unassigned weapons. */
	static FName GetDefaultWeaponSide(UClass* WeaponClass);

	/** Load from a JSON file. Returns empty layout if file missing or parse fails. */
	static FNCPlusHUDLayout LoadFromFile(const FString& Path);

	/** Write layout to JSON file. Returns true on success. */
	bool SaveToFile(const FString& Path) const;

	/** Serialize to a pretty-printed JSON string. Same payload SaveToFile
	 *  writes — share-to-clipboard uses this. */
	FString ToJsonString() const;

	/** Parse a JSON string. Sets bOutOk=true on success, false (and returns
	 *  empty layout) on parse failure or schema mismatch. Used by paste-from-
	 *  clipboard import and unit-test seams. */
	static FNCPlusHUDLayout FromJsonString(const FString& Json, bool& bOutOk);

	/** Default location: <ProjectSaved>/NetcodePlus/HUDLayout.json
	 *  (Single shared file across all NetcodePlus modes — ElimPlus, Wipeout,
	 *  NCPlusCTF, ShockDom — so users configure once and it applies everywhere.) */
	static FString GetDefaultLayoutPath();

	/** Which bottom-bar widget family this client loads at HUD construction:
	 *  true = stock weapon bar / ammo / health-armor (familiar, self-reads the
	 *  player's HUD profile); false = the NCPlus custom widgets. [NetcodePlus]
	 *  StockBottomBar in Mod.ini overrides; default = fresh install (no saved
	 *  HUDLayout.json) so new players get the stock-like bar and customized users
	 *  keep NCPlus. Read once per HUD spawn — a change applies on the next map. */
	static bool WantsStockBottomBar();

	/** Persist the bottom-bar choice ([NetcodePlus] StockBottomBar in Mod.ini). Call ONLY from an
	 *  explicit user toggle — never unconditionally — so a fresh install (no saved HUDLayout.json)
	 *  is not silently locked to stock (see WantsStockBottomBar). Owned by the HUD editor. */
	static void SetStockBottomBar(bool bStock);

	/** Stock top-left team roster (slanted name + HP/armor plates) vs the NCPlus
	 *  portrait strip. [NetcodePlus] StockTeamPanel in Mod.ini overrides; default =
	 *  fresh install (no saved HUDLayout.json) so new players get the stock-style
	 *  roster and customized users keep the portraits. Cached after first resolve
	 *  (called per-frame from DrawHUD); SetStockTeamPanel refreshes the cache. */
	static bool WantsStockTeamPanel();

	/** Persist + refresh-cache the team-panel choice ([NetcodePlus] StockTeamPanel).
	 *  Call ONLY from an explicit user toggle (see WantsStockTeamPanel). */
	static void SetStockTeamPanel(bool bStock);

	/** Background opacity for the NCPlus scoreboards (ElimPlus/Wipeout), 0.05..1.0.
	 *  [NetcodePlus] ScoreboardOpacity in Mod.ini; default 0.3 (the prior hard-coded
	 *  value). Cached; SetScoreboardOpacity refreshes it. */
	static float GetScoreboardOpacity();

	/** Persist + refresh-cache the scoreboard opacity ([NetcodePlus] ScoreboardOpacity), clamped. */
	static void SetScoreboardOpacity(float Opacity);

	/** Deprecated alias for GetDefaultLayoutPath; kept temporarily for compat. */
	static FString GetDefaultElimPlusPath() { return GetDefaultLayoutPath(); }

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

	/** Monotonic revision counter, bumped on every live-layout mutation (reload,
	 *  reset, and each editor edit via MarkLiveDirty). HUD-side per-player caches
	 *  (e.g. fitted names) key on this so a layout edit invalidates them next frame. */
	static uint32 GetLiveRevision();
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

	/** Anchor that visually matches where this element renders by default
	 *  (i.e. with no layout override). Used by the editor to display a
	 *  meaningful anchor in the dropdown when no entry exists, instead of
	 *  the generic "Center". Returns Center for unrecognized aliases. */
	NETCODEPLUS_API ENCPlusHUDAnchor GetStockAnchor(FName Alias);

	/** Default offset (1080p design pixels) paired with GetStockAnchor.
	 *  Used by the editor's X/Y boxes when no override exists, and also
	 *  seeded into newly-created layout entries so the first edit doesn't
	 *  visibly jump the widget. Returns (0, 0) for unrecognized aliases. */
	NETCODEPLUS_API FVector2D GetStockOffset(FName Alias);
}

/**
 * Helpers for C++-drawn HUD pieces (portrait strips, scorebar) that aren't
 * UUTHUDWidget instances. The HUD's DrawHUD calls into these to get the user's
 * configured screen position and visibility.
 *
 * Phase 3.5: these aliases live alongside the widget aliases in the editor list,
 * but `ApplyLayoutToWidgets` ignores them (no widget to mutate).
 */
namespace NCPlusHUDDrawCall
{
	/** Resolve "what screen pixel should this draw-call element start at?"
	 *  Returns Fallback if the layout has no entry for Alias (preserves stock behavior).
	 *  Anchor coords are 0..1 of canvas; Offset is design-pixel; RenderScale = ClipY/1080. */
	NETCODEPLUS_API FVector2D ResolveScreenPos(FName Alias, class UCanvas* Canvas, const FVector2D& Fallback);

	/** True if the layout marks this alias hidden — caller skips its draw. */
	NETCODEPLUS_API bool IsHidden(FName Alias);

	/** Per-element scale (design-px multiplier). 1.0 if no override. */
	NETCODEPLUS_API float GetScale(FName Alias);

	/** Per-element opacity (0..1). 1.0 if no override. C++-drawn pieces should
	 *  multiply this into every SetLinearDrawColor alpha they emit so the
	 *  layout editor's Op slider behaves consistently with widget elements. */
	NETCODEPLUS_API float GetOpacity(FName Alias);

	/** Should this element use the dynamic AUTTeamInfo::TeamColor (true, default)
	 *  or the stock red/blue palette (false)? Honored by portrait_red,
	 *  portrait_blue, and scorebar. */
	NETCODEPLUS_API bool GetUseTeamColor(FName Alias);

	/** Effective anchor for an alias: layout override if present, otherwise
	 *  the alias's stock anchor (NCPlusHUDAliases::GetStockAnchor). */
	NETCODEPLUS_API ENCPlusHUDAnchor GetEffectiveAnchor(FName Alias);

	/** Full-screen damage tint. Call at the end of each NCPlus HUD's DrawHUD.
	 *  Tracks the local pawn's Health+Armor across frames; on a decrease, stamps
	 *  the time and tints the screen for `flash_duration` seconds. No-op when the
	 *  `damage_flash` alias has no layout entry or is hidden (default OFF).
	 *  Canvas is passed by the caller (AHUD::Canvas is protected, can't be
	 *  reached from a free function — the HUD subclass has it in scope already). */
	NETCODEPLUS_API void DrawDamageFlash(class AUTHUD* HUD, class UCanvas* Canvas);

	/** Held-pickup status — amp/berserk/siphon REMAINING TIME + jump-boot charges —
	 *  drawn in C++ for NCPlus modes. Replaces the stock bpHW_Powerups widget, which
	 *  only draws when the player's "Show Powerups on MiniHUD" profile option is off
	 *  and which NCPlus's QuickStats replacement otherwise suppresses (so MiniHUD
	 *  users lost the display). Held-pickup STATUS only — your own powerups/boots —
	 *  NOT map item respawn timers. No-op in stock-bottom-bar mode (the vanilla
	 *  widgets handle it) or when the `powerups` alias is hidden. Honors the
	 *  `powerups` alias position/anchor/offset, scale, and opacity. Call from each
	 *  NCPlus HUD's DrawHUD (Canvas passed by caller — see DrawDamageFlash note). */
	NETCODEPLUS_API void DrawHeldPowerups(class AUTHUD* HUD, class UCanvas* Canvas);

	/** Swap the bottom-bar widget family (stock vs NCPlus) on a LIVE HUD so the menu's
	 *  Stock-bottom-bar toggle applies THIS match instead of next level (the family is
	 *  otherwise fixed at HUD construction). bWantStock = the menu's current choice, passed
	 *  in to avoid a GConfig re-read race against the just-written value. Removes the family
	 *  no longer wanted and adds the one now wanted; no-op on a dedicated server / non-UT HUD. */
	NETCODEPLUS_API void RefreshBottomBarWidgets(class AHUD* HUD, bool bWantStock);

	/** Draw one outlined text string as a single batched FCanvasTextItem (4 diagonal
	 *  outline passes + 1 fill, ONE DrawItem — no per-pass ICU word-wrap / FText churn).
	 *  Drop-in for the hand-rolled 4-offset-DrawText stacks in the HUD paths. Opacity
	 *  scales both fill and outline alpha. Shadow is intentionally omitted — the outline
	 *  replaces it (the old stacks paid a shadow double-raster on every one of 5 passes). */
	NETCODEPLUS_API void DrawOutlinedText(class UCanvas* Canvas, const class UFont* Font,
		const FText& Text, float X, float Y, float Scale,
		FLinearColor Fill, FLinearColor Outline = FLinearColor::Black, float Opacity = 1.f);

	/** Resolve a measured FText for HUD strings that usually remain unchanged for
	 *  hundreds of frames (scores, clocks, labels, spectator names). Cached by
	 *  text/font/scale and invalidated on world or live-layout revision changes. */
	NETCODEPLUS_API void ResolveStableText(class UCanvas* Canvas, class UFont* Font,
		const FString& Source, float ScaleX, float ScaleY,
		FText& OutText, float& OutWidth, float& OutHeight);
	NETCODEPLUS_API void DrawResolvedText(class UCanvas* Canvas, const class UFont* Font,
		const FText& Text, float X, float Y, float ScaleX, float ScaleY, const FColor& Color,
		bool bEnableShadow = false);

	/** Fit a player name into MaxWidthPx at draw Scale, caching the fitted result per
	 *  PlayerState so the per-frame chop-one-char StrLen loop runs only when an input
	 *  (name / font / width bucket / layout revision) actually changes. Fills OutText
	 *  (ready-to-draw FText) and OutWidth/OutHeight (UNSCALED StrLen extents — multiply
	 *  by Scale for pixels). The cache clears wholesale on world change (level
	 *  transition) and on a layout-revision bump. */
	NETCODEPLUS_API void ResolveFittedName(class UCanvas* Canvas, class APlayerState* PS,
		class UFont* Font, const FString& Src, float MaxWidthPx, float Scale,
		FText& OutText, float& OutWidth, float& OutHeight);

	/** Stock-style top-left team roster: red row then blue row of slanted, team-
	 *  colored plates (name over "+HP armor" for teammates, name only for enemies).
	 *  Dead players are skipped so the row reflows — plate count = alive count. Drawn
	 *  by ElimPlus/Wipeout DrawHUD when FNCPlusHUDLayout::WantsStockTeamPanel(), in
	 *  place of the portrait strip. Honors the `team_panel` alias (pos/scale/opacity/
	 *  hide). Canvas passed by caller (see DrawDamageFlash note). */
	NETCODEPLUS_API void DrawStockTeamPanel(class AUTHUD* HUD, class UCanvas* Canvas);

	/** Optional server-name plate. Reads GameState->ServerName, draws at the
	 *  `server_info` alias's position. No-op when no entry / hidden (default OFF).
	 *  Honors font / font_scale / color_text / opacity. Canvas passed by caller
	 *  (see DrawDamageFlash note). */
	NETCODEPLUS_API void DrawServerInfo(class AUTHUD* HUD, class UCanvas* Canvas);

	/** WARMUP-ONLY spawn-point markers (learning aid — pre-aim angles / enemy
	 *  approach lanes): team-colored markers at every placed PlayerStart with a
	 *  facing tick (the way a spawned player looks) and a distance label.
	 *  LINE-OF-SIGHT ONLY — a visibility trace gates each marker, nothing draws
	 *  through walls. Placed level actors exist client-side, so this is pure
	 *  local drawing — no replication, no server involvement, no version bump.
	 *  Self-gates to MatchState::WaitingToStart + cvar ncp.WarmupSpawns (default
	 *  on); caller should skip while the scoreboard is up. */
	NETCODEPLUS_API void DrawWarmupSpawnMarkers(class AUTHUD* HUD, class UCanvas* Canvas);

	/** Replay-only corner feed of fire-validation samples. No-op unless a demo is
	 *  playing back. Reads the server-written FireVal_*.csv (newest in Saved/Logs, or
	 *  the path in the `ncp.FireValReplayCsv` cvar) and draws each shot synced to the
	 *  replayed server clock (GameState->GetServerWorldTimeSeconds). Pure client
	 *  display — no replication, never runs in live play. */
	NETCODEPLUS_API void DrawFireValReplayFeed(class AUTHUD* HUD, class UCanvas* Canvas);

	/** Auto post-match high-res screenshot, shared by ElimPlus/Wipeout/iCTF (and Duel/Shaft via AWipeoutHUD).
	 *  Fires ONCE, on the final scoreboard rather than the instant replay: it counts consecutive qualifying
	 *  DrawHUD frames (match ended, not in a replay), which naturally pauses while the HUD is suspended during
	 *  the replay's separate killcam world and resumes fresh afterward. Client opt-in via [NetcodePlus]
	 *  HighResScreenshotPostMatch. The caller stores StableFrames + bTaken (init false) as HUD members and
	 *  calls this every frame from DrawHUD. */
	NETCODEPLUS_API void ServicePostMatchScreenshot(class AUTHUD* HUD, float& StableFrames, bool& bTaken);
}

/**
 * Snapshot of a widget's stock layout fields, captured once at BeginPlay so that
 * Reset / Reset All can restore the original look (not whatever the user last
 * dragged it to).
 */
struct FNCPlusWidgetDefaults
{
	FVector2D ScreenPosition = FVector2D::ZeroVector;
	FVector2D Position       = FVector2D::ZeroVector;
	FVector2D Origin         = FVector2D::ZeroVector;
	bool      bHidden        = false;

	// WeaponBar quirk: it self-positions in PreDraw from its own internal
	// Horizontal/Vertical*Position fields, stomping anything we set on the
	// generic ScreenPosition/Position. We snapshot those too so reset works.
	FVector2D WB_HorizScreenPos = FVector2D::ZeroVector;
	FVector2D WB_HorizPos       = FVector2D::ZeroVector;
	FVector2D WB_VertScreenPos  = FVector2D::ZeroVector;
	FVector2D WB_VertPos        = FVector2D::ZeroVector;
	bool      bIsWeaponBar      = false;
};

/**
 * Capture stock defaults for every widget the alias map knows about. Call once
 * after Super::BeginPlay() — before the first ApplyLayoutToWidgets() — so the
 * "no override" path can restore an identity-preserving baseline.
 */
NETCODEPLUS_API void CaptureWidgetDefaults(class AUTHUD* HUD);

/**
 * Apply a layout to every widget in HUD->HudWidgets[].
 *
 * For each registered widget:
 *   - Override present → set ScreenPosition + Origin (both match the anchor so
 *     the widget sticks to its anchor corner and content extends inward) +
 *     Position (offset in design pixels) + hidden state.
 *   - No override     → restore from CaptureWidgetDefaults() snapshot.
 */
NETCODEPLUS_API void ApplyLayoutToWidgets(class AUTHUD* HUD, const FNCPlusHUDLayout& Layout);
