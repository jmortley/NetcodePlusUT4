// NCPlusHUDLayout — implementation. JSON I/O + alias map + apply-to-widgets pass.
#include "NCPlusHUDLayout.h"
#include "NCPlusHUDPresets.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTHUDWidget.h"
#include "UTHUDWidgetMessage_KillIconMessages.h"   // killfeed special-case (KillIconWidget stomp bypass)
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "UTGameState.h"
#include "UTInventory.h"
#include "UTTimedPowerup.h"
#include "UTJumpBoots.h"
#include "Engine/Canvas.h"
#include "CanvasItem.h"
#include "Engine/Font.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Engine/World.h"
#include "Engine/DemoNetDriver.h"
#include "GameFramework/GameStateBase.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"

// =============================================================================
// Anchor conversions
// =============================================================================

FVector2D FNCPlusHUDLayout::AnchorToScreenCoords(ENCPlusHUDAnchor Anchor)
{
	switch (Anchor)
	{
		case ENCPlusHUDAnchor::TopLeft:      return FVector2D(0.0f, 0.0f);
		case ENCPlusHUDAnchor::TopCenter:    return FVector2D(0.5f, 0.0f);
		case ENCPlusHUDAnchor::TopRight:     return FVector2D(1.0f, 0.0f);
		case ENCPlusHUDAnchor::CenterLeft:   return FVector2D(0.0f, 0.5f);
		case ENCPlusHUDAnchor::Center:       return FVector2D(0.5f, 0.5f);
		case ENCPlusHUDAnchor::CenterRight:  return FVector2D(1.0f, 0.5f);
		case ENCPlusHUDAnchor::BottomLeft:   return FVector2D(0.0f, 1.0f);
		case ENCPlusHUDAnchor::BottomCenter: return FVector2D(0.5f, 1.0f);
		case ENCPlusHUDAnchor::BottomRight:  return FVector2D(1.0f, 1.0f);
		default:                              return FVector2D(0.5f, 0.5f);
	}
}

ENCPlusHUDAnchor FNCPlusHUDLayout::ParseAnchor(const FString& Name)
{
	// .Trim() mutates in place (UE4 4.15) — need a non-const copy.
	FString N = Name;
	N.Trim();
	N = N.ToLower();
	if (N == TEXT("topleft"))      return ENCPlusHUDAnchor::TopLeft;
	if (N == TEXT("topcenter"))    return ENCPlusHUDAnchor::TopCenter;
	if (N == TEXT("topright"))     return ENCPlusHUDAnchor::TopRight;
	if (N == TEXT("centerleft"))   return ENCPlusHUDAnchor::CenterLeft;
	if (N == TEXT("center"))       return ENCPlusHUDAnchor::Center;
	if (N == TEXT("centerright"))  return ENCPlusHUDAnchor::CenterRight;
	if (N == TEXT("bottomleft"))   return ENCPlusHUDAnchor::BottomLeft;
	if (N == TEXT("bottomcenter")) return ENCPlusHUDAnchor::BottomCenter;
	if (N == TEXT("bottomright"))  return ENCPlusHUDAnchor::BottomRight;
	return ENCPlusHUDAnchor::Center;
}

// =============================================================================
// HP/Armor style enum
// =============================================================================

namespace NCPlusHPArmorStyle
{
	ENCPlusHPArmorStyle Parse(const FString& Name)
	{
		FString N = Name;
		N.Trim();
		N = N.ToLower();
		if (N == TEXT("segmentedbars"))     return ENCPlusHPArmorStyle::SegmentedBars;
		if (N == TEXT("radialarcs"))        return ENCPlusHPArmorStyle::RadialArcs;
		if (N == TEXT("hexchevrons"))       return ENCPlusHPArmorStyle::HexChevrons;
		if (N == TEXT("verticalpills"))     return ENCPlusHPArmorStyle::VerticalPills;
		return ENCPlusHPArmorStyle::MinimalTypography;
	}

	FString ToString(ENCPlusHPArmorStyle Style)
	{
		switch (Style)
		{
			case ENCPlusHPArmorStyle::SegmentedBars:    return TEXT("SegmentedBars");
			case ENCPlusHPArmorStyle::RadialArcs:       return TEXT("RadialArcs");
			case ENCPlusHPArmorStyle::HexChevrons:      return TEXT("HexChevrons");
			case ENCPlusHPArmorStyle::VerticalPills:    return TEXT("VerticalPills");
			default:                                    return TEXT("MinimalTypography");
		}
	}

	TArray<TSharedPtr<FString>> GetChoices()
	{
		TArray<TSharedPtr<FString>> Out;
		Out.Add(MakeShareable(new FString(TEXT("MinimalTypography"))));
		Out.Add(MakeShareable(new FString(TEXT("SegmentedBars"))));
		Out.Add(MakeShareable(new FString(TEXT("RadialArcs"))));
		Out.Add(MakeShareable(new FString(TEXT("HexChevrons"))));
		Out.Add(MakeShareable(new FString(TEXT("VerticalPills"))));
		return Out;
	}
}

// =============================================================================
// Drag overlay activation flag (Phase 4.0a/b)
// =============================================================================

namespace NCPlusHUDDragMode
{
	// Refcounted: the drag overlay AND the nchud/weaponskins/ncpmenu/cosmetics
	// menus each request the cursor-free input mode on open. Active while ANY are
	// open; only the last close lets the HUD's GetInputMode poll re-capture the
	// cursor. (Was a plain bool — one menu closing cleared another's request.)
	static int32 GActiveCount = 0;

	bool IsActive()              { return GActiveCount > 0; }
	void SetActive(bool bActive) { if (bActive) { ++GActiveCount; } else if (GActiveCount > 0) { --GActiveCount; } }
	void Reset()                 { GActiveCount = 0; }   // backstop for a leaked count (map load)
}

// =============================================================================
// Font resolver (Phase 3.8)
// =============================================================================

// Monotonic layout revision — bumped wherever the live layout changes (load / reset /
// edit). The font + scale resolve caches below key off it, so on an unchanged layout
// (the per-frame norm) a resolve is one FName Find instead of GetExtra + a scan.
static uint32 GLayoutRevision = 1;

namespace NCPlusHUDFonts
{
	// Tier A getters — pull from AUTHUD's already-loaded font set.
	static UFont* GetTiny  (AUTHUD* H) { return H ? H->TinyFont   : nullptr; }
	static UFont* GetSmall (AUTHUD* H) { return H ? H->SmallFont  : nullptr; }
	static UFont* GetMedium(AUTHUD* H) { return H ? H->MediumFont : nullptr; }
	static UFont* GetLarge (AUTHUD* H) { return H ? H->LargeFont  : nullptr; }
	static UFont* GetHuge  (AUTHUD* H) { return H ? H->HugeFont   : nullptr; }
	static UFont* GetNumber(AUTHUD* H) { return H ? H->NumberFont : nullptr; }
	static UFont* GetChat  (AUTHUD* H) { return H ? H->ChatFont   : nullptr; }

	struct FFontEntry
	{
		FString  Display;                  // user-facing name (also stored in JSON)
		FString  AssetPath;                // empty for Tier A; full path for Tier B
		UFont* (*HUDFunc)(AUTHUD* H);      // non-null for Tier A; null for Tier B
	};

	static const TArray<FFontEntry>& GetTable()
	{
		static const TArray<FFontEntry> Table = {
			// Tier A — already loaded by AUTHUD ctor, zero cost.
			{ TEXT("Tiny"),         FString(),                                                                              &GetTiny   },
			{ TEXT("Small"),        FString(),                                                                              &GetSmall  },
			{ TEXT("Medium"),       FString(),                                                                              &GetMedium },
			{ TEXT("Large"),        FString(),                                                                              &GetLarge  },
			{ TEXT("Huge"),         FString(),                                                                              &GetHuge   },
			{ TEXT("Number"),       FString(),                                                                              &GetNumber },
			{ TEXT("Chat"),         FString(),                                                                              &GetChat   },
			// Tier B — lazy-loaded on first use, cached for rest of session.
			{ TEXT("Exo2 Bold"),       TEXT("/Game/RestrictedAssets/UI/Fonts/Exo2-Bold.Exo2-Bold"),                         nullptr },
			// Rajdhani Bold + SemiBold removed - the .uasset files exist on
			// disk under /Game/RestrictedAssets/UI/Fonts/ but neither loads as
			// UFont (likely UCompositeFont or unbaked TTF wrapper, or a
			// cooking issue specific to this build). The cache-failure fix
			// suppressed the spam but the fonts never resolved, so they were
			// just dead entries cluttering the picker.
			{ TEXT("Lato"),            TEXT("/Game/RestrictedAssets/Fonts/Lato.Lato"),                                      nullptr },
			{ TEXT("Ambex"),           TEXT("/Game/RestrictedAssets/Fonts/fntAmbex36.fntAmbex36"),                          nullptr },
			{ TEXT("Ambex Large"),     TEXT("/Game/RestrictedAssets/Fonts/fntAmbex72.fntAmbex72"),                          nullptr },
			{ TEXT("Positec"),         TEXT("/Game/RestrictedAssets/Fonts/fntPositec36.fntPositec36"),                      nullptr },
			{ TEXT("Positec Small"),   TEXT("/Game/RestrictedAssets/Fonts/fntPositec14.fntPositec14"),                      nullptr },
			{ TEXT("Extreme"),         TEXT("/Game/RestrictedAssets/Fonts/fntExtreme.fntExtreme"),                          nullptr },
			// External Google-font TEST SET (Inter / Inter Italic / JetBrains Mono x2 /
			// Bebas Neue / Oswald / Russo One) REMOVED 2026-06-23 — they lived in
			// /Game/Blueprints/Netcode/ which was never added to the cook (so they never
			// shipped) and the variable-font ones don't render in UE4.15 anyway. Picking
			// one silently fell back to the default font ("the font bug"). Re-add ONLY a
			// NON-variable weight, and only after confirming it's cooked + loads as UFont.
		};
		return Table;
	}

	UFont* Resolve(FName Alias, AUTHUD* HUD, UFont* Fallback)
	{
		// Per-alias override cache: Alias is already an FName, so a hit is one hashed
		// Find with zero string/FName work. Invalidated when the layout changes
		// (GLayoutRevision). Value = resolved OVERRIDE font, or null meaning no
		// override (use the caller's Fallback), so the same alias works from sites
		// passing different fallbacks (e.g. pip SmallFont vs name TinyFont).
		static TMap<FName, UFont*> OverrideCache;
		static uint32 CacheRev = 0;
		if (CacheRev != GLayoutRevision) { OverrideCache.Reset(); CacheRev = GLayoutRevision; }
		if (UFont** Hit = OverrideCache.Find(Alias)) { return *Hit ? *Hit : Fallback; }

		UFont* const Resolved = [&]() -> UFont*
		{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		if (!E) return nullptr;

		static const FName NAME_Font(TEXT("font"));   // build the key once, not per call
		const FString FontKey = E->GetExtra(NAME_Font);
		if (FontKey.IsEmpty() || FontKey.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
		{
			return nullptr;
		}

		// Lazy-load cache for Tier B fonts. Map by asset path so different
		// display aliases pointing at the same asset only load once.
		static TMap<FString, UFont*> Cache;

		// One-time name -> entry index (display names are unique). Turns an
		// overridden font into a single FName-keyed Find instead of a ~20-entry
		// case-insensitive FString scan every frame — matters at 500+ fps. FName
		// keys make the match case-insensitive for free.
		static const TMap<FName, const FFontEntry*> NameMap = []{
			TMap<FName, const FFontEntry*> M;
			for (const FFontEntry& Ent : GetTable()) { M.Add(FName(*Ent.Display), &Ent); }
			return M;
		}();
		if (const FFontEntry* const* Found = NameMap.Find(FName(*FontKey)))
		{
			const FFontEntry& F = **Found;

			if (F.HUDFunc)
			{
				UFont* HF = F.HUDFunc(HUD);
				return HF;
			}

			// Cache hit. nullptr is a valid cached value meaning "tried, failed,
			// don't retry" — without this short-circuit a missing asset retries
			// LoadObject every frame for every widget that names it, hammering
			// disk and spamming the log. Treat cached-null as known failure.
			if (UFont** Cached = Cache.Find(F.AssetPath))
			{
				return *Cached;
			}

			// First attempt for this asset path. Try as UObject first so we can
			// log the actual class on cast failure (asset may exist but not be
			// a UFont, e.g. UCompositeFont). Cache success or failure.
			UObject* Asset = LoadObject<UObject>(nullptr, *F.AssetPath);
			UFont* Loaded = Cast<UFont>(Asset);
			Cache.Add(F.AssetPath, Loaded);
			if (Loaded)
			{
				// CRITICAL: pin to root so GC doesn't reap the font out from
				// under our cached raw pointer. The cache is a static
				// TMap<FString, UFont*> with no UPROPERTY holding the UFont
				// alive; without AddToRoot, UE4's GC walks the property graph
				// at spawn-time, finds the font unreferenced, and reaps it.
				// The cache then holds a stale pointer that the next DrawText
				// queues into a Slate render command, and the render thread
				// dereferences it the next frame as an EXCEPTION_ACCESS_VIOLATION
				// reading 0x18 inside SlateRHIRenderer. This was the root cause
				// of the "crashes when match countdown ends" bug across all
				// three NetcodePlus team modes.
				Loaded->AddToRoot();
				return Loaded;
			}

			UE_LOG(LogTemp, Warning,
				TEXT("[NCPlusHUDFonts] Failed to load '%s' (%s): asset=%s class=%s — falling back (cached, no retry)."),
				*F.Display, *F.AssetPath,
				Asset ? TEXT("loaded") : TEXT("null"),
				Asset ? *Asset->GetClass()->GetName() : TEXT("(none)"));
			return nullptr;
		}

		// Unknown name -> no override; the wrapper below applies the caller's Fallback.
		return nullptr;
		}();

		OverrideCache.Add(Alias, Resolved);
		return Resolved ? Resolved : Fallback;
	}

	float ResolveScale(FName Alias, float Default)
	{
		// Per-alias cache (see Resolve). Assumes a stable Default per alias; all
		// current callers pass 1.f.
		static TMap<FName, float> ScaleCache;
		static uint32 ScaleCacheRev = 0;
		if (ScaleCacheRev != GLayoutRevision) { ScaleCache.Reset(); ScaleCacheRev = GLayoutRevision; }
		if (float* Hit = ScaleCache.Find(Alias)) { return *Hit; }

		static const FName NAME_FontScale(TEXT("font_scale"));
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		const float V = E ? E->GetExtraFloat(NAME_FontScale, Default) : Default;
		ScaleCache.Add(Alias, V);
		return V;
	}

	TArray<TSharedPtr<FString>> GetChoices()
	{
		TArray<TSharedPtr<FString>> Out;
		Out.Add(MakeShareable(new FString(TEXT("Default"))));
		for (const FFontEntry& F : GetTable())
		{
			Out.Add(MakeShareable(new FString(F.Display)));
		}
		return Out;
	}
}

namespace NCPlusAmmoStyle
{
	ENCPlusAmmoStyle Parse(const FString& Name)
	{
		FString N = Name;
		N.Trim();
		N = N.ToLower();
		if (N == TEXT("iconandcount"))  return ENCPlusAmmoStyle::IconAndCount;
		if (N == TEXT("verticalgauge")) return ENCPlusAmmoStyle::VerticalGauge;
		return ENCPlusAmmoStyle::BigNumber;
	}

