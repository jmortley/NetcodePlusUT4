// TeamArenaCharacter.cpp
#include "TeamArenaCharacter.h"
#include "UTCharacterMovement.h"
#include "TeamArenaCharacterMovement.h"
#include "UTWeaponAttachment.h"
#include "UTWeaponFix.h"
#include "GameFramework/PlayerController.h"
#include "UTWorldSettings.h"
#include "UTPlusSniper.h"
#include "UTPlusShockRifle.h"
#include "UTGameState.h"
#include "UTWeap_LinkGun.h"
#include "UTArmor.h"
#include "UTDamageType.h"
#include "Net/UnrealNetwork.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"            // GetSelectedCharacter (DarkenBodies skeleton fallback)
#include "UTCharacterContent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "NCPlusForceModels.h"
#include "EngineUtils.h"             // TActorIterator (refresh every other pawn on local team change)
#include "TimerManager.h"           // DarkenBodies delayed corpse hide
#include "Kismet/GameplayStatics.h" // SpawnSound2D (own-footstep volume)
#include "CTFStatsReplicator.h"     // iCTF gate (bIsInstagibMatch) for own-footstep volume

static TAutoConsoleVariable<int32> CVarEnableProjectilePrediction(
	TEXT("ut.EnableProjectilePrediction"),
	1, // Default: 1 (Enabled by default)
	TEXT("If 1, enables one-way latency visual prediction for non hitscan weapons.\n")
	TEXT("Players can set to 0 to opt-out (force server positions)."),
	ECVF_Default); // Saves to user config

// Headshot sphere CENTRE distance below the capsule top, in units. LOWER = the sphere moves UP toward the head
// (it does NOT change the sphere SIZE — that's HeadRadius). Live-tunable so it can be calibrated in warmup against
// ncp.DebugHeads instead of a rebuild per tweak. ⚠️ MUST match client + server: the SERVER's value is authoritative
// for the fallback head validation (GetHeadLocation -> IsHeadShot); the client's value only moves the debug ring.
// Set it on your dogfood server (or a listen server, where client == server) so the green ring AND the real hitbox
// move together. NB the PRIMARY sniper client-informed path uses the HeadBand* clamp, not this — see UTPlusSniper.
static TAutoConsoleVariable<float> CVarHeadCapsuleDrop(
	TEXT("ncp.HeadCapsuleDrop"),
	20.0f,   // calibrated 2026-06-19 (was 26; robot/genghis heads sit higher than the stock ~Z+82)
	TEXT("Headshot sphere centre distance below the capsule top (units). Lower = sphere moves UP toward the head; ")
	TEXT("size is unchanged (HeadRadius). Calibrate live in warmup vs ncp.DebugHeads. Server value is authoritative."),
	ECVF_Default);

// Projectile visual-prediction stability smoothing (UTUpdateSimulatedPosition). ASYMMETRIC: when a high-ping enemy
// jukes, the prediction lead must drop FAST so they stop warping; re-arming the lead is SLOW so we don't over-predict
// straight into their next juke. FastDrop = FInterpTo rate when the stability factor is falling (target reversing/
// perpendicular); SlowRise = rate when it climbs back (target straightened). Live-tunable for dogfood feel.
static TAutoConsoleVariable<float> CVarPredStabFastDrop(
	TEXT("ncp.PredStabFastDrop"),
	30.0f,
	TEXT("Projectile-prediction stability: FInterpTo rate when the lead must drop (target juking). High = snap off fast (~1 frame)."),
	ECVF_Default);
static TAutoConsoleVariable<float> CVarPredStabSlowRise(
	TEXT("ncp.PredStabSlowRise"),
	4.0f,
	TEXT("Projectile-prediction stability: FInterpTo rate when re-arming the lead (target straightened). Low = ease up slowly (~250ms)."),
	ECVF_Default);


ATeamArenaCharacter::ATeamArenaCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UTeamArenaCharacterMovement>(ACharacter::CharacterMovementComponentName))
{
    CachedPredictionPC = nullptr;
    bHasCachedPC = false;
    NetUpdateFrequency = 200.0f;
    MinNetUpdateFrequency = 100.0f;
	NetPriority = 10.0f;
    //MaxSavedPositionAge = 0.35f;
    //PositionSaveRate = 120.0f;
    //PositionSaveInterval = 1.0f / PositionSaveRate;
    //LastPositionSaveTime = 0.0f;
}

// ── Force Models (MutForceModels port, phase 1) ─────────────────────────────
// One override covers every apply trigger — spawn (PossessedBy) and team-change
// (OnRep_PlayerState / Team / SelectedCharacter OnRep) all route through the base
// NotifyTeamChanged. The base reverts the pawn to its REAL model each time it runs.
// Because a single replication burst fires this 2-4x, we don't re-force inline; we mark
// the pawn dirty and re-assert the forced model once on the next Tick (FlushForcedModelUpdate).
void ATeamArenaCharacter::NotifyTeamChanged()
{
	Super::NotifyTeamChanged();

	// Coalesce the forced-model apply. PossessedBy / OnRep_PlayerState / the PlayerState's own
	// NotifyTeamChanged / UTTeamInfo each drive this 2-4x in ONE replication burst (spawn, join, team
	// assignment) — so the heavy ApplyCharacterData mesh rebuild + double GetBodyMIs() recolour loop ran
	// 2-4x per remote pawn per spawn (a hitch). Super above has ALREADY reverted the mesh to the real
	// model on THIS call, so instead of re-forcing per OnRep we just mark dirty here and re-assert the
	// forced model EXACTLY ONCE on the next Tick. Tick runs after this frame's replication dispatch, so
	// the reskin still lands before render (no real-model flash). The flush re-forces with
	// bForceReapply=true and must NOT latch-skip — each Super::NotifyTeamChanged unconditionally reverted
	// the mesh, so a skip would silently drop the reskin.
	if (GetNetMode() != NM_DedicatedServer)
	{
		bForcedModelDirty = true;

		// Friend/enemy is relative to the local player. If this is MY pawn and my team just changed,
		// every OTHER pawn's bucket can flip — but their NotifyTeamChanged won't fire. Refresh them
		// (also coalesced to the next-tick flush). Matters for Enemy-Only / Team-Enemy styles; a cheap
		// no-op for absolute Red/Blue.
		if (IsLocalPlayerPawn())
		{
			bRefreshOthersDirty = true;
		}
	}
}

// Apply the coalesced forced-model work, at most once per frame. Called from the top of Tick on clients,
// which runs AFTER this frame's network replication dispatch — so the N NotifyTeamChanged calls from a
// single replication burst collapse into ONE ApplyCharacterData rebuild here. bForceReapply MUST be true:
// every Super::NotifyTeamChanged during the burst reverted the mesh to the real model, so the flush has to
// re-force unconditionally (a latch-skip would silently drop the reskin).
void ATeamArenaCharacter::FlushForcedModelUpdate()
{
	if (bForcedModelDirty)
	{
		bForcedModelDirty = false;
		ApplyForcedModel(/*bForceReapply=*/true);
	}
	if (bRefreshOthersDirty)
	{
		bRefreshOthersDirty = false;
		RefreshOtherForcedModels();
	}
}

// "Is this MY OWN pawn?" — controlled by a LOCAL human player. Deliberately NOT IsLocallyControlled(): in
// NM_Standalone (offline) every controller (incl. bots' AIControllers) reports IsLocalController()==true, so
// IsLocallyControlled() was true for every pawn and Force Models skipped them all offline. Bots use AIController,
// not a PlayerController, so the Cast cleanly excludes them; IsLocalController() keeps remote players (on a listen
// server) from matching. Correct in standalone, client, and listen-server.
bool ATeamArenaCharacter::IsLocalPlayerPawn() const
{
	const APlayerController* const PC = Cast<APlayerController>(Controller);
	return PC && PC->IsLocalController();
}

