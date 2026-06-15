// NCPlusForceModels.cpp — see header. Client-local force-team-models config + resolvers.
#include "NCPlusForceModels.h"
#include "UnrealTournament.h"          // GetAllBlueprintAssetData
#include "UTCharacterContent.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"      // GConfig
#include "AssetRegistryModule.h"      // FAssetData

namespace
{
	FNCPlusForceModelsConfig GFMConfig;          // not GConfig — that is the engine global
	bool                     GFMLoaded = false;
	TMap<FString, UClass*>   GFMClassCache;

	FString ModIniPath()
	{
		// Same store the rest of NetcodePlus uses (see SUTCosmeticSelector).
		return FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	}

	void ReadSide(const TCHAR* Suffix, FNCPlusModelSettings& Out)
	{
		const FString Path = ModIniPath();
		const FString Sec  = FString::Printf(TEXT("ForceModels.Model.%s"), Suffix);
		GConfig->GetString(*Sec, TEXT("Class"), Out.ContentPath, Path);
		GConfig->GetFloat (*Sec, TEXT("H"),     Out.H,           Path);
		GConfig->GetFloat (*Sec, TEXT("S"),     Out.S,           Path);
		GConfig->GetFloat (*Sec, TEXT("V"),     Out.V,           Path);
		int32 Comp = 0; GConfig->GetInt(*Sec, TEXT("Complimentary"), Comp, Path); Out.bComplimentary = (Comp != 0);
		int32 AM   = 0; GConfig->GetInt(*Sec, TEXT("ArmourMode"),    AM,   Path); Out.ArmourMode = (ENCPlusArmourMode)AM;
	}

	void WriteSide(const TCHAR* Suffix, const FNCPlusModelSettings& S)
	{
		const FString Path = ModIniPath();
		const FString Sec  = FString::Printf(TEXT("ForceModels.Model.%s"), Suffix);
		GConfig->SetString(*Sec, TEXT("Class"),         *S.ContentPath,            Path);
		GConfig->SetFloat (*Sec, TEXT("H"),             S.H,                       Path);
		GConfig->SetFloat (*Sec, TEXT("S"),             S.S,                       Path);
		GConfig->SetFloat (*Sec, TEXT("V"),             S.V,                       Path);
		GConfig->SetInt   (*Sec, TEXT("Complimentary"), S.bComplimentary ? 1 : 0,  Path);
		GConfig->SetInt   (*Sec, TEXT("ArmourMode"),    (int32)S.ArmourMode,       Path);
	}
}

void NCPlusForceModels::Reload()
{
	const FString Path = ModIniPath();
	FNCPlusForceModelsConfig& C = GFMConfig;
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Enabled"),      C.bEnabled,      Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Models"),       C.bModels,       Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("HUD"),          C.bHUD,          Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Armour"),       C.bArmour,       Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Flags"),        C.bFlags,        Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("DarkenBodies"), C.bDarkenBodies, Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Cosmetics"),    C.bCosmetics,    Path);
	int32 StyleInt = 0;
	GConfig->GetInt(TEXT("ForceModels"), TEXT("Style"), StyleInt, Path);
	C.Style = (ENCPlusSkinStyle)StyleInt;
	ReadSide(TEXT("Enemy"), C.Enemy);
	ReadSide(TEXT("Team"),  C.Team);
	ReadSide(TEXT("Red"),   C.Red);
	ReadSide(TEXT("Blue"),  C.Blue);

	// Optional recolour-param override (comma-separated; names may contain spaces).
	// Lets you tune which params get team-coloured (e.g. armour-only, leave body/face).
	C.RecolorParams.Reset();
	FString ParamsStr;
	GConfig->GetString(TEXT("ForceModels"), TEXT("RecolorParams"), ParamsStr, Path);
	if (!ParamsStr.IsEmpty())
	{
		TArray<FString> Parts;
		ParamsStr.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Raw : Parts)
		{
			int32 S = 0, E = Raw.Len();
			while (S < E && FChar::IsWhitespace(Raw[S]))     { ++S; }
			while (E > S && FChar::IsWhitespace(Raw[E - 1])) { --E; }
			const FString T = Raw.Mid(S, E - S);
			if (!T.IsEmpty()) { C.RecolorParams.Add(FName(*T)); }
		}
	}

	// Optional material-skip override (comma-separated substrings).
	C.SkipMaterialSubstrings.Reset();
	FString SkipStr;
	GConfig->GetString(TEXT("ForceModels"), TEXT("SkipMaterials"), SkipStr, Path);
	if (!SkipStr.IsEmpty())
	{
		TArray<FString> Parts;
		SkipStr.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Raw : Parts)
		{
			int32 S = 0, E = Raw.Len();
			while (S < E && FChar::IsWhitespace(Raw[S]))     { ++S; }
			while (E > S && FChar::IsWhitespace(Raw[E - 1])) { --E; }
			const FString T = Raw.Mid(S, E - S);
			if (!T.IsEmpty()) { C.SkipMaterialSubstrings.Add(T); }
		}
	}

	GFMLoaded = true;
}

