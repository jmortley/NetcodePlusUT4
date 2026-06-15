// NCPlusForceModels.cpp — see header. Client-local force-team-models config + resolvers.
#include "NCPlusForceModels.h"
#include "UnrealTournament.h"          // GetAllBlueprintAssetData
#include "UTCharacterContent.h"
#include "Misc/Paths.h"
#include "Misc/ConfigCacheIni.h"      // GConfig
#include "AssetRegistryModule.h"      // FAssetData
#include "UTCharacter.h"              // AUTCharacter::GetBodyMIs (dumpmats)
#include "TeamArenaCharacter.h"       // ReapplyAll iterates these pawns
#include "UTGameState.h"              // SyncHudTeamColours: GS->Teams
#include "UTTeamInfo.h"               // AUTTeamInfo::TeamColor
#include "UTPlayerController.h"       // local viewer for friend/enemy
#include "UTCTFGameState.h"           // SyncFlagColours: GetFlagBase
#include "UTCTFFlagBase.h"            // AUTCTFFlagBase::MyFlag
#include "UTFlag.h"                   // AUTFlag::GetMesh (cloth)
#include "Materials/Material.h"       // GetAll{Vector,Scalar}ParameterNames
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerState.h" // GetPlayerName
#include "EngineUtils.h"              // TActorIterator

namespace
{
	FNCPlusForceModelsConfig GFMConfig;          // not GConfig — that is the engine global
	bool                     GFMLoaded = false;
	TMap<FString, UClass*>   GFMClassCache;

	// Saved real TeamColor per team, captured before the first HUD-recolour overwrite so it can be
	// restored when HUD recolour is turned off. Weak keys so teams from a previous map drop out.
	TMap<TWeakObjectPtr<AUTTeamInfo>, FLinearColor> GHudOrigColours;

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