void ATeamArenaCharacter::ApplyForcedModel(bool bForceReapply)
{
	// Client-side render preference only — never on a dedicated server, never replicated.
	if (GetNetMode() == NM_DedicatedServer) { return; }
	if (bApplyingForcedModel) { return; }                 // re-entrancy guard (ApplyCharacterData / base NotifyTeamChanged)
	if (IsLocalPlayerPawn()) { return; }                  // never reskin MY OWN pawn (NOT IsLocallyControlled —
	                                                      // that's true for ALL pawns offline; see IsLocalPlayerPawn)

	UWorld* const World = GetWorld();

	// ── Resolve desired state: the model class + colour to force, or "none" = leave natural. ──
	TSubclassOf<AUTCharacterContent> Content = nullptr;
	FLinearColor Colour = FLinearColor::White;
	float        GlowIntensity = 0.f;          // subtle-highlight emissive strength, from Brightness
	bool bWantForce = false;

	if (NCPlusForceModels::IsEnabled())
	{
		const int32 MyTeam = (int32)GetTeamNum();
		if (MyTeam != 255)                                // FFA / no team: deferred (see ForceModels plan)
		{
			// Resolve friend/enemy against THE LOCAL VIEWER's team. NCPlusForceModels::GetViewerTeam
			// uses GetFirstPlayerController() (the one local PC on a client) and, for a teamless
			// spectator, defaults to red = "ours" so Team/Enemy still buckets (OnSameTeam returns
			// false for a spectator, which made everyone read as an enemy).
			const bool bIsFriendly = (MyTeam == NCPlusForceModels::GetViewerTeam(World));

			const FNCPlusModelSettings& Side = NCPlusForceModels::GetModelSettings(MyTeam, bIsFriendly);
			Content = NCPlusForceModels::GetModelClass(Side);
			if (Content && NCPlusForceModels::IsModelAllowed(Content))
			{
				// "Glow" (1 = normal .. 5 = 5x) brightens the model toward the flat, unlit HUD swatch.
				// The lever that actually brightens the BODY is the ALBEDO (the team-colour params below —
				// proven by the recolour working); driving Emissive Max did NOTHING because these body
				// materials have no emissive source (only the eyes do, which is why only they glowed). So
				// Glow OVERBRIGHTS the recolour colour. The emissive scalars are still fed (harmless; helps
				// any model that does have a body emissive channel).
				const float Glow = FMath::Clamp(Side.Brightness, 1.f, 5.f);
				Colour        = NCPlusForceModels::GetSkinColour(Side) * Glow;
				Colour.A      = 1.f;                       // operator* scales alpha too; keep it opaque
				GlowIntensity = (Glow - 1.f) * 1.25f;      // 1 -> 0, 5 -> 5 emissive (only shows where supported)
				bWantForce    = true;
			}
		}
	}

	// ── Natural: feature off, FFA, or friendly under Enemy-Only → this pawn keeps its real model. ──
	if (!bWantForce)
	{
		if (bForcedModelApplied)
		{
			// NotifyTeamChanged path: the base already restored the real model this call.
			// Refresh path: it did not — restore by re-running the base team-change logic.
			if (!bForceReapply)
			{
				bApplyingForcedModel = true;
				bAllowCharacterDataOverride = true;
				AUTCharacter::NotifyTeamChanged();        // ApplyCharacterData(real) + TeamSelect + weapon/hat
				bApplyingForcedModel = false;
			}
			bForcedModelApplied = false;
			LastForcedContent   = nullptr;
			UpdateCosmeticStrip(false);   // pawn no longer reskinned -> restore any stripped cosmetics
		}
		return;
	}

	// ── Force. Skip only in the refresh path when nothing changed (the base didn't revert us there). ──
	if (!bForceReapply && bForcedModelApplied && LastForcedContent == Content.Get() && LastForcedColour == Colour)
	{
		return;
	}

	bApplyingForcedModel = true;

	// Force the mesh via UT's own swap (rebuilds BodyMIs). Flag must be set BEFORE the call —
	// stock ApplyCharacterData early-returns unless bAllowCharacterDataOverride is true.
	bAllowCharacterDataOverride = true;
	ApplyCharacterData(Content);

	static const FName NAME_TeamSelect(TEXT("TeamSelect"));
	static const FName NAME_TeamBlendMax(TEXT("Team Color Blend Max"));
	static const FName NAME_EmissiveMax(TEXT("Emissive Max"));
	static const FName NAME_EmissionPower(TEXT("Emission Power"));
	const TArray<FName>& Params = NCPlusForceModels::TeamColourParamNames();

	// Decide ONCE whether this model can be recoloured. It can't if either (a) no non-skipped body
	// material exposes any of our team-colour params (param-less models, e.g. Garog — auto-detected),
	// or (b) a material is on the [ForceModels] BakedMaterials denylist (params exist but are inert and
	// indistinguishable at runtime, e.g. the community Robot). Non-recolourable models fall back to
	// their baked red/blue team skin below, so they stay team-readable instead of a flat default colour.
	bool bHasParam = false, bDenylisted = false;
	for (UMaterialInstanceDynamic* MID : GetBodyMIs())
	{
		if (!MID) { continue; }
		const UMaterialInterface* Src = MID->Parent;
		const FString MatName = Src ? Src->GetName() : MID->GetName();
		if (NCPlusForceModels::IsRecolorSkippedMaterial(MatName)) { continue; }
		if (NCPlusForceModels::IsBakedMaterial(MatName)) { bDenylisted = true; }
		if (!bHasParam)
		{
			FLinearColor Tmp;
			for (const FName& P : Params)
			{
				if (MID->GetVectorParameterValue(P, Tmp)) { bHasParam = true; break; }
			}
		}
	}
	const bool bRecolour = bHasParam && !bDenylisted;

	// Baked fallback: pick the model's baked red (0) or blue (1) skin from the chosen colour — more red
	// than blue -> red skin, else blue. In Enemy-Only this makes every (non-recolourable) enemy one
	// uniform baked skin; the user's H choice still steers which one.
	const float BakedTeamSelect = (Colour.R >= Colour.B) ? 0.f : 1.f;

	// UT character materials are three-way (TeamSelect 0=Red, 1=Blue, 255=NoTeam). Recolour forces the
	// neutral NoTeam path then tints it (the Red/Blue paths are the model's baked team skins, which a
	// colour param only accents); the fallback instead selects a baked team skin directly.
	for (UMaterialInstanceDynamic* MID : GetBodyMIs())
	{
		if (!MID) { continue; }
		const UMaterialInterface* Src = MID->Parent;
		const FString MatName = Src ? Src->GetName() : MID->GetName();
		if (NCPlusForceModels::IsRecolorSkippedMaterial(MatName))
		{
			// Face/eyes/hair: leave UNTOUCHED so they keep the model's own team tint.
			continue;
		}

		if (!bRecolour)
		{
			// Non-recolourable model: route to its baked red/blue skin rather than the futile NoTeam
			// recolour (which would leave it a flat default). The baked textures carry the team look.
			MID->SetScalarParameterValue(NAME_TeamSelect, BakedTeamSelect);
			continue;
		}

		MID->SetScalarParameterValue(NAME_TeamSelect, 255.f);
		// Some masters bake the team skin into TEXTURES and only blend the colour params over them at a
		// strength gated by this scalar; crank it so the colour actually paints. No-op where absent.
		MID->SetScalarParameterValue(NAME_TeamBlendMax, 1.f);
		// Highlight: drive the emissive intensity from Brightness (0 = off). Only shows where the
		// material has a live emissive channel.
		MID->SetScalarParameterValue(NAME_EmissiveMax, GlowIntensity);
		// Some masters gate the emissive via an "Emission Power" scalar too — drive it (no-op where absent).
		MID->SetScalarParameterValue(NAME_EmissionPower, GlowIntensity);
		for (const FName& P : Params)
		{
			MID->SetVectorParameterValue(P, Colour);
		}
	}

	LastForcedContent   = Content.Get();
	LastForcedColour    = Colour;
	bForcedModelApplied = true;

	// Cosmetic strip (the "Cosmetics" flag, on = remove): drop + suppress hats/eyewear on this reskinned
	// pawn. Set BEFORE OnRep_PlayerState's later SetCosmeticsFromPlayerState so the setter overrides catch
	// the re-add. (NotifyTeamChanged runs first at OnRep, this gate second.)
	UpdateCosmeticStrip(NCPlusForceModels::Get().bCosmetics);

	bApplyingForcedModel = false;
}