const FNCPlusForceModelsConfig& NCPlusForceModels::Get()
{
	if (!GFMLoaded) { Reload(); }
	return GFMConfig;
}

FNCPlusForceModelsConfig& NCPlusForceModels::Mutable()
{
	if (!GFMLoaded) { Reload(); }
	return GFMConfig;
}

void NCPlusForceModels::Save()
{
	const FString Path = ModIniPath();
	const FNCPlusForceModelsConfig& C = GFMConfig;
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Enabled"),      C.bEnabled,      Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Models"),       C.bModels,       Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("HUD"),          C.bHUD,          Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Armour"),       C.bArmour,       Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Flags"),        C.bFlags,        Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("DarkenBodies"), C.bDarkenBodies, Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Cosmetics"),    C.bCosmetics,    Path);
	GConfig->SetInt (TEXT("ForceModels"), TEXT("Style"),        (int32)C.Style,  Path);
	WriteSide(TEXT("Enemy"), C.Enemy);
	WriteSide(TEXT("Team"),  C.Team);
	WriteSide(TEXT("Red"),   C.Red);
	WriteSide(TEXT("Blue"),  C.Blue);
	GConfig->Flush(false, Path);
}

bool NCPlusForceModels::IsEnabled()
{
	const FNCPlusForceModelsConfig& C = Get();
	return C.bEnabled && C.bModels;
}

const FNCPlusModelSettings& NCPlusForceModels::GetModelSettings(int32 TheirTeamIndex, bool bIsFriendly)
{
	static const FNCPlusModelSettings EmptySide;   // empty ContentPath -> applier skips this pawn
	const FNCPlusForceModelsConfig& C = Get();
	switch (C.Style)
	{
	case ENCPlusSkinStyle::RedBlue:   return (TheirTeamIndex == 0) ? C.Red : C.Blue;   // red team = 0
	case ENCPlusSkinStyle::EnemyOnly: return bIsFriendly ? EmptySide : C.Enemy;
	case ENCPlusSkinStyle::TeamEnemy:
	default:                          return bIsFriendly ? C.Team : C.Enemy;
	}
}

FLinearColor NCPlusForceModels::GetSkinColour(const FNCPlusModelSettings& Side)
{
	// Matches the BP's "HSV to RGB" node (Kismet): H in degrees, S/V 0-1.
	return FLinearColor(Side.H, Side.S, Side.V, 1.f).HSVToLinearRGB();
}

TSubclassOf<AUTCharacterContent> NCPlusForceModels::GetModelClass(const FNCPlusModelSettings& Side)
{
	if (Side.ContentPath.IsEmpty()) { return nullptr; }
	if (UClass** Found = GFMClassCache.Find(Side.ContentPath)) { return *Found; }
	UClass* Loaded = StaticLoadClass(AUTCharacterContent::StaticClass(), nullptr, *Side.ContentPath);
	if (Loaded)
	{
		Loaded->AddToRoot();                          // StaticLoadClass results are not auto-rooted
		GFMClassCache.Add(Side.ContentPath, Loaded);
	}
	return Loaded;
}