	FString ToString(ENCPlusAmmoStyle Style)
	{
		switch (Style)
		{
			case ENCPlusAmmoStyle::IconAndCount:  return TEXT("IconAndCount");
			case ENCPlusAmmoStyle::VerticalGauge: return TEXT("VerticalGauge");
			default:                              return TEXT("BigNumber");
		}
	}

	TArray<TSharedPtr<FString>> GetChoices()
	{
		TArray<TSharedPtr<FString>> Out;
		Out.Add(MakeShareable(new FString(TEXT("BigNumber"))));
		Out.Add(MakeShareable(new FString(TEXT("IconAndCount"))));
		Out.Add(MakeShareable(new FString(TEXT("VerticalGauge"))));
		return Out;
	}
}

FString FNCPlusHUDLayout::AnchorToString(ENCPlusHUDAnchor Anchor)
{
	switch (Anchor)
	{
		case ENCPlusHUDAnchor::TopLeft:      return TEXT("TopLeft");
		case ENCPlusHUDAnchor::TopCenter:    return TEXT("TopCenter");
		case ENCPlusHUDAnchor::TopRight:     return TEXT("TopRight");
		case ENCPlusHUDAnchor::CenterLeft:   return TEXT("CenterLeft");
		case ENCPlusHUDAnchor::Center:       return TEXT("Center");
		case ENCPlusHUDAnchor::CenterRight:  return TEXT("CenterRight");
		case ENCPlusHUDAnchor::BottomLeft:   return TEXT("BottomLeft");
		case ENCPlusHUDAnchor::BottomCenter: return TEXT("BottomCenter");
		case ENCPlusHUDAnchor::BottomRight:  return TEXT("BottomRight");
		default:                              return TEXT("Center");
	}
}

// =============================================================================
// Color hex parser
// =============================================================================

namespace NCPlusHUDColor
{
	static int32 HexCharVal(TCHAR C)
	{
		if (C >= TEXT('0') && C <= TEXT('9')) return C - TEXT('0');
		if (C >= TEXT('a') && C <= TEXT('f')) return 10 + (C - TEXT('a'));
		if (C >= TEXT('A') && C <= TEXT('F')) return 10 + (C - TEXT('A'));
		return -1;
	}

	bool TryParse(const FString& Hex, FLinearColor& Out)
	{
		FString S = Hex;
		S.Trim();
		if (S.StartsWith(TEXT("#"))) S = S.RightChop(1);
		if (S.Len() != 6 && S.Len() != 8) return false;

		int32 V[8];
		for (int32 i = 0; i < S.Len(); i++)
		{
			V[i] = HexCharVal(S[i]);
			if (V[i] < 0) return false;
		}

		const float R = (V[0] * 16 + V[1]) / 255.f;
		const float G = (V[2] * 16 + V[3]) / 255.f;
		const float B = (V[4] * 16 + V[5]) / 255.f;
		const float A = (S.Len() == 8) ? (V[6] * 16 + V[7]) / 255.f : 1.f;

		// Treat hex as sRGB-encoded (matches what users expect from web color pickers).
		Out = FLinearColor(FColor(
			FMath::RoundToInt(R * 255.f),
			FMath::RoundToInt(G * 255.f),
			FMath::RoundToInt(B * 255.f),
			FMath::RoundToInt(A * 255.f)));
		return true;
	}

	FString ToHexString(const FLinearColor& Color, bool bIncludeAlpha)
	{
		const FColor C = Color.ToFColor(true);
		if (bIncludeAlpha)
		{
			return FString::Printf(TEXT("#%02X%02X%02X%02X"), C.R, C.G, C.B, C.A);
		}
		return FString::Printf(TEXT("#%02X%02X%02X"), C.R, C.G, C.B);
	}
}

FLinearColor FNCPlusHUDElement::GetExtraColor(FName Key, const FLinearColor& Fallback) const
{
	const FString* V = Extras.Find(Key);
	if (!V) return Fallback;
	FLinearColor Out;
	return NCPlusHUDColor::TryParse(*V, Out) ? Out : Fallback;
}

float FNCPlusHUDElement::GetExtraFloat(FName Key, float Fallback) const
{
	const FString* V = Extras.Find(Key);
	if (!V || V->IsEmpty()) return Fallback;
	return FCString::Atof(**V);
}

bool FNCPlusHUDElement::GetExtraBool(FName Key, bool Fallback) const
{
	const FString* V = Extras.Find(Key);
	if (!V || V->IsEmpty()) return Fallback;
	FString S = *V;
	S.Trim();
	S = S.ToLower();
	if (S == TEXT("true")  || S == TEXT("1")) return true;
	if (S == TEXT("false") || S == TEXT("0")) return false;
	return Fallback;
}

// =============================================================================
// Find + WeaponBar side resolution
// =============================================================================

const FNCPlusHUDElement* FNCPlusHUDLayout::Find(FName ElementId) const
{
	return Elements.Find(ElementId);
}

FName FNCPlusHUDLayout::GetDefaultWeaponSide(UClass* WeaponClass)
{
	if (!WeaponClass) return TEXT("right");
	const FString N = WeaponClass->GetName().ToLower();
	// Hitscan-ish weapons go on the left. Catches both stock (UTWeap_Sniper,
	// LightningGun, ShockRifle, Enforcer) and our NetcodePlus subclasses
	// (UTPlusSniper, UTPlusShockRifle, etc.) by substring match.
	if (N.Contains(TEXT("sniper"))    ||
	    N.Contains(TEXT("lightning")) ||
	    N.Contains(TEXT("shock"))     ||
	    N.Contains(TEXT("enforcer")))
	{
		return TEXT("left");
	}
	return TEXT("right");
}

FName FNCPlusHUDLayout::GetWeaponSide(UClass* WeaponClass) const
{
	if (!WeaponClass) return TEXT("right");
	const FName ClassKey(*WeaponClass->GetName());
	if (const FName* Assigned = WeaponGroupAssignments.Find(ClassKey))
	{
		return *Assigned;
	}
	return GetDefaultWeaponSide(WeaponClass);
}

// =============================================================================
// JSON I/O
// =============================================================================

FString FNCPlusHUDLayout::GetDefaultLayoutPath()
{
	return FPaths::GameSavedDir() / TEXT("NetcodePlus") / TEXT("HUDLayout.json");
}

// Cached so the per-frame DrawHeldPowerups call never hits GConfig/FileExists (mirror
// GStockTeamPanelCache). -1 = unresolved. Invalidated by SetStockBottomBar (explicit
// change) and SaveLive (the FileExists fallback flips the first time a layout is saved).
static int8 GStockBottomBarCache = -1;

bool FNCPlusHUDLayout::WantsStockBottomBar()
{
	if (GStockBottomBarCache >= 0)
	{
		return GStockBottomBarCache != 0;
	}
	bool bResult;
	// Explicit choice in Mod.ini wins (written by the menu / Stock-Like preset).
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	FString Val;
	if (GConfig && GConfig->GetString(TEXT("NetcodePlus"), TEXT("StockBottomBar"), Val, ModIni) && !Val.IsEmpty())
	{
		bResult = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase) || Val.Equals(TEXT("1"));
	}
	else
	{
		// No explicit choice — default to the familiar stock bar for a fresh install
		// (no saved HUD layout). Anyone who has saved a layout keeps the NCPlus widgets.
		bResult = !FPaths::FileExists(GetDefaultLayoutPath());
	}
	GStockBottomBarCache = bResult ? 1 : 0;
	return bResult;
}

void FNCPlusHUDLayout::SetStockBottomBar(bool bStock)
{
	// Mirror WantsStockBottomBar's source: [NetcodePlus] StockBottomBar in the generated Mod.ini.
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	if (GConfig)
	{
		GConfig->SetString(TEXT("NetcodePlus"), TEXT("StockBottomBar"),
			bStock ? TEXT("True") : TEXT("False"), ModIni);
		GConfig->Flush(false, ModIni);
	}
	// Refresh the cache so the next WantsStockBottomBar() reflects the choice.
	GStockBottomBarCache = bStock ? 1 : 0;
}

// -----------------------------------------------------------------------------
// Stock team panel toggle + scoreboard opacity (mirror the StockBottomBar plumbing)
// -----------------------------------------------------------------------------

// Cached so the per-frame DrawHUD call never hits FileExists. -1 = unresolved.
static int8 GStockTeamPanelCache = -1;

bool FNCPlusHUDLayout::WantsStockTeamPanel()
{
	if (GStockTeamPanelCache >= 0)
	{
		return GStockTeamPanelCache != 0;
	}
	bool bResult;
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	FString Val;
	if (GConfig && GConfig->GetString(TEXT("NetcodePlus"), TEXT("StockTeamPanel"), Val, ModIni) && !Val.IsEmpty())
	{
		// Explicit choice in Mod.ini wins (written by the editor toggle).
		bResult = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase) || Val.Equals(TEXT("1"));
	}
	else
	{
		// No explicit choice — default to the stock roster for a fresh install (no
		// saved HUD layout). Anyone who has customized their HUD keeps the portraits.
		bResult = !FPaths::FileExists(GetDefaultLayoutPath());
	}
	GStockTeamPanelCache = bResult ? 1 : 0;
	return bResult;
}

void FNCPlusHUDLayout::SetStockTeamPanel(bool bStock)
{
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	if (GConfig)
	{
		GConfig->SetString(TEXT("NetcodePlus"), TEXT("StockTeamPanel"),
			bStock ? TEXT("True") : TEXT("False"), ModIni);
		GConfig->Flush(false, ModIni);
	}
	// Refresh the cache so the change applies on the very next DrawHUD frame
	// (the panel draws directly from this value — no widget swap needed).
	GStockTeamPanelCache = bStock ? 1 : 0;
}

static float GScoreboardOpacityCache = -1.f;

float FNCPlusHUDLayout::GetScoreboardOpacity()
{
	if (GScoreboardOpacityCache >= 0.f)
	{
		return GScoreboardOpacityCache;
	}
	float Result = 0.3f;  // prior hard-coded default
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	FString Val;
	if (GConfig && GConfig->GetString(TEXT("NetcodePlus"), TEXT("ScoreboardOpacity"), Val, ModIni) && !Val.IsEmpty())
	{
		Result = FCString::Atof(*Val);
	}
	Result = FMath::Clamp(Result, 0.05f, 1.f);
	GScoreboardOpacityCache = Result;
	return Result;
}