// dc's "Remove Cosmetic": IsValid -> DetachFromActor (Keep Relative) -> DestroyActor. The explicit
// detach matches the BP (bare Destroy() would also detach, but we mirror dc's proven recipe).
static void RemoveCosmetic(AActor* Cosmetic)
{
	if (Cosmetic)
	{
		Cosmetic->DetachFromActor(FDetachmentTransformRules::KeepRelativeTransform);
		Cosmetic->Destroy();
	}
}

void ATeamArenaCharacter::StripCosmetics()
{
	RemoveCosmetic(Hat);       Hat       = nullptr;
	RemoveCosmetic(Eyewear);   Eyewear   = nullptr;
	RemoveCosmetic(LeaderHat); LeaderHat = nullptr;
}

void ATeamArenaCharacter::UpdateCosmeticStrip(bool bShouldStrip)
{
	const bool bWasStripping = bForceModelStripCosmetics;
	bForceModelStripCosmetics = bShouldStrip;
	if (bShouldStrip)        { StripCosmetics(); }            // clear any already-spawned (overrides stop re-adds)
	else if (bWasStripping)  { SetCosmeticsFromPlayerState(); } // restore what we suppressed
}

void ATeamArenaCharacter::SetHatClass(TSubclassOf<AUTHat> HatClass)
{
	if (bForceModelStripCosmetics)
	{
		RemoveCosmetic(Hat); Hat = nullptr;       // don't spawn the hat on a stripped pawn
		return;
	}
	Super::SetHatClass(HatClass);
}

void ATeamArenaCharacter::SetEyewearClass(TSubclassOf<AUTEyewear> EyewearClass)
{
	if (bForceModelStripCosmetics)
	{
		RemoveCosmetic(Eyewear); Eyewear = nullptr;
		return;
	}
	Super::SetEyewearClass(EyewearClass);
}

void ATeamArenaCharacter::LeaderHatStatusChanged_Implementation()
{
	if (bForceModelStripCosmetics)
	{
		RemoveCosmetic(LeaderHat); LeaderHat = nullptr;
		return;
	}
	Super::LeaderHatStatusChanged_Implementation();
}

void ATeamArenaCharacter::PlayDying()
{
	Super::PlayDying();
	SpawnSkeletonDissolve();
}

// On death, hide the corpse mesh after a delay. Two roles:
//  (1) "Darken Bodies" toggle (any mode): hide after the ~1s death-effects fade (instant hide looked abrupt).
//      Replaces dc's ModelDissolveEffect (exec-chain dissolve that didn't run reliably across models); this
//      clean hide works on any model with zero asset dependency.
//  (2) iCTF safety net (regardless of DarkenBodies): the body is supposed to be removed by the BP CleanUpRagdoll
//      at [InstagibCTF] RagdollTime, but if that doesn't happen (e.g. DarkenBodies off and the BP cleanup never
//      fires) the corpse would linger. So in iCTF we ALSO hide it at the ragdoll lifespan, so a low Ragdoll Time
//      reliably makes the body vanish. Hide-only (SetVisibility) — never destroys the actor, so it can't fight
//      the BP destroy or the engine corpse cleanup. Client-side, per pawn; no-op on a dedicated server.
void ATeamArenaCharacter::SpawnSkeletonDissolve()
{
	if (GetNetMode() == NM_DedicatedServer) { return; }

	const FNCPlusForceModelsConfig& C = NCPlusForceModels::Get();
	const bool bDarken = (C.bEnabled && C.bDarkenBodies);

	// iCTF detection: RagdollTime is an iCTF setting (the BP CleanUpRagdoll only runs for the instagib damage
	// type). ACTFStatsReplicator is present only in NCPlusCTF instagib; absent in ElimPlus etc.
	bool bIsInstagib = false;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ACTFStatsReplicator> It(World); It; ++It) { bIsInstagib = It->bIsInstagibMatch; break; }
	}

	// Nothing to do unless DarkenBodies is on (the fade, any mode) OR this is iCTF (the ragdoll-cleanup backup).
	// Outside both, leave corpses to the stock/engine cleanup — no change to ElimPlus and friends.
	if (!bDarken && !bIsInstagib) { return; }

	float RagdollTime = 3.0f;
	FString Val;
	const FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	if (GConfig && GConfig->GetString(TEXT("InstagibCTF"), TEXT("RagdollTime"), Val, ConfigPath))
	{
		RagdollTime = FCString::Atof(*Val);
	}

	// DarkenBodies hides early (~1s death-effects fade, but never longer than the ragdoll lives); otherwise
	// (iCTF, DarkenBodies off) hide exactly at the ragdoll lifespan so the body still vanishes when the BP
	// CleanUpRagdoll doesn't. Floor 0.01s (SetTimer never schedules rate<=0). Timer is bound to this actor, so
	// it auto-clears if the corpse is destroyed/cleaned up sooner (gib, respawn, DeathCleanupTimer).
	const float HideDelay = FMath::Max(0.01f, bDarken ? FMath::Min(1.0f, RagdollTime) : RagdollTime);
	FTimerHandle TempHandle;
	GetWorldTimerManager().SetTimer(TempHandle, this, &ATeamArenaCharacter::HideDeadBody, HideDelay, false);
}

void ATeamArenaCharacter::HideDeadBody()
{
	if (USkeletalMeshComponent* BodyMesh = GetMesh())
	{
		BodyMesh->SetVisibility(false, /*bPropagateToChildren=*/true);
	}
}

// When the local player's team changes, every other pawn's friend/enemy bucket can flip without
// their own NotifyTeamChanged firing. Collect first, then apply — re-running a pawn's base
// NotifyTeamChanged (the un-force restore) can spawn/destroy its LeaderHat, which is unsafe to
// do while a TActorIterator is live.
void ATeamArenaCharacter::RefreshOtherForcedModels()
{
	if (GetNetMode() == NM_DedicatedServer) { return; }
	UWorld* const World = GetWorld();
	if (!World) { return; }

	TArray<ATeamArenaCharacter*> Others;
	for (TActorIterator<ATeamArenaCharacter> It(World); It; ++It)
	{
		ATeamArenaCharacter* Other = *It;
		if (Other && Other != this && !Other->IsLocalPlayerPawn())
		{
			Others.Add(Other);
		}
	}

	// The dirty-latch inside ApplyForcedModel(false) makes this cheap when a bucket didn't change.
	for (ATeamArenaCharacter* Other : Others)
	{
		Other->ApplyForcedModel(/*bForceReapply=*/false);
	}
}

void ATeamArenaCharacter::UpdateArmorOverlay()
{
	Super::UpdateArmorOverlay();   // sets up the armour overlay (+ the stock hardcoded yellow "Color")

	// Redirect that yellow to our match/complimentary armour colour, for pawns we reskin. Client-only
	// (OverlayMesh's MID only exists off the dedicated server). This is the ArmorType OnRep, so it
	// re-fires on every armour change and always runs AFTER the stock colour, winning cleanly.
	if (GetNetMode() == NM_DedicatedServer || IsLocalPlayerPawn() || !OverlayMesh) { return; }  // skip MY pawn (offline-safe)

	const FNCPlusForceModelsConfig& C = NCPlusForceModels::Get();
	if (!C.bEnabled || !C.bArmour) { return; }

	const int32 MyTeam = (int32)GetTeamNum();
	if (MyTeam == 255) { return; }                                  // FFA: deferred

	UWorld* const World = GetWorld();
	const bool bIsFriendly = (MyTeam == NCPlusForceModels::GetViewerTeam(World));   // spectator -> red is "ours"
	if (C.Style == ENCPlusSkinStyle::EnemyOnly && bIsFriendly) { return; }   // Enemy-Only leaves teammates stock

	UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(OverlayMesh->GetMaterial(0));
	if (!MID) { return; }

	const FLinearColor ArmourColour = NCPlusForceModels::GetArmourColour(NCPlusForceModels::GetModelSettings(MyTeam, bIsFriendly));

	// Stock "Color" is a BRIGHT ~(1,1,0) yellow that drives the armour's emissive glow; our configured
	// colour is usually dimmer (V<1), so reusing it flat washed the glow out. Push our hue to full
	// brightness (normalise so the brightest channel = 1, like the stock yellow) for the emissive "Color",
	// and keep the real tint in "TeamColor". Preserves the armour's emissive intensity in our hue.
	FLinearColor Glow = ArmourColour;
	const float MaxCh = FMath::Max3(Glow.R, Glow.G, Glow.B);
	if (MaxCh > KINDA_SMALL_NUMBER) { Glow /= MaxCh; }
	static const FName NAME_ArmorColor(TEXT("Color"));
	static const FName NAME_ArmorTeamColor(TEXT("TeamColor"));
	MID->SetVectorParameterValue(NAME_ArmorColor, Glow);
	MID->SetVectorParameterValue(NAME_ArmorTeamColor, ArmourColour);
}

