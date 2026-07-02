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
#include "UTPlayerController.h"       // local viewer for friend/enemy; bTacComView (X-Ray) guard
#include "Camera/PlayerCameraManager.h"  // OutlinePlayers: viewer eye for the LOS traces
#include "UTPlayerCameraManager.h"    // SwapOutlineMaterial: OutlineMat + DefaultPPSettings blendable
#include "UTCTFGameState.h"           // SyncFlagColours: GetFlagBase
#include "UTCTFFlagBase.h"            // AUTCTFFlagBase::MyFlag
#include "UTFlag.h"                   // AUTFlag::GetMesh (cloth)
#include "Materials/Material.h"       // GetAll{Vector,Scalar}ParameterNames
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialParameterCollection.h"       // MPC_NCPOutline: per-team outline colours
#include "Kismet/KismetMaterialLibrary.h"                // SetVectorParameterValue on the MPC
#include "Engine/SkeletalMesh.h"      // SyncFlagColours: swap to dc's FlagMesh
#include "Engine/WindDirectionalSource.h"            // TickFlagWind: cloth wind (ports dc's FlagWind)
#include "Components/WindDirectionalSourceComponent.h"
#include "UnrealEngine.h"                             // GetCachedScalabilityCVars().DetailMode (flag cloth)
#include "GameFramework/PlayerState.h" // GetPlayerName
#include "EngineUtils.h"              // TActorIterator
#include "Engine/Canvas.h"            // DrawHeadDebug: Canvas->Project / K2_DrawLine

namespace
{
	FString ModIniPath();   // defined below; forward-declared for the flag-mesh/wind helpers above it

	FNCPlusForceModelsConfig GFMConfig;          // not GConfig — that is the engine global
	bool                     GFMLoaded = false;
	TMap<FString, UClass*>   GFMClassCache;

	// Saved real TeamColor per team, captured before the first HUD-recolour overwrite so it can be
	// restored when HUD recolour is turned off. Weak keys so teams from a previous map drop out.
	TMap<TWeakObjectPtr<AUTTeamInfo>, FLinearColor> GHudOrigColours;

	// Flag carriers we've forced bForceNoOutline on, so we can restore them when they drop the flag
	// (or the suppression is turned off). Weak keys so GC'd pawns drop out.
	TSet<TWeakObjectPtr<AUTCharacter>> GOutlineSuppressed;

	// Players the ForceModels "Outline" pass is managing -> whether their outline is currently ON
	// (= they were visible on the last LOS check). Calls into SetOutlineLocal are gated on transitions
	// of this state (see OutlinePlayers); ones that drop out (toggle off / left / dead / round change)
	// are restored. Weak keys so GC'd pawns drop out.
	TMap<TWeakObjectPtr<AUTCharacter>, bool> GOutlined;

	// ── Outline content paths (Mod.ini [ForceModels], authoring-time constants — no F5 UI) ──────────
	// OutlineMPC      = full object path of the colour MaterialParameterCollection the outline pass feeds.
	// OutlineMaterial = full object path of the recoloured outline PP material (EMPTY = keep stock).
	// M_OutlinePP (and MF_TeamColorOutlines) are RestrictedAssets — the UT editor refuses to SAVE edits
	// under that path — so the recolour ships as NEW assets in an NCP content pak and the plugin
	// re-points the renderer at them (SwapOutlineMaterial below).
	FString GOutlineMPCPath(TEXT("/Game/RestrictedAssets/Materials/MPC_NCPOutline.MPC_NCPOutline"));
	FString GOutlineMatPath;
	bool    GOutlinePathsLoaded = false;
	// LoadObject is tried at most once per WORLD per asset: GC can unload them on travel (nothing roots
	// them between camera managers), so FindObject alone would go stale after a map change — but a
	// missing asset must not load-attempt (and warn) every slow tick either.
	TWeakObjectPtr<UWorld> GOutlineAssetWorld;
	bool GOutlineTriedMPC = false;
	bool GOutlineTriedMat = false;

	// Last OutlineModeActive verdict, refreshed once per frame by OutlinePlayers. For per-pawn
	// per-frame call sites (TeamArenaCharacter::Tick overlay retint) — the full check copies the
	// side settings (FStrings) + does HSV math, too heavy to run per pawn per frame.
	bool GOutlineModeActiveCache = false;

	template<typename T>
	T* ResolveOutlineAsset(const FString& Path, bool& bTriedThisWorld)
	{
		if (Path.IsEmpty()) { return nullptr; }
		T* Found = FindObject<T>(nullptr, *Path);
		if (!Found && !bTriedThisWorld)
		{
			bTriedThisWorld = true;
			Found = LoadObject<T>(nullptr, *Path);
			if (!Found)
			{
				UE_LOG(LogTemp, Warning, TEXT("[ForceModels] outline asset '%s' not found (pak missing / path typo) — stock look stands"), *Path);
			}
		}
		return Found;
	}

	// The stock outline PP material is injected in TWO places (UTPlayerCameraManager.cpp): baked into
	// DefaultPPSettings.WeightedBlendables in the ctor (:48) and re-added per frame from the OutlineMat
	// UPROPERTY on maps that have their own PP volumes (:569, a function-local — no hook point). So we
	// REPLACE the pointer in both sites. Never AddBlendable a second outline material: blendables are
	// additive full-screen passes and the stock pass would still composite its red/blue rim under ours
	// (double draw + an extra pass). Re-asserted each slow tick — camera managers respawn per level /
	// possession, and the idempotent pointer-compare makes the re-assert free.
	void SwapOutlineMaterial(UWorld* World)
	{
		UMaterialInterface* const OurMat = ResolveOutlineAsset<UMaterialInterface>(GOutlineMatPath, GOutlineTriedMat);
		if (!OurMat) { return; }
		APlayerController* const PC = World->GetFirstPlayerController();
		AUTPlayerCameraManager* const PCM = PC ? Cast<AUTPlayerCameraManager>(PC->PlayerCameraManager) : nullptr;
		if (!PCM || PCM->OutlineMat == OurMat) { return; }
		for (FWeightedBlendable& WB : PCM->DefaultPPSettings.WeightedBlendables.Array)
		{
			if (WB.Object != nullptr && WB.Object == PCM->OutlineMat) { WB.Object = OurMat; }
		}
		PCM->OutlineMat = OurMat;
	}

	// Flag recolour is now PURE STOCK — no dc mesh, no dc material. The stock CTF flag material
	// (MI_CTF_RedFlag / MI_CTF_BlueFlag, parent M_CTF_Flag) already exposes a "FlagColor" vector param;
	// SyncFlagColours just MIDs the stock flag's slots and sets it. No loaders / no swap state needed.

	// ── Flag wind (ports dc's FlagWind BP) ──────────────────────────────────────────────────────────
	// One client-local WindDirectionalSource whose speed + direction wander each frame so the cloth waves.
	TWeakObjectPtr<AWindDirectionalSource> GFlagWind;
	float   GWindSpeed   = 0.f;
	FVector GWindAngVel  = FVector::ZeroVector;   // deg/s, euler; BP only randomises the X component
	FVector GWindRot     = FVector::ZeroVector;   // accumulated euler (X=roll, Y=pitch, Z=yaw)
	// Tuning (defaults are GUESSES — dc's BP variable defaults not on the screenshot; overridable below).
	bool    GWindKnobsLoaded   = false;
	float   GWindMinSpeed      = 0.3f;
	float   GWindMaxSpeed      = 2.0f;
	float   GWindMaxAccel      = 1.5f;            // units/s^2
	float   GWindMaxAngSpeed   = 25.f;           // deg/s
	float   GWindMaxAngAccel   = 50.f;           // deg/s^2