void FNCPlusHUDLayout::SetScoreboardOpacity(float Opacity)
{
	const float Clamped = FMath::Clamp(Opacity, 0.05f, 1.f);
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	if (GConfig)
	{
		GConfig->SetString(TEXT("NetcodePlus"), TEXT("ScoreboardOpacity"),
			*FString::SanitizeFloat(Clamped), ModIni);
		GConfig->Flush(false, ModIni);
	}
	GScoreboardOpacityCache = Clamped;
}

FNCPlusHUDLayout FNCPlusHUDLayout::LoadFromFile(const FString& Path)
{
	FNCPlusHUDLayout Layout;

	FString Raw;
	if (!FFileHelper::LoadFileToString(Raw, *Path))
	{
		// File missing — return empty layout. Not an error; means "use defaults everywhere".
		return Layout;
	}

	bool bOk = false;
	Layout = FromJsonString(Raw, bOk);
	if (!bOk)
	{
		UE_LOG(LogTemp, Warning, TEXT("[NCPlusHUDLayout] Failed to parse JSON at %s — falling back to defaults."), *Path);
		return FNCPlusHUDLayout();
	}
	UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] Loaded %d element(s), %d weapon group(s) from %s"),
		Layout.Elements.Num(), Layout.WeaponGroupAssignments.Num(), *Path);
	return Layout;
}

FNCPlusHUDLayout FNCPlusHUDLayout::FromJsonString(const FString& Raw, bool& bOutOk)
{
	bOutOk = false;
	FNCPlusHUDLayout Layout;

	if (Raw.IsEmpty()) return Layout;

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return Layout;   // bOutOk stays false
	}

	// Version (forward-compat: future loaders can branch on this)
	int32 Version = SchemaVersion;
	Root->TryGetNumberField(TEXT("version"), Version);
	Layout.Version = Version;

	const TSharedPtr<FJsonObject>* ElementsObjPtr = nullptr;
	if (!Root->TryGetObjectField(TEXT("elements"), ElementsObjPtr) || !ElementsObjPtr || !(*ElementsObjPtr).IsValid())
	{
		return Layout;
	}

	for (const auto& Pair : (*ElementsObjPtr)->Values)
	{
		const FName Key(*Pair.Key);
		const TSharedPtr<FJsonObject>* ElemObjPtr;
		if (!Pair.Value->TryGetObject(ElemObjPtr) || !ElemObjPtr || !(*ElemObjPtr).IsValid()) continue;
		const TSharedPtr<FJsonObject>& E = *ElemObjPtr;

		FNCPlusHUDElement Elem;

		FString AnchorStr;
		if (E->TryGetStringField(TEXT("anchor"), AnchorStr))
		{
			Elem.Anchor = ParseAnchor(AnchorStr);
		}

		double X = 0, Y = 0;
		E->TryGetNumberField(TEXT("offset_x"), X);
		E->TryGetNumberField(TEXT("offset_y"), Y);
		Elem.Offset = FVector2D(static_cast<float>(X), static_cast<float>(Y));

		double Scale = 1.0;
		if (E->TryGetNumberField(TEXT("scale"), Scale))
		{
			Elem.Scale = static_cast<float>(Scale);
		}

		bool bHidden = false;
		if (E->TryGetBoolField(TEXT("hidden"), bHidden))
		{
			Elem.bHidden = bHidden;
		}

		// Free-form extras (per-element settings like "style", colors, etc.).
		// We accept any string-valued top-level keys we don't already recognize.
		static const TSet<FString> KnownKeys = { TEXT("anchor"), TEXT("offset_x"), TEXT("offset_y"), TEXT("scale"), TEXT("hidden") };
		for (const auto& Field : E->Values)
		{
			if (KnownKeys.Contains(Field.Key)) continue;
			FString StrVal;
			if (Field.Value.IsValid() && Field.Value->TryGetString(StrVal))
			{
				Elem.Extras.Add(FName(*Field.Key), StrVal);
			}
		}

		Layout.Elements.Add(Key, Elem);
	}

	// weapon_groups: top-level object mapping class short-name → "left"/"right".
	const TSharedPtr<FJsonObject>* WGObjPtr = nullptr;
	if (Root->TryGetObjectField(TEXT("weapon_groups"), WGObjPtr) && WGObjPtr && (*WGObjPtr).IsValid())
	{
		for (const auto& Pair : (*WGObjPtr)->Values)
		{
			FString Side;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Side))
			{
				FString SideLow = Side;
				SideLow = SideLow.ToLower();
				if (SideLow == TEXT("left") || SideLow == TEXT("right"))
				{
					Layout.WeaponGroupAssignments.Add(FName(*Pair.Key), FName(*SideLow));
				}
			}
		}
	}

	bOutOk = true;
	return Layout;
}

FString FNCPlusHUDLayout::ToJsonString() const
{
	TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject);
	Root->SetNumberField(TEXT("version"), Version);

	TSharedRef<FJsonObject> ElementsObj = MakeShareable(new FJsonObject);
	for (const auto& Pair : Elements)
	{
		const FNCPlusHUDElement& E = Pair.Value;
		TSharedRef<FJsonObject> EObj = MakeShareable(new FJsonObject);
		EObj->SetStringField(TEXT("anchor"),    AnchorToString(E.Anchor));
		EObj->SetNumberField(TEXT("offset_x"),  E.Offset.X);
		EObj->SetNumberField(TEXT("offset_y"),  E.Offset.Y);
		EObj->SetNumberField(TEXT("scale"),     E.Scale);
		EObj->SetBoolField  (TEXT("hidden"),    E.bHidden);
		// Round-trip extras as string fields next to the known keys.
		for (const auto& X : E.Extras)
		{
			EObj->SetStringField(X.Key.ToString(), X.Value);
		}
		ElementsObj->SetObjectField(Pair.Key.ToString(), EObj);
	}
	Root->SetObjectField(TEXT("elements"), ElementsObj);

	// weapon_groups
	if (WeaponGroupAssignments.Num() > 0)
	{
		TSharedRef<FJsonObject> WGObj = MakeShareable(new FJsonObject);
		for (const auto& Pair : WeaponGroupAssignments)
		{
			WGObj->SetStringField(Pair.Key.ToString(), Pair.Value.ToString());
		}
		Root->SetObjectField(TEXT("weapon_groups"), WGObj);
	}

	FString Out;
	TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer
		= TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);
	return Out;
}

bool FNCPlusHUDLayout::SaveToFile(const FString& Path) const
{
	const FString Out = ToJsonString();
	if (Out.IsEmpty()) return false;

	// Ensure target directory exists.
	const FString Dir = FPaths::GetPath(Path);
	IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);

	return FFileHelper::SaveStringToFile(Out, *Path);
}

// =============================================================================
// Aliases — table of (alias, class path, display name) in stable order.
// =============================================================================

namespace NCPlusHUDAliases
{
	struct FAliasEntry
	{
		FName    Alias;
		FString  ClassPath;     // empty for draw-call elements
		FText    DisplayName;
		bool     bIsDrawCall;   // true → no widget; HUD's DrawHUD consults layout directly
		ENCPlusHUDAnchor StockAnchor;  // visual default — what to show in editor when no override exists
		FVector2D StockOffset;         // default offset paired with StockAnchor (design pixels)

		// Explicit constructors so brace-init still works in UE4 4.15 (which
		// can't deduce aggregate-init when a member has a default initializer).
		FAliasEntry(FName InAlias, FString InPath, FText InName, bool bInDrawCall = false,
		            ENCPlusHUDAnchor InStockAnchor = ENCPlusHUDAnchor::Center,
		            FVector2D InStockOffset = FVector2D::ZeroVector)
			: Alias(InAlias), ClassPath(MoveTemp(InPath)), DisplayName(MoveTemp(InName))
			, bIsDrawCall(bInDrawCall), StockAnchor(InStockAnchor), StockOffset(InStockOffset)
		{}
	};