bool NCPlusForceModels::IsModelAllowed(TSubclassOf<AUTCharacterContent> Content)
{
	// Phase 1: client-local, no server policy yet. The server-side AMutForceModels will later
	// own a replicated allow/deny policy (Disabled / Restricted+allowlist / Free) consulted here.
	return Content != nullptr;
}

void NCPlusForceModels::EnumerateContent(TArray<FContentEntry>& Out)
{
	TArray<FAssetData> Assets;
	GetAllBlueprintAssetData(AUTCharacterContent::StaticClass(), Assets, /*bRequireEntitlements=*/false);
	Out.Reserve(Assets.Num());
	for (const FAssetData& A : Assets)
	{
		FContentEntry E;
		E.ClassPath   = A.ObjectPath.ToString() + TEXT("_C");
		E.DisplayName = A.AssetName.ToString();
		Out.Add(E);
	}
}

bool NCPlusForceModels::IsRecolorSkippedMaterial(const FString& MaterialName)
{
	static const TArray<FString> DefaultSkip = {
		TEXT("head"), TEXT("face"), TEXT("eye"), TEXT("hair"),
		TEXT("teeth"), TEXT("tongue"), TEXT("mouth"), TEXT("brow"),
	};
	const FNCPlusForceModelsConfig& C = Get();
	const TArray<FString>& Skip = (C.SkipMaterialSubstrings.Num() > 0) ? C.SkipMaterialSubstrings : DefaultSkip;
	for (const FString& Sub : Skip)
	{
		if (MaterialName.Contains(Sub, ESearchCase::IgnoreCase)) { return true; }
	}
	return false;
}

const TArray<FName>& NCPlusForceModels::TeamColourParamNames()
{
	// Mod.ini override wins if set ([ForceModels] RecolorParams=Name1,Name2,...).
	const FNCPlusForceModelsConfig& C = Get();
	if (C.RecolorParams.Num() > 0) { return C.RecolorParams; }

	// Default: known UT character team-colour params MINUS the head/face params
	// (so faces stay natural). NOTE: the broad "...Team Color" / "TeamColor" params
	// tint body/skin on some models (e.g. Skaarj) as well as armour — if that over-
	// colours, set RecolorParams in Mod.ini to a narrower armour-only list.
	static const TArray<FName> Names = {
		FName(TEXT("Blue Team Camo")),         FName(TEXT("Red Team Camo")),
		FName(TEXT("Blue Team Color")),        FName(TEXT("Red Team Color")),
		FName(TEXT("Blue Team Nylon")),        FName(TEXT("Red Team Nylon")),
		FName(TEXT("Blue Team Plastic")),      FName(TEXT("Red Team Plastic")),
		FName(TEXT("Blue Team SubPlastic")),   FName(TEXT("Red Team SubPlastic")),
		FName(TEXT("Blue Team Cloth")),        FName(TEXT("Red Team Cloth")),
		FName(TEXT("Blue Team Dark Plastic")), FName(TEXT("Red Team Dark Plastic")),
		FName(TEXT("Blue Team Latex")),        FName(TEXT("Red Team Latex")),
		FName(TEXT("Blue Team PseudoMetal")),  FName(TEXT("Red Team PseudoMetal")),
		FName(TEXT("Blue Team Armor")),        FName(TEXT("Red Team Armor")),
		FName(TEXT("Blue Overlay")),           FName(TEXT("Red Overlay")),
		FName(TEXT("Blue Emissive Color")),    FName(TEXT("Red Emissive Color")),
		FName(TEXT("Blue Emissive")),          FName(TEXT("Red Emissive")),
		FName(TEXT("Blue tint on alpha")),     FName(TEXT("Red tint on alpha")),
		FName(TEXT("Solo Overlay")),           FName(TEXT("Solo Distance Color")),
		FName(TEXT("TeamColor")),              // stock UT base team vector param
	};
	return Names;
}