int32 ATeamArenaCharacter::GetNetcodeVersion()
{
	// Reads the #define NETCODE_PLUGIN_VERSION from NetcodePlus.h
	return NETCODE_PLUGIN_VERSION;
}



float ATeamArenaCharacter::GetClientVisualPredictionTime() const
{
	if (PlayerState && GetNetMode() == NM_Client)
	{
		// 1. Opt-Out Check
		if (CVarEnableProjectilePrediction.GetValueOnGameThread() == 0)
		{
			return 0.0f;
		}

		// 2. Weapon Logic - Hitscan weapons get 0 prediction
		AUTWeapon* MyWeapon = GetWeapon();
		if (MyWeapon)
		{
			// Hitscan weapons: No visual prediction (server authoritative)
			if (Cast<AUTPlusSniper>(MyWeapon) || Cast<AUTPlusShockRifle>(MyWeapon))
			{
				return 0.0f;
			}

			// Projectile weapons: Apply visual prediction
			float Fudge = 20.0f;
			float AdjustedPing = FMath::Max(0.0f, PlayerState->ExactPing - Fudge);
			float OneWayLatency = AdjustedPing * 0.0005f;
			return FMath::Min(OneWayLatency, 0.10f);
		}
	}

	return 0.0f;
}





bool ATeamArenaCharacter::IsHeadShot(FVector HitLocation, FVector ShotDirection, float WeaponHeadScaling,
	AUTCharacter* ShotInstigator, float PredictionTime)
{
	// Team check (same as Epic)
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (ShotInstigator && GS && GS->OnSameTeam(this, ShotInstigator))
	{
		return false;
	}

	// CRITICAL FIX: Pass PredictionTime to GetHeadLocation!
	// Epic's bug: FVector HeadLocation = GetHeadLocation(); // Never passed PredictionTime!
	FVector HeadLocation = GetHeadLocation(PredictionTime);

	bool bHeadShot = FMath::PointDistToLine(HeadLocation, ShotDirection, HitLocation)
		< HeadRadius * HeadScale * WeaponHeadScaling;

	return bHeadShot;
}




void ATeamArenaCharacter::UTUpdateSimulatedPosition(const FVector& NewLocation, const FRotator& NewRotation, const FVector& NewVelocity)
{
	if (UTCharacterMovement)
	{
		// Cache the OLD velocity before we update it
		FVector OldVelocity = GetVelocity();

		UTCharacterMovement->SimulatedVelocity = NewVelocity;
		float NoSmoothThreshold = UTCharacterMovement->NetworkNoSmoothUpdateDistance;
		float SmoothThreshold = UTCharacterMovement->NetworkMaxSmoothUpdateDistance;
		// If location changed or just spawned...
		if ((NewLocation != GetActorLocation()) || (CreationTime == GetWorld()->TimeSeconds))
		{
			// Standard geometry check
			if (GetWorld()->EncroachingBlockingGeometry(this, NewLocation, NewRotation))
			{
				bSimGravityDisabled = true;
			}
			else
			{
				bSimGravityDisabled = false;
			}

			// 1. Move Capsule to EXACT Server Location (The Anchor)
			SetActorLocationAndRotation(NewLocation, NewRotation, false);

			// 2. Prediction Logic
			if (GetCharacterMovement())
			{
				GetCharacterMovement()->bJustTeleported = true;

				float PredictionTime = 0.0f;

				if (GetNetMode() != NM_DedicatedServer)
				{
					APlayerController* LocalPC = GEngine ? GEngine->GetFirstLocalPlayerController(GetWorld()) : nullptr;
					if (LocalPC && LocalPC->GetPawn())
					{
						ATeamArenaCharacter* ViewerChar = Cast<ATeamArenaCharacter>(LocalPC->GetPawn());
						if (ViewerChar)
						{
							float BasePrediction = ViewerChar->GetClientVisualPredictionTime();

							if (BasePrediction > 0.0f)
							{
								// --- ADAPTIVE SCALING (replaces binary bStableDirection) ---
								float VelocityDot = OldVelocity.GetSafeNormal() | NewVelocity.GetSafeNormal();

								// Map dot [-1, 1] -> scale [0, 1]
								// -1 (opposite dir) = 0% prediction
								//  0 (perpendicular) = ~50% prediction
								// +1 (same dir)      = 100% prediction
								float StabilityFactor = FMath::Clamp((VelocityDot + 1.0f) * 0.5f, 0.0f, 1.0f);

								// ASYMMETRIC smoothing (replaces the symmetric rate-6 FInterpTo). A high-ping enemy's
								// juke must STOP warping immediately, but re-arming the lead should be gradual so we
								// don't over-predict into their NEXT juke. So drop the factor FAST when it's falling
								// (target reversing) and rise SLOWLY when it climbs (target straightened). Plus a hard-
								// zero on a genuine horizontal reversal (VelocityDot<0 = >90 deg flip), gated on
								// horizontal speed so a jump arc's vertical Z-velocity flip at apex doesn't trip it.
								const float dt = GetWorld()->GetDeltaSeconds();
								if (OldVelocity.Size2D() > 200.0f && VelocityDot < 0.0f)
								{
									SmoothedStabilityFactor = 0.0f;   // dodge/strafe-flip: kill the lead NOW (no warp)
								}
								else
								{
									const float InterpRate = (StabilityFactor < SmoothedStabilityFactor)
										? CVarPredStabFastDrop.GetValueOnGameThread()    // falling  -> snap the lead down
										: CVarPredStabSlowRise.GetValueOnGameThread();   // rising   -> ease it back up
									SmoothedStabilityFactor = FMath::FInterpTo(SmoothedStabilityFactor, StabilityFactor, dt, InterpRate);
								}

								PredictionTime = BasePrediction * SmoothedStabilityFactor;
							}
						}
					}
				}

				// --- RUN SIMULATION & TETHER ---
				if (PredictionTime > 0.005f)
				{
					// 1. Simulate Forward
					UTCharacterMovement->UTSimulateMovement(PredictionTime);

					// 2. Tether (unchanged)
					FVector PredictedLocation = GetActorLocation();
					FVector ErrorDelta = PredictedLocation - NewLocation;
					float Speed = NewVelocity.Size();
					float MaxDistance = FMath::Clamp(Speed * PredictionTime, 40.0f, NoSmoothThreshold);

					if (ErrorDelta.SizeSquared() > MaxDistance * MaxDistance)
					{
						FVector ClampedLocation = NewLocation + (ErrorDelta.GetSafeNormal() * MaxDistance);
						SetActorLocation(ClampedLocation);
					}
				}
			}
		}
		else if (NewRotation != GetActorRotation())
		{
			GetRootComponent()->MoveComponent(FVector::ZeroVector, NewRotation, false);
		}
	}
}