	static const TArray<FAliasEntry>& GetAliasTable()
	{
		static const TArray<FAliasEntry> Table = []()
		{
			TArray<FAliasEntry> T;
			// Display order = list-view order in the editor. Group by region.
			// 5th arg = stock anchor — the anchor that visually matches where the
			// element renders out-of-the-box. Used by editor for combo display when
			// no layout override exists, so users see truthful state.
			// 6th arg = StockOffset (design pixels), paired with StockAnchor.
			// These mirror the constructor-set Position so that the editor's X/Y
			// boxes show the actual visual default (not 0/0) and so that creating
			// a fresh entry by tweaking one field doesn't visually jump the widget.
			T.Emplace(TEXT("hp_armor"),         TEXT("/Script/NetcodePlus.NCPlusHUDWidget_QuickStats"),                          FText::FromString(TEXT("Health & Armor")),     false, ENCPlusHUDAnchor::BottomCenter, FVector2D(0.f, -180.f));
			T.Emplace(TEXT("ammo"),             TEXT("/Script/NetcodePlus.NCPlusHUDWidget_AmmoCounter"),                         FText::FromString(TEXT("Ammo Counter")),       false, ENCPlusHUDAnchor::BottomRight, FVector2D(-20.f, -20.f));
			T.Emplace(TEXT("weapon_bar_left"),  TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Left"),                      FText::FromString(TEXT("Weapon Bar (Left)")), false, ENCPlusHUDAnchor::CenterLeft,  FVector2D( 20.f, -20.f));
			T.Emplace(TEXT("weapon_bar_right"), TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Right"),                     FText::FromString(TEXT("Weapon Bar (Right)")), false, ENCPlusHUDAnchor::CenterRight, FVector2D(-20.f, -20.f));
			T.Emplace(TEXT("weapon_crosshair"), TEXT("/Script/UnrealTournament.UTHUDWidget_WeaponCrosshair"),                    FText::FromString(TEXT("Crosshair")),          false, ENCPlusHUDAnchor::Center);
			T.Emplace(TEXT("powerups"),         TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Powerups.bpHW_Powerups_C"),      FText::FromString(TEXT("Powerups")),           false, ENCPlusHUDAnchor::BottomLeft, FVector2D(20.f, -20.f));
			// StockAnchor is TopLeft because that IS the stock base: AUTHUD::DrawHUD
			// stomps KillIconWidget->ScreenPosition to (0,0) every frame (UTHUD.cpp:842-845)
			// — the widget never actually sat at TopRight. The old TopRight seed made the
			// first drag jump a full screen width once anchors take real effect (see the
			// killfeed special-case in ApplyLayoutToWidgets).
			T.Emplace(TEXT("killfeed"),         TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpWH_KillIconMessages.bpWH_KillIconMessages_C"), FText::FromString(TEXT("Killfeed")), false, ENCPlusHUDAnchor::TopLeft);
			T.Emplace(TEXT("spectator"),        TEXT("/Script/UnrealTournament.UTHUDWidget_Spectator"),                          FText::FromString(TEXT("Spectator Score / KDA")), false, ENCPlusHUDAnchor::TopRight);
			T.Emplace(TEXT("announcements"),    TEXT("/Script/UnrealTournament.UTHUDWidgetAnnouncements"),                       FText::FromString(TEXT("Announcements")),      false, ENCPlusHUDAnchor::TopCenter);
			T.Emplace(TEXT("console_msgs"),     TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_ConsoleMessages"),             FText::FromString(TEXT("Console Messages")),   false, ENCPlusHUDAnchor::BottomLeft);
			T.Emplace(TEXT("voice_status"),     TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_VoiceChatStatus"),             FText::FromString(TEXT("Voice Chat Status")),  false, ENCPlusHUDAnchor::TopCenter);
			// CTF flag status — the top-of-screen team flag silhouettes (DrawFlagStatus).
			// IMPORTANT: this is a DRAW-CALL alias (empty ClassPath), NOT a widget alias.
			// The NCPlus subclass UNCPlusHUDWidget_CTFFlagStatus hosts THREE independent
			// pieces on one widget — silhouettes (this alias), the carrier indicator
			// (ctf_carrier_indicator), and the banners (ctf_you_have_flag /
			// ctf_enemy_has_flag). When this mapped to the widget CLASS, hiding the
			// silhouettes made ApplyLayoutToWidgets SetHidden the entire widget, which
			// silently killed the carrier indicator AND both banners too (the render
			// loop skips hidden widgets before any Draw runs). As a draw-call alias the
			// widget is never hidden by layout; the subclass's DrawFlagStatus override
			// honors this alias's hidden flag to drop ONLY the silhouettes.
			T.Emplace(TEXT("ctf_flag_status"),  FString(),                                                                  FText::FromString(TEXT("CTF Flag Silhouettes")), true, ENCPlusHUDAnchor::TopCenter);
			// Three CTF sub-element aliases routed through the NCPlus subclass's
			// Draw* overrides (NOT through ApplyLayoutToWidgets). Empty ClassPath
			// keeps GetAliasForClass from colliding with ctf_flag_status above;
			// the subclass reads each entry directly from FNCPlusHUDLayout::GetLive().
			//
			// World-projected flag icon over the enemy carrier's head. Anchor
			// nominal (world projection, not screen-anchored). Offset shifts the
			// icon in screen pixels AFTER world projection.
			T.Emplace(TEXT("ctf_carrier_indicator"), FString(),                                                                  FText::FromString(TEXT("CTF Carrier Indicator")), true, ENCPlusHUDAnchor::Center);
			// "You have the flag!" banner (yellow, pulsing). Engine already drew
			// this; we expose position + scale + color via nchud. BottomCenter
			// matches the stock engine placement.
			T.Emplace(TEXT("ctf_you_have_flag"),     FString(),                                                                  FText::FromString(TEXT("CTF \"You Have the Flag\"")), true, ENCPlusHUDAnchor::BottomCenter, FVector2D(0.f, -120.f));
			// "The enemy has your flag, recover it!" banner (red, pulsing). NEW
			// in NCPlus - the engine had the FText defined but never rendered it.
			// UT99-style persistent alarm for audio-announcement-off players.
			// TopCenter with a pixel offset below the scorebar so the score and
			// alarm read as a single top cluster.
			T.Emplace(TEXT("ctf_enemy_has_flag"),    FString(),                                                                  FText::FromString(TEXT("CTF \"Enemy Has Your Flag\"")), true, ENCPlusHUDAnchor::TopCenter, FVector2D(0.f, 110.f));
			// Engine crosshair flag-grab flash: the team-colored flag that balloons
			// over the crosshair for 3s after YOU grab a flag. It's drawn by the
			// stock UTHUDWidget_WeaponCrosshair (which NCPlusCTFHUD does NOT subclass)
			// off AUTHUD::LastFlagGrabTime — entirely separate from the flag-status
			// widget above. Default OFF: NCPlusCTFHUD::DrawHUD suppresses it unless
			// this entry exists and is visible. On/off only — the flash is engine-
			// drawn at a fixed crosshair offset, so offset/scale/color aren't honored.
			T.Emplace(TEXT("crosshair_flag_grab"),   FString(),                                                                  FText::FromString(TEXT("CTF Grab Flash (crosshair)")), true, ENCPlusHUDAnchor::Center);
			// C++-drawn pieces (Phase 3.5).
			// Portrait strips: TopCenter anchor with offsets that approximately
			// match where the renderers' StockXRed / StockXBlue fallback puts them
			// (~0.4 / 0.6 of canvas width). Frames in drag-view + first-edit
			// seeding both use these — without distinct offsets, both frames
			// would stack at the same screen point and the user couldn't tell
			// them apart in the visual editor.
			T.Emplace(TEXT("portrait_red"),     FString(),                                                                       FText::FromString(TEXT("Portraits (Red)")),    true,  ENCPlusHUDAnchor::TopCenter, FVector2D(-200.f, 30.f));
			T.Emplace(TEXT("portrait_blue"),    FString(),                                                                       FText::FromString(TEXT("Portraits (Blue)")),   true,  ENCPlusHUDAnchor::TopCenter, FVector2D( 200.f, 30.f));
			// Stock top-left team roster (alternative to the portrait strip; see
			// NCPlusHUDDrawCall::DrawStockTeamPanel). Draw-call alias → movable, with
			// Scale / Opacity / Hide honored. Default top-left with a small inset.
			T.Emplace(TEXT("team_panel"),       FString(),                                                                       FText::FromString(TEXT("Team Panel (Stock Roster)")), true, ENCPlusHUDAnchor::TopLeft, FVector2D(16.f, 12.f));
			// Full-screen tint when the local pawn takes damage. Polls Health+Armor
			// each frame; on a decrease, stamps the time and tints the screen for
			// `flash_duration` seconds. Extras: color_text (tint color, default red),
			// flash_duration (seconds, default 0.30), opacity (multiplier, default 1.0),
			// hidden. Anchor/offset/scale ignored — the draw covers the entire viewport.
			T.Emplace(TEXT("damage_flash"),     FString(),                                                                       FText::FromString(TEXT("Damage Flash (screen tint)")), true, ENCPlusHUDAnchor::Center);
			// Small server identification line (server name from GameState->ServerName).
			// Default OFF: no layout entry = no draw, so it stays invisible until the
			// user opts in by adding the entry + unchecking Hide. Useful for streamers
			// who want server attribution baked into clips. Honors font / font_scale /
			// color_text / opacity.
			T.Emplace(TEXT("server_info"),      FString(),                                                                       FText::FromString(TEXT("Server Name Plate")),  true,  ENCPlusHUDAnchor::TopLeft,     FVector2D(20.f, 14.f));
			T.Emplace(TEXT("scorebar"),         FString(),                                                                       FText::FromString(TEXT("Score Bar / Clock")),  true,  ENCPlusHUDAnchor::TopCenter);
			// Top-right "Score: N" + "KDA: K/D/A" mini panel. Drawn inline by
			// ElimPlusHUD::DrawHUD and WipeoutHUD::DrawHUD. Default offset
			// approximates the original hard-coded (ClipX*0.98, ClipY*0.015).
			T.Emplace(TEXT("score_kda"),        FString(),                                                                       FText::FromString(TEXT("Score / KDA Mini")),   true,  ENCPlusHUDAnchor::TopRight,    FVector2D(-40.f, 16.f));
			// Game-mode-specific draw calls.
			T.Emplace(TEXT("shockdom_controls"),FString(),                                                                       FText::FromString(TEXT("ShockDom A/B/C Indicators")), true, ENCPlusHUDAnchor::TopCenter, FVector2D(0.f, 78.f));
			// Live-accuracy widget for NCShaftArena (and any other mode that opts
			// in by listing the class in its HudWidgetClasses). Default sits at
			// bottom-right above the ammo counter (which lives at BottomRight
			// (-107, -46) in stock layout) so the eye doesn't have to leave the
			// HP/Armor/Ammo cluster to see live aim feedback.
			T.Emplace(TEXT("accuracy"),         TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Accuracy"),                            FText::FromString(TEXT("Accuracy Widget")),    false, ENCPlusHUDAnchor::BottomRight, FVector2D(-20.f, 0.f));
			// Candy pickup marker (ElimPlus-only): BP widget that draws through-
			// wall world-space markers for orbs dropped on player death. Anchor
			// is nominal - the widget paints at world-projected screen positions,
			// not at the alias's Position/Offset. The Hide toggle still works.
			T.Emplace(TEXT("candy_marker"),     TEXT("/Game/Blueprints/ElimPlusStuff/CandyPickupMarker.CandyPickupMarker_C"), FText::FromString(TEXT("Candy Pickup Marker")), false, ENCPlusHUDAnchor::Center);
			// Default-hidden opt-in widgets. Stock anchor + offset mirror each
			// widget's constructor defaults so the editor's first edit doesn't
			// visibly jump the element when seeded.
			T.Emplace(TEXT("speedometer"),      TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Speedometer"),                         FText::FromString(TEXT("Speedometer")),        false, ENCPlusHUDAnchor::Center,      FVector2D(0.f, 80.f));
			T.Emplace(TEXT("minimap"),          TEXT("/Script/NetcodePlus.NCPlusHUDWidget_Minimap"),                             FText::FromString(TEXT("Minimap")),            false, ENCPlusHUDAnchor::TopLeft,     FVector2D(20.f, 20.f));
			T.Emplace(TEXT("heal_ability"),     TEXT("/Script/NetcodePlus.NCPlusHUDWidget_HealAbility"),                         FText::FromString(TEXT("Heal Ability Bind")),  false, ENCPlusHUDAnchor::BottomCenter, FVector2D(635.f, -52.f));
			return T;
		}();
		return Table;
	}

	FString GetClassPath(FName Alias)
	{
		for (const FAliasEntry& E : GetAliasTable())
		{
			if (E.Alias == Alias) return E.ClassPath;
		}
		return FString();
	}

	FName GetAliasForClass(UClass* Cls)
	{
		if (!Cls) return NAME_None;
		const FString ClassPath = Cls->GetPathName();
		for (const FAliasEntry& E : GetAliasTable())
		{
			if (E.ClassPath.Equals(ClassPath, ESearchCase::IgnoreCase)) return E.Alias;
		}
		return NAME_None;
	}

	TArray<FName> GetAllAliases()
	{
		TArray<FName> Out;
		Out.Reserve(GetAliasTable().Num());
		for (const FAliasEntry& E : GetAliasTable()) Out.Add(E.Alias);
		return Out;
	}

	FText GetDisplayName(FName Alias)
	{
		for (const FAliasEntry& E : GetAliasTable())
		{
			if (E.Alias == Alias) return E.DisplayName;
		}
		return FText::FromName(Alias);
	}

	ENCPlusHUDAnchor GetStockAnchor(FName Alias)
	{
		for (const FAliasEntry& E : GetAliasTable())
		{
			if (E.Alias == Alias) return E.StockAnchor;
		}
		return ENCPlusHUDAnchor::Center;
	}

	FVector2D GetStockOffset(FName Alias)
	{
		for (const FAliasEntry& E : GetAliasTable())
		{
			if (E.Alias == Alias) return E.StockOffset;
		}
		return FVector2D::ZeroVector;
	}
}

// =============================================================================
// C++-drawn piece helpers (Phase 3.5)
// =============================================================================

namespace NCPlusHUDDrawCall
{
	// ---- Outlined-text + fitted-name helpers -------------------------------------
	// Collapse the hand-rolled "4 offset DrawText + 1 fill, all shadowed" stacks (each
	// UCanvas::DrawText paid a full ICU word-wrap + allocs, x2 for the shadow) into one
	// batched FCanvasTextItem, and cache the per-frame name-fit StrLen loops per player.

	void DrawOutlinedText(UCanvas* Canvas, const UFont* Font, const FText& Text,
		float X, float Y, float Scale, FLinearColor Fill, FLinearColor Outline, float Opacity)
	{
		if (!Canvas || !Font || Text.IsEmpty()) return;
		Fill.A    *= Opacity;
		Outline.A *= Opacity;
		FCanvasTextItem Item(FVector2D(X, Y), Text, Font, Fill);
		Item.Scale        = FVector2D(Scale, Scale);
		Item.bOutlined    = true;   // 4 diagonal ±1px passes, one batched item, no re-wrap
		Item.OutlineColor = Outline;
		// Deliberately NO EnableShadow(): the outline replaces the old drop shadow, and a
		// shadow would double-raster every one of the 5 passes — the cost we're removing.
		Canvas->DrawItem(Item);
	}

	// Per-PlayerState fitted-name cache. Each entry remembers the inputs it was built
	// from and is recomputed only when one changes; the whole map is dropped on a world
	// change (level transition) or a layout-revision bump so nothing goes stale.
	namespace
	{
		struct FNCFittedNameEntry
		{
			FString      Src;
			const UFont* Font     = nullptr;
			int32        WidthKey = -1;
			FText        Text;
			float        Width  = 0.f;
			float        Height = 0.f;
		};
		TMap<TWeakObjectPtr<APlayerState>, FNCFittedNameEntry> GNameFitCache;
		uint32                 GNameFitRev   = 0;
		TWeakObjectPtr<UWorld> GNameFitWorld;
	}

	void ResolveFittedName(UCanvas* Canvas, APlayerState* PS, UFont* Font,
		const FString& Src, float MaxWidthPx, float Scale,
		FText& OutText, float& OutWidth, float& OutHeight)
	{
		// Degenerate inputs — measure once (uncached) and bail.
		if (!Canvas || !Font || !PS || Scale <= 0.f)
		{
			float XL = 0.f, YL = 0.f;
			if (Canvas && Font) Canvas->StrLen(Font, Src, XL, YL);
			OutText = FText::FromString(Src); OutWidth = XL; OutHeight = YL;
			return;
		}

		// Drop the whole cache on a world change or a layout edit.
		UWorld* World = PS->GetWorld();
		const uint32 Rev = FNCPlusHUDLayout::GetLiveRevision();
		if (Rev != GNameFitRev || GNameFitWorld.Get() != World)
		{
			GNameFitCache.Reset();
			GNameFitRev   = Rev;
			GNameFitWorld = World;
		}

		// The fit depends only on (Src, Font, MaxWidthPx/Scale) — bucket the ratio so
		// tiny float jitter doesn't thrash the cache.
		const int32 WidthKey = FMath::RoundToInt(MaxWidthPx / Scale);
		FNCFittedNameEntry& E = GNameFitCache.FindOrAdd(PS);
		if (E.Font != Font || E.WidthKey != WidthKey || E.Src != Src)
		{
			FString Text = Src;
			float XL = 0.f, YL = 0.f;
			Canvas->StrLen(Font, Text, XL, YL);
			while (XL * Scale > MaxWidthPx && Text.Len() > 3)
			{
				Text = Text.Left(Text.Len() - 1);
				Canvas->StrLen(Font, Text, XL, YL);
			}
			E.Src      = Src;
			E.Font     = Font;
			E.WidthKey = WidthKey;
			E.Text     = FText::FromString(Text);
			E.Width    = XL;
			E.Height   = YL;
		}
		OutText   = E.Text;
		OutWidth  = E.Width;
		OutHeight = E.Height;
	}

	FVector2D ResolveScreenPos(FName Alias, UCanvas* Canvas, const FVector2D& Fallback)
	{
		if (!Canvas) return Fallback;
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		if (!E) return Fallback;

		const FVector2D Anchor = FNCPlusHUDLayout::AnchorToScreenCoords(E->Anchor);
		const float RenderScale = Canvas->ClipY / 1080.f;
		return FVector2D(
			Anchor.X * Canvas->ClipX + E->Offset.X * RenderScale,
			Anchor.Y * Canvas->ClipY + E->Offset.Y * RenderScale
		);
	}

	bool IsHidden(FName Alias)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		return E && E->bHidden;
	}

	float GetScale(FName Alias)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		return E ? FMath::Max(E->Scale, 0.01f) : 1.f;
	}

	float GetOpacity(FName Alias)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		return E ? FMath::Clamp(E->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f) : 1.f;
	}

	bool GetUseTeamColor(FName Alias)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		return E ? E->GetExtraBool(TEXT("use_team_color"), true) : true;
	}

	ENCPlusHUDAnchor GetEffectiveAnchor(FName Alias)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		return E ? E->Anchor : NCPlusHUDAliases::GetStockAnchor(Alias);
	}

	// =============================================================================
	// Damage flash
	// =============================================================================
	//
	// Polls the local pawn's (Health + Armor) sum each frame and stamps a flash
	// time on a decrease. Static state is single-client safe — UT4 has no
	// split-screen and the HUD only ever runs for the local client. Resets the
	// cached HP when the world changes (map travel / PIE re-load) so the first
	// frame of a new match doesn't false-trigger.
	void DrawDamageFlash(AUTHUD* HUD, UCanvas* Canvas)
	{
		if (HUD == nullptr || Canvas == nullptr || HUD->UTPlayerOwner == nullptr)
		{
			return;
		}

		static TWeakObjectPtr<UWorld> SCachedWorld;
		static int32 SLastHPSum = -1;
		static float SLastFlashTime = -1.f;

		UWorld* World = HUD->GetWorld();
		if (SCachedWorld.Get() != World)
		{
			SCachedWorld = World;
			SLastHPSum = -1;
			SLastFlashTime = -1.f;
		}

		// Read pawn HP+Armor. Spectators / dead = no pawn = reset cache and bail.
		AUTCharacter* MyChar = Cast<AUTCharacter>(HUD->UTPlayerOwner->GetViewTarget());
		if (MyChar == nullptr || MyChar->IsDead())
		{
			SLastHPSum = -1;
			return;
		}
		const int32 NowHPSum = MyChar->Health + MyChar->GetArmorAmount();
		if (SLastHPSum >= 0 && NowHPSum < SLastHPSum && World != nullptr)
		{
			SLastFlashTime = World->GetTimeSeconds();
		}
		SLastHPSum = NowHPSum;

		// Layout consult. No entry / hidden = feature off; cache continues to
		// track HP so toggling on mid-match doesn't false-flash from a stale baseline.
		const FNCPlusHUDElement* Elem = FNCPlusHUDLayout::GetLive().Find(TEXT("damage_flash"));
		if (Elem == nullptr || Elem->bHidden || SLastFlashTime < 0.f || World == nullptr)
		{
			return;
		}

		const float Duration = FMath::Max(0.05f, Elem->GetExtraFloat(TEXT("flash_duration"), 0.30f));
		const float Elapsed  = World->GetTimeSeconds() - SLastFlashTime;
		if (Elapsed >= Duration)
		{
			return;
		}

		const FLinearColor TintColor = Elem->GetExtraColor(TEXT("color_text"), FLinearColor(1.f, 0.f, 0.f, 1.f));
		const float OpacityMul       = FMath::Clamp(Elem->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f);
		// Linear fade from full alpha at t=0 down to 0 at t=Duration. Cap the peak
		// at the tint color's own alpha so an "almost transparent red" stays subtle.
		const float Alpha = FMath::Clamp(TintColor.A, 0.f, 1.f) * OpacityMul * (1.f - Elapsed / Duration);
		if (Alpha <= 0.001f)
		{
			return;
		}

		Canvas->SetLinearDrawColor(FLinearColor(TintColor.R, TintColor.G, TintColor.B, Alpha));
		Canvas->DrawTile(Canvas->DefaultTexture, 0.f, 0.f, Canvas->ClipX, Canvas->ClipY, 0.f, 0.f, 1.f, 1.f, BLEND_Translucent);
	}

	// =============================================================================
	// Stock team panel — top-left team roster (alternative to the portrait strip)
	// =============================================================================
	//
	// Red row then blue row of slanted, team-colored plates. Teammate plates show
	// name + "+HP  armor"; enemy plates show the name only (no live enemy HP) —
	// EXCEPT once the round is over (reveal the winners' final health) and for
	// TRUE spectators (bOnlySpectator), who see both teams' vitals at all times.
	// Dead players are skipped so the row reflows — plate count = alive count.
	// Same data source as the portrait strip (teammate Health/GetArmorAmount);
	// only the look differs. Honors the `team_panel` alias (position / scale /
	// opacity / hide).
	void DrawStockTeamPanel(AUTHUD* HUD, UCanvas* Canvas)
	{
		if (HUD == nullptr || Canvas == nullptr) return;
		if (IsHidden(TEXT("team_panel"))) return;

		UWorld* World = HUD->GetWorld();
		if (!World) return;
		AUTGameState* GS = World->GetGameState<AUTGameState>();
		if (!GS) return;

		// Font + FontSz come from the `team_panel` nchud alias (Font dropdown + FontSz
		// slider in the editor), falling back to the stock HUD fonts when unset.
		UFont* NameFont  = NCPlusHUDFonts::Resolve(TEXT("team_panel"), HUD, HUD->SmallFont);
		if (!NameFont) NameFont = HUD->SmallFont;
		UFont* StatFont  = NameFont;
		UFont* ScoreFont = NCPlusHUDFonts::Resolve(TEXT("team_panel"), HUD, HUD->MediumFont ? HUD->MediumFont : HUD->SmallFont);
		if (!ScoreFont) ScoreFont = HUD->MediumFont ? HUD->MediumFont : HUD->SmallFont;
		if (!NameFont) return;

		// Local viewer's team — teammates get an HP/armor readout, enemies name-only.
		// Vitals are revealed for EVERYONE when the round is over (this panel only
		// draws in InProgress/RoundCooldown, so != InProgress IS the round-win
		// window — show the winners' final health) and for TRUE spectators
		// (bOnlySpectator; deliberately NOT bOutOfLives — a dead player must not
		// gain live enemy info mid-round).
		uint8 MyTeam = 255;
		bool  bRevealAllVitals = (GS->GetMatchState() != MatchState::InProgress);
		if (HUD->UTPlayerOwner)
		{
			if (AUTPlayerState* MyPS = Cast<AUTPlayerState>(HUD->UTPlayerOwner->PlayerState))
			{
				MyTeam = MyPS->GetTeamNum();
				bRevealAllVitals |= MyPS->bOnlySpectator;
			}
		}

		const float RenderScale = float(Canvas->SizeX) / 1920.0f;
		const float PanelScale  = GetScale(TEXT("team_panel"));
		const float S  = FMath::Max(0.2f, RenderScale * PanelScale);
		const float Op = FMath::Clamp(GetOpacity(TEXT("team_panel")), 0.f, 1.f);

		// Geometry (design px * S).
		const float PlateW = 150.f * S;
		const float PlateH = 30.f  * S;
		const float Skew   = 10.f  * S;
		const float Gap    = 4.f   * S;
		const float ScoreW = 34.f  * S;
		const float RowGap = 6.f   * S;
		const float Pitch  = PlateW + Gap;

		const FVector2D Origin = ResolveScreenPos(TEXT("team_panel"), Canvas,
			FVector2D(16.f * RenderScale, 12.f * RenderScale));

		const float FontExtra  = NCPlusHUDFonts::ResolveScale(TEXT("team_panel"), 1.f);
		const float NameScale  = (float(Canvas->SizeY) / 1080.0f) * 0.42f * PanelScale * FontExtra;
		const float StatScale  = (float(Canvas->SizeY) / 1080.0f) * 0.42f * PanelScale * FontExtra;
		const float ScoreScale = (float(Canvas->SizeY) / 1080.0f) * 0.65f * PanelScale * FontExtra;

		// White texture for the slanted plate fills (DrawTile is axis-aligned, so the
		// slant needs triangle items).
		FTexture* WhiteTex = (Canvas->DefaultTexture) ? Canvas->DefaultTexture->Resource : nullptr;
		if (!WhiteTex) return;

		// Two accumulation passes → one batched draw. Collect every slanted plate quad
		// as triangles and every label as deferred text, then flush the quads in ONE
		// DrawItem (was 4 translucent triangle items PER plate) and draw all text on top.
		// Batching is visually identical: plates never overlap a neighbour's text, so
		// "all fills, then all text" preserves z-order.
		TArray<FCanvasUVTri> PlateTris;
		struct FPanelLabel { UFont* Font; FText Text; float X; float Y; float Scale; FLinearColor Color; };
		TArray<FPanelLabel> Labels;

		auto AddQuad = [&](const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D, FLinearColor Col)
		{
			Col.A *= Op;
			FCanvasUVTri T1;
			T1.V0_Pos = A; T1.V1_Pos = B; T1.V2_Pos = C;
			T1.V0_UV = T1.V1_UV = T1.V2_UV = FVector2D::ZeroVector;   // white 1x1 tex — UV irrelevant
			T1.V0_Color = T1.V1_Color = T1.V2_Color = Col;
			PlateTris.Add(T1);
			FCanvasUVTri T2;
			T2.V0_Pos = A; T2.V1_Pos = C; T2.V2_Pos = D;
			T2.V0_UV = T2.V1_UV = T2.V2_UV = FVector2D::ZeroVector;
			T2.V0_Color = T2.V1_Color = T2.V2_Color = Col;
			PlateTris.Add(T2);
		};

		// Center a SHORT label on (CenterX, CenterY) and queue it. StrLen is fine here —
		// these are 2-5 char score/clock/HP/armor strings, not the long player names
		// (those go through the cached ResolveFittedName below).
		auto PushCentered = [&](UFont* Font, const FString& Text, float CenterX, float CenterY, float Scale, const FLinearColor& Color)
		{
			if (!Font || Text.IsEmpty()) return;
			float XL, YL;
			Canvas->StrLen(Font, Text, XL, YL);
			Labels.Add(FPanelLabel{ Font, FText::FromString(Text),
				CenterX - XL * Scale * 0.5f, CenterY - YL * Scale * 0.5f, Scale, Color });
		};

		for (int32 TeamIdx = 0; TeamIdx < 2; ++TeamIdx)
		{
			const float RowY = Origin.Y + TeamIdx * (PlateH + RowGap);

			// Honor the panel's OWN Team-Color toggle (parity with the portraits, which
			// each read their own alias): custom TeamSkins color when on, stock red/blue
			// when off. Defaults ON (GetUseTeamColor defaults true); toggle in nchud.
			FLinearColor TeamCol = (TeamIdx == 1)
				? FLinearColor(0.05f, 0.10f, 0.90f, 1.f)
				: FLinearColor(0.80f, 0.05f, 0.05f, 1.f);
			if (GetUseTeamColor(TEXT("team_panel")) && GS->Teams.IsValidIndex(TeamIdx) && GS->Teams[TeamIdx])
			{
				TeamCol = GS->Teams[TeamIdx]->TeamColor;
			}
			// Muted dark plate (was full-saturation team color — too bright/glary behind
			// the white names). Body darkened; the team hue lives in a thin top accent.
			const FLinearColor PlateCol(TeamCol.R * 0.5f, TeamCol.G * 0.5f, TeamCol.B * 0.5f, 0.92f);
			const FLinearColor ScoreBoxCol(TeamCol.R * 0.33f, TeamCol.G * 0.33f, TeamCol.B * 0.33f, 0.95f);
			const FLinearColor EdgeCol(TeamCol.R, TeamCol.G, TeamCol.B, 0.8f);

			// Score box (slanted), with the team score.
			{
				const float x = Origin.X;
				const FVector2D A(x + Skew, RowY), B(x + Skew + ScoreW, RowY), C(x + ScoreW, RowY + PlateH), D(x, RowY + PlateH);
				AddQuad(A, B, C, D, ScoreBoxCol);
				const int32 TeamScore = (GS->Teams.IsValidIndex(TeamIdx) && GS->Teams[TeamIdx]) ? GS->Teams[TeamIdx]->Score : 0;
				PushCentered(ScoreFont, FString::Printf(TEXT("%d"), TeamScore),
					x + Skew * 0.5f + ScoreW * 0.5f, RowY + PlateH * 0.5f, ScoreScale, FLinearColor::White);
			}

			float PlateX = Origin.X + ScoreW + Gap;

			for (APlayerState* PSBase : GS->PlayerArray)
			{
				AUTPlayerState* PS = Cast<AUTPlayerState>(PSBase);
				if (!PS || PS->bOnlySpectator || PS->bIsInactive) continue;
				if (PS->GetTeamNum() != TeamIdx) continue;

				// Alive check — controller's pawn on the server, GetUTCharacter()
				// fallback on clients (GetOwner() is null for remote player states).
				// Same logic as the portrait strip; dead/unknown players are skipped
				// so the row reflows (plate count = alive count).
				AUTCharacter* UTC = nullptr;
				if (AController* Ctrl = Cast<AController>(PS->GetOwner())) { UTC = Cast<AUTCharacter>(Ctrl->GetPawn()); }
				else                                                        { UTC = PS->GetUTCharacter(); }
				if (!UTC || UTC->IsDead()) continue;

				const float x = PlateX;
				const FVector2D A(x + Skew, RowY), B(x + Skew + PlateW, RowY), C(x + PlateW, RowY + PlateH), D(x, RowY + PlateH);
				AddQuad(A, B, C, D, PlateCol);
				// Thin lighter top edge for definition.
				AddQuad(FVector2D(x + Skew, RowY), FVector2D(x + Skew + PlateW, RowY),
					FVector2D(x + Skew + PlateW, RowY + 2.f * S), FVector2D(x + Skew, RowY + 2.f * S), EdgeCol);

				const float CenterX  = x + Skew * 0.5f + PlateW * 0.5f;
				const bool  bShowVitals = (TeamIdx == MyTeam) || bRevealAllVitals;

				if (bShowVitals)
				{
					// Name on top (cached fit), HP/armor below.
					FText NameText; float NW, NH;
					ResolveFittedName(Canvas, PS, NameFont, PS->PlayerName, PlateW - 8.f * S, NameScale, NameText, NW, NH);
					Labels.Add(FPanelLabel{ NameFont, NameText,
						CenterX - NW * NameScale * 0.5f, (RowY + PlateH * 0.30f) - NH * NameScale * 0.5f,
						NameScale, FLinearColor(0.90f, 0.90f, 0.92f, 1.f) });

					const int32 HP = UTC->Health;
					const int32 AR = UTC->GetArmorAmount();
					const FString HPStr = FString::Printf(TEXT("+%d"), HP);
					const FString ARStr = FString::Printf(TEXT("%d"), AR);
					const FLinearColor HPCol = (HP <= 34) ? FLinearColor(1.f, 0.42f, 0.42f, 1.f)
					                         : (HP <= 90) ? FLinearColor(0.92f, 0.82f, 0.30f, 1.f)
					                                      : FLinearColor(0.37f, 0.85f, 0.37f, 1.f);
					const FLinearColor ARCol = (AR <= 0) ? FLinearColor(0.60f, 0.60f, 0.60f, 1.f)
					                                     : FLinearColor(0.93f, 0.83f, 0.29f, 1.f);

					// "+HP   armor", centered as a group (green health, yellow armor).
					float hpXL, hpYL, arXL, arYL;
					Canvas->StrLen(StatFont, HPStr, hpXL, hpYL);
					Canvas->StrLen(StatFont, ARStr, arXL, arYL);
					const float SpaceW = 10.f * S;
					const float TotalW = (hpXL + arXL) * StatScale + SpaceW;
					const float StatY  = RowY + PlateH - (hpYL * StatScale) - 1.f * S;
					const float gx = CenterX - TotalW * 0.5f;
					const float arX = gx + hpXL * StatScale + SpaceW;
					Labels.Add(FPanelLabel{ StatFont, FText::FromString(HPStr), gx,  StatY, StatScale, HPCol });
					Labels.Add(FPanelLabel{ StatFont, FText::FromString(ARStr), arX, StatY, StatScale, ARCol });
				}
				else
				{
					// Enemy: name only, vertically centered (no live enemy HP).
					FText NameText; float NW, NH;
					ResolveFittedName(Canvas, PS, NameFont, PS->PlayerName, PlateW - 8.f * S, NameScale, NameText, NW, NH);
					Labels.Add(FPanelLabel{ NameFont, NameText,
						CenterX - NW * NameScale * 0.5f, (RowY + PlateH * 0.5f) - NH * NameScale * 0.5f,
						NameScale, FLinearColor(0.92f, 0.92f, 0.95f, 1.f) });
				}

				PlateX += Pitch;
			}
		}

		// Round clock — the center scorebar (which normally carries it) is suppressed
		// while this panel is on, so draw the clock here. RoundSecondsRemaining off the
		// BP GameState via reflection (static class+prop cache, like the scorebar).
		{
			int32 RoundTime = -1;
			static UClass* CachedClockCls = nullptr;
			static UIntProperty* CachedClockProp = nullptr;
			UClass* GSCls = GS->GetClass();
			if (CachedClockCls != GSCls)
			{
				CachedClockCls  = GSCls;
				CachedClockProp = FindField<UIntProperty>(GSCls, TEXT("RoundSecondsRemaining"));
			}
			if (CachedClockProp)
			{
				RoundTime = CachedClockProp->GetPropertyValue_InContainer(GS);
			}
			if (RoundTime >= 0)
			{
				const FString ClockStr = FString::Printf(TEXT("%02d:%02d"), RoundTime / 60, RoundTime % 60);
				// Below both team rows, centered on the score-box column (left edge is free
				// once the spectator slide-out is suppressed).
				const float ClockCX = Origin.X + Skew * 0.5f + ScoreW * 0.5f;
				const float ClockCY = Origin.Y + 2.f * PlateH + RowGap + PlateH * 0.5f;
				const FLinearColor ClockCol = (RoundTime <= 30) ? FLinearColor(1.f, 0.24f, 0.24f, 1.f) : FLinearColor::White;
				PushCentered(ScoreFont, ClockStr, ClockCX, ClockCY, ScoreScale, ClockCol);
			}
		}

		// Flush: every plate fill in a single triangle batch (one DrawItem), then every
		// label as an outlined text item on top. Op is applied per-label by DrawOutlinedText.
		if (PlateTris.Num() > 0)
		{
			FCanvasTriangleItem TriBatch(PlateTris, WhiteTex);
			TriBatch.BlendMode = ESimpleElementBlendMode::SE_BLEND_Translucent;
			Canvas->DrawItem(TriBatch);
		}
		for (const FPanelLabel& L : Labels)
		{
			DrawOutlinedText(Canvas, L.Font, L.Text, L.X, L.Y, L.Scale, L.Color, FLinearColor::Black, Op);
		}

		Canvas->SetLinearDrawColor(FLinearColor::White);
	}

	// =============================================================================
	// Held-pickup status — amp / berserk / siphon remaining time + jump-boot charges
	// =============================================================================
	//
	// Pure-C++ stand-in for the stock bpHW_Powerups widget in NCPlus modes. Stock UT4
	// shows held-pickup status via the MiniHUD QuickStats cluster (when "Show Powerups
	// on MiniHUD" is on) OR the standalone bpHW_Powerups widget (which suppresses
	// itself whenever the cluster is on). NCPlus replaces QuickStats with one that
	// draws no powerups/boots — so any player who enabled the cluster lost amp/berserk/
	// siphon/boots entirely. We draw it ourselves here, always, in NCPlus mode.
	//
	// Held-pickup STATUS only (your own carried powerups/boots) — NOT map item respawn
	// timers. In stock-bottom-bar mode the vanilla widgets still work, so we defer.
	// Configurable via the "powerups" alias: position/anchor/offset, scale, opacity,
	// and on/off (the nchud Hide checkbox).
	void DrawHeldPowerups(AUTHUD* HUD, UCanvas* Canvas)
	{
		if (HUD == nullptr || Canvas == nullptr || HUD->UTPlayerOwner == nullptr)
		{
			return;
		}
		// Stock-bottom-bar mode keeps the vanilla QuickStats/Paperdoll/bpHW_Powerups
		// widgets, which already show held-pickup status — defer to them.
		if (FNCPlusHUDLayout::WantsStockBottomBar())
		{
			return;
		}
		// nchud on/off for the "powerups" element.
		if (IsHidden(TEXT("powerups")))
		{
			return;
		}

		// View target = self when alive, the spectated player when spectating.
		AUTCharacter* C = Cast<AUTCharacter>(HUD->UTPlayerOwner->GetViewTarget());
		if (C == nullptr || C->IsDead())
		{
			return;
		}

		// Gather held timed powerups (amp / berserk / siphon …) + jump boots. Each
		// supplies its own HUD icon + remaining-time / charge text via GetHUDText —
		// the same data the stock powerups widget reads.
		TArray<AUTInventory*> Items;
		for (TInventoryIterator<> It(C); It; ++It)
		{
			AUTInventory* Inv = *It;
			if (Inv == nullptr || Inv->HUDIcon.Texture == nullptr)
			{
				continue;
			}
			if (Cast<AUTTimedPowerup>(Inv) != nullptr || Cast<AUTJumpBoots>(Inv) != nullptr)
			{
				Items.Add(Inv);
			}
		}
		if (Items.Num() == 0)
		{
			return;
		}

		const float ResScale = FMath::Max(Canvas->ClipY, 1.f) / 1080.f;
		const float Scale    = ResScale * GetScale(TEXT("powerups"));
		const float Op       = FMath::Clamp(GetOpacity(TEXT("powerups")), 0.f, 1.f) * HUD->GetHUDWidgetOpacity();
		if (Op <= 0.001f)
		{
			return;
		}

		const float IconH  = 36.f * Scale;
		const float Pad    = 8.f  * Scale;
		const float RowH   = IconH + Pad;
		const float BlockH = RowH * Items.Num();

		// Default lower-left; the block grows UPWARD from the anchor so the stock
		// BottomLeft default reads naturally and never runs off the bottom edge.
		const FVector2D Start = ResolveScreenPos(TEXT("powerups"), Canvas,
			FVector2D(20.f * ResScale, Canvas->ClipY - 20.f * ResScale));
		const float X = Start.X;
		float Y = Start.Y - BlockH;

		UFont* Font = HUD->MediumFont ? HUD->MediumFont : HUD->SmallFont;
		FFontRenderInfo RI;
		RI.bEnableShadow = true;
		const FColor TextColor(255, 255, 255, (uint8)FMath::Clamp(FMath::RoundToInt(Op * 255.f), 0, 255));

		for (AUTInventory* Inv : Items)
		{
			const FCanvasIcon& Ic = Inv->HUDIcon;
			const float AspectW = (Ic.VL != 0.f) ? (Ic.UL / Ic.VL) : 1.f;
			const float IconW = IconH * AspectW;

			Canvas->SetLinearDrawColor(FLinearColor(1.f, 1.f, 1.f, Op));
			Canvas->DrawTile(Ic.Texture, X, Y, IconW, IconH, Ic.U, Ic.V, Ic.UL, Ic.VL);

			const FText HudText = Inv->GetHUDText();
			if (Font != nullptr && !HudText.IsEmpty())
			{
				float XL, YL;
				Canvas->TextSize(Font, HudText.ToString(), XL, YL, Scale, Scale);
				Canvas->DrawColor = TextColor;
				Canvas->DrawText(Font, HudText, X + IconW + Pad, Y + (IconH - YL) * 0.5f, Scale, Scale, RI);
			}

			Y += RowH;
		}
	}

	// =============================================================================
	// Live bottom-bar swap — apply the Stock-vs-NCPlus toggle THIS match, not next level
	// =============================================================================
	void RefreshBottomBarWidgets(AHUD* HUDBase, bool bWantStock)
	{
		AUTHUD* HUD = Cast<AUTHUD>(HUDBase);
		if (HUD == nullptr || HUD->GetNetMode() == NM_DedicatedServer)
		{
			return;
		}

		// The two interchangeable bottom-bar families (weapon bar + ammo + health [+ powerups]).
		// Same sets every NCPlus mode uses, so one generic swap covers ElimPlus/Wipeout/CTF/ShockDom/etc.
		static const TCHAR* const StockPaths[] = {
			TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponBar.bpHW_WeaponBar_C"),
			TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponInfo.bpHW_WeaponInfo_C"),
			TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Paperdoll.bpHW_Paperdoll_C"),
			// QuickStats pairs with Paperdoll/WeaponInfo (the profile flag picks ONE
			// family at runtime) — the HUD ctor stock branches load it too (2026-07-01).
			TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_QuickStats.bpHW_QuickStats_C"),
			TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Powerups.bpHW_Powerups_C"),
		};
		static const TCHAR* const NCPlusPaths[] = {
			TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Left"),
			TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Right"),
			TEXT("/Script/NetcodePlus.NCPlusHUDWidget_AmmoCounter"),
			TEXT("/Script/NetcodePlus.NCPlusHUDWidget_QuickStats"),
		};

		auto Resolve = [](const TCHAR* const* Paths, int32 Num) -> TArray<UClass*>
		{
			TArray<UClass*> Out;
			for (int32 i = 0; i < Num; i++)
			{
				if (UClass* C = StaticLoadClass(UUTHUDWidget::StaticClass(), nullptr, Paths[i]))
				{
					Out.Add(C);
				}
			}
			return Out;
		};

		const TArray<UClass*> WantClasses = bWantStock
			? Resolve(StockPaths, ARRAY_COUNT(StockPaths)) : Resolve(NCPlusPaths, ARRAY_COUNT(NCPlusPaths));
		const TArray<UClass*> DropClasses = bWantStock
			? Resolve(NCPlusPaths, ARRAY_COUNT(NCPlusPaths)) : Resolve(StockPaths, ARRAY_COUNT(StockPaths));

		// Drop the family we no longer want (stops it drawing; the widget GCs once unreferenced).
		for (int32 i = HUD->HudWidgets.Num() - 1; i >= 0; i--)
		{
			UUTHUDWidget* W = HUD->HudWidgets[i];
			if (W && DropClasses.Contains(W->GetClass()))
			{
				HUD->HudWidgets.RemoveAt(i);
			}
		}

		// Add any wanted family member not already present (AddHudWidget instantiates + initializes).
		for (UClass* C : WantClasses)
		{
			bool bPresent = false;
			for (UUTHUDWidget* W : HUD->HudWidgets)
			{
				if (W && W->GetClass() == C) { bPresent = true; break; }
			}
			if (!bPresent)
			{
				HUD->AddHudWidget(C);
			}
		}
	}

	// =============================================================================
	// Post-match screenshot (shared by ElimPlus / Wipeout / iCTF + Duel/Shaft via AWipeoutHUD)
	// =============================================================================
	void ServicePostMatchScreenshot(AUTHUD* HUD, float& StableFrames, bool& bTaken)
	{
		if (bTaken || HUD == nullptr)
		{
			return;
		}
		UWorld* World = HUD->GetWorld();
		if (World == nullptr)
		{
			return;
		}

		// Capture the FINAL scoreboard, never the instant replay. The replay renders a SEPARATE killcam
		// world (UUTGameViewportClient world override), so this HUD's DrawHUD — and therefore this poll —
		// is SUSPENDED for the whole replay and only resumes on the post-replay scoreboard. So we count
		// CONSECUTIVE qualifying DrawHUD frames rather than wall-clock: the counter naturally pauses during
		// the replay (no calls) and resumes fresh afterward, so it can't be tricked by time elapsing while
		// suspended (matters for non-deferred replay modes like Wipeout, where HasMatchEnded() is already
		// true during the replay). The DemoNetDriver->IsPlaying() check is defensive only — on a live client
		// the source world's recording driver always reports IsPlaying()==false; it covers a true full
		// demo-playback session.
		AUTGameState* GS = World->GetGameState<AUTGameState>();
		const bool bReplaying = (World->DemoNetDriver != nullptr && World->DemoNetDriver->IsPlaying());
		if (GS == nullptr || !GS->HasMatchEnded() || bReplaying)
		{
			StableFrames = 0.f;
			return;
		}

		// A small buffer of scoreboard frames before the (slow) high-res capture — enough for it to be up,
		// frame-rate-robust because suspended-replay frames don't count toward it.
		StableFrames += 1.f;
		if (StableFrames < 20.f)
		{
			return;
		}

		bTaken = true;   // one-shot regardless of the opt-in, so we stop polling

		bool bEnabled = true;   // client opt-in via F5 "High Res Screenshot PostMatch"
		FString Val;
		const FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
		if (GConfig->GetString(TEXT("NetcodePlus"), TEXT("HighResScreenshotPostMatch"), Val, ConfigPath))
		{
			bEnabled = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
		}
		if (bEnabled)
		{
			if (APlayerController* PC = World->GetFirstPlayerController())
			{
				PC->ConsoleCommand(TEXT("HighResShot 2"));
			}
		}
	}

	// =============================================================================
	// Server info name plate
	// =============================================================================
	void DrawServerInfo(AUTHUD* HUD, UCanvas* Canvas)
	{
		if (HUD == nullptr || Canvas == nullptr)
		{
			return;
		}
		const FNCPlusHUDElement* Elem = FNCPlusHUDLayout::GetLive().Find(TEXT("server_info"));
		if (Elem == nullptr || Elem->bHidden)
		{
			return;     // default OFF — no entry = no draw
		}
		// Source priority: user override (Extras["name_override"]) wins so a hub
		// instance can show "phantaci's UT4 Hub" even when GS->ServerName has been
		// overwritten by the hub with the ruleset/match label. Then ServerName,
		// then ServerDescription, then "(server)". The override is a free-text
		// Extras key — set it in the editor's clipboard JSON paste or by hand
		// editing the layout (no dedicated UI; first-class enough to keep simple).
		const FString Override = Elem->GetExtra(TEXT("name_override"));
		FString Label;
		if (!Override.IsEmpty())
		{
			Label = Override;
		}
		else
		{
			AUTGameState* GS = HUD->GetWorld() ? HUD->GetWorld()->GetGameState<AUTGameState>() : nullptr;
			if (GS == nullptr)
			{
				return;
			}
			if (!GS->ServerName.IsEmpty())          Label = GS->ServerName;
			else if (!GS->ServerDescription.IsEmpty()) Label = GS->ServerDescription;
			else                                    Label = TEXT("(server)");
		}

		const FVector2D Pos = ResolveScreenPos(TEXT("server_info"), Canvas,
			FVector2D(20.f * (Canvas->ClipY / 1080.f), 14.f * (Canvas->ClipY / 1080.f)));

		UFont* Font = NCPlusHUDFonts::Resolve(TEXT("server_info"), HUD, HUD->SmallFont);
		if (Font == nullptr) Font = HUD->SmallFont;
		if (Font == nullptr) return;

		const float RenderScale = Canvas->ClipY / 1080.f;
		const float FontExtra   = NCPlusHUDFonts::ResolveScale(TEXT("server_info"), 1.f);
		const float Scale       = RenderScale * FontExtra;

		const FLinearColor Tint = Elem->GetExtraColor(TEXT("color_text"), FLinearColor(0.85f, 0.85f, 0.85f, 1.f));
		const float OpacityMul  = FMath::Clamp(Elem->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f);

		Canvas->DrawColor = FLinearColor(Tint.R, Tint.G, Tint.B, Tint.A * OpacityMul).ToFColor(true);
		Canvas->DrawText(Font, Label, Pos.X, Pos.Y, Scale, Scale);
	}

	// ── Replay-only fire-validation corner feed ────────────────────────
	// Reads the server-written FireVal_*.csv and overlays each sampled shot during
	// demo playback, synced to the replayed server clock. Pure client display.

	struct FNCFireValReplayEvent { float Time; FString Name; int32 Dwell; int32 Frame; bool bHit; };

	static TArray<FNCFireValReplayEvent> GFireValEvents;        // sorted ascending by Time
	static FString                   GFireValLoadedPath;    // CSV currently cached ("" = none)
	static TWeakObjectPtr<UWorld>    GFireValLoadedWorld;   // reload when the replay world changes

	static TAutoConsoleVariable<FString> CVarFireValReplayCsv(
		TEXT("ncp.FireValReplayCsv"), TEXT(""),
		TEXT("Path to a FireVal_*.csv for the replay overlay. Empty = newest in Saved/Logs."));

	static FString FindNewestFireValCsv()
	{
		const FString Dir = FPaths::GameSavedDir() / TEXT("Logs");
		TArray<FString> Names;
		IFileManager::Get().FindFiles(Names, *(Dir / TEXT("FireVal_*.csv")), true, false);
		FString Best;
		FDateTime BestTime = FDateTime::MinValue();
		for (const FString& N : Names)
		{
			const FString Full = Dir / N;
			const FDateTime T = IFileManager::Get().GetTimeStamp(*Full);
			if (T >= BestTime) { BestTime = T; Best = Full; }
		}
		return Best;
	}

	static void LoadFireValCsv(const FString& Path)
	{
		GFireValEvents.Reset();
		GFireValLoadedPath = Path;
		if (Path.IsEmpty()) return;

		TArray<FString> Lines;
		FString Whole;
		if (!FFileHelper::LoadFileToString(Whole, *Path)) return;  // 4.15 has LoadFileToString, not ...Array
		Whole.ParseIntoArray(Lines, TEXT("\n"), /*CullEmpty=*/true);

		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			if (i == 0 && Lines[i].StartsWith(TEXT("server_time"))) continue; // header
			TArray<FString> F;
			Lines[i].ParseIntoArray(F, TEXT(","), false);
			if (F.Num() < 5) continue;
			FNCFireValReplayEvent E;
			E.Time  = FCString::Atof(*F[0]);
			E.Name  = F[1];
			E.Dwell = FCString::Atoi(*F[2]);
			E.Frame = FCString::Atoi(*F[3]);
			E.bHit  = (FCString::Atoi(*F[4]) != 0);
			GFireValEvents.Add(E);
		}
		GFireValEvents.Sort([](const FNCFireValReplayEvent& A, const FNCFireValReplayEvent& B) { return A.Time < B.Time; });
	}

	void DrawFireValReplayFeed(AUTHUD* HUD, UCanvas* Canvas)
	{
		if (HUD == nullptr || Canvas == nullptr) return;

		UWorld* World = HUD->GetWorld();
		// Replay-only: identical guard the plugin uses elsewhere (UTPlusProj_ShockBall).
		if (World == nullptr || World->DemoNetDriver == nullptr || !World->DemoNetDriver->IsPlaying()) return;

		AGameStateBase* GS = World->GetGameState();
		UFont* Font = HUD->SmallFont;
		if (GS == nullptr || Font == nullptr) return;

		const float NowServer = GS->GetServerWorldTimeSeconds();

		// (Re)load once per replay, or when the cvar points somewhere new. Once
		// we've attempted a load for this world we don't re-scan disk per frame —
		// even if no CSV was found (drop one in + restart the replay, or set the
		// cvar, to pick it up).
		const FString CVarPath = CVarFireValReplayCsv.GetValueOnGameThread();
		const bool bSameWorld  = (GFireValLoadedWorld.Get() == World);
		FString Desired;
		if (!CVarPath.IsEmpty())  Desired = CVarPath;
		else                      Desired = bSameWorld ? GFireValLoadedPath : FindNewestFireValCsv();
		if (!bSameWorld || Desired != GFireValLoadedPath)
		{
			LoadFireValCsv(Desired);
			GFireValLoadedWorld = World;
		}

		const float RenderScale = Canvas->ClipY / 1080.f;
		const float Scale = RenderScale * 0.9f;
		const float X     = 24.f  * RenderScale;
		float       Y     = 220.f * RenderScale;   // below the top-left clock region
		const float LineH = 22.f  * RenderScale;
		const float Window = 6.0f;                 // seconds of history shown
		const int32 MaxLines = 8;

		Canvas->DrawColor = FColor(170, 170, 170, 200);
		Canvas->DrawText(Font, GFireValEvents.Num() > 0
			? FString(TEXT("Fire (replay)"))
			: FString(TEXT("Fire (replay): no FireVal_*.csv in Saved/Logs")),
			X, Y, Scale, Scale);
		Y += LineH * 1.2f;
		if (GFireValEvents.Num() == 0) return;

		int32 Drawn = 0;
		for (int32 i = GFireValEvents.Num() - 1; i >= 0 && Drawn < MaxLines; --i)
		{
			const FNCFireValReplayEvent& E = GFireValEvents[i];
			const float Age = NowServer - E.Time;
			if (Age < -0.25f) continue;  // shot is ahead of the playback head — skip
			if (Age > Window) break;     // sorted: everything earlier is older still

			const float Alpha = FMath::Clamp(1.f - Age / Window, 0.15f, 1.f);
			const bool bFirstFrame = (E.Frame > 0) ? (E.Dwell <= E.Frame) : (E.Dwell <= 16);
			FLinearColor C = bFirstFrame ? FLinearColor(1.f, 0.25f, 0.2f, 1.f)
				: (E.Dwell <= 50 ? FLinearColor(1.f, 0.7f, 0.15f, 1.f)
				                 : FLinearColor(0.9f, 0.9f, 0.9f, 1.f));
			C.A = Alpha;
			Canvas->DrawColor = C.ToFColor(true);

			const FString Line = FString::Printf(TEXT("%s  %dms  %s%s"),
				*E.Name, E.Dwell, E.bHit ? TEXT("hit") : TEXT("miss"),
				bFirstFrame ? TEXT("  <<first-frame") : TEXT(""));
			Canvas->DrawText(Font, Line, X, Y, Scale, Scale);
			Y += LineH;
			++Drawn;
		}
	}
}

// =============================================================================
// Live in-memory singleton (Phase 2)
// =============================================================================

// Dirty flag — gates per-frame apply so DrawHUD does ~zero work when nothing changed.
// Starts true so the first frame applies whatever was loaded on PIE start.
static bool GLiveLayoutDirty = true;

FNCPlusHUDLayout& FNCPlusHUDLayout::GetLive()
{
	static FNCPlusHUDLayout Live;
	return Live;
}

void FNCPlusHUDLayout::ReloadLive()
{
	const FString NewPath = GetDefaultLayoutPath();
	if (FPaths::FileExists(NewPath))
	{
		// Existing player: honor their saved layout exactly. This branch is
		// the backward-compat guarantee — anyone updating from a prior plugin
		// version with a saved HUDLayout.json gets their exact prior state,
		// including custom colors, weapon group assignments, and the
		// "empty overrides after Reset All + Save" state.
		GetLive() = LoadFromFile(NewPath);
		GLiveLayoutDirty = true; ++GLayoutRevision;
		return;
	}

	// Legacy fallback: use the pre-3.4 per-mode file if the unified one is absent.
	// On next Save, we'll write to NewPath, effectively migrating.
	const FString LegacyPath = FPaths::GameSavedDir() / TEXT("NetcodePlus") / TEXT("ElimPlusHUDLayout.json");
	if (FPaths::FileExists(LegacyPath))
	{
		GetLive() = LoadFromFile(LegacyPath);
		GLiveLayoutDirty = true; ++GLayoutRevision;
		return;
	}

	// First-run seed. No layout file at either path - this is a fresh install
	// (or a player who deleted their layout). Seed with the curated default
	// (NCPlusHUDPresets::GetCurated()[0] = "Stock") so new players get the
	// familiar stock-style layout (default UT font). On first Save,
	// the seeded layout is written to NewPath.
	const TArray<FNCPlusHUDPreset>& Curated = NCPlusHUDPresets::GetCurated();
	if (Curated.Num() > 0)
	{
		bool bOk = false;
		FNCPlusHUDLayout Seed = FromJsonString(Curated[0].JsonString, bOk);
		GetLive() = bOk ? MoveTemp(Seed) : FNCPlusHUDLayout();
		UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] First-run seed: applied '%s' preset (%d elements)."),
			*Curated[0].DisplayName, GetLive().Elements.Num());
	}
	else
	{
		GetLive() = FNCPlusHUDLayout();
	}
	GLiveLayoutDirty = true; ++GLayoutRevision;
}