	// Optional baked-material denylist (comma-separated substrings) — models whose colour params are
	// inert; matched models fall back to their baked red/blue skin. See BakedMaterialSubstrings (header).
	C.BakedMaterialSubstrings.Reset();
	FString BakedStr;
	GConfig->GetString(TEXT("ForceModels"), TEXT("BakedMaterials"), BakedStr, Path);
	if (!BakedStr.IsEmpty())
	{
		TArray<FString> Parts;
		BakedStr.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Raw : Parts)
		{
			int32 S = 0, E = Raw.Len();
			while (S < E && FChar::IsWhitespace(Raw[S]))     { ++S; }
			while (E > S && FChar::IsWhitespace(Raw[E - 1])) { --E; }
			const FString T = Raw.Mid(S, E - S);
			if (!T.IsEmpty()) { C.BakedMaterialSubstrings.Add(T); }
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

void NCPlusForceModels::ReapplyAll(UWorld* World)
{
	if (!World) { return; }
	// Collect first: NotifyTeamChanged re-runs the base team-change, which can spawn/destroy a pawn's
	// LeaderHat — unsafe while a TActorIterator is live (same reason RefreshOtherForcedModels collects
	// before applying). Route through NotifyTeamChanged (not ApplyForcedModel directly) so the base
	// restores the real model first; a side the user just disabled then correctly un-forces.
	TArray<ATeamArenaCharacter*> Chars;
	for (TActorIterator<ATeamArenaCharacter> It(World); It; ++It)
	{
		if (ATeamArenaCharacter* C = *It) { Chars.Add(C); }
	}
	for (ATeamArenaCharacter* C : Chars)
	{
		C->NotifyTeamChanged();
	}
}

void NCPlusForceModels::SyncHudTeamColours(UWorld* World)
{
	// Client-side HUD/weapon/chat recolour (mirrors the BP's 0.25s "Update team colour" timer):
	// overwrite each team's replicated TeamColor with the configured skin colour for that team
	// relative to the local viewer, re-asserted each call so server replication can't revert it.
	// Originals are captured before the first overwrite and restored when HUD recolour is off.
	if (!World || World->GetNetMode() == NM_DedicatedServer) { return; }
	AUTGameState* GS = World->GetGameState<AUTGameState>();
	if (!GS) { return; }

	const FNCPlusForceModelsConfig& C = Get();
	const bool bWant = C.bEnabled && C.bHUD;
	AUTPlayerController* LocalPC = Cast<AUTPlayerController>(World->GetFirstPlayerController());

	for (AUTTeamInfo* Team : GS->Teams)
	{
		if (!Team) { continue; }
		const bool bFriendly = (LocalPC && GS->OnSameTeam(Team, LocalPC));
		// Enemy-Only leaves teammates untouched; every other style recolours both teams.
		const bool bApply = bWant && !(C.Style == ENCPlusSkinStyle::EnemyOnly && bFriendly);

		if (bApply)
		{
			if (!GHudOrigColours.Contains(Team)) { GHudOrigColours.Add(Team, Team->TeamColor); }
			Team->TeamColor = GetSkinColour(GetModelSettings(Team->GetTeamNum(), bFriendly));
		}
		else if (FLinearColor* Orig = GHudOrigColours.Find(Team))
		{
			Team->TeamColor = *Orig;          // restore when HUD recolour turns off / style excludes this team
			GHudOrigColours.Remove(Team);
		}
	}
}

void NCPlusForceModels::SyncFlagColours(UWorld* World)
{
	// Recolour the CTF flag cloth to each team's configured skin colour (relative to the local viewer).
	// Same shotgun param set as the character recolour; flag mesh MIDs are created on demand and reused
	// (re-created automatically when a flag respawns on capture). ClearParameterValues() restores the
	// flag's real colour when the feature/flag is off. No-op outside CTF.
	if (!World || World->GetNetMode() == NM_DedicatedServer) { return; }
	AUTCTFGameState* GS = World->GetGameState<AUTCTFGameState>();
	if (!GS) { return; }   // not a CTF mode -> no flags

	const FNCPlusForceModelsConfig& C = Get();
	const bool bWant = C.bEnabled && C.bFlags;
	AUTPlayerController* LocalPC = Cast<AUTPlayerController>(World->GetFirstPlayerController());
	const TArray<FName>& Params = TeamColourParamNames();

	static const FName NAME_FlagTeamSelect(TEXT("TeamSelect"));
	static const FName NAME_FlagBlendMax(TEXT("Team Color Blend Max"));

	for (uint8 Team = 0; Team < 2; ++Team)
	{
		AUTCTFFlagBase* Base = GS->GetFlagBase(Team);
		if (!Base || !Base->MyFlag) { continue; }
		USkeletalMeshComponent* Mesh = Base->MyFlag->GetMesh();
		if (!Mesh) { continue; }

		const bool bFriendly = (LocalPC && GS->OnSameTeam(Base->MyFlag, LocalPC));
		const FLinearColor Colour = GetSkinColour(GetModelSettings(Team, bFriendly));

		for (int32 i = 0; i < Mesh->GetNumMaterials(); ++i)
		{
			UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(i));
			if (!MID)
			{
				if (!bWant) { continue; }                                  // nothing to restore if we never made a MID
				MID = Mesh->CreateAndSetMaterialInstanceDynamic(i);
				if (!MID) { continue; }
			}

			if (bWant)
			{
				MID->SetScalarParameterValue(NAME_FlagTeamSelect, 255.f);  // NoTeam path, like the body recolour
				MID->SetScalarParameterValue(NAME_FlagBlendMax, 1.f);
				for (const FName& P : Params) { MID->SetVectorParameterValue(P, Colour); }
			}
			else
			{
				MID->ClearParameterValues();                               // restore the flag's real colour
			}
		}
	}
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

bool NCPlusForceModels::IsBakedMaterial(const FString& MaterialName)
{
	// No built-in default — param-LESS models are auto-detected at apply time; this denylist only
	// catches models whose params exist but are inert (can't be told apart from the material API).
	const FNCPlusForceModelsConfig& C = Get();
	for (const FString& Sub : C.BakedMaterialSubstrings)
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
		static const FName NAME_EmisPower(TEXT("Emission Power"));
		static const FName NAME_NoTeam(TEXT("NoTeamColor"));
		static const FName NAME_Red(TEXT("Red Team Color"));
		static const FName NAME_Blue(TEXT("Blue Team Color"));
		for (UMaterialInstanceDynamic* MID : MIDs)
		{
			if (!MID) { continue; }
			const UMaterialInterface* Src = MID->Parent;
			float TeamSel = -1.f, BlendMax = -1.f, EmisMax = -1.f, EmisPower = -1.f;
			MID->GetScalarParameterValue(NAME_TeamSelect, TeamSel);
			const bool bBlend = MID->GetScalarParameterValue(NAME_BlendMax, BlendMax);
			const bool bEmis  = MID->GetScalarParameterValue(NAME_EmisMax,  EmisMax);
			const bool bPow   = MID->GetScalarParameterValue(NAME_EmisPower, EmisPower);
			FLinearColor NoTeam(ForceInit), RedC(ForceInit), BlueC(ForceInit);
			const bool bNo  = MID->GetVectorParameterValue(NAME_NoTeam, NoTeam);
			const bool bRed = MID->GetVectorParameterValue(NAME_Red,    RedC);
			const bool bBlu = MID->GetVectorParameterValue(NAME_Blue,   BlueC);
			UE_LOG(LogTemp, Warning, TEXT("   mat='%s'  TeamSelect=%.0f  BlendMax=%.2f[%d]  EmisMax=%.2f[%d]  EmisPow=%.2f[%d]  NoTeam=%s[%d]  Red=%s[%d]  Blue=%s[%d]"),
				Src ? *Src->GetName() : *MID->GetName(), TeamSel,
				BlendMax, bBlend ? 1 : 0, EmisMax, bEmis ? 1 : 0, EmisPower, bPow ? 1 : 0,
				*NoTeam.ToString(), bNo ? 1 : 0,
				*RedC.ToString(),   bRed ? 1 : 0,
				*BlueC.ToString(),  bBlu ? 1 : 0);
		}
		++Count;
	}
	UE_LOG(LogTemp, Warning, TEXT("[ForceModels] dumpmats: %d character(s) total"), Count);
}