void ATeamArenaCharacter::FiringInfoUpdated()
{
    // 1. Interrupt Animation (Standard)
    UAnimInstance* AnimInstance = GetMesh()->GetAnimInstance();
    if (AnimInstance != NULL)
    {
        AnimInstance->Montage_Stop(0.2f);
    }

    //if (Weapon && Weapon->ZoomState != EZoomState::EZS_NotZoomed)
    //{
        // Prevent any FiringInfoUpdated visual calls while zoomed
    //    return;
    //}
	
    if (IsLocallyControlled() && !bLocalFlashLoc)
    {
        // If this is our custom weapon, we know we handled visuals locally in FireShot.
        // We suppress the server echo to prevent Double Tracers.
        if (Weapon && Weapon->IsA(AUTWeaponFix::StaticClass()))
        {
            return;
        }

        // If this is a Stock Weapon (Sniper, etc), it DOES NOT predict locally.
        // It relies on this server echo to draw the beam. We let it pass.
    }
	


    // 2. Safe Getters
    AUTPlayerController* UTPC = GetLocalViewer();

    // Returns 0.0f in your override, so this is safe/dependency-free
    float MyPredictionTime = GetClientVisualPredictionTime();
	


    // --- FIX START: NULL-SAFE LOGIC ---
    bool bShouldPlayServerEffect = true;

	//bool bIsSpectating = (UTPC != nullptr && UTPC->UTPlayerState && UTPC->UTPlayerState->bIsSpectator);

    if (UTPC != nullptr && !bLocalFlashLoc && MyPredictionTime > 0.f)
    {
        bShouldPlayServerEffect = false;
    }

    // 4. Play Effects if Allowed
    if (bShouldPlayServerEffect && Weapon != NULL && Weapon->ShouldPlay1PVisuals())
    {
        if (!FlashLocation.Position.IsZero())
        {
            uint8 EffectFiringMode = Weapon->GetCurrentFireMode();

            // Spectator Logic (Replication)
            if (Controller == NULL)
            {
                EffectFiringMode = FireMode;
                Weapon->FiringInfoUpdated(FireMode, FlashCount, FlashLocation.Position);
                Weapon->FiringEffectsUpdated(FireMode, FlashLocation.Position);
            }
            // Other Player Logic (Local View)
            else
            {
                FVector SpawnLocation;
                FRotator SpawnRotation;
                Weapon->GetImpactSpawnPosition(FlashLocation.Position, SpawnLocation, SpawnRotation);
                Weapon->PlayImpactEffects(FlashLocation.Position, EffectFiringMode, SpawnLocation, SpawnRotation);
            }
        }
        else if (Controller == NULL)
        {
            Weapon->FiringInfoUpdated(FireMode, FlashCount, FlashLocation.Position);
        }

        if (FlashCount == 0 && FlashLocation.Position.IsZero() && WeaponAttachment != NULL)
        {
            WeaponAttachment->StopFiringEffects();
        }
    }
    // 4. Standard Third Person Effects (Always Safe)
    // 4. Play 3P Effects (Enemies / Other Players)
    else if (WeaponAttachment != NULL)
    {
        if (FlashCount != 0 || !FlashLocation.Position.IsZero())
        {
            // A. Always run standard logic (Audio, Muzzle Flash, etc.)
            WeaponAttachment->PlayFiringEffects();

            // B. ALWAYS FORCE BEAM VISIBILITY
            // We removed the 'bEngineHandledIt' check. We now force this 100% of the time.
            // This ensures hits on yourself (close range) are drawn even if the engine 
            // glitches out on the Enemy mesh visibility.

            if (!FlashLocation.Position.IsZero() &&
                WeaponAttachment->FireEffect.IsValidIndex(FireMode) &&
                WeaponAttachment->FireEffect[FireMode] != nullptr)
            {
                FVector SpawnLocation = GetActorLocation();

                // Calculate Start Location from Attachment
                if (WeaponAttachment->MuzzleFlash.IsValidIndex(FireMode) && WeaponAttachment->MuzzleFlash[FireMode] != nullptr)
                {
                    SpawnLocation = WeaponAttachment->MuzzleFlash[FireMode]->GetComponentLocation();
                }
                else if (WeaponAttachment->Mesh != nullptr)
                {
                    SpawnLocation = WeaponAttachment->Mesh->GetSocketLocation(WeaponAttachment->AttachSocket);
                }

                // Force Spawn the Beam
                UParticleSystemComponent* PSC = UGameplayStatics::SpawnEmitterAtLocation(
                    GetWorld(),
                    WeaponAttachment->FireEffect[FireMode],
                    SpawnLocation,
                    (FlashLocation.Position - SpawnLocation).Rotation(),
                    true
                );

                if (PSC)
                {
                    static FName NAME_HitLocation(TEXT("HitLocation"));
                    static FName NAME_LocalHitLocation(TEXT("LocalHitLocation"));

                    PSC->SetVectorParameter(NAME_HitLocation, FlashLocation.Position);
                    PSC->SetVectorParameter(NAME_LocalHitLocation, PSC->ComponentToWorld.InverseTransformPosition(FlashLocation.Position));

                    // CRITICAL: Ensure visual parameters (Colors, Lightning Arcs) are applied
                    WeaponAttachment->ModifyFireEffect(PSC);
                }
            }
        }
        else
        {
            WeaponAttachment->StopFiringEffects();
        }
    }

    K2_FiringInfoUpdated();
}




FVector ATeamArenaCharacter::GetRewindLocation(float PredictionTime, AUTPlayerController* DebugViewer)
{
    float ActualPredictionTime = PredictionTime;

    // --- CRITICAL FIX ---
    // CLIENT: Force 0ms. 
    // We are viewing a replicated transaction. We want the tracer to align 
    // with the mesh exactly as it is currently rendered on screen.
    if (GetNetMode() == NM_Client)
    {
        ActualPredictionTime = GetClientVisualPredictionTime(); // Returns 0.0f
    }
    // SERVER: Keep 'PredictionTime'. 
    // The server passes in the exact rewind amount needed for lag compensation 
    // (calculated in UTWeaponFix::HitScanTrace). If we zero this out, 
    // we break hit registration.

    // --- STANDARD UT LOGIC (Adapted) ---
    FVector TargetLocation = GetActorLocation();
    FVector PrePosition = GetActorLocation();
    FVector PostPosition = GetActorLocation();

    // Use the calculated time based on the logic above
    float TargetTime = GetWorld()->GetTimeSeconds() - ActualPredictionTime;
    float Percent = 0.999f;
    bool bTeleported = false;

    if (ActualPredictionTime > 0.f)
    {
        for (int32 i = SavedPositions.Num() - 1; i >= 0; i--)
        {
            TargetLocation = SavedPositions[i].Position;
            if (SavedPositions[i].Time < TargetTime)
            {
                if (!SavedPositions[i].bTeleported && (i < SavedPositions.Num() - 1))
                {
                    PrePosition = SavedPositions[i].Position;
                    PostPosition = SavedPositions[i + 1].Position;
                    if (SavedPositions[i + 1].Time == SavedPositions[i].Time)
                    {
                        Percent = 1.f;
                        TargetLocation = SavedPositions[i + 1].Position;
                    }
                    else
                    {
                        Percent = (TargetTime - SavedPositions[i].Time) / (SavedPositions[i + 1].Time - SavedPositions[i].Time);
                        TargetLocation = SavedPositions[i].Position + Percent * (SavedPositions[i + 1].Position - SavedPositions[i].Position);
                    }
                }
                else
                {
                    bTeleported = SavedPositions[i].bTeleported;
                }
                break;
            }
        }
    }

    if (DebugViewer)
    {
        DebugViewer->ClientDebugRewind(GetActorLocation(), TargetLocation, PrePosition, PostPosition, GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), ActualPredictionTime, Percent, bTeleported);
    }

    return TargetLocation;
}