bool FNCPlusHUDLayout::SaveLive()
{
	// Save doesn't change in-memory state → no need to mark dirty.
	const bool bOk = GetLive().SaveToFile(GetDefaultElimPlusPath());
	// The first successful save creates the layout file, which flips the
	// "no saved layout → stock" fallback shared by BOTH WantsStockBottomBar and
	// WantsStockTeamPanel ("anyone who saved a layout keeps the NCPlus widgets/
	// portraits"). Drop both caches so they re-resolve next call — WantsStockTeamPanel
	// is consulted live every frame, so a fresh-install user who customizes + saves
	// flips to the portrait strip immediately instead of staying on the stock roster
	// until restart (mirrors the SetStock* refresh).
	if (bOk) { GStockBottomBarCache = -1; GStockTeamPanelCache = -1; }
	return bOk;
}

void FNCPlusHUDLayout::ResetLive()
{
	GetLive() = FNCPlusHUDLayout();
	GLiveLayoutDirty = true; ++GLayoutRevision;
}

void FNCPlusHUDLayout::MarkLiveDirty() { GLiveLayoutDirty = true; ++GLayoutRevision; }
bool FNCPlusHUDLayout::IsLiveDirty()   { return GLiveLayoutDirty; }
void FNCPlusHUDLayout::ClearLiveDirty(){ GLiveLayoutDirty = false; }
uint32 FNCPlusHUDLayout::GetLiveRevision() { return GLayoutRevision; }

