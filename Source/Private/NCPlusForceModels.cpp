// NCPlusForceModels.cpp — see header. Client-local force-team-models config + resolvers.
#include "NCPlusForceModels.h"
#include "UnrealTournament.h"          // GetAllBlueprintAssetData
#include "UTCharacterContent.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"      // GConfig
#include "AssetRegistryModule.h"      // FAssetData
#include "UTCharacter.h"              // AUTCharacter::GetBodyMIs (dumpmats)
#include "Materials/Material.h"       // GetAll{Vector,Scalar}ParameterNames
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerState.h" // GetPlayerName
#include "EngineUtils.h"              // TActorIterator

namespace
{
	FNCPlusForceModelsConfig GFMConfig;          // not GConfig — that is the engine global
	bool                     GFMLoaded = false;
	TMap<FString, UClass*>   GFMClassCache;

	// Hard ceiling on the highlight-glow brightness — the REAL cap. A client-side Mod.ini "cap"
	// would be meaningless (the client owns its own config), so the only enforceable limits are
	// this compiled-in ceiling (shipped in the signed plugin) and a future server-replicated cap
	// owned by AMutForceModels (consulted like IsModelAllowed). Tune here during dev.
	constexpr float kMaxSkinBrightness = 1.75f;

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
		GConfig->GetFloat (*Sec, TEXT("Brightness"), Out.Brightness, Path);
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
		GConfig->SetFloat (*Sec, TEXT("Brightness"),    S.Brightness,              Path);
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
	// Base albedo tint. Matches the BP's "HSV to RGB" node (Kismet): H in degrees, S/V 0-1.
	return FLinearColor(Side.H, Side.S, Side.V, 1.f).HSVToLinearRGB();
}

FLinearColor NCPlusForceModels::GetEmissiveColour(const FNCPlusModelSettings& Side)
{
	// Highlight glow = base colour scaled by Brightness, fed to emissive/overlay params only.
	// Clamped to the compiled-in kMaxSkinBrightness ceiling (see note there) so it stays well
	// short of UTComp fullbright skins, regardless of what a client puts in Mod.ini.
	const float B  = FMath::Clamp(Side.Brightness, 0.f, kMaxSkinBrightness);
	FLinearColor C = GetSkinColour(Side);
	C.R *= B; C.G *= B; C.B *= B; C.A = 1.f;
	return C;
}

bool NCPlusForceModels::IsEmissiveParam(FName Param)
{
	// Glow channels: only these get the (possibly >1) emissive boost; albedo params stay at the
	// base colour so a boost never blows out or hue-shifts the skin.
	const FString S = Param.ToString();
	return S.Contains(TEXT("Emissive"), ESearchCase::IgnoreCase)
		|| S.Contains(TEXT("Overlay"),  ESearchCase::IgnoreCase);
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

		// No-Team / neutral-path colour params. REQUIRED for EDMSkin_Base community models: they
		// render on the NoTeam path (TeamSelect=255) even while on a team, so the Red/Blue params
		// above are never read and the model would stay its default neutral. Observed: Robot
		// 'NoTeamColor', Necris 'No Team Latex/Dark Plastic/Cloth/PseudoMetal'. Mirror the full zone
		// set; names a material lacks just no-op.
		FName(TEXT("NoTeamColor")),            FName(TEXT("No Team Color")),
		FName(TEXT("No Team Camo")),           FName(TEXT("No Team Nylon")),
		FName(TEXT("No Team Plastic")),        FName(TEXT("No Team SubPlastic")),
		FName(TEXT("No Team Cloth")),          FName(TEXT("No Team Dark Plastic")),
		FName(TEXT("No Team Latex")),          FName(TEXT("No Team PseudoMetal")),
		FName(TEXT("No Team Armor")),
	};
	return Names;
}

void NCPlusForceModels::DumpAllCharacterMaterials(UWorld* World)
{
	if (!World) { UE_LOG(LogTemp, Warning, TEXT("[ForceModels] dumpmats: no world")); return; }
	int32 Count = 0;
	for (TActorIterator<AUTCharacter> It(World); It; ++It)
	{
		AUTCharacter* Char = *It;
		if (!Char) { continue; }
		const FString PawnName = Char->PlayerState ? Char->PlayerState->PlayerName : Char->GetName();
		const TArray<UMaterialInstanceDynamic*>& MIDs = Char->GetBodyMIs();
		UE_LOG(LogTemp, Warning, TEXT("[ForceModels] '%s' — %d body material(s), LIVE values:"), *PawnName, MIDs.Num());
		static const FName NAME_TeamSelect(TEXT("TeamSelect"));
		static const FName NAME_BlendMax(TEXT("Team Color Blend Max"));
		static const FName NAME_EmisMax(TEXT("Emissive Max"));
		static const FName NAME_NoTeam(TEXT("NoTeamColor"));
		static const FName NAME_Red(TEXT("Red Team Color"));
		static const FName NAME_Blue(TEXT("Blue Team Color"));
		for (UMaterialInstanceDynamic* MID : MIDs)
		{
			if (!MID) { continue; }
			const UMaterialInterface* Src = MID->Parent;
			float TeamSel = -1.f, BlendMax = -1.f, EmisMax = -1.f;
			MID->GetScalarParameterValue(NAME_TeamSelect, TeamSel);
			const bool bBlend = MID->GetScalarParameterValue(NAME_BlendMax, BlendMax);
			const bool bEmis  = MID->GetScalarParameterValue(NAME_EmisMax,  EmisMax);
			FLinearColor NoTeam(ForceInit), RedC(ForceInit), BlueC(ForceInit);
			const bool bNo  = MID->GetVectorParameterValue(NAME_NoTeam, NoTeam);
			const bool bRed = MID->GetVectorParameterValue(NAME_Red,    RedC);
			const bool bBlu = MID->GetVectorParameterValue(NAME_Blue,   BlueC);
			UE_LOG(LogTemp, Warning, TEXT("   mat='%s'  TeamSelect=%.0f  BlendMax=%.2f[%d]  EmisMax=%.2f[%d]  NoTeam=%s[%d]  Red=%s[%d]  Blue=%s[%d]"),
				Src ? *Src->GetName() : *MID->GetName(), TeamSel,
				BlendMax, bBlend ? 1 : 0, EmisMax, bEmis ? 1 : 0,
				*NoTeam.ToString(), bNo ? 1 : 0,
				*RedC.ToString(),   bRed ? 1 : 0,
				*BlueC.ToString(),  bBlu ? 1 : 0);
		}
		++Count;
	}
	UE_LOG(LogTemp, Warning, TEXT("[ForceModels] dumpmats: %d character(s) total"), Count);
}
