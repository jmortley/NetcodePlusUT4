// NCPlusHUDLayout — implementation. JSON I/O + alias map + apply-to-widgets pass.
#include "NCPlusHUDLayout.h"
#include "NCPlusHUDPresets.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTHUDWidget.h"
#include "UTPlayerController.h"
#include "UTCharacter.h"
#include "UTGameState.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Json.h"
#include "JsonUtilities.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"

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
	static bool GIsActive = false;

	bool IsActive()              { return GIsActive; }
	void SetActive(bool bActive) { GIsActive = bActive; }
}

// =============================================================================
// Font resolver (Phase 3.8)
// =============================================================================

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
			// External Google Fonts (OFL) - TEST SET, pointing at the exact UFont
			// asset names currently in /Game/Blueprints/Netcode/ (incl. italic +
			// variable-font variants) for a quick render test.
			// COOK NOTE: these are string-path refs the cooker won't auto-pull from
			// Blueprints/Netcode - either add that folder to "Additional Asset
			// Directories to Cook" or keep a holder asset (the FontTest BP) that
			// hard-references them so the cook drags them in.
			// UE4.15 may not render the *VariableFont* ones (Inter/JetBrains/Oswald);
			// Bebas/Russo are static and should be safe. Once a final set is chosen,
			// trim to one weight each + rename to clean names.
			{ TEXT("Inter"),                 TEXT("/Game/Blueprints/Netcode/Inter-VariableFont_opsz_wght_Font.Inter-VariableFont_opsz_wght_Font"),                     nullptr },
			{ TEXT("Inter Italic"),          TEXT("/Game/Blueprints/Netcode/Inter-Italic-VariableFont_opsz_wght_Font.Inter-Italic-VariableFont_opsz_wght_Font"),       nullptr },
			{ TEXT("JetBrains Mono"),        TEXT("/Game/Blueprints/Netcode/JetBrainsMono-VariableFont_wght_Font.JetBrainsMono-VariableFont_wght_Font"),               nullptr },
			{ TEXT("JetBrains Mono Italic"), TEXT("/Game/Blueprints/Netcode/JetBrainsMono-Italic-VariableFont_wght_Font.JetBrainsMono-Italic-VariableFont_wght_Font"), nullptr },
			{ TEXT("Bebas Neue"),            TEXT("/Game/Blueprints/Netcode/BebasNeue-Regular_Font.BebasNeue-Regular_Font"),                                            nullptr },
			{ TEXT("Oswald"),                TEXT("/Game/Blueprints/Netcode/Oswald-VariableFont_wght_Font.Oswald-VariableFont_wght_Font"),                              nullptr },
			{ TEXT("Russo One"),             TEXT("/Game/Blueprints/Netcode/RussoOne-Regular_Font.RussoOne-Regular_Font"),                                              nullptr },
		};
		return Table;
	}

	UFont* Resolve(FName Alias, AUTHUD* HUD, UFont* Fallback)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		if (!E) return Fallback;

		const FString FontKey = E->GetExtra(TEXT("font"));
		if (FontKey.IsEmpty() || FontKey.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
		{
			return Fallback;
		}

		// Lazy-load cache for Tier B fonts. Map by asset path so different
		// display aliases pointing at the same asset only load once.
		static TMap<FString, UFont*> Cache;

		for (const FFontEntry& F : GetTable())
		{
			if (!F.Display.Equals(FontKey, ESearchCase::IgnoreCase)) continue;

			if (F.HUDFunc)
			{
				UFont* HF = F.HUDFunc(HUD);
				return HF ? HF : Fallback;
			}

			// Cache hit. nullptr is a valid cached value meaning "tried, failed,
			// don't retry" — without this short-circuit a missing asset retries
			// LoadObject every frame for every widget that names it, hammering
			// disk and spamming the log. Treat cached-null as known failure.
			if (UFont** Cached = Cache.Find(F.AssetPath))
			{
				return *Cached ? *Cached : Fallback;
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
			return Fallback;
		}

		// Unknown name → fall back silently. Likely an old JSON entry from a
		// future build; leaving it as-is on disk so it survives a re-save.
		return Fallback;
	}

	float ResolveScale(FName Alias, float Default)
	{
		const FNCPlusHUDElement* E = FNCPlusHUDLayout::GetLive().Find(Alias);
		return E ? E->GetExtraFloat(TEXT("font_scale"), Default) : Default;
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
			T.Emplace(TEXT("powerups"),         TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Powerups.bpHW_Powerups_C"),      FText::FromString(TEXT("Powerups")),           false, ENCPlusHUDAnchor::BottomLeft);
			T.Emplace(TEXT("killfeed"),         TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpWH_KillIconMessages.bpWH_KillIconMessages_C"), FText::FromString(TEXT("Killfeed")), false, ENCPlusHUDAnchor::TopRight);
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
	void DrawDamageFlash(AUTHUD* HUD)
	{
		if (HUD == nullptr || HUD->Canvas == nullptr || HUD->UTPlayerOwner == nullptr)
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

		UCanvas* C = HUD->Canvas;
		C->SetLinearDrawColor(FLinearColor(TintColor.R, TintColor.G, TintColor.B, Alpha));
		C->DrawTile(C->DefaultTexture, 0.f, 0.f, C->ClipX, C->ClipY, 0.f, 0.f, 1.f, 1.f, BLEND_Translucent);
	}

	// =============================================================================
	// Server info name plate
	// =============================================================================
	void DrawServerInfo(AUTHUD* HUD)
	{
		if (HUD == nullptr || HUD->Canvas == nullptr)
		{
			return;
		}
		const FNCPlusHUDElement* Elem = FNCPlusHUDLayout::GetLive().Find(TEXT("server_info"));
		if (Elem == nullptr || Elem->bHidden)
		{
			return;     // default OFF — no entry = no draw
		}
		AUTGameState* GS = HUD->GetWorld() ? HUD->GetWorld()->GetGameState<AUTGameState>() : nullptr;
		if (GS == nullptr)
		{
			return;
		}
		const FString Label = GS->ServerName.IsEmpty() ? FString(TEXT("(server)")) : GS->ServerName;

		const FVector2D Pos = ResolveScreenPos(TEXT("server_info"), HUD->Canvas,
			FVector2D(20.f * (HUD->Canvas->ClipY / 1080.f), 14.f * (HUD->Canvas->ClipY / 1080.f)));

		UFont* Font = NCPlusHUDFonts::Resolve(TEXT("server_info"), HUD, HUD->SmallFont);
		if (Font == nullptr) Font = HUD->SmallFont;
		if (Font == nullptr) return;

		const float RenderScale = HUD->Canvas->ClipY / 1080.f;
		const float FontExtra   = NCPlusHUDFonts::ResolveScale(TEXT("server_info"), 1.f);
		const float Scale       = RenderScale * FontExtra;

		const FLinearColor Tint = Elem->GetExtraColor(TEXT("color_text"), FLinearColor(0.85f, 0.85f, 0.85f, 1.f));
		const float OpacityMul  = FMath::Clamp(Elem->GetExtraFloat(TEXT("opacity"), 1.f), 0.f, 1.f);

		HUD->Canvas->DrawColor = FLinearColor(Tint.R, Tint.G, Tint.B, Tint.A * OpacityMul).ToFColor(true);
		HUD->Canvas->DrawText(Font, Label, Pos.X, Pos.Y, Scale, Scale);
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
		GLiveLayoutDirty = true;
		return;
	}

	// Legacy fallback: use the pre-3.4 per-mode file if the unified one is absent.
	// On next Save, we'll write to NewPath, effectively migrating.
	const FString LegacyPath = FPaths::GameSavedDir() / TEXT("NetcodePlus") / TEXT("ElimPlusHUDLayout.json");
	if (FPaths::FileExists(LegacyPath))
	{
		GetLive() = LoadFromFile(LegacyPath);
		GLiveLayoutDirty = true;
		return;
	}

	// First-run seed. No layout file at either path - this is a fresh install
	// (or a player who deleted their layout). Seed with the curated default
	// (NCPlusHUDPresets::GetCurated()[0] = "Streamer Friendly") so new players
	// get a polished baseline instead of stock UT defaults. On first Save,
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
	GLiveLayoutDirty = true;
}

bool FNCPlusHUDLayout::SaveLive()
{
	// Save doesn't change in-memory state → no need to mark dirty.
	return GetLive().SaveToFile(GetDefaultElimPlusPath());
}

void FNCPlusHUDLayout::ResetLive()
{
	GetLive() = FNCPlusHUDLayout();
	GLiveLayoutDirty = true;
}

void FNCPlusHUDLayout::MarkLiveDirty() { GLiveLayoutDirty = true; }
bool FNCPlusHUDLayout::IsLiveDirty()   { return GLiveLayoutDirty; }
void FNCPlusHUDLayout::ClearLiveDirty(){ GLiveLayoutDirty = false; }

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
			// No override → restore stock snapshot so Reset / removed-entry
			// returns the widget to exactly where the engine put it on spawn.
			if (FNCPlusWidgetDefaults* D = GWidgetDefaults.Find(Alias))
			{
				W->ScreenPosition = D->ScreenPosition;
				W->Position       = D->Position;
				W->Origin         = D->Origin;
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
		// Scale reserved for Phase 3.

		NumApplied++;
	}

	FNCPlusHUDLayout::ClearLiveDirty();
	// Only fires on a real mutation now (ReloadLive, ResetLive, editor edit).
	// Was previously per-frame for any non-empty layout — log spam.
	UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] Applied layout: %d override(s), %d default(s) restored."),
		NumApplied, GWidgetDefaults.Num() - NumApplied);
}