// =============================================================================
// Apply pass
// =============================================================================

// Defaults snapshot — captured once per HUD lifetime in CaptureWidgetDefaults,
// consulted by ApplyLayoutToWidgets when an element has no override.
static TMap<FName, FNCPlusWidgetDefaults> GWidgetDefaults;

// =============================================================================
// Reflection helpers — UUTHUDWidget_WeaponBar's positioning fields are declared
// `protected` in C++ even though they're UPROPERTYs. We can't access them via
// the typed pointer; reflection bypasses the access check.
// =============================================================================

static FVector2D GetVec2Prop(UObject* Obj, FName PropName, const FVector2D& Fallback)
{
	if (!Obj) return Fallback;
	UStructProperty* SP = FindField<UStructProperty>(Obj->GetClass(), PropName);
	if (SP && SP->Struct == TBaseStructure<FVector2D>::Get())
	{
		if (FVector2D* Ptr = SP->ContainerPtrToValuePtr<FVector2D>(Obj))
		{
			return *Ptr;
		}
	}
	return Fallback;
}

static void SetVec2Prop(UObject* Obj, FName PropName, const FVector2D& Val)
{
	if (!Obj) return;
	UStructProperty* SP = FindField<UStructProperty>(Obj->GetClass(), PropName);
	if (SP && SP->Struct == TBaseStructure<FVector2D>::Get())
	{
		if (FVector2D* Ptr = SP->ContainerPtrToValuePtr<FVector2D>(Obj))
		{
			*Ptr = Val;
		}
	}
}

