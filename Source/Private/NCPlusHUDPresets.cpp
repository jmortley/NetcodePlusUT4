// NCPlusHUDPresets - curated layout presets + user-saved customs.
#include "NCPlusHUDPresets.h"
#include "NCPlusHUDLayout.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFilemanager.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace NCPlusHUDPresetsImpl
{
	// =========================================================================
	// Curated preset JSON literals.
	//
	// These are hand-authored snapshots of the live layout schema. Adjacent
	// TEXT() literals concatenate at compile time. Numeric extras (opacity,
	// font_scale) MUST be stored as JSON strings - FromJsonString's extras
	// reader at NCPlusHUDLayout.cpp:506 only accepts string-valued fields.
	// =========================================================================

	// "Streamer Friendly" - bold, legible, viewer-readable.
	// Big bottom-center HP/Armor (VerticalPills, scale 1.3, bright HP green +
	// armor yellow, Extreme font), big BigNumber ammo, prominent portraits +
	// scorebar, killfeed visible at default, score/KDA top-right.
	static const FString StreamerFriendlyJson(
		TEXT("{")
		TEXT("\"version\":1,")
		TEXT("\"elements\":{")
			TEXT("\"hp_armor\":{")
				TEXT("\"anchor\":\"BottomCenter\",\"offset_x\":0,\"offset_y\":-200,\"scale\":1.3,\"hidden\":false,")
				TEXT("\"style\":\"VerticalPills\",")
				TEXT("\"color_health\":\"#5FFF7AFF\",\"color_armor\":\"#FFD940FF\",")
				TEXT("\"color_health_number\":\"#FFFFFFFF\",\"color_armor_number\":\"#FFFFFFFF\",")
				TEXT("\"font\":\"Extreme\"")
			TEXT("},")
			TEXT("\"ammo\":{")
				TEXT("\"anchor\":\"BottomRight\",\"offset_x\":-30,\"offset_y\":-30,\"scale\":1.3,\"hidden\":false,")
				TEXT("\"style\":\"BigNumber\",")
				TEXT("\"color_number\":\"#FFFFFFFF\",")
				TEXT("\"font\":\"Extreme\"")
			TEXT("},")
			TEXT("\"weapon_bar_left\":{")
				TEXT("\"anchor\":\"CenterLeft\",\"offset_x\":20,\"offset_y\":-20,\"scale\":1.0,\"hidden\":false")
			TEXT("},")
			TEXT("\"weapon_bar_right\":{")
				TEXT("\"anchor\":\"CenterRight\",\"offset_x\":-20,\"offset_y\":-20,\"scale\":1.0,\"hidden\":false")
			TEXT("},")
			TEXT("\"portrait_red\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":-200,\"offset_y\":30,\"scale\":1.2,\"hidden\":false,")
				TEXT("\"use_team_color\":\"true\"")
			TEXT("},")
			TEXT("\"portrait_blue\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":200,\"offset_y\":30,\"scale\":1.2,\"hidden\":false,")
				TEXT("\"use_team_color\":\"true\"")
			TEXT("},")
			TEXT("\"scorebar\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":0,\"offset_y\":4,\"scale\":1.15,\"hidden\":false,")
				TEXT("\"use_team_color\":\"true\",\"font\":\"Extreme\"")
			TEXT("},")
			TEXT("\"score_kda\":{")
				TEXT("\"anchor\":\"TopRight\",\"offset_x\":-40,\"offset_y\":16,\"scale\":1.1,\"hidden\":false")
			TEXT("},")
			TEXT("\"announcements\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":0,\"offset_y\":0,\"scale\":1.1,\"hidden\":false")
			TEXT("}")
		TEXT("}")
		TEXT("}")
	);

	// "Comp Minimal" - maximum screen real estate, minimum distraction.
	// Small bottom-center MinimalTypography HP/Armor, small BigNumber ammo,
	// accuracy widget visible, portraits + killfeed + powerups + voice
	// hidden, scorebar slimmed.
	static const FString CompMinimalJson(
		TEXT("{")
		TEXT("\"version\":1,")
		TEXT("\"elements\":{")
			TEXT("\"hp_armor\":{")
				TEXT("\"anchor\":\"BottomCenter\",\"offset_x\":0,\"offset_y\":-180,\"scale\":0.85,\"hidden\":false,")
				TEXT("\"style\":\"MinimalTypography\"")
			TEXT("},")
			TEXT("\"ammo\":{")
				TEXT("\"anchor\":\"BottomRight\",\"offset_x\":-30,\"offset_y\":-30,\"scale\":0.9,\"hidden\":false,")
				TEXT("\"style\":\"BigNumber\"")
			TEXT("},")
			TEXT("\"accuracy\":{")
				TEXT("\"anchor\":\"BottomRight\",\"offset_x\":-20,\"offset_y\":-110,\"scale\":1.0,\"hidden\":false")
			TEXT("},")
			TEXT("\"weapon_bar_left\":{")
				TEXT("\"anchor\":\"CenterLeft\",\"offset_x\":20,\"offset_y\":-20,\"scale\":0.9,\"hidden\":false")
			TEXT("},")
			TEXT("\"weapon_bar_right\":{")
				TEXT("\"anchor\":\"CenterRight\",\"offset_x\":-20,\"offset_y\":-20,\"scale\":0.9,\"hidden\":false")
			TEXT("},")
			TEXT("\"portrait_red\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":-200,\"offset_y\":30,\"scale\":1.0,\"hidden\":true")
			TEXT("},")
			TEXT("\"portrait_blue\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":200,\"offset_y\":30,\"scale\":1.0,\"hidden\":true")
			TEXT("},")
			TEXT("\"scorebar\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":0,\"offset_y\":4,\"scale\":0.85,\"hidden\":false,")
				TEXT("\"font\":\"Lato\",\"font_scale\":\"0.9\"")
			TEXT("},")
			TEXT("\"score_kda\":{")
				TEXT("\"anchor\":\"TopRight\",\"offset_x\":-40,\"offset_y\":16,\"scale\":0.9,\"hidden\":false")
			TEXT("},")
			TEXT("\"killfeed\":{")
				TEXT("\"anchor\":\"TopRight\",\"offset_x\":-20,\"offset_y\":80,\"scale\":1.0,\"hidden\":true")
			TEXT("},")
			TEXT("\"voice_status\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":0,\"offset_y\":0,\"scale\":1.0,\"hidden\":true")
			TEXT("},")
			TEXT("\"powerups\":{")
				TEXT("\"anchor\":\"BottomLeft\",\"offset_x\":40,\"offset_y\":-60,\"scale\":1.0,\"hidden\":true")
			TEXT("},")
			TEXT("\"announcements\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":0,\"offset_y\":0,\"scale\":0.8,\"hidden\":false")
			TEXT("}")
		TEXT("}")
		TEXT("}")
	);

	// "Quake Live Throwback" - left-stack nostalgia.
	// HP/Armor top-left in classic Q3 green/yellow (SegmentedBars), big
	// yellow BigNumber ammo bottom-right, no portraits (QL has none),
	// scorebar slim top-center, killfeed default.
	static const FString QLThrowbackJson(
		TEXT("{")
		TEXT("\"version\":1,")
		TEXT("\"elements\":{")
			TEXT("\"hp_armor\":{")
				TEXT("\"anchor\":\"TopLeft\",\"offset_x\":40,\"offset_y\":100,\"scale\":1.1,\"hidden\":false,")
				TEXT("\"style\":\"SegmentedBars\",")
				TEXT("\"color_health\":\"#66FF66FF\",\"color_armor\":\"#FFCC33FF\",")
				TEXT("\"color_health_number\":\"#FFFFFFFF\",\"color_armor_number\":\"#FFFFFFFF\"")
			TEXT("},")
			TEXT("\"ammo\":{")
				TEXT("\"anchor\":\"BottomRight\",\"offset_x\":-30,\"offset_y\":-30,\"scale\":1.2,\"hidden\":false,")
				TEXT("\"style\":\"BigNumber\",")
				TEXT("\"color_number\":\"#FFCC33FF\"")
			TEXT("},")
			TEXT("\"weapon_bar_left\":{")
				TEXT("\"anchor\":\"CenterLeft\",\"offset_x\":20,\"offset_y\":-20,\"scale\":1.0,\"hidden\":false")
			TEXT("},")
			TEXT("\"weapon_bar_right\":{")
				TEXT("\"anchor\":\"CenterRight\",\"offset_x\":-20,\"offset_y\":-20,\"scale\":1.0,\"hidden\":false")
			TEXT("},")
			TEXT("\"portrait_red\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":-200,\"offset_y\":30,\"scale\":1.0,\"hidden\":true")
			TEXT("},")
			TEXT("\"portrait_blue\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":200,\"offset_y\":30,\"scale\":1.0,\"hidden\":true")
			TEXT("},")
			TEXT("\"scorebar\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":0,\"offset_y\":4,\"scale\":0.95,\"hidden\":false,")
				TEXT("\"font\":\"Lato\"")
			TEXT("},")
			TEXT("\"score_kda\":{")
				TEXT("\"anchor\":\"TopRight\",\"offset_x\":-40,\"offset_y\":16,\"scale\":1.0,\"hidden\":false")
			TEXT("},")
			TEXT("\"announcements\":{")
				TEXT("\"anchor\":\"TopCenter\",\"offset_x\":0,\"offset_y\":0,\"scale\":1.0,\"hidden\":false")
			TEXT("}")
		TEXT("}")
		TEXT("}")
	);

	// Helper: build an entry via explicit field assignment. UE 4.15 + the
	// default member initializer on FNCPlusHUDPreset::bIsCustom disqualifies
	// the struct from aggregate brace-init, so T.Add({...}) fails to compile.
	// Field-assignment is the portable alternative.
	static FNCPlusHUDPreset MakePreset(const TCHAR* Id, const TCHAR* Display,
	                                   const TCHAR* Description, const FString& Json)
	{
		FNCPlusHUDPreset P;
		P.Id          = Id;
		P.DisplayName = Display;
		P.Description = Description;
		P.JsonString  = Json;
		P.bIsCustom   = false;
		return P;
	}

	static const TArray<FNCPlusHUDPreset>& BuildCuratedTable()
	{
		static const TArray<FNCPlusHUDPreset> Table = []()
		{
			TArray<FNCPlusHUDPreset> T;
			// Index 0 is the first-run default seed (referenced by
			// FNCPlusHUDLayout::ReloadLive when no on-disk layout exists).
			T.Add(MakePreset(
				TEXT("streamer_friendly"),
				TEXT("Streamer Friendly"),
				TEXT("Big legible HP/Armor, prominent portraits + scorebar, killfeed visible. Tuned for chat readers to follow the action."),
				StreamerFriendlyJson));
			T.Add(MakePreset(
				TEXT("comp_minimal"),
				TEXT("Comp Minimal"),
				TEXT("Slim HP/Armor, accuracy widget visible, portraits and killfeed hidden. Maximum screen for competitive play."),
				CompMinimalJson));
			T.Add(MakePreset(
				TEXT("ql_throwback"),
				TEXT("Quake Live Throwback"),
				TEXT("HP/Armor top-left in classic Q3 green/yellow, big yellow ammo bottom-right. Old-school left-stack feel."),
				QLThrowbackJson));
			return T;
		}();
		return Table;
	}

	// =========================================================================
	// Filesystem support - slug, dir, wrapper compose/decompose.
	// =========================================================================

	static FString MakeSlug(const FString& Name)
	{
		FString S = Name.ToLower();
		FString Out;
		Out.Reserve(S.Len());
		bool bLastWasUnderscore = true;  // suppress leading underscore
		for (TCHAR Ch : S)
		{
			const bool bAlnum = (Ch >= TEXT('a') && Ch <= TEXT('z'))
			                 || (Ch >= TEXT('0') && Ch <= TEXT('9'));
			if (bAlnum)
			{
				Out.AppendChar(Ch);
				bLastWasUnderscore = false;
			}
			else if (!bLastWasUnderscore)
			{
				Out.AppendChar(TEXT('_'));
				bLastWasUnderscore = true;
			}
		}
		// Trim trailing underscore.
		while (Out.Len() > 0 && Out[Out.Len() - 1] == TEXT('_'))
		{
			Out = Out.LeftChop(1);
		}
		return Out;
	}

	static FString WrapForDisk(const FString& Name, const FString& Description, const FString& InnerLayoutJson)
	{
		// Parse inner so we can embed it as a real object (not a string).
		TSharedPtr<FJsonObject> InnerObj;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(InnerLayoutJson);
		if (!FJsonSerializer::Deserialize(Reader, InnerObj) || !InnerObj.IsValid())
		{
			return FString();
		}
		TSharedRef<FJsonObject> Root = MakeShareable(new FJsonObject);
		Root->SetStringField(TEXT("preset_name"),        Name);
		Root->SetStringField(TEXT("preset_description"), Description);
		Root->SetObjectField(TEXT("layout"),             InnerObj);
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer
			= TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	static bool UnwrapFromDisk(const FString& WrapperJson, FString& OutName, FString& OutDescription, FString& OutLayoutJson)
	{
		TSharedPtr<FJsonObject> Root;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(WrapperJson);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return false;
		Root->TryGetStringField(TEXT("preset_name"),        OutName);
		Root->TryGetStringField(TEXT("preset_description"), OutDescription);
		const TSharedPtr<FJsonObject>* LayoutObjPtr = nullptr;
		if (!Root->TryGetObjectField(TEXT("layout"), LayoutObjPtr) || !LayoutObjPtr || !(*LayoutObjPtr).IsValid())
		{
			return false;
		}
		// Re-serialize the inner object back to a string so the caller can
		// pass it through FromJsonString unchanged.
		TSharedRef<TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>> Writer
			= TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&OutLayoutJson);
		FJsonSerializer::Serialize((*LayoutObjPtr).ToSharedRef(), Writer);
		return true;
	}
}