FVector ATeamArenaCharacter::GetHeadLocation(float PredictionTime)
{
	// Model-INDEPENDENT head, derived from the CAPSULE instead of the skeletal mesh's "head" bone.
	//
	// SERVER-SIDE INTERIM for the Force-Models headshot bug (the full client-informed fix is on the
	// 327-experimental branch). With Force Models the client renders a different model than the server
	// validates against, so a bone-derived head desyncs and headshots aimed at the visible head miss. The
	// capsule is identical client+server, so anchoring the head to the capsule top makes the server's
	// sphere model-independent — a forced model's visible head (UT scales every model to fill the standard
	// capsule, UTCharacter.cpp:5351) lines up with it. Also makes the rewind exact (fixed capsule geometry,
	// no animated-bone pose to approximate) and drops the per-check RefreshBoneTransforms.
	//
	// kHeadCapsuleDrop = sphere CENTRE below the capsule top (standing half-height 108, stock head ~Z+82 -> ~26).
	// Now a live cvar (ncp.HeadCapsuleDrop) so it can be calibrated in warmup vs ncp.DebugHeads without a rebuild —
	// LOWER it to raise the sphere onto the head. Read on any thread (validation is game-thread; harmless elsewhere).
	// Client + server must use the same value (server authoritative for validation). GetScaledCapsuleHalfHeight() is
	// crouch-aware. NB a forced model whose head sits higher than stock (e.g. a tall robot) needs a SMALLER drop than
	// 26 — that's why the default lands at the chest on some models; pick a value that fits the models you run.
	const float kHeadCapsuleDrop = CVarHeadCapsuleDrop.GetValueOnAnyThread();

	const float HalfHeight = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	// GetRewindLocation rewinds the actor (capsule) position on the server and returns the current position
	// on clients / for PredictionTime <= 0, so this one call covers both the rewound and live cases.
	return GetRewindLocation(PredictionTime) + FVector(0.f, 0.f, HalfHeight - kHeadCapsuleDrop);
}

void ATeamArenaCharacter::BeginPlay()
{
	Super::BeginPlay();

	// SERVER ONLY: Register our custom material so SetCharacterOverlayEffect works without warnings
	if (Role == ROLE_Authority && SpawnProtectionMaterial)
	{
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS)
		{
			GS->AddOverlayMaterial(SpawnProtectionMaterial, nullptr);
		}
	}

	// Note: Ping-compensated spawn hiding is done in the game mode's
	// RestartPlayer() AFTER Super::RestartPlayer() returns (which calls BeginPlay).
	// The game mode sets bPingCompensatedSpawnPending, hides, and disables collision.
}