void CaptureWidgetDefaults(AUTHUD* HUD)
{
	if (!HUD) return;
	GWidgetDefaults.Empty();
	for (UUTHUDWidget* W : HUD->HudWidgets)
	{
		if (!W) continue;
		const FName Alias = NCPlusHUDAliases::GetAliasForClass(W->GetClass());
		if (Alias == NAME_None) continue;

		FNCPlusWidgetDefaults D;
		D.ScreenPosition = W->ScreenPosition;
		D.Position       = W->Position;
		D.Origin         = W->Origin;
		D.bHidden        = W->IsHidden();

		// WeaponBar special-case: it self-positions in PreDraw from these
		// internal fields (declared protected in C++; we use reflection).
		// Detect by alias rather than UClass to avoid pulling in the header.
		if (Alias == TEXT("weapon_bar"))
		{
			D.bIsWeaponBar      = true;
			D.WB_HorizScreenPos = GetVec2Prop(W, TEXT("HorizontalScreenPosition"), FVector2D::ZeroVector);
			D.WB_HorizPos       = GetVec2Prop(W, TEXT("HorizontalPosition"),       FVector2D::ZeroVector);
			D.WB_VertScreenPos  = GetVec2Prop(W, TEXT("VerticalScreenPosition"),   FVector2D::ZeroVector);
			D.WB_VertPos        = GetVec2Prop(W, TEXT("VerticalPosition"),         FVector2D::ZeroVector);
		}

		GWidgetDefaults.Add(Alias, D);
	}
	UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] Captured stock defaults for %d widget(s)."), GWidgetDefaults.Num());
}