	void LoadWindKnobs()
	{
		if (GWindKnobsLoaded) { return; }
		GWindKnobsLoaded = true;
		const FString P = ModIniPath();
		GConfig->GetFloat(TEXT("ForceModels.FlagWind"), TEXT("MinSpeed"),              GWindMinSpeed,    P);
		GConfig->GetFloat(TEXT("ForceModels.FlagWind"), TEXT("MaxSpeed"),              GWindMaxSpeed,    P);
		GConfig->GetFloat(TEXT("ForceModels.FlagWind"), TEXT("MaxAcceleration"),       GWindMaxAccel,    P);
		GConfig->GetFloat(TEXT("ForceModels.FlagWind"), TEXT("MaxAngularSpeed"),       GWindMaxAngSpeed, P);
		GConfig->GetFloat(TEXT("ForceModels.FlagWind"), TEXT("MaxAngularAcceleration"), GWindMaxAngAccel, P);
	}

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

// One-time dc MutTeamSkins -> ForceModels onboarding seed (defined below, after StemOf so its
// file-scope definition is in scope there). file-internal linkage matches the definition.
static void MaybeMigrateFromTeamSkins(const FString& Path);

void NCPlusForceModels::Reload()
{
	const FString Path = ModIniPath();

	// First-init only: if this player has NO [ForceModels] config yet AND dc's older [TeamSkins]
	// config is present in the same Mod.ini, seed [ForceModels.*] from it (run-once flag in
	// [ForceModels.Versioning]). Writes + Flush happen BEFORE the reads below, so the seeded values
	// land in GFMConfig in this same call. Cheap no-op on the common already-configured path.
	MaybeMigrateFromTeamSkins(Path);

	FNCPlusForceModelsConfig& C = GFMConfig;
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Enabled"),      C.bEnabled,      Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Models"),       C.bModels,       Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("HUD"),          C.bHUD,          Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Armour"),       C.bArmour,       Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Flags"),        C.bFlags,        Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("DarkenBodies"), C.bDarkenBodies, Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Cosmetics"),    C.bCosmetics,    Path);
	GConfig->GetBool(TEXT("ForceModels"), TEXT("Outline"),      C.bOutline,      Path);
	int32 StyleInt = 0;
	GConfig->GetInt(TEXT("ForceModels"), TEXT("Style"), StyleInt, Path);
	C.Style = (ENCPlusSkinStyle)StyleInt;
	ReadSide(TEXT("Enemy"), C.Enemy);
	ReadSide(TEXT("Team"),  C.Team);
	ReadSide(TEXT("Red"),   C.Red);
	ReadSide(TEXT("Blue"),  C.Blue);

	// Seed usable Red/Blue side colours. The Red/Blue style takes S/V/Glow from these sides, but
	// nothing ever seeded them — fresh configs sat at the struct defaults (H=0 S=1 V=1 Glow=1), so
	// the style rendered flat pure-hue colours incl. a near-black pure blue (linear (0,0,1) ~7%
	// luma vs red's ~21%) — community "brightness is busted on red/blue" (2026-06-30). A PRISTINE
	// side == exactly the untouched defaults; that also heals configs that already SAVED those
	// defaults (indistinguishable, and nobody plausibly wants zero-overbright pure-red "Blue").
	// Blue seeds S=0.9 (≈ stock BLUEHUDCOLOR) to close the red-vs-blue luminance gap. Glow 1.5 —
	// NOT dc's 3.0: dc ran x3 over DESATURATED colours (S/V ≈ 0.4); on near-pure hues x3 is
	// radioactive (user-verified "way too strong" 2026-07-01). A user-edited side is never touched;
	// the F5 Red/Blue rows are live for taste-tuning.
	{
		auto SeedRedBlueSide = [](FNCPlusModelSettings& Side, float SeedH, float SeedS)
		{
			const bool bPristine =
				(Side.H == 0.f && Side.S == 1.f && Side.V == 1.f && Side.Brightness == 1.f);
			if (!bPristine) { return; }
			Side.H = SeedH;
			Side.S = SeedS;
			Side.Brightness = 1.5f;
		};
		SeedRedBlueSide(C.Red,  0.f,   1.0f);
		SeedRedBlueSide(C.Blue, 240.f, 0.9f);
	}

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
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Outline"),      C.bOutline,      Path);
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

// TEMP head-hitbox calibration aid. ECVF_Default (NOT cheat) so it can be toggled on a live server —
// strip / cheat-gate before final ship (it visualises enemy head positions).
static TAutoConsoleVariable<int32> CVarNCPDebugHeads(
	TEXT("ncp.DebugHeads"), 0,
	TEXT("NetcodePlus: draw the capsule-relative headshot sphere (GREEN = what the server validates) and the ")
	TEXT("mesh head bone (RED cross = the visible head) for every other pawn, client-side. 1=on."),
	ECVF_Default);