void ATeamArenaCharacter::Tick(float DeltaTime)
{
	// Flush any forced-model apply coalesced from this frame's replication burst (see NotifyTeamChanged).
	// Runs here — after net dispatch, before Super::Tick/render — so N team OnReps collapse to one mesh
	// rebuild and the reskin is in place for this frame. No-op on a dedicated server (flags never set).
	if (bForcedModelDirty || bRefreshOthersDirty)
	{
		FlushForcedModelUpdate();
	}

	// ── Ping-compensated spawn: client confirms control ──
	if (bPingCompensatedSpawnPending)
	{
		if (IsLocallyControlled() && Controller != nullptr)
		{
			// Client has possession — tell server to reveal us
			bPingCompensatedSpawnPending = false;
			ServerConfirmSpawnReady();
		}
		else if (Role == ROLE_Authority)
		{
			// Server timeout: force-reveal after 500ms to prevent permanently hidden pawns
			if (GetWorld()->GetTimeSeconds() - SpawnHiddenTimestamp > 0.5f)
			{
				bPingCompensatedSpawnPending = false;
				SetActorHiddenInGame(false);
				SetActorEnableCollision(true);
			}
		}
	}

	// --- Per-weapon hide: detect weapon switch and apply hide state ---
	// Works for ALL weapons (stock and NetcodePlus) since TeamArenaCharacter
	// is the character class for all players.
	// UPROPERTY ensures LastEquippedWeapon is nulled by GC on weapon destroy (death/drop).
	if (GetNetMode() != NM_DedicatedServer && IsLocallyControlled())
	{
		AUTWeapon* CurrentWeapon = GetWeapon();
		// Only check when weapon actually changes — the pointer comparison
		// is near-zero cost and skips 99.9% of frames instantly.
		if (CurrentWeapon && CurrentWeapon != LastEquippedWeapon
			&& !CurrentWeapon->IsPendingKillPending()
			&& CurrentWeapon->GetMesh() && CurrentWeapon->GetMesh()->IsRegistered())
		{
			LastEquippedWeapon = CurrentWeapon;
			// Check hide by class name (allows Lightning Gun and Sniper to hide independently)
			FName HideKey = FName(*CurrentWeapon->GetClass()->GetName());
			bool bShouldHide = false;
			{
				bool* bHidden = AUTWeaponFix::HiddenWeaponsByTag.Find(HideKey);
				bShouldHide = bHidden && *bHidden;
			}

			if (bShouldHide)
			{
				CurrentWeapon->GetMesh()->SetHiddenInGame(true);
				if (FirstPersonMesh)
				{
					FirstPersonMesh->SetHiddenInGame(true);
				}
			}
			else
			{
				CurrentWeapon->GetMesh()->SetHiddenInGame(false);
				if (FirstPersonMesh)
				{
					FirstPersonMesh->SetHiddenInGame(false);
				}
			}
		}
		// Clear stale reference if weapon was removed (death, drop)
		if (LastEquippedWeapon && (!CurrentWeapon || CurrentWeapon->IsPendingKillPending()))
		{
			LastEquippedWeapon = nullptr;
		}
	}

	// --- FIX: Kill ambient sounds from inactive weapons (stock link gun overheat bug) ---
	// Stock UTWeap_LinkGun::Tick() sets overheat ambient sound on the owner even when
	// the link gun is inactive in inventory. This causes spectators to hear a looping
	// overheat sound on players who aren't even holding the link gun.
	if (GetNetMode() != NM_DedicatedServer && AmbientSound)
	{
		AUTWeapon* ActiveWeapon = GetWeapon();
		if (ActiveWeapon)
		{
			// If active weapon is a link gun, the sound is legitimate
			AUTWeap_LinkGun* ActiveLinkGun = Cast<AUTWeap_LinkGun>(ActiveWeapon);
			if (!ActiveLinkGun || ActiveLinkGun->OverheatSound != AmbientSound)
			{
				// Active weapon is NOT a link gun, or its overheat sound doesn't match.
				// Check if an inactive link gun in inventory is the culprit.
				for (TInventoryIterator<AUTWeapon> It(this); It; ++It)
				{
					if (*It != ActiveWeapon)
					{
						AUTWeap_LinkGun* InactiveLinkGun = Cast<AUTWeap_LinkGun>(*It);
						if (InactiveLinkGun && InactiveLinkGun->OverheatSound == AmbientSound)
						{
							SetAmbientSound(nullptr, true);
							break;
						}
					}
				}
			}
		}
	}

	// Kill ambient sounds on dead characters
	if (IsDead() && AmbientSoundComp && AmbientSoundComp->IsPlaying())
	{
		AmbientSoundComp->Stop();
	}

	// --- PERF: Skip base class spawn protection material loop ---
	// Base AUTCharacter::Tick (line 4698-4734) loops BodyMIs setting SpawnProtectionPct
	// every frame. We handle spawn protection ourselves, so skip the base class work
	// by temporarily clearing the flag. Saves ~27K material calls/sec.
	//
	// IMPORTANT: We still need the base class expiry logic to run, so we handle it
	// ourselves here before skipping. The base class checks elapsed time against
	// GS->SpawnProtectionTime and clears bSpawnProtectionEligible when time is up.
	bool bSavedSpawnProtectionEligible = bSpawnProtectionEligible;
	if (bSpawnProtectionEligible && GetNetMode() != NM_DedicatedServer)
	{
		// Run the expiry check that base class would have done
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
		if (GS != nullptr && GS->SpawnProtectionTime > 0.0f)
		{
			float Pct = 1.0f - (GetWorld()->TimeSeconds - SpawnProtectionStartTime) / GS->SpawnProtectionTime;
			if (Pct <= 0.0f)
			{
				// Time's up — actually expire spawn protection
				bSpawnProtectionEligible = false;
				bSavedSpawnProtectionEligible = false;
			}
		}
		else
		{
			// No spawn protection time configured — disable immediately
			bSpawnProtectionEligible = false;
			bSavedSpawnProtectionEligible = false;
		}

		// Hide from base class material loop (we do our own visuals below)
		bSpawnProtectionEligible = false;
	}

	// --- PERF: Throttle OverlayMesh->MarkRenderStateDirty() ---
	// Base AUTCharacter::Tick calls this EVERY FRAME as a workaround for an engine bug.
	// We throttle to 60Hz regardless of render frame rate (works at 480fps and 720fps).
	USkeletalMeshComponent* SavedOverlayMesh = nullptr;
	if (OverlayMesh && OverlayMesh->IsRegistered() && GetNetMode() != NM_DedicatedServer)
	{
		constexpr float OverlayDirtyInterval = 1.f / 60.f;
		const float CurrentTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		if (CurrentTime - LastOverlayDirtyTime < OverlayDirtyInterval)
		{
			SavedOverlayMesh = OverlayMesh;
			OverlayMesh = nullptr;
		}
		else
		{
			LastOverlayDirtyTime = CurrentTime;
		}
	}

	Super::Tick(DeltaTime);

	// Restore OverlayMesh if we hid it
	if (SavedOverlayMesh)
	{
		OverlayMesh = SavedOverlayMesh;
	}

	// Restore spawn protection flag ONLY on clients (where we cleared it for the
	// material loop optimization). On the dedicated server we never touched it,
	// so the base class expiry in Super::Tick() must be allowed to persist —
	// otherwise the unconditional restore overwrites the server's "time's up"
	// clear back to true, making spawn protection permanent.
	if (GetNetMode() != NM_DedicatedServer)
	{
		bSpawnProtectionEligible = bSavedSpawnProtectionEligible;
	}

	// Visuals are for clients only
	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// --- PERF: Overlay visibility cull ---
	// OverlayMesh has BoundsScale=15000 set by Epic in UTCharacter::UpdateCharOverlays,
	// which disables engine frustum/HZB culling on it. We piggy-back on the main mesh
	// (normal bounds) to cull overlays for characters that are off-screen, occluded,
	// or far from the local viewer. SetVisibility only fires when state changes to
	// avoid redundant MarkRenderStateDirty.
	if (OverlayMesh && OverlayMesh->IsRegistered())
	{
		bool bShouldShow = true;

		if (USkeletalMeshComponent* MainMesh = GetMesh())
		{
			const float SinceRendered = GetWorld()->GetTimeSeconds() - MainMesh->LastRenderTime;
			if (SinceRendered > 0.1f)
			{
				bShouldShow = false;
			}
		}

		if (bShouldShow)
		{
			if (APlayerController* LocalPC = GetWorld()->GetFirstPlayerController())
			{
				FVector ViewLoc;
				FRotator ViewRot;
				LocalPC->GetPlayerViewPoint(ViewLoc, ViewRot);
				constexpr float OverlayCullDistSq = 5500.f * 5500.f;
				if (FVector::DistSquared(GetActorLocation(), ViewLoc) > OverlayCullDistSq)
				{
					bShouldShow = false;
				}
			}
		}

		if (OverlayMesh->IsVisible() != bShouldShow)
		{
			OverlayMesh->SetVisibility(bShouldShow, true);
		}
	}

	// --- CASE 1: Active Spawn Protection ---
	if (bSpawnProtectionEligible)
	{
		bHasSpawnOverlay = true;

		// --- TEAM FILTERING ---
		bool bShowGlowToViewer = true;
		AUTPlayerController* LocalViewer = GetLocalViewer();
		AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();

		if (GS && LocalViewer && GS->OnSameTeam(this, LocalViewer))
		{
			bShowGlowToViewer = false;
		}

		// --- PERF: Dirty flag — only update materials when state changes ---
		// Values are constant within each state (glow on vs glow off).
		// bLastShowGlowState initialized to 0xFF to force first-frame apply.
		uint8 CurrentState = bShowGlowToViewer ? 1 : 0;
		if (CurrentState == bLastShowGlowState)
		{
			return; // No state change, skip all material work
		}
		bLastShowGlowState = CurrentState;

		static FName NAME_SpawnProtectionPct(TEXT("SpawnProtectionPct"));
		static FName NAME_HitFlashColor(TEXT("HitFlashColor"));
		static FName NAME_FullBodyFlashPct(TEXT("FullBodyFlashPct"));

		if (bShowGlowToViewer)
		{
			FLinearColor BaseTeamColor = GetTeamColor();
			float BrightnessMult = 20.0f;
			FLinearColor ObviousColor = FLinearColor(
				BaseTeamColor.R * BrightnessMult,
				BaseTeamColor.G * BrightnessMult,
				BaseTeamColor.B * BrightnessMult,
				1.0f
			);

			for (UMaterialInstanceDynamic* MI : BodyMIs)
			{
				if (MI)
				{
					MI->SetScalarParameterValue(NAME_SpawnProtectionPct, 0.0f);
					MI->SetVectorParameterValue(NAME_HitFlashColor, ObviousColor);
					MI->SetScalarParameterValue(NAME_FullBodyFlashPct, 0.4f);
				}
			}
		}
		else
		{
			for (UMaterialInstanceDynamic* MI : BodyMIs)
			{
				if (MI)
				{
					MI->SetScalarParameterValue(NAME_SpawnProtectionPct, 0.0f);
					MI->SetVectorParameterValue(NAME_HitFlashColor, FLinearColor(0.f, 0.f, 0.f, 0.f));
					MI->SetScalarParameterValue(NAME_FullBodyFlashPct, 0.0f);
				}
			}
		}
	}
	// --- CASE 2: Cleanup (Runs once when protection ends) ---
	else if (bHasSpawnOverlay)
	{
		bHasSpawnOverlay = false;
		bLastShowGlowState = 0xFF; // Reset dirty flag for next spawn

		static FName NAME_HitFlashColor(TEXT("HitFlashColor"));
		static FName NAME_FullBodyFlashPct(TEXT("FullBodyFlashPct"));
		static FName NAME_SpawnProtectionPct(TEXT("SpawnProtectionPct"));

		for (UMaterialInstanceDynamic* MI : BodyMIs)
		{
			if (MI)
			{
				MI->SetVectorParameterValue(NAME_HitFlashColor, FLinearColor(0.f, 0.f, 0.f, 0.f));
				MI->SetScalarParameterValue(NAME_FullBodyFlashPct, 0.0f);
				MI->SetScalarParameterValue(NAME_SpawnProtectionPct, 0.0f);
			}
		}
	}
}


void ATeamArenaCharacter::BecomeViewTarget(APlayerController* PC)
{
	Super::BecomeViewTarget(PC);


	AUTWeap_LinkGun* LinkGun = Cast<AUTWeap_LinkGun>(Weapon);
	if (LinkGun)
	{
		LinkGun->OverheatFactor = 0.0f;
	}

	// Clear any stuck audio on the pawn
	SetAmbientSound(nullptr);
}


FRotator ATeamArenaCharacter::GetViewRotation() const
{
	FRotator BaseRotation = Super::GetViewRotation();

	// Only smooth for remote characters (not locally controlled, not on dedicated server)
	if (IsLocallyControlled() || GetNetMode() == NM_DedicatedServer)
	{
		return BaseRotation;
	}

	// Initialize on first call to prevent interpolating from (0,0,0)
	if (!bSmoothedViewRotationInitialized)
	{
		SmoothedViewRotation = BaseRotation;
		bSmoothedViewRotationInitialized = true;
		return BaseRotation;
	}

	// Smooth the rotation for spectators viewing this character.
	// Engine default SmoothTargetViewRotationSpeed is 20.0 which at 480fps
	// catches up in ~2 frames, making 100Hz network rotation updates look jerky.
	// 10.0 spreads each step over ~5 frames, eliminating visible stepping.
	float DeltaTime = GetWorld()->GetDeltaSeconds();
	SmoothedViewRotation = FMath::RInterpTo(SmoothedViewRotation, BaseRotation, DeltaTime, 10.0f);

	return SmoothedViewRotation;
}