// =============================================================================
// Public API
// =============================================================================

namespace NCPlusHUDPresets
{
	const TArray<FNCPlusHUDPreset>& GetCurated()
	{
		return NCPlusHUDPresetsImpl::BuildCuratedTable();
	}

	FString GetCustomPresetsDir()
	{
		// UE 4.15 uses GameSavedDir(); ProjectSavedDir was renamed later (UE 4.18+).
		const FString Dir = FPaths::Combine(FPaths::GameSavedDir(), TEXT("NetcodePlus"), TEXT("Presets"));
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		return Dir;
	}

	TArray<FNCPlusHUDPreset> ScanCustoms()
	{
		TArray<FNCPlusHUDPreset> Out;
		const FString Dir = GetCustomPresetsDir();
		const FString Pattern = FPaths::Combine(Dir, TEXT("*.preset.json"));

		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *Pattern, /*Files=*/true, /*Directories=*/false);

		for (const FString& Filename : Files)
		{
			const FString FullPath = FPaths::Combine(Dir, Filename);
			FString Wrapper;
			if (!FFileHelper::LoadFileToString(Wrapper, *FullPath)) continue;

			FString Name, Description, LayoutJson;
			if (!NCPlusHUDPresetsImpl::UnwrapFromDisk(Wrapper, Name, Description, LayoutJson)) continue;

			// Slug = filename minus ".preset.json" suffix.
			FString Slug = Filename;
			Slug.RemoveFromEnd(TEXT(".preset.json"));
			if (Slug.IsEmpty()) continue;

			FNCPlusHUDPreset P;
			P.Id          = Slug;
			P.DisplayName = Name.IsEmpty() ? Slug : Name;
			P.Description = Description;
			P.JsonString  = LayoutJson;
			P.bIsCustom   = true;
			Out.Add(P);
		}