void NCPlusForceModels::DrawHeadDebug(UCanvas* Canvas, APlayerController* PC)
{
	// Why this exists: ut.DebugHeadshots is wrapped in #if ENABLE_DRAW_DEBUG (compiled out of Shipping) and
	// draws server-side anyway, so it's useless online. This reproduces the head sphere client-side via the
	// HUD canvas (Shipping-safe). It's faithful because the head is now capsule-derived — the client can
	// compute the exact same sphere the server validates from the replicated capsule. GREEN ring = that
	// sphere; RED cross = the mesh "head" bone (~the visible head). Calibrate kHeadCapsuleDrop so they overlap.
	if (!Canvas || !PC || CVarNCPDebugHeads.GetValueOnGameThread() == 0) { return; }
	UWorld* const World = PC->GetWorld();
	if (!World) { return; }

	FVector CamLoc; FRotator CamRot;
	PC->GetPlayerViewPoint(CamLoc, CamRot);
	const FVector CamRight = FRotationMatrix(CamRot).GetScaledAxis(EAxis::Y);

	static const FName NAME_Head(TEXT("head"));
	const float HeadHeight = 8.f;     // AUTCharacter engine defaults (debug-only references)
	const float HeadRadius = 18.f;

	for (TActorIterator<ATeamArenaCharacter> It(World); It; ++It)
	{
		ATeamArenaCharacter* C = *It;
		if (!C || C->IsLocalPlayerPawn()) { continue; }        // skip MY own pawn (offline-safe; NOT IsLocallyControlled,
		                                                       // which is true for ALL pawns in standalone)

		// GREEN ring: the capsule-relative head sphere the server actually validates.
		const FVector HeadWorld = C->GetHeadLocation(0.f);
		const FVector S = Canvas->Project(HeadWorld);
		if (S.Z <= 0.f) { continue; }                          // behind camera

		const FVector SEdge = Canvas->Project(HeadWorld + CamRight * HeadRadius);
		const float ScreenR = FMath::Max(3.f, FMath::Abs(SEdge.X - S.X));

		const int32 Segs = 24;
		FVector2D Prev(S.X + ScreenR, S.Y);
		for (int32 i = 1; i <= Segs; ++i)
		{
			const float Ang = (2.f * PI * i) / Segs;
			const FVector2D Cur(S.X + ScreenR * FMath::Cos(Ang), S.Y + ScreenR * FMath::Sin(Ang));
			Canvas->K2_DrawLine(Prev, Cur, 1.5f, FLinearColor::Green);
			Prev = Cur;
		}

		// RED cross: the mesh "head" bone (~where the visible head is) for comparison.
		if (C->GetMesh() && C->GetMesh()->DoesSocketExist(NAME_Head))
		{
			const FVector BoneWorld = C->GetMesh()->GetSocketLocation(NAME_Head) + FVector(0.f, 0.f, HeadHeight);
			const FVector B = Canvas->Project(BoneWorld);
			if (B.Z > 0.f)
			{
				Canvas->K2_DrawLine(FVector2D(B.X - 7.f, B.Y), FVector2D(B.X + 7.f, B.Y), 1.5f, FLinearColor::Red);
				Canvas->K2_DrawLine(FVector2D(B.X, B.Y - 7.f), FVector2D(B.X, B.Y + 7.f), 1.5f, FLinearColor::Red);
			}
		}
	}
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
	const int32 ViewerTeam = GetViewerTeam(World);   // spectator -> red is "ours"

	for (AUTTeamInfo* Team : GS->Teams)
	{
		if (!Team) { continue; }
		const bool bFriendly = ((int32)Team->GetTeamNum() == ViewerTeam);
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

static TAutoConsoleVariable<int32> CVarFlagDebug(
	TEXT("ncp.FlagDebug"), 0,
	TEXT("Log each CTF flag base/flag/mesh state ~every 1.5s (ForceModels flag-visibility debug). 0=off."));

void NCPlusForceModels::SyncFlagColours(UWorld* World)
{
	// Recolour the stock CTF flag to each team's skin colour (relative to the local viewer). The stock
	// flag material (MI_CTF_RedFlag / MI_CTF_BlueFlag -> M_CTF_Flag) already exposes a "FlagColor" vector
	// param (American spelling — dc's separate mesh used "FlagColour", which is why the old swap path
	// existed and kept breaking). So we just MID the stock flag's material slots and set FlagColor: NO
	// mesh swap, NO dc assets, cloth left ON (the stock APEX flag waves via TickFlagWind's wind source).
	// Re-asserted from the ticker so it survives flag respawns on capture/return. No-op outside CTF / on
	// a dedicated server.
	if (!World || World->GetNetMode() == NM_DedicatedServer) { return; }
	AUTCTFGameState* GS = World->GetGameState<AUTCTFGameState>();
	if (!GS) { return; }   // not a CTF mode -> no flags

	const FNCPlusForceModelsConfig& C = Get();
	const bool bWant = C.bEnabled && C.bFlags;
	const int32 ViewerTeam = GetViewerTeam(World);   // spectator -> red is "ours"
	static const FName NAME_FlagColor(TEXT("FlagColor"));

	// Flag-visibility debug (ncp.FlagDebug): dump per-base flag state ~every 1.5s. Logs even when a
	// base/flag/mesh is missing — exactly the "map maker did something funny" case (a map with no
	// flag base for a team, or a flag parked off-world / with collapsed render bounds).
	const bool bFlagDbg = CVarFlagDebug.GetValueOnGameThread() != 0;
	static float GLastFlagLog = -1000.f;
	const float TNow = World->TimeSeconds;
	const bool bLogNow = bFlagDbg && (TNow < GLastFlagLog || TNow - GLastFlagLog > 1.5f);
	if (bLogNow) { GLastFlagLog = TNow; }

	for (uint8 Team = 0; Team < 2; ++Team)
	{
		AUTCTFFlagBase* Base = GS->GetFlagBase(Team);
		AUTFlag* Flag = Base ? Base->MyFlag : nullptr;
		USkeletalMeshComponent* Mesh = Flag ? Flag->GetMesh() : nullptr;

		if (bLogNow)
		{
			if (!Base)
			{
				UE_LOG(LogTemp, Display, TEXT("[NCPFlag] T%d: GetFlagBase=NULL (no flag base for this team on this map)"), Team);
			}
			else if (!Flag)
			{
				UE_LOG(LogTemp, Display, TEXT("[NCPFlag] T%d: base=%s MyFlag=NULL baseLoc=%s"),
					Team, *GetNameSafe(Base->GetClass()), *Base->GetActorLocation().ToCompactString());
			}
			else if (!Mesh)
			{
				UE_LOG(LogTemp, Display, TEXT("[NCPFlag] T%d: flagClass=%s GetMesh=NULL"), Team, *GetNameSafe(Flag->GetClass()));
			}
			else
			{
				UE_LOG(LogTemp, Display, TEXT("[NCPFlag] T%d friendly=%d held=%d | flagClass=%s baseClass=%s | mesh=%s mats=%d boundsR=%.0f hidden=%d vis=%d | worldLoc=%s relZ=%.0f scale=%.2f | baseLoc=%s"),
					Team, ((int32)Team == ViewerTeam) ? 1 : 0, (Flag->HoldingPawn != nullptr) ? 1 : 0,
					*GetNameSafe(Flag->GetClass()), *GetNameSafe(Base->GetClass()),
					*GetNameSafe(Mesh->SkeletalMesh), Mesh->GetNumMaterials(), Mesh->Bounds.SphereRadius,
					Mesh->bHiddenInGame ? 1 : 0, Mesh->IsVisible() ? 1 : 0,
					*Mesh->GetComponentLocation().ToCompactString(), Mesh->RelativeLocation.Z, Mesh->RelativeScale3D.X,
					*Base->GetActorLocation().ToCompactString());
			}
		}

		if (!Base || !Flag || !Mesh) { continue; }

		if (bWant)
		{
			// Tint the stock flag in place: MID each material slot and set the stock "FlagColor" param to
			// the team skin colour. Cloth stays ON (waving). No swap / no override material = never
			// collapses. Element 0 is already a stock MID (Flag->MeshMID) so we just retint it; any slot
			// without a FlagColor param no-ops harmlessly. Re-asserted each slow tick (viewer-relative).
			const bool bFriendly = ((int32)Team == ViewerTeam);
			const FLinearColor Colour = GetSkinColour(GetModelSettings(Team, bFriendly));
			for (int32 i = 0; i < Mesh->GetNumMaterials(); ++i)
			{
				UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Mesh->GetMaterial(i));
				if (!MID) { MID = Mesh->CreateAndSetMaterialInstanceDynamic(i); }
				if (MID) { MID->SetVectorParameterValue(NAME_FlagColor, Colour); }
			}
		}
	}

}

void NCPlusForceModels::TickFlagWind(UWorld* World, float DeltaTime)
{
	// Port of dc's FlagWind BP: spawn one WindDirectionalSource and wander its speed + direction so the
	// swapped-in cloth waves (stock UTFlag has no wind; UE4 APEX cloth reads scene wind). Client-local,
	// non-replicated. Spawned while the cloth recolour is active (CTF + Flags), destroyed otherwise.
	if (!World || World->GetNetMode() == NM_DedicatedServer) { return; }
	const FNCPlusForceModelsConfig& C = Get();
	const bool bWant = C.bEnabled && C.bFlags && (World->GetGameState<AUTCTFGameState>() != nullptr);

	AWindDirectionalSource* Wind = GFlagWind.Get();
	if (!bWant)
	{
		if (Wind) { Wind->Destroy(); GFlagWind = nullptr; }
		return;
	}

	if (!Wind)
	{
		LoadWindKnobs();
		FActorSpawnParameters SP;
		SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Wind = World->SpawnActor<AWindDirectionalSource>(AWindDirectionalSource::StaticClass(), FTransform::Identity, SP);
		if (!Wind) { return; }
		GFlagWind   = Wind;
		GWindSpeed  = GWindMinSpeed;
		GWindAngVel = FVector::ZeroVector;
		GWindRot    = FVector::ZeroVector;
	}

	const float Dt = FMath::Max(DeltaTime, 0.f);

	// Speed: bounded random walk in [MinSpeed, MaxSpeed].
	GWindSpeed = FMath::Clamp(GWindSpeed + FMath::FRandRange(-GWindMaxAccel, GWindMaxAccel) * Dt,
	                          GWindMinSpeed, GWindMaxSpeed);

	// Angular velocity: random walk (BP randomises the X/roll axis only), magnitude capped.
	GWindAngVel += FVector(FMath::FRandRange(-GWindMaxAngAccel, GWindMaxAngAccel), 0.f, 0.f) * Dt;
	if (GWindAngVel.Size() > GWindMaxAngSpeed) { GWindAngVel = GWindAngVel.GetSafeNormal() * GWindMaxAngSpeed; }

	// Integrate the wind direction (euler degrees) and push speed + rotation onto the source.
	GWindRot += GWindAngVel * Dt;
	if (UWindDirectionalSourceComponent* Comp = Wind->GetComponent())
	{
		// UWindDirectionalSourceComponent::SetSpeed isn't ENGINE_API-exported, so inline what it does
		// (set the property + queue the render-thread update). MarkRenderDynamicDataDirty IS exported.
		Comp->Speed = GWindSpeed;
		Comp->MarkRenderDynamicDataDirty();
	}
	Wind->SetActorRotation(FRotator(GWindRot.Y, GWindRot.Z, GWindRot.X));   // Pitch=Y, Yaw=Z, Roll=X
}

void NCPlusForceModels::SuppressFlagCarrierOutlines(UWorld* World)
{
	// Base UT outlines the flag carrier through walls — to teammates (AUTCarriedObject::SetHolder sets
	// HoldingPawn->bSpecialTeamPlayer) and to everyone while pinged (AUTFlag::Tick sets bSpecialPlayer).
	// dc's TeamFlag BP hid this by forcing bForceNoOutline on the holder each tick; our C++ flags don't,
	// so the stock outline reappeared. Reproduce dc's suppression client-side: bForceNoOutline short-
	// circuits AUTCharacter::IsOutlined(), and SetOutlineLocal(false) refreshes UpdateOutline(), which
	// unregisters the CustomDepth stencil on the body, weapon, attachment AND the carried flag. Purely
	// local — bForceNoOutline is not replicated, so the server never reverts it.
	if (!World || World->GetNetMode() == NM_DedicatedServer) { return; }
	// Always on (it's a fix, not a setting). CTF only — NOT Blitz: Blitz (Flag Run) uses AUTFlagRunGameState
	// (derives from AUTGameState, not AUTCTFGameState), so GetGameState<AUTCTFGameState>() is null there;
	// real CTF / iCTF / NCPlusCTF all use an AUTCTFGameState. When false, Current stays empty and any prior
	// suppression is restored below, so a CTF->Blitz map change cleanly hands the stock outline back.
	const bool bWant = World->GetGameState<AUTCTFGameState>() != nullptr;

	TSet<TWeakObjectPtr<AUTCharacter>> Current;
	if (bWant)
	{
		for (TActorIterator<AUTCharacter> It(World); It; ++It)
		{
			AUTCharacter* C = *It;
			if (!C || C->IsPendingKill() || !Cast<AUTFlag>(C->GetCarriedObject())) { continue; }
			Current.Add(C);
			if (!C->bForceNoOutline)
			{
				C->bForceNoOutline = true;
				C->SetOutlineLocal(false);   // refresh -> drops the stencil mesh
			}
		}
	}

	// Restore anyone we previously suppressed who is no longer a carrier (or once suppression is off).
	for (const TWeakObjectPtr<AUTCharacter>& Prev : GOutlineSuppressed)
	{
		if (Current.Contains(Prev)) { continue; }
		if (AUTCharacter* C = Prev.Get())
		{
			C->bForceNoOutline = false;
			C->SetOutlineLocal(false);       // refresh -> stock outline state re-applies as needed
		}
	}
	GOutlineSuppressed = MoveTemp(Current);
}

bool NCPlusForceModels::OutlineModeActive(UWorld* World)
{
	// Outline mode is CLIENT/STANDALONE-only: SetOutlineLocal writes bOutlineWhenUnoccluded, which is
	// REPLICATED (ReplicatedUsing=UpdateOutline) — on a listen server our per-frame LOS gating would
	// push the host's occlusion state to every connected client and clobber their own outline flags.
	// Also keys the TeamArenaCharacter tint-gating, so a host with the flag on keeps the normal
	// super-tint (neutral bodies with no outline would be strictly worse).
	if (!World) { return false; }
	const FNCPlusForceModelsConfig& C = Get();
	if (!C.bEnabled || !C.bOutline) { return false; }
	const ENetMode NM = World->GetNetMode();
	if (NM != NM_Client && NM != NM_Standalone) { return false; }

	// STOCK-PALETTE GATE (user decision 2026-07-01 — "not remaking the material for now"): the stock
	// M_OutlinePP rim is FIXED red (team 0) / blue (team 1), so outline mode only engages when every
	// side it would outline is configured to READ as that colour — otherwise e.g. a green-configured
	// team gets a blue rim (community-reported mismatch) and the neutral body makes it worse. The
	// Red/Blue style passes with its seeded/default hues (0/240; a user who repaints the Red/Blue
	// sides elsewhere is gated like any other colour); Team/Enemy styles pass only when the user's
	// colours line up with the absolute stencil teams. Falls back to the normal super-tint when the
	// gate fails — the classifier below always evaluates the FINAL resolved colour.
	const int32 ViewerTeam = GetViewerTeam(World);
	for (int32 TeamIdx = 0; TeamIdx < 2; ++TeamIdx)
	{
		// EnemyOnly never outlines friendlies (OutlinePlayers skips them) — their colour is moot.
		if (C.Style == ENCPlusSkinStyle::EnemyOnly && TeamIdx == ViewerTeam) { continue; }
		const FLinearColor Col = GetSkinColour(GetModelSettings(TeamIdx, TeamIdx == ViewerTeam));
		const FLinearColor HSV = Col.LinearRGBToHSV();   // R = hue 0-360, G = sat, B = value
		if (HSV.G < 0.3f || HSV.B < 0.05f) { return false; }   // too grey/dark to read as a team colour
		const float H = HSV.R;
		const bool bReadsAsRim = (TeamIdx == 0)
			? (H <= 35.f || H >= 335.f)      // red-ish
			: (H >= 195.f && H <= 275.f);    // blue-ish
		if (!bReadsAsRim) { return false; }
	}
	return true;
}

bool NCPlusForceModels::OutlineModeActiveCached()
{
	return GOutlineModeActiveCache;
}

void NCPlusForceModels::OutlinePlayers(UWorld* World, bool bSlowTick)
{
	// "Outline" flag: a client-local, team-coloured, LOS-gated player outline as a cleaner alternative to
	// the body/armour super-tint (which is gated off in ATeamArenaCharacter when this is on).
	//
	// ⚠ Engine stencil semantics (UTCharacter.cpp:4447-4456): M_OutlinePP draws the custom-depth
	// silhouette where it is OCCLUDED — SetOutlineLocal(true, false) is the through-wall X-ray used by
	// spectator TacCom (UTPlayerController.cpp:2837) and Showdown, NOT a depth-masked outline. The +128
	// stencil bit (bWhenUnoccluded=true) ADDS the visible-pixel rim; no stencil combination gives
	// "visible only". So LOS is gated HERE: every frame each candidate is line-traced from the viewer's
	// camera (eye/centre/feet, ECC_Visibility) — visible -> SetOutlineLocal(true, /*bWhenUnoccluded*/true),
	// occluded -> SetOutlineLocal(false), so nothing renders through walls. Known compromise until the
	// M_OutlinePP content edit: while a player is PARTIALLY visible, their occluded parts still X-ray
	// through cover (the occluded-pixel path stays live whenever the outline is on).
	//
	// SetOutlineLocal calls are gated on TRANSITIONS of our per-pawn state (the UpdateOutline cascade's
	// weapon-attachment OFF path unregisters unguarded -> repeated off-calls would churn/log), with an
	// unconditional ON re-assert each slow tick to recover bLocalOutline/bOutlineWhenUnoccluded from
	// server writes (both are ReplicatedUsing=UpdateOutline). Side perf win: an outlined mesh anim-ticks
	// ALWAYS (AlwaysTickPoseAndRefreshBones) — LOS gating returns occluded players to
	// OnlyTickPoseWhenRendered. Nothing replicates; no-op on a dedicated server.
	if (!World || World->GetNetMode() == NM_DedicatedServer) { return; }

	// Refresh the per-frame gate cache BEFORE any early-out so the tint call sites always read a
	// current verdict (TacCom/intermission freeze the outline STATE, not the mode decision).
	// A verdict FLIP (menu save, style/colour change, viewer team switch under Team/Enemy styles)
	// must re-tint every body — outline-mode NEUTRAL <-> normal super-tint — or pawns keep the old
	// look until their next reapply event.
	{
		const bool bGate = OutlineModeActive(World);
		if (bGate != GOutlineModeActiveCache)
		{
			GOutlineModeActiveCache = bGate;
			ReapplyAll(World);
		}
	}

	// Authoring paths (once) + per-world load-attempt re-arm, then keep the camera manager pointed at
	// the recoloured outline material (if configured) — BEFORE the TacCom/intermission early-outs so
	// the swap holds during X-Ray and between rounds too.
	if (bSlowTick)
	{
		if (!GOutlinePathsLoaded)
		{
			GOutlinePathsLoaded = true;
			GConfig->GetString(TEXT("ForceModels"), TEXT("OutlineMPC"),      GOutlineMPCPath, ModIniPath());
			GConfig->GetString(TEXT("ForceModels"), TEXT("OutlineMaterial"), GOutlineMatPath, ModIniPath());
		}
		if (GOutlineAssetWorld.Get() != World)
		{
			GOutlineAssetWorld = World;
			GOutlineTriedMPC = false;
			GOutlineTriedMat = false;
		}
		SwapOutlineMaterial(World);
	}

	// Spectator X-Ray (TacCom) outlines everyone THROUGH WALLS by design and re-asserts it every PC tick
	// (UTPlayerController.cpp:3344) — while it's on, leave the outlines entirely to it (no clears, no
	// re-asserts; our state resumes via the slow-tick ON re-assert once X-Ray is toggled off).
	APlayerController* const LocalPC = World->GetFirstPlayerController();
	{
		const AUTPlayerController* const UTPC = Cast<AUTPlayerController>(LocalPC);
		if (UTPC && UTPC->bTacComView) { return; }
	}

	// Intermission force-hides every outline (AUTCharacter::IsOutlined) — freeze our state instead of
	// churning no-op calls; the slow-tick ON re-assert restores the outlines when play resumes.
	if (AUTGameState* const GS = World->GetGameState<AUTGameState>())
	{
		if (GS->IsMatchIntermission()) { return; }
	}

	const FNCPlusForceModelsConfig& C = Get();
	const bool bWant = GOutlineModeActiveCache;   // computed above this frame
	const int32 ViewerTeam = GetViewerTeam(World);

	// Push the per-team outline colours into MPC_NCPOutline (slow tick) so a matching M_OutlinePP renders
	// the ForceModels skin colours (green/etc.) instead of the stock red/blue palette. Graceful no-op
	// until that collection asset exists. Colours are by ABSOLUTE team; NB our LOS outline's stencil
	// carries the +128 unoccluded bit, so the material's team decode must mask it (stencil & 0x7F:
	// 129 -> Team0, 130 -> Team1).
	if (bWant && bSlowTick)
	{
		// Resolved FRESH each slow tick (FindObject = cheap hash lookup, silent): a raw static cache
		// dangles once GC purges the collection on map travel (it's only world/material-referenced) and
		// SetVectorParameterValue on a stale pointer crashes in Shipping. ResolveOutlineAsset retries
		// the load once per world so a travel-unload recovers.
		UMaterialParameterCollection* const MPC =
			ResolveOutlineAsset<UMaterialParameterCollection>(GOutlineMPCPath, GOutlineTriedMPC);
		if (MPC)
		{
			auto TeamColourFor = [ViewerTeam](int32 TeamIdx) -> FLinearColor
			{
				const FNCPlusModelSettings Side = GetModelSettings(TeamIdx, TeamIdx == ViewerTeam);
				FLinearColor Col = GetSkinColour(Side);
				Col.A = 1.f;
				return Col;
			};
			static const FName NAME_Team0(TEXT("Team0"));
			static const FName NAME_Team1(TEXT("Team1"));
			UKismetMaterialLibrary::SetVectorParameterValue(World, MPC, NAME_Team0, TeamColourFor(0));
			UKismetMaterialLibrary::SetVectorParameterValue(World, MPC, NAME_Team1, TeamColourFor(1));
		}
	}

	// The LOS traces need a viewer eye; during a map transition the camera manager can briefly be
	// missing — freeze rather than mass-clear (states recover next frame).
	if (bWant && (!LocalPC || !LocalPC->PlayerCameraManager)) { return; }
	const FVector ViewLoc = (LocalPC && LocalPC->PlayerCameraManager)
		? LocalPC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;

	// Never outline the local viewer's own pawn (null while spectating -> outline everyone, which is fine).
	const APawn* const LocalPawn = LocalPC ? LocalPC->GetPawn() : nullptr;

	TMap<TWeakObjectPtr<AUTCharacter>, bool> Current;
	if (bWant)
	{
		FCollisionQueryParams TraceParams(FName(TEXT("NCPOutlineLOS")), /*bTraceComplex*/ true, LocalPawn);
		for (TActorIterator<AUTCharacter> It(World); It; ++It)
		{
			AUTCharacter* Ch = *It;
			if (!Ch || Ch->IsPendingKill() || Ch->IsDead() || Ch == LocalPawn) { continue; }
			// Enemy-Only style: leave friendlies un-outlined (mirrors the reskin scoping).
			if (C.Style == ENCPlusSkinStyle::EnemyOnly && (int32)Ch->GetTeamNum() == ViewerTeam) { continue; }
			// Suppressed flag carriers belong to SuppressFlagCarrierOutlines — leave their state alone.
			if (Ch->bForceNoOutline) { continue; }

			// LOS: eye / centre / mirrored-feet — any unblocked sample = visible. Pawns don't block
			// ECC_Visibility, so only world geometry occludes.
			FCollisionQueryParams P = TraceParams;
			P.AddIgnoredActor(Ch);
			const FVector Head   = Ch->GetPawnViewLocation();
			const FVector Centre = Ch->GetActorLocation();
			const FVector Pts[3] = { Head, Centre, Centre + (Centre - Head) };
			bool bVisible = false;
			for (const FVector& Pt : Pts)
			{
				if (!World->LineTraceTestByChannel(ViewLoc, Pt, ECC_Visibility, P)) { bVisible = true; break; }
			}

			const bool* PrevOn = GOutlined.Find(Ch);
			const bool bWasOn = (PrevOn && *PrevOn);
			if (bVisible && (!bWasOn || bSlowTick)) { Ch->SetOutlineLocal(true, /*bWhenUnoccluded*/ true); }
			else if (!bVisible && bWasOn)           { Ch->SetOutlineLocal(false); }
			Current.Add(Ch, bVisible);
		}
	}

	// Restore anyone we previously outlined who is no longer a target (toggle off / left / round change).
	// Dead pawns skip the call — the engine already clears outlines in PlayDying/StartRagdoll.
	for (const TPair<TWeakObjectPtr<AUTCharacter>, bool>& Prev : GOutlined)
	{
		if (!Prev.Value || Current.Contains(Prev.Key)) { continue; }
		AUTCharacter* Ch = Prev.Key.Get();
		if (Ch && !Ch->IsDead()) { Ch->SetOutlineLocal(false); }
	}
	GOutlined = MoveTemp(Current);
}

int32 NCPlusForceModels::GetViewerTeam(UWorld* World)
{
	if (World)
	{
		if (AUTPlayerController* PC = Cast<AUTPlayerController>(World->GetFirstPlayerController()))
		{
			const uint8 T = PC->GetTeamNum();
			if (T == 0 || T == 1) { return (int32)T; }
		}
	}
	return 0;   // spectator / no team -> red is "our" team, blue is enemy
}

FNCPlusModelSettings NCPlusForceModels::GetModelSettings(int32 TheirTeamIndex, bool bIsFriendly)
{
	static const FNCPlusModelSettings EmptySide;   // empty ContentPath -> applier skips this pawn
	const FNCPlusForceModelsConfig& C = Get();
	switch (C.Style)
	{
	case ENCPlusSkinStyle::RedBlue:
	{
		// Red/Blue is ABSOLUTE: team 0 = red side, team 1 = blue side. Reload() seeds pristine sides
		// to proper red/blue (H 0/240, Glow 3), so the old UNCONDITIONAL hue force is gone — it was
		// stomping user-picked hues, which made the F5 Red/Blue H spinbox silently dead and the
		// preview swatch lie (Blue row previewed RED). Kept only as a safety net: a Blue side still
		// carrying the default RED hue (pre-seed value mid-session) must never render "red vs red".
		FNCPlusModelSettings Out = (TheirTeamIndex == 0) ? C.Red : C.Blue;
		if (TheirTeamIndex == 1 && Out.H == 0.f) { Out.H = 240.f; }
		// Model fallback: a Red/Blue side with no model of its own borrows the Team (then Enemy) model,
		// so switching to Red/Blue from a Team/Enemy-only setup still forces a model instead of nothing.
		if (Out.ContentPath.IsEmpty())
		{
			Out.ContentPath = !C.Team.ContentPath.IsEmpty() ? C.Team.ContentPath : C.Enemy.ContentPath;
		}
		return Out;
	}
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

FLinearColor NCPlusForceModels::GetArmourColour(const FNCPlusModelSettings& Side)
{
	if (Side.ArmourMode == ENCPlusArmourMode::Complimentary)
	{
		// Complement = hue rotated 180 degrees, keep S/V. Matches the BP's GetComplimentaryColour.
		float H = Side.H + 180.f;
		while (H >= 360.f) { H -= 360.f; }
		return FLinearColor(H, Side.S, Side.V, 1.f).HSVToLinearRGB();
	}
	return GetSkinColour(Side);   // MatchSkin
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

// Per-class bHideInUI cache (the property isn't AssetRegistrySearchable, so we must load the CDO once).
// Loading a char class pulls its mesh — heavy — so cache the bool and only ever load each once.
static bool IsCharHiddenInUI(const FString& ClassPath)
{
	static TMap<FString, bool> Cache;
	if (const bool* Found = Cache.Find(ClassPath)) { return *Found; }
	bool bHide = false;
	if (UClass* Cls = LoadObject<UClass>(nullptr, *ClassPath))
	{
		if (const AUTCharacterContent* CDO = Cls->GetDefaultObject<AUTCharacterContent>())
		{
			bHide = CDO->bHideInUI;   // Epic's "hide from menus — testing / mod-internal" flag
		}
	}
	Cache.Add(ClassPath, bHide);
	return bHide;
}

// Asset name minus a trailing run of digits — the "stem" we group variant families by
// ("NecrisMale05" -> "NecrisMale", "NecrisMale_Necroth03" -> "NecrisMale_Necroth"). A name with no
// trailing digits is its own stem; an all-digit name is left whole (never collapse to an empty stem).
static FString StemOf(const FString& Name)
{
	int32 End = Name.Len();
	while (End > 0 && FChar::IsDigit(Name[End - 1])) { --End; }
	return (End > 0) ? Name.Left(End) : Name;
}

// Representative rank within a coalesced family: lower wins. The trailing number, or MAX_int32 for a
// non-numbered member, so the lowest-numbered variant (e.g. SkaarjMale01) is preferred over both higher
// numbers and the un-numbered base (e.g. plain "NecrisMale") — per the picker design.
static int32 VariantRank(const FString& Name)
{
	int32 S = Name.Len();
	while (S > 0 && FChar::IsDigit(Name[S - 1])) { --S; }
	if (S == Name.Len()) { return MAX_int32; }   // no trailing digits
	return FCString::Atoi(*Name.Mid(S));
}

// ── One-time client onboarding: dc's [TeamSkins.*] (MutTeamSkins) -> our [ForceModels.*] ─────────
// File-scope statics, placed AFTER StemOf/VariantRank so StemOf (file static, above) and the
// anonymous-namespace WriteSide are both visible with their real definitions. Forward-declared
// above Reload() (which calls this at its top). Client-local; no-op on a dedicated server.

// dc Model.<Side>.ID -> our picker stem (index table read off dc's SkinUpdater model array).
static FString DCModelIdToStem(int32 ID)
{
	switch (ID)
	{
		case 0:  return TEXT("TC_Male");        // Malcolm
		case 1:  return TEXT("NecrisFemale");
		case 2:  return TEXT("SkaarjMale");     // Skaarj
		case 3:  return TEXT("NecrisMale");
		default: return FString();              // unknown -> leave Class empty (side un-forced)
	}
}

// Resolve a stem to the picker's own ClassPath (so the seeded path is exactly what the applier
// expects). Empty if the stem isn't an installed picker entry -> caller leaves Class empty.
static FString ResolveStemToClassPath(const TArray<NCPlusForceModels::FContentEntry>& Entries, const FString& Stem)
{
	if (Stem.IsEmpty()) { return FString(); }
	// 1) Exact picker label (a coalesced stock family's DisplayName IS the bare stem).
	for (const NCPlusForceModels::FContentEntry& E : Entries)
	{
		if (E.DisplayName.Equals(Stem, ESearchCase::IgnoreCase)) { return E.ClassPath; }
	}
	// 2) Stem of the DisplayName (a non-coalesced single keeps its asset name); the exact match
	//    above already resolved bare "NecrisMale", so this can't mis-hit NecrisMale_Damian/_Necroth.
	for (const NCPlusForceModels::FContentEntry& E : Entries)
	{
		if (StemOf(E.DisplayName).Equals(Stem, ESearchCase::IgnoreCase)) { return E.ClassPath; }
	}
	// 3) Last-ditch: the ClassPath (or a variant) embeds the stem.
	for (const NCPlusForceModels::FContentEntry& E : Entries)
	{
		if (E.ClassPath.Contains(Stem, ESearchCase::IgnoreCase)) { return E.ClassPath; }
		for (const FString& VP : E.VariantPaths)
		{
			if (VP.Contains(Stem, ESearchCase::IgnoreCase)) { return E.ClassPath; }
		}
	}
	return FString();
}

// Map one dc side's [TeamSkins.SkinColour.<Side>] + [TeamSkins.Model.<Side>] into Out.
static void MigrateOneSide(const TArray<NCPlusForceModels::FContentEntry>& Entries,
                           const TCHAR* DcSide, const FString& Path, FNCPlusModelSettings& Out)
{
	// Model.<Side>.ID -> stem -> ClassPath (absent ID / unknown -> empty path -> side un-forced).
	int32 ModelId = -1;
	const FString ModelSec = FString::Printf(TEXT("TeamSkins.Model.%s"), DcSide);
	GConfig->GetInt(*ModelSec, TEXT("ID"), ModelId, Path);
	Out.ContentPath = ResolveStemToClassPath(Entries, DCModelIdToStem(ModelId));

	// Colour: SkinColour.<Side>.{H,S,V} — dc stores H in degrees / S,V 0-1, same as FNCPlusModelSettings
	// (verified: dc H values >1), so copy straight. IsComplimentary -> bComplimentary.
	const FString ColSec = FString::Printf(TEXT("TeamSkins.SkinColour.%s"), DcSide);
	GConfig->GetFloat(*ColSec, TEXT("H"), Out.H, Path);   // absent -> struct default kept
	GConfig->GetFloat(*ColSec, TEXT("S"), Out.S, Path);
	GConfig->GetFloat(*ColSec, TEXT("V"), Out.V, Path);
	int32 Comp = 0;
	GConfig->GetInt(*ColSec, TEXT("IsComplimentary"), Comp, Path);
	Out.bComplimentary = (Comp != 0);

	// User's explicit choices: dc had no emissive -> Brightness 3; armour matches skin.
	Out.Brightness = 3.0f;
	Out.ArmourMode = ENCPlusArmourMode::MatchSkin;
}

static void MaybeMigrateFromTeamSkins(const FString& Path)
{
	// 1) Run-once. GetInt's return value is "did the key exist?"; absent -> Migrated stays 0.
	int32 Migrated = 0;
	GConfig->GetInt(TEXT("ForceModels.Versioning"), TEXT("MigratedFromTeamSkins"), Migrated, Path);
	if (Migrated != 0) { return; }

	// 2) NEVER clobber an existing [ForceModels] config (the common case). Section probe + a key
	//    belt-and-suspenders against a degenerate empty header. DoesSectionExist is read-only.
	bool bHasEnabledKey = false;
	const bool bForceModelsPresent =
		GConfig->GetBool(TEXT("ForceModels"), TEXT("Enabled"), bHasEnabledKey, Path);
	if (GConfig->DoesSectionExist(TEXT("ForceModels"), Path) || bForceModelsPresent)
	{
		GConfig->SetInt(TEXT("ForceModels.Versioning"), TEXT("MigratedFromTeamSkins"), 1, Path);
		GConfig->Flush(false, Path);
		return;
	}

	// 3) Only migrate if dc's config is actually present. [TeamSkins.Enable] is dc's anchor section.
	if (!GConfig->DoesSectionExist(TEXT("TeamSkins.Enable"), Path))
	{
		GConfig->SetInt(TEXT("ForceModels.Versioning"), TEXT("MigratedFromTeamSkins"), 1, Path);
		GConfig->Flush(false, Path);
		return;
	}

	// 4) Need the asset registry to resolve model class paths. If it isn't populated yet, bail WITHOUT
	//    writing or stamping so the migration retries on a later launch (don't permanently seed empty
	//    model paths). Enumerate ONCE and reuse for all four sides.
	TArray<NCPlusForceModels::FContentEntry> Entries;
	NCPlusForceModels::EnumerateContent(Entries, /*bIncludeHidden=*/false);
	if (Entries.Num() == 0) { return; }

	// ── Seed [ForceModels.*] from dc's [TeamSkins.*]. ───────────────────────────────────────────
	// Top-level [TeamSkins.Enable] flags (absent -> 0 = off, dc's default semantics).
	int32 EnStyle = 0, EnHUD = 0, EnFlags = 0, EnDarken = 0, EnCosmetics = 0, EnArmour = 0;
	GConfig->GetInt(TEXT("TeamSkins.Enable"), TEXT("Style"),        EnStyle,     Path);
	GConfig->GetInt(TEXT("TeamSkins.Enable"), TEXT("HUD"),          EnHUD,       Path);
	GConfig->GetInt(TEXT("TeamSkins.Enable"), TEXT("Flags"),        EnFlags,     Path);
	GConfig->GetInt(TEXT("TeamSkins.Enable"), TEXT("DarkenBodies"), EnDarken,    Path);
	GConfig->GetInt(TEXT("TeamSkins.Enable"), TEXT("Cosmetics"),    EnCosmetics, Path);
	GConfig->GetInt(TEXT("TeamSkins.Enable"), TEXT("Armour"),       EnArmour,    Path);

	// Migrating => force models ON. ENCPlusSkinStyle int matches dc's (TeamEnemy=0/RedBlue=1/EnemyOnly=2).
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Enabled"),      true,               Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Models"),       true,               Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("HUD"),          (EnHUD != 0),       Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Armour"),       (EnArmour != 0),    Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Flags"),        (EnFlags != 0),     Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("DarkenBodies"), (EnDarken != 0),    Path);
	GConfig->SetBool(TEXT("ForceModels"), TEXT("Cosmetics"),    (EnCosmetics != 0), Path);
	GConfig->SetInt (TEXT("ForceModels"), TEXT("Style"),        EnStyle,            Path);

	// Per-side: map colour + model, then write all 7 keys via the existing WriteSide.
	FNCPlusModelSettings Enemy, Team, Red, Blue;
	MigrateOneSide(Entries, TEXT("Enemy"), Path, Enemy);
	MigrateOneSide(Entries, TEXT("Team"),  Path, Team);
	MigrateOneSide(Entries, TEXT("Red"),   Path, Red);
	MigrateOneSide(Entries, TEXT("Blue"),  Path, Blue);
	WriteSide(TEXT("Enemy"), Enemy);
	WriteSide(TEXT("Team"),  Team);
	WriteSide(TEXT("Red"),   Red);
	WriteSide(TEXT("Blue"),  Blue);

	// 5) Stamp run-once + persist everything in one flush.
	GConfig->SetInt(TEXT("ForceModels.Versioning"), TEXT("MigratedFromTeamSkins"), 1, Path);
	GConfig->Flush(false, Path);
	UE_LOG(LogTemp, Log, TEXT("[ForceModels] Seeded config from dc MutTeamSkins (one-time onboarding)."));
}

// Name-substring denylist of engine base / test / unusable character stems — kept out of BOTH the
// forcemodels_list audit listing AND the AllowAnyModel picker. [ForceModels] HiddenModels= appends.
static const TCHAR* const GFMDefaultHiddenStems[] = {
	TEXT("HumanMaleBase"), TEXT("NecrisFemaleBase"), TEXT("NecrisMaleBase"), TEXT("SkaarjMaleBase"),
	TEXT("LTC_Bot_CharacterData"), TEXT("BP_Char_Oct2015"),
	TEXT("TC_GodKing"), TEXT("TC_Siris"),
	TEXT("UNUSED_"), TEXT("TC_ArmorNewV"),
};

void NCPlusForceModels::EnumerateContent(TArray<FContentEntry>& Out, bool bIncludeHidden)
{
	TArray<FAssetData> Assets;
	GetAllBlueprintAssetData(AUTCharacterContent::StaticClass(), Assets, /*bRequireEntitlements=*/false);

	if (bIncludeHidden)
	{
		// ── AUDIT PATH (forcemodels_list): every installed character, uncoalesced, curated ones flagged.
		// Denylist (name substrings): baked stock/unusable set + [ForceModels] HiddenModels= overrides; plus
		// Epic's bHideInUI test chars ([ForceModels] HideTestModels, default on — loads each char CDO once,
		// cached). This whole cost is paid ONLY here, never on the menu/picker path below.
		bool bHideTest = true;
		GConfig->GetBool(TEXT("ForceModels"), TEXT("HideTestModels"), bHideTest, ModIniPath());

		TArray<FString> Hidden;
		for (const TCHAR* D : GFMDefaultHiddenStems) { Hidden.Add(D); }
		FString HiddenStr;
		GConfig->GetString(TEXT("ForceModels"), TEXT("HiddenModels"), HiddenStr, ModIniPath());
		if (!HiddenStr.IsEmpty())
		{
			TArray<FString> Parts;
			HiddenStr.ParseIntoArray(Parts, TEXT(","), true);
			for (const FString& Raw : Parts)
			{
				int32 S = 0, E = Raw.Len();
				while (S < E && FChar::IsWhitespace(Raw[S]))     { ++S; }
				while (E > S && FChar::IsWhitespace(Raw[E - 1])) { --E; }
				const FString T = Raw.Mid(S, E - S);
				if (!T.IsEmpty()) { Hidden.Add(T); }
			}
		}

		Out.Reserve(Assets.Num());
		for (const FAssetData& A : Assets)
		{
			const FString Name      = A.AssetName.ToString();
			const FString ClassPath = A.ObjectPath.ToString() + TEXT("_C");
			bool bHidden = false;
			for (const FString& Sub : Hidden)
			{
				if (Name.Contains(Sub, ESearchCase::IgnoreCase)) { bHidden = true; break; }
			}
			if (!bHidden && bHideTest && IsCharHiddenInUI(ClassPath)) { bHidden = true; }
			FContentEntry E;
			E.DisplayName = Name;
			E.ClassPath   = ClassPath;
			E.VariantPaths.Add(ClassPath);
			E.bHidden     = bHidden;
			Out.Add(E);
		}
		return;
	}

	// ── PICKER PATH: locked to an explicit ALLOWLIST of stems, then coalesce numbered families.
	// Only these stems ever reach the menu — new/unknown installed content can't auto-appear (327 lock).
	// The audit path above still lists everything. [ForceModels] AllowModels= (comma-separated stems)
	// appends more without a rebuild (e.g. a newly-cooked custom char). No bHideInUI CDO loads here.
	static const TCHAR* const DefaultAllowed[] = {
		TEXT("NecrisFemaleCoat"), TEXT("Genghis"), TEXT("LiandriRobot"),       // custom content
		TEXT("NecrisFemale"), TEXT("NecrisMale"), TEXT("NecrisMale_Damian"),   // stock families (coalesced)
		TEXT("NecrisMale_Necroth"), TEXT("SkaarjMale"), TEXT("TC_Male"),
	};
	TArray<FString> Allowed;
	for (const TCHAR* D : DefaultAllowed) { Allowed.Add(D); }
	FString AllowStr;
	GConfig->GetString(TEXT("ForceModels"), TEXT("AllowModels"), AllowStr, ModIniPath());
	if (!AllowStr.IsEmpty())
	{
		TArray<FString> Parts;
		AllowStr.ParseIntoArray(Parts, TEXT(","), true);
		for (const FString& Raw : Parts)
		{
			int32 S = 0, E = Raw.Len();
			while (S < E && FChar::IsWhitespace(Raw[S]))     { ++S; }
			while (E > S && FChar::IsWhitespace(Raw[E - 1])) { --E; }
			const FString T = Raw.Mid(S, E - S);
			if (!T.IsEmpty()) { Allowed.Add(T); }
		}
	}

	// [ForceModels] AllowAnyModel=true (server-owner opt-in; read from THIS install's Mod.ini) drops
	// the curated allowlist so every installed character is selectable — still minus the cheap
	// name-substring denylist (engine base/test/unusable + HiddenModels=). Default false keeps the
	// shipped allowlist. (The bHideInUI CDO-load curation stays off the picker path for cost.)
	bool bAllowAny = false;
	GConfig->GetBool(TEXT("ForceModels"), TEXT("AllowAnyModel"), bAllowAny, ModIniPath());
	TArray<FString> Hidden;
	if (bAllowAny)
	{
		for (const TCHAR* D : GFMDefaultHiddenStems) { Hidden.Add(D); }
		FString HiddenStr;
		GConfig->GetString(TEXT("ForceModels"), TEXT("HiddenModels"), HiddenStr, ModIniPath());
		if (!HiddenStr.IsEmpty())
		{
			TArray<FString> Parts;
			HiddenStr.ParseIntoArray(Parts, TEXT(","), true);
			for (const FString& Raw : Parts)
			{
				int32 S = 0, E = Raw.Len();
				while (S < E && FChar::IsWhitespace(Raw[S]))     { ++S; }
				while (E > S && FChar::IsWhitespace(Raw[E - 1])) { --E; }
				const FString T = Raw.Mid(S, E - S);
				if (!T.IsEmpty()) { Hidden.Add(T); }
			}
		}
	}

	// 1) Collect stems: default = the curated allowlist; AllowAnyModel = all installed minus denylist.
	struct FRaw { FString Name; FString ClassPath; };
	TArray<FRaw> Visible;
	Visible.Reserve(Assets.Num());
	for (const FAssetData& A : Assets)
	{
		const FString Name      = A.AssetName.ToString();
		const FString ClassPath = A.ObjectPath.ToString() + TEXT("_C");
		const FString Stem      = StemOf(Name);
		if (bAllowAny)
		{
			bool bDenied = false;
			for (const FString& Sub : Hidden)
			{
				if (Name.Contains(Sub, ESearchCase::IgnoreCase)) { bDenied = true; break; }
			}
			if (bDenied) { continue; }
		}
		else
		{
			bool bAllowed = false;
			for (const FString& Al : Allowed)
			{
				if (Stem.Equals(Al, ESearchCase::IgnoreCase)) { bAllowed = true; break; }
			}
			if (!bAllowed) { continue; }
		}
		Visible.Add({ Name, ClassPath });
	}

	// 2) Group by stem (name minus a trailing digit run), preserving first-seen order.
	TArray<FString>       GroupStem;   // stem per group (parallel to Groups)
	TArray<TArray<int32>> Groups;      // member indices into Visible
	TMap<FString, int32>  StemToGroup;
	for (int32 i = 0; i < Visible.Num(); ++i)
	{
		const FString Stem = StemOf(Visible[i].Name);
		int32* GI = StemToGroup.Find(Stem);
		if (!GI)
		{
			const int32 NewIdx = Groups.AddDefaulted();
			GroupStem.Add(Stem);
			GI = &StemToGroup.Add(Stem, NewIdx);
		}
		Groups[*GI].Add(i);
	}

	// 3) One entry per group. Families (>=2 members) collapse to the stem label applying the lowest-
	//    numbered member; singles (lone or non-numbered) keep their real asset name.
	Out.Reserve(Groups.Num());
	for (int32 g = 0; g < Groups.Num(); ++g)
	{
		const TArray<int32>& Members = Groups[g];
		FContentEntry E;
		E.bHidden = false;
		if (Members.Num() == 1)
		{
			const FRaw& R = Visible[Members[0]];
			E.DisplayName = R.Name;
			E.ClassPath   = R.ClassPath;
			E.VariantPaths.Add(R.ClassPath);
		}
		else
		{
			int32 BestMember = Members[0];
			int32 BestRank   = VariantRank(Visible[Members[0]].Name);
			for (int32 k = 1; k < Members.Num(); ++k)
			{
				const int32 Rank = VariantRank(Visible[Members[k]].Name);
				if (Rank < BestRank) { BestRank = Rank; BestMember = Members[k]; }
			}
			E.DisplayName = GroupStem[g];                  // e.g. "SkaarjMale"
			E.ClassPath   = Visible[BestMember].ClassPath; // e.g. SkaarjMale01 (lowest-numbered)
			for (int32 m : Members) { E.VariantPaths.Add(Visible[m].ClassPath); }
		}
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
