// NCPlusHUDLayout — implementation. JSON I/O + alias map + apply-to-widgets pass.
#include "NCPlusHUDLayout.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "UTHUDWidget.h"
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

FString FNCPlusHUDLayout::GetDefaultElimPlusPath()
{
	return FPaths::GameSavedDir() / TEXT("NetcodePlus") / TEXT("ElimPlusHUDLayout.json");
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

	TSharedPtr<FJsonObject> Root;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("[NCPlusHUDLayout] Failed to parse JSON at %s — falling back to defaults."), *Path);
		return Layout;
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

	UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] Loaded %d element(s), %d weapon group(s) from %s"),
		Layout.Elements.Num(), Layout.WeaponGroupAssignments.Num(), *Path);
	return Layout;
}

bool FNCPlusHUDLayout::SaveToFile(const FString& Path) const
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
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		return false;
	}

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
		FString  ClassPath;
		FText    DisplayName;
	};

	static const TArray<FAliasEntry>& GetAliasTable()
	{
		static const TArray<FAliasEntry> Table = []()
		{
			TArray<FAliasEntry> T;
			// Display order = list-view order in the editor. Group by region.
			T.Add({ TEXT("hp_armor"),         TEXT("/Script/NetcodePlus.NCPlusHUDWidget_QuickStats"),                          FText::FromString(TEXT("Health & Armor")) });
			T.Add({ TEXT("weapon_info"),      TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponInfo.bpHW_WeaponInfo_C"),  FText::FromString(TEXT("Weapon Info / Ammo")) });
			// Custom split WeaponBar — two independent positionable strips.
			T.Add({ TEXT("weapon_bar_left"),  TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Left"),                    FText::FromString(TEXT("Weapon Bar (Left)")) });
			T.Add({ TEXT("weapon_bar_right"), TEXT("/Script/NetcodePlus.NCPlusHUDWidget_WeaponBar_Right"),                   FText::FromString(TEXT("Weapon Bar (Right)")) });
			T.Add({ TEXT("weapon_crosshair"), TEXT("/Script/UnrealTournament.UTHUDWidget_WeaponCrosshair"),                    FText::FromString(TEXT("Crosshair")) });
			T.Add({ TEXT("powerups"),         TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_Powerups.bpHW_Powerups_C"),      FText::FromString(TEXT("Powerups")) });
			T.Add({ TEXT("killfeed"),         TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpWH_KillIconMessages.bpWH_KillIconMessages_C"), FText::FromString(TEXT("Killfeed")) });
			T.Add({ TEXT("spectator"),        TEXT("/Script/UnrealTournament.UTHUDWidget_Spectator"),                          FText::FromString(TEXT("Spectator Score / KDA")) });
			T.Add({ TEXT("announcements"),    TEXT("/Script/UnrealTournament.UTHUDWidgetAnnouncements"),                       FText::FromString(TEXT("Announcements")) });
			T.Add({ TEXT("console_msgs"),     TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_ConsoleMessages"),             FText::FromString(TEXT("Console Messages")) });
			T.Add({ TEXT("voice_status"),     TEXT("/Script/UnrealTournament.UTHUDWidgetMessage_VoiceChatStatus"),             FText::FromString(TEXT("Voice Chat Status")) });
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
	GetLive() = LoadFromFile(GetDefaultElimPlusPath());
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
	// Defaults restore is handled the same frame the user removed an entry
	// (which marks dirty), so a "clean" empty layout means nothing changed.
	if (!FNCPlusHUDLayout::IsLiveDirty() && Layout.Elements.Num() == 0) return;

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
	UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] Applied layout: %d override(s), %d default(s) restored."),
		NumApplied, GWidgetDefaults.Num() - NumApplied);
}