		Out.Sort([](const FNCPlusHUDPreset& A, const FNCPlusHUDPreset& B) {
			return A.DisplayName < B.DisplayName;
		});
		return Out;
	}

	TArray<FNCPlusHUDPreset> GetAll()
	{
		TArray<FNCPlusHUDPreset> Out;
		Out.Append(GetCurated());
		Out.Append(ScanCustoms());
		return Out;
	}

	bool SaveCustom(const FString& Name, const FString& Description, FString& OutId)
	{
		const FString Slug = NCPlusHUDPresetsImpl::MakeSlug(Name);
		if (Slug.IsEmpty()) return false;

		const FString Inner   = FNCPlusHUDLayout::GetLive().ToJsonString();
		const FString Wrapper = NCPlusHUDPresetsImpl::WrapForDisk(Name, Description, Inner);
		if (Wrapper.IsEmpty()) return false;

		const FString Dir  = GetCustomPresetsDir();
		const FString Path = FPaths::Combine(Dir, Slug + TEXT(".preset.json"));
		if (!FFileHelper::SaveStringToFile(Wrapper, *Path)) return false;

		OutId = Slug;
		return true;
	}

	bool DeleteCustom(const FString& Id)
	{
		// Curated ids are reserved.
		for (const FNCPlusHUDPreset& C : GetCurated())
		{
			if (C.Id == Id) return false;
		}
		const FString Path = FPaths::Combine(GetCustomPresetsDir(), Id + TEXT(".preset.json"));
		if (!FPaths::FileExists(Path)) return false;
		return IFileManager::Get().Delete(*Path, /*RequireExists=*/false, /*EvenReadOnly=*/true);
	}
}