void ApplyLayoutToWidgets(AUTHUD* HUD, const FNCPlusHUDLayout& Layout)
{
	if (!HUD) return;

	// Fast path: layout unchanged since last apply → nothing to do.
	// Old condition gated the fast path on `Layout.Elements.Num() == 0`,
	// which meant the moment a user added ANY layout entry, the apply
	// loop ran every frame at 144Hz+ — full HudWidgets iteration plus a
	// reflection lookup per widget plus a UE_LOG. Now we trust the dirty
	// flag exclusively: ReloadLive / ResetLive / MarkLiveDirty all set it
	// true, so the first frame after a mutation re-applies. After that,
	// widget state already reflects the layout — re-asserting the same
	// values is wasted work.
	if (!FNCPlusHUDLayout::IsLiveDirty()) return;

	int32 NumApplied = 0;
	for (UUTHUDWidget* W : HUD->HudWidgets)
	{
		if (!W || W->IsPendingKill()) continue;
		UClass* WClass = W->GetClass();
		if (!WClass) continue;
		const FName Alias = NCPlusHUDAliases::GetAliasForClass(WClass);
		if (Alias == NAME_None) continue;

		const FNCPlusHUDElement* Elem = Layout.Find(Alias);
		if (!Elem)
		{
			// Killfeed: no override → hand the widget back to the engine's stomp
			// (re-cache the pointer we null below) so Reset / removed-entry restores
			// the stock scoreboard-aware positioning exactly.
			if (Alias == TEXT("killfeed") && HUD->KillIconWidget == nullptr)
			{
				HUD->KillIconWidget = Cast<UUTHUDWidgetMessage_KillIconMessages>(W);
			}

			// No override → restore stock snapshot so Reset / removed-entry
			// returns the widget to exactly where the engine put it on spawn.
			if (FNCPlusWidgetDefaults* D = GWidgetDefaults.Find(Alias))
			{
				W->ScreenPosition = D->ScreenPosition;
				W->Position       = D->Position;
				W->Origin         = D->Origin;
				// Engine PreDraw re-asserts Origin from RealOrigin every frame
				// (UTHUDWidget.cpp:413) — writing Origin alone is a no-op.
				W->RealOrigin     = D->Origin;
				W->SetHidden(D->bHidden);

				if (D->bIsWeaponBar)
				{
					SetVec2Prop(W, TEXT("HorizontalScreenPosition"), D->WB_HorizScreenPos);
					SetVec2Prop(W, TEXT("HorizontalPosition"),       D->WB_HorizPos);
					SetVec2Prop(W, TEXT("VerticalScreenPosition"),   D->WB_VertScreenPos);
					SetVec2Prop(W, TEXT("VerticalPosition"),         D->WB_VertPos);
				}
			}
			else if (W->IsHidden())
			{
				// No snapshot for some reason — at least un-hide.
				W->SetHidden(false);
			}
			continue;
		}

		// Override present.
		// Set Origin = anchor coords too: the widget's pivot lands AT the anchor
		// corner, and its bounding box (Size.X × Size.Y) extends inward from
		// that corner. This is what users intuitively expect for "anchor X" —
		// e.g. BottomLeft means content sits in the bottom-left, not centered
		// horizontally over screen X=0.
		const FVector2D AnchorCoords = FNCPlusHUDLayout::AnchorToScreenCoords(Elem->Anchor);
		W->ScreenPosition = AnchorCoords;
		W->Origin         = AnchorCoords;
		// Engine PreDraw re-asserts Origin from RealOrigin every frame (UTHUDWidget.cpp:413)
		// — without this the anchor pivot silently reverted to the ctor origin, which is
		// why seeded CenterLeft/CenterRight weapon bars rendered ~270px above intent.
		W->RealOrigin     = AnchorCoords;
		W->Position       = Elem->Offset;
		W->SetHidden(Elem->bHidden);

		// WeaponBar special-case: it self-positions in PreDraw from its own
		// Horizontal/Vertical*Position fields, ignoring the generic
		// ScreenPosition/Position above. Push the override down to BOTH the
		// horizontal AND vertical sets so it sticks regardless of layout mode.
		// (Fields are protected in C++ → reflection access.)
		if (Alias == TEXT("weapon_bar"))
		{
			SetVec2Prop(W, TEXT("HorizontalScreenPosition"), AnchorCoords);
			SetVec2Prop(W, TEXT("HorizontalPosition"),       Elem->Offset);
			SetVec2Prop(W, TEXT("VerticalScreenPosition"),   AnchorCoords);
			SetVec2Prop(W, TEXT("VerticalPosition"),         Elem->Offset);
		}
		// Killfeed special-case: stock AUTHUD::DrawHUD re-stomps
		// KillIconWidget->ScreenPosition EVERY frame (UTHUD.cpp:842-845, to (0,0) or
		// the scoreboard spot) right before the widget render loop — and our apply
		// runs BEFORE Super::DrawHUD, so the anchor write above never survived to
		// PreDraw. That's why nchud moves "didn't take until map change" (only the
		// pixel Offset ever applied; anchors never did). Null the HUD's cached
		// pointer while an override exists: the stomp branch is skipped, our writes
		// stick, and AddHudWidget only re-caches on widget creation so the null
		// holds for the match. Restored in the no-override branch above. Trade-off
		// (accepted): the stock "drop the feed below the scoreboard" reposition is
		// disabled while the user has a custom placement.
		if (Alias == TEXT("killfeed"))
		{
			HUD->KillIconWidget = nullptr;
		}
		// Scale reserved for Phase 3.

		NumApplied++;
	}

	FNCPlusHUDLayout::ClearLiveDirty();
	// Only fires on a real mutation now (ReloadLive, ResetLive, editor edit).
	// Was previously per-frame for any non-empty layout — log spam.
	UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] Applied layout: %d override(s), %d default(s) restored."),
		NumApplied, GWidgetDefaults.Num() - NumApplied);
}