bool ATeamArenaCharacter::Died(AController* EventInstigator, const FDamageEvent& DamageEvent, AActor* DamageCauser)
{
	// Force-clear ALL ambient sounds on death to prevent looping
	// (e.g. link gun overheat sound continuing after death)
	SetAmbientSound(nullptr, true);
	SetStatusAmbientSound(nullptr, true);
	SetLocalAmbientSound(nullptr, true);

	if (AmbientSoundComp && AmbientSoundComp->IsPlaying())
	{
		AmbientSoundComp->Stop();
	}

	return Super::Died(EventInstigator, DamageEvent, DamageCauser);
}

bool ATeamArenaCharacter::ModifyDamageTaken_Implementation(
	int32& AppliedDamage, int32& Damage, FVector& Momentum,
	AUTInventory*& HitArmor, const FHitResult& HitInfo,
	AController* EventInstigator, AActor* DamageCauser,
	TSubclassOf<UDamageType> DamageType)
{
	// Inventory damage events (same as stock UTCharacter)
	for (TInventoryIterator<> It(this); It; ++It)
	{
		if (It->bCallDamageEvents)
		{
			It->ModifyDamageTaken(Damage, Momentum, HitArmor,
				EventInstigator, HitInfo, DamageCauser, DamageType);
		}
	}

	// ArmorPlus absorption
	int32 CurrentArmor = GetArmorAmount();
	if (Damage > 0 && CurrentArmor > 0)
	{
		const UDamageType* const DamageTypeCDO = DamageType
			? DamageType->GetDefaultObject<UDamageType>()
			: GetDefault<UDamageType>();
		const UUTDamageType* const UTDamageTypeCDO = Cast<UUTDamageType>(DamageTypeCDO);

		if (UTDamageTypeCDO == nullptr || UTDamageTypeCDO->bBlockedByArmor)
		{
			HitArmor = ArmorType;
			int32 AbsorbedDamage = 0;
			int32 InitialDamage = Damage;

			// Sync BeltArmorRemaining when current armor type is belt (CDO ArmorAmount > 100)
			if (ArmorType != nullptr)
			{
				const AUTArmor* ArmorCDO = ArmorType->GetClass()->GetDefaultObject<AUTArmor>();
				if (ArmorCDO != nullptr && ArmorCDO->ArmorAmount > 100)
				{
					BeltArmorRemaining = FMath::Max(BeltArmorRemaining, CurrentArmor);
				}
			}

			// Clamp belt remaining to actual armor pool
			int32 BeltPortion = FMath::Min(BeltArmorRemaining, CurrentArmor);
			int32 RegularPortion = CurrentArmor - BeltPortion;

			// Phase 1: Belt armor absorbs at 100%
			if (BeltPortion > 0 && Damage > 0)
			{
				int32 BeltAbsorbed = FMath::Min(Damage, BeltPortion);
				Damage -= BeltAbsorbed;
				AbsorbedDamage += BeltAbsorbed;
				BeltArmorRemaining -= BeltAbsorbed;
			}

			// Phase 2: Regular armor absorbs at 66.67%
			if (RegularPortion > 0 && Damage > 0)
			{
				int32 RegularAbsorbed = FMath::Min(RegularPortion,
					FMath::Max<int32>(1, Damage * 0.6667f));
				Damage -= RegularAbsorbed;
				AbsorbedDamage += RegularAbsorbed;
			}

			// Momentum absorption (same as stock)
			if (ArmorType != nullptr && ArmorType->bAbsorbMomentum)
			{
				Momentum *= 1.0f - float(AbsorbedDamage) / float(InitialDamage);
			}
			RemoveArmor(AbsorbedDamage);
		}
	}
	return false;
}

// ── Ping-Compensated Spawn ──────────────────────────────────────────

void ATeamArenaCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ATeamArenaCharacter, bPingCompensatedSpawnPending);
}

bool ATeamArenaCharacter::ServerConfirmSpawnReady_Validate()
{
	return true;
}

void ATeamArenaCharacter::ServerConfirmSpawnReady_Implementation()
{
	if (!bPingCompensatedSpawnPending)
	{
		return;
	}

	bPingCompensatedSpawnPending = false;
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
}

// ── Own footstep volume (iCTF) ─────────────────────────────────────────────
// Scale THIS local player's OWN footstep volume by the F5 "Own Footstep Volume" setting. Only the local
// human's own pawn, only in iCTF, only when the setting is below stock (1.0); everything else falls through
// to the stock AUTCharacter::PlayFootstep. UTPlaySound has no volume argument, so the non-stock case is
// played via SpawnSoundAttached with a VolumeMultiplier.
void ATeamArenaCharacter::PlayFootstep(uint8 FootNum, bool bFirstPerson)
{
	if (GetNetMode() != NM_DedicatedServer && IsLocalPlayerPawn())
	{
		// Read the 0..1 setting once (mid-match F5 changes apply next life). Default 1.0 = stock.
		if (!bOwnFootstepVolumeRead)
		{
			bOwnFootstepVolumeRead = true;
			FString Val;
			const FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
			if (GConfig && GConfig->GetString(TEXT("NetcodePlus"), TEXT("OwnFootstepVolume"), Val, ConfigPath))
			{
				OwnFootstepVolumeScale = FMath::Clamp(FCString::Atof(*Val), 0.f, 1.f);
			}
		}

		// Only do the iCTF lookup / custom play when the feature is actually active (vol < stock), so
		// default users pay nothing. ACTFStatsReplicator exists only in NCPlusCTF (instagib); searched
		// lazily while null so it still binds if the first footstep precedes its replication.
		if (OwnFootstepVolumeScale < 1.f)
		{
			// Resolve the iCTF gate once: search until the replicator is found, or give up ~8s after spawn
			// so non-iCTF modes (ElimPlus etc., which have no ACTFStatsReplicator) don't iterate every
			// footstep forever. The window also covers a first footstep that precedes the replicator's arrival.
			if (!bIctfFootstepResolved)
			{
				for (TActorIterator<ACTFStatsReplicator> It(GetWorld()); It; ++It)
				{
					CachedCTFRep = *It;
					break;
				}
				if (CachedCTFRep.IsValid() || (GetWorld()->GetTimeSeconds() - CreationTime) > 8.f)
				{
					bIctfFootstepResolved = true;
				}
			}
			if (CachedCTFRep.IsValid() && CachedCTFRep->bIsInstagibMatch)
			{
				// Mirror stock's double-footstep filter: drop the 3rd-person step while in first-person view
				// (otherwise both the 1P and 3P notifies would play the own footstep twice).
				AUTPlayerController* UTPC = Cast<AUTPlayerController>(Controller);
				if (UTPC && !bFirstPerson && !UTPC->IsBehindView())
				{
					return;
				}
				PlayOwnFootstepScaled(FootNum);
				return;
			}
		}
	}

	Super::PlayFootstep(FootNum, bFirstPerson);
}

void ATeamArenaCharacter::PlayOwnFootstepScaled(uint8 FootNum)
{
	// Mirror AUTCharacter::PlayFootstep's gating, but play the local player's own footstep at
	// OwnFootstepVolumeScale via SpawnSound2D — non-spatialized, matching the character of the stock own
	// footstep (UTPlaySound has no volume arg). The fork stubs GetFootstepSoundForSurfaceType to always
	// return null, so footsteps always use FootstepSound (or WaterFootstepSound) — there's no per-surface
	// own variant to reproduce. SAT_Footstep amplification is bypassed, so the volume is a direct multiplier
	// of the asset (a deliberate reduction; there's a small loudness step vs the stock 1.0 path). Cadence
	// (LastFoot/LastFootstepTime) is preserved so anim-timed steps stay correct.
	if ((GetWorld()->TimeSeconds - LastFootstepTime < 0.1f) || bFeigningDeath || IsDead() || bIsCrouched)
	{
		return;
	}

	USoundBase* FootstepSoundToPlay = FeetAreInWater() ? WaterFootstepSound : FootstepSound;

	// Volume 0 -> play nothing (silent own footsteps), which is the whole point at 0.
	if (FootstepSoundToPlay && OwnFootstepVolumeScale > 0.f)
	{
		UGameplayStatics::SpawnSound2D(this, FootstepSoundToPlay, OwnFootstepVolumeScale);
	}

	LastFoot = FootNum;
	LastFootstepTime = GetWorld()->TimeSeconds;
}