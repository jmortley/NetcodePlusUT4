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
// Find
// =============================================================================

const FNCPlusHUDElement* FNCPlusHUDLayout::Find(FName ElementId) const
{
	return Elements.Find(ElementId);
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

		Layout.Elements.Add(Key, Elem);
	}

	UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] Loaded %d element(s) from %s"), Layout.Elements.Num(), *Path);
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
		ElementsObj->SetObjectField(Pair.Key.ToString(), EObj);
	}
	Root->SetObjectField(TEXT("elements"), ElementsObj);

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
			T.Add({ TEXT("weapon_bar"),       TEXT("/Game/RestrictedAssets/UI/HUDWidgets/bpHW_WeaponBar.bpHW_WeaponBar_C"),    FText::FromString(TEXT("Weapon Bar")) });
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

void ApplyLayoutToWidgets(AUTHUD* HUD, const FNCPlusHUDLayout& Layout)
{
	if (!HUD) return;

	// Fast path: when the layout hasn't changed since last apply AND has no
	// entries, do nothing. This is the common case during normal gameplay.
	if (!FNCPlusHUDLayout::IsLiveDirty() && Layout.Elements.Num() == 0) return;

	// Even when "dirty but empty" (e.g. user just hit Reset), we still walk
	// HudWidgets[] once to clear hidden state on widgets that may have been
	// hidden by an earlier layout. Otherwise a Reset wouldn't un-hide anything.

	int32 NumApplied = 0;
	for (UUTHUDWidget* W : HUD->HudWidgets)
	{
		if (!W) continue;
		const FName Alias = NCPlusHUDAliases::GetAliasForClass(W->GetClass());
		if (Alias == NAME_None) continue;

		const FNCPlusHUDElement* Elem = Layout.Find(Alias);
		if (!Elem)
		{
			// No override → make sure we're not still applying a stale hidden flag.
			if (W->IsHidden()) W->SetHidden(false);
			continue;
		}

		// Apply: ScreenPosition (anchor 0..1), Position (offset in design px), hidden.
		// Origin intentionally untouched — each widget carries its preferred pivot.
		W->ScreenPosition = FNCPlusHUDLayout::AnchorToScreenCoords(Elem->Anchor);
		W->Position       = Elem->Offset;
		W->SetHidden(Elem->bHidden);
		// Per-widget scale is reserved for Phase 3 (most UUTHUDWidget classes
		// have non-trivial scale plumbing; one field doesn't propagate cleanly).

		NumApplied++;
	}

	FNCPlusHUDLayout::ClearLiveDirty();

	// Verbose log left at Log level so it appears once after a change but not every frame.
	UE_LOG(LogTemp, Log, TEXT("[NCPlusHUDLayout] Applied layout to %d widget(s)."), NumApplied);
}
