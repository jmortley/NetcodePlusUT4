// TeamArenaCharacter.cpp
#include "TeamArenaCharacter.h"
#include "UTCharacterMovement.h"
#include "TeamArenaCharacterMovement.h"
#include "UTWeaponAttachment.h"
#include "UTWeaponFix.h"
#include "UTWeaponSkin.h"
#include "GameFramework/PlayerController.h"
#include "UTWorldSettings.h"
#include "UTPlusSniper.h"
#include "UTPlusShockRifle.h"
#include "UTGameState.h"
#include "UTGameMode.h"
#include "UTCTFBaseGame.h"
#include "UTWeap_LinkGun.h"
#include "UTWeap_LightningRifle.h"
#include "UTArmor.h"
#include "UTDamageType.h"
#include "Net/UnrealNetwork.h"
#include "UTPlayerController.h"
#include "UTPlayerState.h"            // GetSelectedCharacter (DarkenBodies skeleton fallback)
#include "UTCharacterContent.h"
#include "Engine/SkeletalMesh.h"      // identify the two curated *_bright meshes without class-name guessing
#include "Materials/Material.h"       // default material for the locally-created outline duplicate
#include "Materials/MaterialInstanceDynamic.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "NCPlusForceModels.h"
#include "NCPlusPerformanceSettings.h"
#include "NCPlusICTFAudioSettings.h"
#include "EngineUtils.h"             // TActorIterator (refresh every other pawn on local team change)
#include "TimerManager.h"           // DarkenBodies delayed corpse hide
#include "UTCarriedObject.h"        // HideDeadBody: don't blank a carried flag still parented to the corpse
#include "Kismet/GameplayStatics.h" // SpawnSound2D (own-footstep volume)
#include "CTFStatsReplicator.h"     // iCTF gate (bIsInstagibMatch) for own-footstep volume
#include "UTMutator.h"              // iCTF WARMUP gate: find the replicated MutInstagibNCP mutator
#include "ClutchRoundState.h"       // Clutch defender footstep role/phase lookup
#include "Engine/DemoNetDriver.h"   // deferred-outline warning: tag killcam/instant-replay worlds

// Shipping clients report every ensure with a synchronous game-thread minidump, which hitches the
// match and hard-crashes under AV hooks (328 Blitz/iCTF cap crash). Recoverable outline states must
// log through this category instead of ensuring.
DEFINE_LOG_CATEGORY_STATIC(LogTeamArenaOutline, Log, All);

static TAutoConsoleVariable<int32> CVarEnableProjectilePrediction(
	TEXT("ut.EnableProjectilePrediction"),
	1, // Default: 1 (Enabled by default)
	TEXT("If 1, enables one-way latency visual prediction for non hitscan weapons.\n")
	TEXT("Players can set to 0 to opt-out (force server positions)."),
	ECVF_Default); // Saves to user config

// Server-side balance rule, ON by default since 328 (announced in the 328 patch
// notes; set 0 to restore stock). The decision and the charge state are
// server-only — nothing about this cvar needs the client.
static TAutoConsoleVariable<int32> CVarHelmetBlocksHeadshot(
	TEXT("ncp.HelmetBlocksHeadshot"),
	1,
	TEXT("1 = an Armor_Small (helmet) pickup blocks exactly one headshot, UT3-style: both players hear the ding, BlockedHeadshotDamage applies, and the charge is consumed — re-armed only by another helmet pickup. 0 = stock (headshots are never blocked)."));

namespace
{
	constexpr int32 ArmorPlusMaxTotal = 150;
	constexpr int32 ArmorPlusSoftLimit = 100;
	constexpr float ClutchDefenderFootstepVolume = 0.10f;

	// Stock belt is the only armor grant above 100. Use the behavior-proven amount
	// instead of relying on the descriptive ArmorType tag being populated in the BP.
	bool IsArmorPlusBelt(const AUTArmor* Armor)
	{
		return Armor != nullptr && Armor->ArmorAmount > ArmorPlusSoftLimit;
	}

	// The live "helmet slot" pickup. Class-path match, walking Super for BP
	// children — NOT an ArmorAmount heuristic, because starting/bespoke armour
	// classes that happen to be small must not grant head protection.
	// Armor_Helmet is the deprecated thin wrapper around Armor_Small (not
	// reliably cooked, see NCLeagueDuelScoreboard); matched directly and via
	// inheritance in case a map still places it.
	bool IsHelmetArmor(const AUTArmor* Armor)
	{
		if (Armor == nullptr)
		{
			return false;
		}
		for (const UClass* C = Armor->GetClass(); C != nullptr; C = C->GetSuperClass())
		{
			const FString Path = C->GetPathName();
			if (Path == TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_Small.Armor_Small_C") ||
				Path == TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_Helmet.Armor_Helmet_C"))
			{
				return true;
			}
		}
		return false;
	}
}

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
static TAutoConsoleVariable<int32> CVarHideArmorShield(
	TEXT("ncp.HideArmorShield"),
	0,
	TEXT("ForceModels armour-shield fallback: 1 = HIDE the shield-belt overlay instead of recolouring it (for community models whose shield material bakes the gold and ignores the Color recolour). Default 0 = recolour to the team skin colour. Runtime-toggleable; client-side."),
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
	if (GetNetMode() != NM_DedicatedServer)
	{
		// Super may rebuild the armour overlay through our UpdateArmorOverlay override.
		bForcedArmourOverlayDirty = true;
	}
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

void ATeamArenaCharacter::ApplyCharacterData(TSubclassOf<AUTCharacterContent> Data)
{
	if (GetNetMode() == NM_DedicatedServer)
	{
		Super::ApplyCharacterData(Data);
		return;
	}

	// Stock ApplyCharacterData rebuilds CustomDepthMesh while GetMesh() is still inside an
	// FComponentReregisterContext. If X-ray is active, that replacement can register without
	// its CharacterMesh0 parent and remain at world origin. Remember whether stock would have
	// refreshed the outline, suppress that premature rebuild, and recreate it next Tick after
	// every ApplyCharacterData/reregister stack has unwound.
	const bool bNeedsDeferredOutlineUpdate = (CustomDepthMesh != nullptr) || IsOutlined();
	{
		TGuardValue<bool> SuppressOutline(bForceNoOutline, true);
		Super::ApplyCharacterData(Data);
	}
	if (bNeedsDeferredOutlineUpdate && !bForceNoOutline)
	{
		bDeferredOutlineUpdatePending = true;
	}
}

void ATeamArenaCharacter::UpdateOutline()
{
	// TacCom and replicated outline state can request another refresh after ApplyCharacterData
	// but before this pawn's next Tick. Keep those requests coalesced so none can recreate the
	// body duplicate during the unsafe window; the flush reads the latest outline state.
	if (!bDeferredOutlineUpdatePending)
	{
		Super::UpdateOutline();
	}
}

void ATeamArenaCharacter::FlushDeferredOutlineUpdate()
{
	bDeferredOutlineUpdatePending = false;

	USkeletalMeshComponent* const BodyMesh = GetMesh();
	if (GetNetMode() == NM_DedicatedServer || bForceNoOutline || BodyMesh == nullptr)
	{
		return;
	}

	// A character tick can coincide with component teardown/reregistration. Keep the outline
	// absent and try again on the next character tick instead of registering against that state.
	if (!BodyMesh->IsRegistered())
	{
		bDeferredOutlineUpdatePending = true;
		return;
	}

	// Create and initialize the duplicate without registering it. This lets us verify/repair the
	// critical parent invariant before stock gets the opportunity to call RegisterComponent().
	if (IsOutlined() && CustomDepthMesh == nullptr)
	{
		CustomDepthMesh = DuplicateObject<USkeletalMeshComponent>(BodyMesh, this);
		if (CustomDepthMesh == nullptr)
		{
			UE_LOG(LogTeamArenaOutline, Warning,
				TEXT("Failed to create deferred outline for %s; refusing to register it."),
				*GetName());
			return;
		}

		// Mirror UT's CreateCustomDepthOutlineMesh initialization locally. That helper is not
		// exported from the UnrealTournament module, so a plugin cannot link against it safely.
		CustomDepthMesh->DetachFromComponent(FDetachmentTransformRules::KeepRelativeTransform);
		USkeletalMeshComponent* const DefaultMesh =
			CustomDepthMesh->GetClass()->GetDefaultObject<USkeletalMeshComponent>();
		CustomDepthMesh->PrimaryComponentTick = DefaultMesh->PrimaryComponentTick;
		CustomDepthMesh->PostPhysicsComponentTick = DefaultMesh->PostPhysicsComponentTick;
		CustomDepthMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		CustomDepthMesh->SetSimulatePhysics(false);
		CustomDepthMesh->SetCastShadow(false);
		CustomDepthMesh->SetMasterPoseComponent(BodyMesh);
		for (int32 MaterialIndex = 0; MaterialIndex < CustomDepthMesh->GetNumMaterials(); ++MaterialIndex)
		{
			CustomDepthMesh->SetMaterial(MaterialIndex, UMaterial::GetDefaultMaterial(MD_Surface));
		}
		CustomDepthMesh->BoundsScale = 15000.f;
		CustomDepthMesh->bVisible = true;
		CustomDepthMesh->bHiddenInGame = false;
		CustomDepthMesh->bRenderInMainPass = false;
		CustomDepthMesh->bRenderCustomDepth = true;
		CustomDepthMesh->AttachToComponent(BodyMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		CustomDepthMesh->RelativeLocation = FVector::ZeroVector;
		CustomDepthMesh->RelativeRotation = FRotator::ZeroRotator;
		CustomDepthMesh->RelativeScale3D = FVector(1.0f);
	}

	if (CustomDepthMesh != nullptr && CustomDepthMesh->GetAttachParent() != BodyMesh)
	{
		// Routine in killcam/instant-replay worlds: demo seeks destroy/respawn actors while OnRep
		// bursts (bServerOutline is ReplicatedUsing=UpdateOutline) land mid-registration. Log only —
		// this state is repaired below, and an ensure here is what crashed 328 clients at caps.
		UE_LOG(LogTeamArenaOutline, Warning,
			TEXT("Deferred outline for %s was detached before registration; repairing %s attachment (outlined=%d registered=%d demo=%d)."),
			*GetName(), *BodyMesh->GetName(),
			IsOutlined() ? 1 : 0,
			CustomDepthMesh->IsRegistered() ? 1 : 0,
			(GetWorld() != nullptr && GetWorld()->DemoNetDriver != nullptr) ? 1 : 0);

		// A different outline request may have registered the component between ApplyCharacterData
		// and this tick. Retire that render state before repairing so no detached primitive survives.
		if (CustomDepthMesh->IsRegistered())
		{
			CustomDepthMesh->UnregisterComponent();
		}
		CustomDepthMesh->SetMasterPoseComponent(BodyMesh);
		CustomDepthMesh->AttachToComponent(BodyMesh, FAttachmentTransformRules::SnapToTargetNotIncludingScale);
		CustomDepthMesh->RelativeLocation = FVector::ZeroVector;
		CustomDepthMesh->RelativeRotation = FRotator::ZeroRotator;
		CustomDepthMesh->RelativeScale3D = FVector(1.0f);
	}

	if (CustomDepthMesh != nullptr && CustomDepthMesh->GetAttachParent() != BodyMesh)
	{
		UE_LOG(LogTeamArenaOutline, Warning,
			TEXT("Deferred outline for %s could not attach to %s; discarding it."),
			*GetName(), *BodyMesh->GetName());
		CustomDepthMesh->DestroyComponent();
		CustomDepthMesh = nullptr;
		return;
	}

	Super::UpdateOutline();
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
	if (bForcedArmourOverlayDirty)
	{
		RefreshForcedArmourOverlay();
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
	// MY OWN pawn → COLOUR-ONLY: keep your real model, run the team-colour tint only (gated below) so your
	// own body matches the recoloured teammates and the line-up / post-game line-up. (NOT IsLocallyControlled
	// — that's true for every pawn offline; see IsLocalPlayerPawn.)
	const bool bColourOnly = IsLocalPlayerPawn();

	UWorld* const World = GetWorld();

	// ── Resolve desired state: the model class + colour to force, or "none" = leave natural. ──
	TSubclassOf<AUTCharacterContent> Content = nullptr;
	FLinearColor Colour = FLinearColor::White;
	float        GlowIntensity = 0.f;          // subtle-highlight emissive strength, from Brightness
	bool bWantForce = false;
	bool bWantTint  = false;

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
			const bool bModelOK = Content && NCPlusForceModels::IsModelAllowed(Content);
			// A side is active when it forces a model OR opts into tint-only (the F5
			// "Tint skin" checkbox / [ForceModels.Model.<side>] Tint). The colour was
			// historically gated on the model pick, which read as a bug: the chosen
			// colour showed on the HUD (ungated) but never on bodies until a model
			// was also selected.
			if (bModelOK || Side.bTint)
			{
				// "Glow" (1 = normal .. 5 = 5x) brightens the model toward the flat, unlit HUD swatch.
				// The lever that actually brightens the BODY is the ALBEDO (the team-colour params below —
				// proven by the recolour working); driving Emissive Max did NOTHING because these body
				// materials have no emissive source (only the eyes do, which is why only they glowed). So
				// Glow OVERBRIGHTS the recolour colour. The emissive scalars are still fed (harmless; helps
				// any model that does have a body emissive channel).
				// Hard cap at 3.5 (was 5): a blinding-bright forced model is a visibility
				// advantage, so clamp here at the authoritative apply point — this covers the
				// F5 slider, a hand-edited Mod.ini, and any stale stored 5.0 alike.
				const float Glow = FMath::Clamp(Side.Brightness, 1.f, 3.5f);
				Colour        = NCPlusForceModels::GetSkinColour(Side) * Glow;
				Colour.A      = 1.f;                       // operator* scales alpha too; keep it opaque
				// Emissive is DECOUPLED from the albedo overbright and capped harder. The albedo (above)
				// keeps scaling to x3.5 so models stay vivid/readable, but the self-lit emissive — the
				// "radioactive" bloom that ignores scene lighting — is clamped to EmissiveGlowCap so a
				// high Glow can't turn a lineup into neon. Emissive still ramps 0..cap for Glow 1..~2.4,
				// then holds flat while the colour keeps brightening.
				static const float EmissiveGlowCap = 2.5f;   // lower = calmer bloom
				GlowIntensity = FMath::Min((Glow - 1.f) * 1.25f, EmissiveGlowCap);
				bWantForce    = bModelOK;
				bWantTint     = true;
			}
			if (!bModelOK)
			{
				Content = nullptr;   // tint-only: never a mesh-swap target (also keeps the dirty latch honest)
			}
		}
	}

	// ── Natural: feature off, FFA, or friendly under Enemy-Only → this pawn keeps its real model. ──
	if (!bWantTint)
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

	// Forced MESH dropped but tint kept (model unpicked in F5, or the bucket flipped
	// to a tint-only side on a team change): on the refresh path the base team-change
	// has NOT run, so restore the real model first — mirroring the un-force branch
	// above — and let the tint land on the pawn's own rebuilt BodyMIs. On the
	// NotifyTeamChanged/flush path the base already reverted the mesh this frame.
	// Not for the own pawn: bColourOnly never swapped its mesh, so there is nothing
	// to restore and the base re-run would be a needless full mesh rebuild.
	if (!bWantForce && !bColourOnly && bForcedModelApplied && LastForcedContent != nullptr)
	{
		if (!bForceReapply)
		{
			bApplyingForcedModel = true;
			bAllowCharacterDataOverride = true;
			AUTCharacter::NotifyTeamChanged();        // ApplyCharacterData(real) + TeamSelect + weapon/hat
			bApplyingForcedModel = false;
		}
		UpdateCosmeticStrip(false);   // no longer reskinned -> cosmetics come back
	}

	bApplyingForcedModel = true;

	// Force the mesh via UT's own swap (rebuilds BodyMIs). Flag must be set BEFORE the call —
	// stock ApplyCharacterData early-returns unless bAllowCharacterDataOverride is true.
	// Own pawn (bColourOnly) and tint-only sides (no model picked): skip the mesh swap —
	// keep the real model and its existing BodyMIs, tint only.
	if (bWantForce && !bColourOnly)
	{
		bAllowCharacterDataOverride = true;
		ApplyCharacterData(Content);
	}

	static const FName NAME_TeamSelect(TEXT("TeamSelect"));
	static const FName NAME_TeamBlendMax(TEXT("Team Color Blend Max"));
	static const FName NAME_EmissiveMax(TEXT("Emissive Max"));
	static const FName NAME_EmissionPower(TEXT("Emission Power"));
	static const FName NAME_GenghisBrightMesh(TEXT("ghengis_3p_bright"));
	static const FName NAME_LiandriRobotBrightMesh(TEXT("robot_3p_bright"));
	const TArray<FName>& Params = NCPlusForceModels::TeamColourParamNames();
	// These two curated content classes use dedicated *_bright meshes whose static material instances
	// deliberately author HDR team-colour values (2.5). The generic recolour pass used to replace those
	// values with the raw F5 colour and silently throw away the very compensation the bright variants
	// were created to provide. Restrict preservation to the two known variants: an arbitrary community
	// material with an HDR colour parameter must not acquire a visibility advantage automatically.
	const USkeletalMesh* const ActiveBodyMesh = GetMesh() ? GetMesh()->SkeletalMesh : nullptr;
	const bool bPreserveBrightVariantTint = ActiveBodyMesh
		&& (ActiveBodyMesh->GetFName() == NAME_GenghisBrightMesh
			|| ActiveBodyMesh->GetFName() == NAME_LiandriRobotBrightMesh);

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
	// Outline mode ("Outline" flag): keep the forced mesh but render the body NEUTRAL (no team colour) —
	// the team read comes from the LOS outline (NCPlusForceModels::OutlinePlayers) instead, so both teams
	// look the same and only the outline distinguishes them (not red/blue, not "super green").
	// OutlineModeActive (not the raw flag) so a listen host, where the outline pass is disabled, keeps
	// the normal tint instead of neutral-bodies-with-no-outline.
	const bool bOutlineMode = NCPlusForceModels::OutlineModeActive(GetWorld());

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

		if (bOutlineMode)
		{
			// Outline mode: NEUTRAL body — NoTeam path (255) with NO team-colour blend, so both teams look
			// the same and the LOS outline is the SOLE team indicator (not red/blue). TeamBlendMax 0 keeps
			// the model's base albedo un-tinted. (Exact neutral look is model-dependent.)
			// Tint-only pawns whose REAL model is param-less/baked are left untouched — same "force no
			// skin on a model the user didn't force" carve-out as below; the outline still renders.
			if (bWantForce || bRecolour)
			{
				MID->SetScalarParameterValue(NAME_TeamSelect, 255.f);
				MID->SetScalarParameterValue(NAME_TeamBlendMax, 0.f);
			}
			continue;
		}

		if (!bRecolour)
		{
			if (bWantForce)
			{
				// Non-recolourable model: route to its baked red/blue skin rather than the futile NoTeam
				// recolour (which would leave it a flat default). The baked textures carry the team look.
				MID->SetScalarParameterValue(NAME_TeamSelect, BakedTeamSelect);
			}
			// Tint-only on a param-less/baked NATURAL model: leave it untouched — re-routing
			// a real player's baked team skin off the colour heuristic (R vs B) could dress a
			// blue player in the red baked skin. The user forced no model, so force no skin.
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

		FLinearColor MaterialColour = Colour;
		if (bPreserveBrightVariantTint && Src)
		{
			// Read the immutable parent MI, not the MID we modify below, so NotifyTeamChanged/reapply can
			// never compound the boost. Both shipped bright variants resolve to a 2.5 authored peak here.
			float AuthoredPeak = 1.f;
			for (const FName& P : Params)
			{
				FLinearColor AuthoredColour;
				if (Src->GetVectorParameterValue(P, AuthoredColour))
				{
					AuthoredPeak = FMath::Max(AuthoredPeak,
						FMath::Max3(AuthoredColour.R, AuthoredColour.G, AuthoredColour.B));
				}
			}

			// The regular models retain the global 3.5 peak cap. These intrinsically dark variants may
			// preserve their authored boost, but stop at the old global maximum of 5 rather than reaching
			// 8.75 when the F5 Glow slider is also at its current 3.5 maximum.
			static const float BrightVariantMaxPeak = 5.f;
			const float RequestedPeak = FMath::Max3(Colour.R, Colour.G, Colour.B);
			const float MaxSafeBoost = (RequestedPeak > KINDA_SMALL_NUMBER)
				? BrightVariantMaxPeak / RequestedPeak
				: AuthoredPeak;
			const float AppliedBoost = FMath::Clamp(AuthoredPeak, 1.f, FMath::Max(1.f, MaxSafeBoost));
			MaterialColour *= AppliedBoost;
			MaterialColour.A = 1.f;
		}
		for (const FName& P : Params)
		{
			MID->SetVectorParameterValue(P, MaterialColour);
		}
	}

	LastForcedContent   = Content.Get();
	LastForcedColour    = Colour;
	bForcedModelApplied = true;

	// Cosmetic strip (the "Cosmetics" flag, on = remove): drop + suppress hats/eyewear on this reskinned
	// pawn. Set BEFORE OnRep_PlayerState's later SetCosmeticsFromPlayerState so the setter overrides catch
	// the re-add. (NotifyTeamChanged runs first at OnRep, this gate second.)
	// Only when actually reskinned — the own pawn and tint-only sides keep the real
	// character, so they keep its hat/eyewear too.
	if (bWantForce && !bColourOnly)
	{
		UpdateCosmeticStrip(NCPlusForceModels::Get().bCosmetics);
	}

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
	ClearLocalOutlineRenderState();
	SpawnSkeletonDissolve();
}

void ATeamArenaCharacter::Destroyed()
{
	// AUTLineUpHelper destroys its prematch preview pawns while they are still alive. That
	// bypasses PlayDying(), and stock AUTCharacter::Destroyed() destroys WeaponAttachment
	// without first unregistering either duplicated CustomDepth mesh. Retire both while all
	// pointers are still valid, before handing the pawn to stock teardown.
	ClearLocalOutlineRenderState();
	Super::Destroyed();
}

void ATeamArenaCharacter::ClearLocalOutlineRenderState()
{
	bDeferredOutlineUpdatePending = false;

	if (GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	// A dying/destroying pawn must never be re-outlined by a later spectator TacCom pass.
	// Stock UpdateOutline unregisters both the body duplicate and WeaponAttachment duplicate
	// while the attachment pointer is still valid.
	bForceNoOutline = true;
	SetOutlineLocal(false);

	// Stock only unregisters the character duplicate; destroy it so no registered render
	// primitive can outlive an alive-destroyed lineup pawn through GC/teardown ordering.
	if (CustomDepthMesh != nullptr)
	{
		CustomDepthMesh->DestroyComponent();
		CustomDepthMesh = nullptr;
	}
}

void ATeamArenaCharacter::SetOutlineLocal(bool bNowOutlined, bool bWhenUnoccluded)
{
	// UT renders outlines with a separate CustomDepth skeletal-mesh duplicate. SetVisibility(false)
	// on the normal body does not make that duplicate ineligible, and true-spectator TacCom calls
	// SetOutlineLocal(true) every tick. That can expose a cleaned-up corpse as an otherwise invisible
	// red/blue pawn. Explicit component visibility is the narrow gate we want: ping-compensated spawn
	// uses SetActorHiddenInGame (not SetVisibility), and living visible pawns retain stock X-ray.
	const USkeletalMeshComponent* BodyMesh = GetMesh();
	if (bNowOutlined && BodyMesh != nullptr && !BodyMesh->IsVisible())
	{
		bNowOutlined = false;
	}

	Super::SetOutlineLocal(bNowOutlined, bWhenUnoccluded);
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

	FString Val;
	const FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");

	// F5 "Show Ragdoll" ([InstagibCTF] bShowRagdoll). INTENDED consumer = the BP instagib DAMAGE TYPE
	// (it reads these keys and drives ragdoll time for instagib kills) — which by construction only
	// ever fires in iCTF, so in every other mode the setting did nothing and the only hide path was
	// ForceModels' DarkenBodies (community: "uncheck force models and you can see dead bodys" /
	// "ragdoll setting overridden by force models"). This C++ read is the MODE-AGNOSTIC stand-in:
	// when the key exists (any F5 save writes it) it is AUTHORITATIVE over the Darken fade in both
	// directions — unticked hides corpses in every mode with FM off, ticked shows them even with
	// Darken on. Hide-only, so it composes with the BP in iCTF (ShowRagdoll-on defers to the
	// RagdollTime safety net below = the BP's own cleanup time). Key ABSENT (never saved F5 — e.g. a
	// dc-TeamSkins migrant) -> Darken keeps its old dc-parity hide role.
	bool bShowRagdoll = true;
	bool bShowRagdollExplicit = false;
	if (GConfig && GConfig->GetString(TEXT("InstagibCTF"), TEXT("bShowRagdoll"), Val, ConfigPath))
	{
		bShowRagdoll = Val.Equals(TEXT("True"), ESearchCase::IgnoreCase);
		bShowRagdollExplicit = true;
	}

	// iCTF detection: RagdollTime is an iCTF setting (the BP CleanUpRagdoll only runs for the instagib damage
	// type). ACTFStatsReplicator is present only in NCPlusCTF instagib; absent in ElimPlus etc.
	bool bIsInstagib = false;
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<ACTFStatsReplicator> It(World); It; ++It) { bIsInstagib = It->bIsInstagibMatch; break; }
	}

	// Fade-hide when: Show Ragdoll explicitly unticked (any mode), or — with the key never written —
	// the legacy DarkenBodies fade. iCTF keeps its ragdoll-cleanup backup regardless. Outside all of
	// that, leave corpses to the stock/engine cleanup — no change to ElimPlus and friends.
	const bool bFadeHide = bShowRagdollExplicit ? !bShowRagdoll : bDarken;
	if (!bFadeHide && !bIsInstagib) { return; }

	float RagdollTime = 3.0f;
	if (GConfig && GConfig->GetString(TEXT("InstagibCTF"), TEXT("RagdollTime"), Val, ConfigPath))
	{
		RagdollTime = FCString::Atof(*Val);
	}
	// The menu stores iCTF "Ragdoll Time = 0" (remove instantly) as 0.01 so the BP SetTimer fires.
	// That sentinel must not leak into the OTHER modes' fade cap — an iCTF "instant" choice would
	// make bodies vanish frame-one in ElimPlus/Wipeout. Outside iCTF, treat it as the normal fade.
	if (!bIsInstagib && RagdollTime <= 0.011f)
	{
		RagdollTime = 1.0f;
	}

	// The fade paths (Show-Ragdoll-off / legacy DarkenBodies) hide early (~1s death-effects fade, but never
	// longer than the ragdoll lives); otherwise (iCTF, no fade) hide exactly at the ragdoll lifespan so the
	// body still vanishes when the BP CleanUpRagdoll doesn't. Floor 0.01s (SetTimer never schedules
	// rate<=0). Timer is bound to this actor, so it auto-clears if the corpse is destroyed/cleaned up
	// sooner (gib, respawn, DeathCleanupTimer).
	const float HideDelay = FMath::Max(0.01f, bFadeHide ? FMath::Min(1.0f, RagdollTime) : RagdollTime);
	FTimerHandle TempHandle;
	GetWorldTimerManager().SetTimer(TempHandle, this, &ATeamArenaCharacter::HideDeadBody, HideDelay, false);
}

void ATeamArenaCharacter::HideDeadBody()
{
	USkeletalMeshComponent* BodyMesh = GetMesh();
	if (!BodyMesh) { return; }

	BodyMesh->SetVisibility(false, /*bPropagateToChildren=*/true);

	// Drop any already-registered CustomDepth mesh immediately. Future TacCom re-assertions are
	// rejected by SetOutlineLocal() while the body remains explicitly invisible.
	SetOutlineLocal(false);

	// Don't let the corpse-hide blank a carried FLAG. An instagib flag carrier who dies drops the flag,
	// but if that detach hasn't processed yet the flag's root is still parented to this body mesh, and the
	// propagate above traverses the ENTIRE attach tree (USceneComponent::SetVisibility, SceneComponent.cpp)
	// — across the actor boundary into the flag — setting the flag mesh's bVisible=false. That survives the
	// detach, so the flag is invisible when dropped and after it returns home (the "flag goes invisible
	// after a cap/return"; near-certain with a low [InstagibCTF] RagdollTime firing this inside the still-
	// attached window). Re-show any attached carried object so the corpse-hide can never strand a flag.
	// Cosmetics (owned by this character) stay hidden — only foreign carried objects are re-shown.
	TArray<USceneComponent*> Descendants;
	BodyMesh->GetChildrenComponents(/*bIncludeAllDescendants=*/true, Descendants);
	for (USceneComponent* Child : Descendants)
	{
		if (Child && Child->GetOwner() && Child->GetOwner()->IsA(AUTCarriedObject::StaticClass()))
		{
			Child->SetVisibility(true, /*bPropagateToChildren=*/false);
		}
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
		// The local viewer's team can move this pawn between friendly/enemy colour buckets without
		// any armour replication on the pawn itself. Re-run stock overlay setup, then our tint once.
		Other->UpdateArmorOverlay();
	}
}

void ATeamArenaCharacter::UpdateArmorOverlay()
{
	Super::UpdateArmorOverlay();   // sets up the armour overlay (+ the stock hardcoded yellow "Color")
	bForcedArmourOverlayDirty = true;
	RefreshForcedArmourOverlay();
}

void ATeamArenaCharacter::RefreshForcedArmourOverlay()
{
	// Redirect that yellow to our match/complimentary armour colour, for pawns we reskin. Client-only
	// (OverlayMesh's MID only exists off the dedicated server). Callers run this after stock overlay
	// setup, on viewer-team/config changes, or when Tick observes a replacement material.
	if (GetNetMode() == NM_DedicatedServer || IsLocalPlayerPawn())
	{
		bForcedArmourOverlayDirty = false;
		return;
	}
	// Keep the dirty bit armed while the component is temporarily absent/unregistered. If the same MID
	// is reused when registration completes, pointer identity alone cannot tell that stock rewrote it.
	if (!OverlayMesh)
	{
		ObservedArmourOverlayMaterial.Reset();
		bForcedArmourOverlayDirty = false;
		return;
	}
	if (!OverlayMesh->IsRegistered()) { return; }
	UMaterialInterface* const OverlayMaterial = OverlayMesh->GetMaterial(0);
	ObservedArmourOverlayMaterial = OverlayMaterial;
	bForcedArmourOverlayDirty = false;

	const FNCPlusForceModelsConfig& C = NCPlusForceModels::Get();
	if (!C.bEnabled || !C.bArmour || NCPlusForceModels::OutlineModeActive(GetWorld())) { return; }   // Outline mode: leave stock armour (no super-tint)

	const int32 MyTeam = (int32)GetTeamNum();
	if (MyTeam == 255) { return; }                                  // FFA: deferred

	UWorld* const World = GetWorld();
	const bool bIsFriendly = (MyTeam == NCPlusForceModels::GetViewerTeam(World));   // spectator -> red is "ours"
	if (C.Style == ENCPlusSkinStyle::EnemyOnly && bIsFriendly) { return; }   // Enemy-Only leaves teammates stock

	UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(OverlayMaterial);
	if (!MID) { return; }

	const FNCPlusModelSettings Side = NCPlusForceModels::GetModelSettings(MyTeam, bIsFriendly);
	// Same model-or-tint gate as ApplyForcedModel and the spawn-protection glow. A side with neither
	// a forced model nor "Tint skin" leaves the stock overlay untouched.
	TSubclassOf<AUTCharacterContent> GateContent = NCPlusForceModels::GetModelClass(Side);
	if (!((GateContent && NCPlusForceModels::IsModelAllowed(GateContent)) || Side.bTint)) { return; }
	const FLinearColor ArmourColour = NCPlusForceModels::GetArmourColour(Side);

	// Stock "Color" is a BRIGHT ~(1,1,0) yellow that drives the armour's emissive glow; our configured
	// colour is usually dimmer (V<1), so reusing it flat washed the glow out. Push our hue to full
	// brightness (normalise so the brightest channel = 1, like the stock yellow) for the emissive "Color",
	// and keep the real tint in "TeamColor". Preserves the armour's emissive intensity in our hue.
	FLinearColor Glow = ArmourColour;
	const float MaxCh = FMath::Max3(Glow.R, Glow.G, Glow.B);
	if (MaxCh > KINDA_SMALL_NUMBER) { Glow /= MaxCh; }
	// Per-side "Armour Glow" (F5): dim the emissive shell so armoured/shielded pawns aren't radioactive.
	// 1.0 = stock full-bright (bit-identical to before this knob existed); lower = calmer; 0 = no glow
	// (armour still tinted via TeamColor below, just not emissive). This scales ONLY the emissive "Color".
	// Shared helper also folds in the r.SimpleForwardShading auto-dim.
	Glow *= NCPlusForceModels::GetArmourEmissiveScale(Side);
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
    // The stock Lightning Rifle is a hybrid projectile/hitscan weapon whose attachment
    // remaps FireMode and FireEffect from FlashExtra. Identify it by the actual weapon
    // class, not by its attachment class: the NC+ Lightning Gun is an AUTPlusSniper BP
    // that deliberately reuses the stock LR attachment. Remote viewers receive
    // WeaponClass, which is also the source AUTWeaponAttachment::BeginPlay() uses
    // internally for its protected WeaponType field.
    TSubclassOf<AUTWeapon> ActiveWeaponClass = GetWeaponClass();
    if (Weapon != nullptr)
    {
        ActiveWeaponClass = Weapon->GetClass();
    }

    const bool bStockLightningRifle =
        ActiveWeaponClass != nullptr &&
        ActiveWeaponClass->IsChildOf(AUTWeap_LightningRifle::StaticClass());
    if (GetNetMode() != NM_DedicatedServer && bStockLightningRifle)
    {
        Super::FiringInfoUpdated();
        return;
    }

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

	if (Role == ROLE_Authority)
	{
		AUTGameMode* GM = GetWorld() ? GetWorld()->GetAuthGameMode<AUTGameMode>() : nullptr;
		const bool bIsICTF = GM && GM->bIsInstagib && GM->IsA(AUTCTFBaseGame::StaticClass());

		// ArmorPlus rule: armor pickups are always consumable, even when the
		// resulting belt/regular total is already capped. Stock defaults this true,
		// but reassert it in case a BP game mode serialized an older false value.
		if (GM)
		{
			GM->bAllowAllArmorPickups = true;
		}

		// iCTF has no spawn protection, so do not briefly render/register its visual
		// overlay on every respawn. The registration itself also emits a late-startup
		// warning for every pawn even when the material is already in the GameState.
		if (bIsICTF)
		{
			bSpawnProtectionEligible = false;
		}
		else if (SpawnProtectionMaterial)
		{
			AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
			if (GS && GS->FindOverlayMaterial(SpawnProtectionMaterial) == INDEX_NONE)
			{
				GS->AddOverlayMaterial(SpawnProtectionMaterial, nullptr);
			}
		}
	}

	// Note: Ping-compensated spawn hiding is done in the game mode's
	// RestartPlayer() AFTER Super::RestartPlayer() returns (which calls BeginPlay).
	// The game mode sets bPingCompensatedSpawnPending, hides, and disables collision.
}




void ATeamArenaCharacter::SetAmbientSound(USoundBase* NewAmbientSound, bool bClear)
{
	// Stock AUTWeap_LinkGun::Tick() applies its overheat sound to the owner while
	// cooling down even when the weapon is inactive. Inventory ticks run after the
	// owner, so a Tick-side cleanup alone is too early and the sound is immediately
	// restored. Reject the inactive weapon's assignment at the character boundary
	// on both authority and clients, without changing the weapon's cooldown state.
	if (!bClear && NewAmbientSound != nullptr)
	{
		AUTWeapon* ActiveWeapon = GetWeapon();
		AUTWeap_LinkGun* ActiveLinkGun = Cast<AUTWeap_LinkGun>(ActiveWeapon);
		const bool bActiveLinkOwnsSound =
			ActiveLinkGun != nullptr && ActiveLinkGun->OverheatSound == NewAmbientSound;

		if (!bActiveLinkOwnsSound)
		{
			for (TInventoryIterator<AUTWeapon> It(this); It; ++It)
			{
				AUTWeap_LinkGun* InactiveLinkGun = Cast<AUTWeap_LinkGun>(*It);
				if (*It != ActiveWeapon && InactiveLinkGun != nullptr &&
					InactiveLinkGun->OverheatSound == NewAmbientSound)
				{
					// Clear an already-active copy using the exact-match API, then
					// ignore the inactive Link Gun's attempt to restore it.
					if (AmbientSound == NewAmbientSound)
					{
						Super::SetAmbientSound(NewAmbientSound, true);
					}
					return;
				}
			}
		}
	}

	Super::SetAmbientSound(NewAmbientSound, bClear);
}

void ATeamArenaCharacter::SetStatusAmbientSound(USoundBase* NewAmbientSound,
	float SoundVolume, float PitchMultiplier, bool bClear)
{
	// AUTCharacter::Tick reapplies the local viewer's HeldFlagAmbientSound every
	// frame. Intercept that exact assignment at the virtual boundary; stopping the
	// component once would only let stock restart it on the next tick.
	if (!bClear && NewAmbientSound != nullptr && GetNetMode() != NM_DedicatedServer &&
		IsLocalPlayerPawn() && !NCPlusICTFAudioSettings::GetPlayFlagCarrierSound())
	{
		AUTCarriedObject* CarriedObject = GetCarriedObject();
		if (CarriedObject != nullptr && NewAmbientSound == CarriedObject->HeldFlagAmbientSound &&
			IsICTFMatch())
		{
			// A live F5 change can find the flag loop already playing. Clear the
			// occupied status slot once: stock gives the flag loop priority over
			// low health, so retaining the previous low-health loop here could leave
			// it stuck after the carrier heals. Stock restores the appropriate
			// low-health state on the first tick after the flag is dropped.
			if (StatusAmbientSound != nullptr ||
				(StatusAmbientSoundComp != nullptr && StatusAmbientSoundComp->IsPlaying()))
			{
				Super::SetStatusAmbientSound(nullptr, 0.f, 1.f, false);
			}
			return;
		}
	}

	Super::SetStatusAmbientSound(NewAmbientSound, SoundVolume, PitchMultiplier, bClear);
}


static bool NCPHasExactWeaponSkinSelection(
	const TArray<UUTWeaponSkin*>& Skins, FName WeaponTag, UUTWeaponSkin* Selection)
{
	int32 MatchingEntries = 0;
	bool bFoundSelection = false;
	for (UUTWeaponSkin* ExistingSkin : Skins)
	{
		if (ExistingSkin != nullptr &&
			ExistingSkin->WeaponSkinCustomizationTag == WeaponTag)
		{
			++MatchingEntries;
			bFoundSelection = bFoundSelection || ExistingSkin == Selection;
		}
	}
	return Selection != nullptr
		? MatchingEntries == 1 && bFoundSelection
		: MatchingEntries == 0;
}


static void NCPReplaceWeaponSkinSelection(
	TArray<UUTWeaponSkin*>& Skins, FName WeaponTag, UUTWeaponSkin* Selection)
{
	for (int32 Index = Skins.Num() - 1; Index >= 0; --Index)
	{
		UUTWeaponSkin* ExistingSkin = Skins[Index];
		if (ExistingSkin != nullptr &&
			ExistingSkin->WeaponSkinCustomizationTag == WeaponTag)
		{
			Skins.RemoveAt(Index);
		}
	}
	if (Selection != nullptr)
	{
		Skins.Add(Selection);
	}
}


void ATeamArenaCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	if (Role == ROLE_Authority)
	{
		if (AUTPlayerState* PS = Cast<AUTPlayerState>(PlayerState))
		{
			bool bRemovedNullEntry = false;
			for (int32 Index = PS->WeaponSkins.Num() - 1; Index >= 0; --Index)
			{
				if (PS->WeaponSkins[Index] == nullptr)
				{
					PS->WeaponSkins.RemoveAt(Index);
					bRemovedNullEntry = true;
				}
			}
			// Preserve any stock/profile entries outside our catalog. NCP resolves
			// only its manifest, while explicit F5 changes replace the matching tag.
			WeaponSkins = PS->WeaponSkins;
			if (bRemovedNullEntry)
			{
				PS->ForceNetUpdate();
			}
			ForceNetUpdate();
		}
	}
}


bool ATeamArenaCharacter::AddInventory(AUTInventory* InvToAdd, bool bAutoActivate)
{
	const bool bAdded = Super::AddInventory(InvToAdd, bAutoActivate);
	if (bAdded && Role == ROLE_Authority)
	{
		AUTWeapon* AddedWeapon = Cast<AUTWeapon>(InvToAdd);
		// A dropped weapon keeps the physical skin already copied into it. Fresh
		// inventory inherits this player's persistent server-approved preference.
		if (AddedWeapon != nullptr && AddedWeapon->WeaponSkin == nullptr)
		{
			AddedWeapon->WeaponSkin = ResolveWeaponSkinForClass(AddedWeapon->GetClass());
		}
	}
	return bAdded;
}


void ATeamArenaCharacter::UpdateWeaponAttachment()
{
	Super::UpdateWeaponAttachment();

	// AUTWeaponAttachment exists only outside dedicated-server worlds, so allowing
	// it to block queries creates geometry that the authoritative server never has.
	// Disable the whole actor rather than Mesh3P alone: Blueprint-added primitives
	// and runtime overlay duplicates must remain cosmetic too.
	if (WeaponAttachment != nullptr)
	{
		WeaponAttachment->SetActorEnableCollision(false);
	}
}


void ATeamArenaCharacter::UpdateHolsteredWeaponAttachment()
{
	Super::UpdateHolsteredWeaponAttachment();

	if (HolsteredWeaponAttachment != nullptr)
	{
		HolsteredWeaponAttachment->SetActorEnableCollision(false);
	}
}


UUTWeaponSkin* ATeamArenaCharacter::ResolveWeaponSkinForClass(UClass* WeaponClassToMatch) const
{
	return AUTWeaponFix::FindWeaponSkinForClass(WeaponSkins, WeaponClassToMatch);
}


void ATeamArenaCharacter::SetSkinForWeapon(UUTWeaponSkin* Skin)
{
	if (Skin == nullptr)
	{
		return;
	}
	if (AUTWeaponFix::FindPreloadedWeaponSkin(Skin->GetPathName()) != Skin)
	{
		// Keep Epic's stock/profile path intact for assets outside NCP's manifest.
		Super::SetSkinForWeapon(Skin);
		return;
	}
	if (Role != ROLE_Authority || Skin->WeaponSkinCustomizationTag == NAME_None)
	{
		return;
	}

	bool bMatchesInventory = false;
	for (TInventoryIterator<AUTWeapon> It(this); It; ++It)
	{
		AUTWeapon* InventoryWeapon = *It;
		if (InventoryWeapon != nullptr &&
			AUTWeaponFix::IsWeaponSkinCompatible(Skin, InventoryWeapon->GetClass()))
		{
			bMatchesInventory = true;
			break;
		}
	}
	if (!bMatchesInventory)
	{
		return;
	}

	const FName WeaponTag = Skin->WeaponSkinCustomizationTag;
	NCPReplaceWeaponSkinSelection(WeaponSkins, WeaponTag, Skin);

	for (TInventoryIterator<AUTWeapon> It(this); It; ++It)
	{
		AUTWeapon* InventoryWeapon = *It;
		if (InventoryWeapon != nullptr &&
			InventoryWeapon->WeaponSkinCustomizationTag == WeaponTag)
		{
			InventoryWeapon->WeaponSkin =
				ResolveWeaponSkinForClass(InventoryWeapon->GetClass());
		}
	}

	UpdateWeaponSkin();
	if (Role == ROLE_Authority)
	{
		ForceNetUpdate();
	}
}


bool ATeamArenaCharacter::ServerSetNCPWeaponSkin_Validate(
	AUTWeapon* /*InWeapon*/, const FString& SkinPath)
{
	// Functional rejection happens in the implementation so a stale weapon ref
	// during a switch cannot disconnect an otherwise legitimate client.
	return SkinPath.Len() <= 512;
}


void ATeamArenaCharacter::ServerSetNCPWeaponSkin_Implementation(
	AUTWeapon* InWeapon, const FString& SkinPath)
{
	if (Role != ROLE_Authority || InWeapon == nullptr || InWeapon->IsPendingKillPending() ||
		!InWeapon->IsA(AUTWeaponFix::StaticClass()) || InWeapon->GetUTOwner() != this)
	{
		return;
	}

	bool bOwnedWeapon = false;
	for (TInventoryIterator<AUTWeapon> It(this); It; ++It)
	{
		if (*It == InWeapon)
		{
			bOwnedWeapon = true;
			break;
		}
	}
	AUTPlayerState* PS = Cast<AUTPlayerState>(PlayerState);
	if (!bOwnedWeapon || PS == nullptr || PS->bOnlySpectator)
	{
		return;
	}
	const FName WeaponTag = InWeapon->WeaponSkinCustomizationTag;
	if (WeaponTag == NAME_None)
	{
		return;
	}

	// Count every owned, well-formed request before path resolution. This caps
	// invalid and semantic no-op calls as well as successful visual mutations.
	if (UWorld* World = GetWorld())
	{
		const float Now = World->GetTimeSeconds();
		if (Now - WeaponSkinRequestWindowStart >= 1.f)
		{
			WeaponSkinRequestWindowStart = Now;
			WeaponSkinRequestsInWindow = 0;
		}
		if (WeaponSkinRequestsInWindow >= 4)
		{
			return;
		}
		++WeaponSkinRequestsInWindow;
	}

	UUTWeaponSkin* Skin = nullptr;
	if (!SkinPath.IsEmpty())
	{
		Skin = AUTWeaponFix::FindPreloadedWeaponSkin(SkinPath);
		if (Skin == nullptr ||
			Skin->WeaponSkinCustomizationTag != InWeapon->WeaponSkinCustomizationTag ||
			!AUTWeaponFix::IsWeaponSkinCompatible(Skin, InWeapon->GetClass()) ||
			!PS->ValidateEntitlementSingleObject(Skin))
		{
			return;
		}
	}
	if (NCPHasExactWeaponSkinSelection(PS->WeaponSkins, WeaponTag, Skin) &&
		NCPHasExactWeaponSkinSelection(WeaponSkins, WeaponTag, Skin))
	{
		return;
	}

	ApplyServerWeaponSkinSelection(InWeapon, Skin);
}


void ATeamArenaCharacter::ApplyServerWeaponSkinSelection(
	AUTWeapon* InWeapon, UUTWeaponSkin* Skin)
{
	AUTPlayerState* PS = Cast<AUTPlayerState>(PlayerState);
	if (Role != ROLE_Authority || InWeapon == nullptr || PS == nullptr)
	{
		return;
	}

	const FName WeaponTag = InWeapon->WeaponSkinCustomizationTag;
	if (WeaponTag == NAME_None ||
		(Skin != nullptr && Skin->WeaponSkinCustomizationTag != WeaponTag))
	{
		return;
	}
	if (NCPHasExactWeaponSkinSelection(PS->WeaponSkins, WeaponTag, Skin) &&
		NCPHasExactWeaponSkinSelection(WeaponSkins, WeaponTag, Skin))
	{
		return;
	}

	// Persistent choice and this pawn's physical choice share the requested
	// family only. Unrelated skins picked up from dropped weapons remain intact.
	NCPReplaceWeaponSkinSelection(PS->WeaponSkins, WeaponTag, Skin);
	NCPReplaceWeaponSkinSelection(WeaponSkins, WeaponTag, Skin);
	for (TInventoryIterator<AUTWeapon> It(this); It; ++It)
	{
		AUTWeapon* InventoryWeapon = *It;
		if (InventoryWeapon != nullptr &&
			InventoryWeapon->WeaponSkinCustomizationTag == WeaponTag)
		{
			InventoryWeapon->WeaponSkin =
				AUTWeaponFix::FindWeaponSkinForClass(WeaponSkins, InventoryWeapon->GetClass());
		}
	}

	PS->ForceNetUpdate();
	ForceNetUpdate();
	UpdateWeaponSkin();
}


void ATeamArenaCharacter::UpdateWeaponSkinPrefFromProfile(AUTWeapon* InWeapon)
{
	AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(InWeapon);
	if (FixWeapon == nullptr || !IsLocalPlayerPawn())
	{
		Super::UpdateWeaponSkinPrefFromProfile(InWeapon);
		return;
	}

	SubmitConfiguredWeaponSkin(FixWeapon, false);
}


void ATeamArenaCharacter::SubmitConfiguredWeaponSkin(AUTWeaponFix* FixWeapon, bool bForce)
{
	if (FixWeapon == nullptr || !IsLocalPlayerPawn())
	{
		return;
	}

	UUTWeaponSkin* DesiredSkin = AUTWeaponFix::GetConfiguredWeaponSkin(FixWeapon);
	const FString DesiredPath = DesiredSkin != nullptr
		? DesiredSkin->GetPathName()
		: FString();
	AUTPlayerState* PS = Cast<AUTPlayerState>(PlayerState);
	UUTWeaponSkin* AuthoritativeSkin = PS != nullptr
		? AUTWeaponFix::FindWeaponSkinForClass(PS->WeaponSkins, FixWeapon->GetClass())
		: nullptr;
	const FString AuthoritativePath = AuthoritativeSkin != nullptr
		? AuthoritativeSkin->GetPathName()
		: FString();
	if (!bForce && AuthoritativePath == DesiredPath)
	{
		return;
	}

	// PlayerState is the persistent acknowledgement. Character state may differ
	// intentionally after a skinned dropped pickup; ordinary re-equips preserve it.
	ServerSetNCPWeaponSkin(FixWeapon, DesiredPath);
}


void ATeamArenaCharacter::ApplyWeaponAttachmentSkin(UUTWeaponSkin* Skin)
{
	AUTWeaponAttachment* Attachment = WeaponAttachment;
	if (Attachment == nullptr || Attachment->Mesh == nullptr ||
		Attachment->Mesh->GetNumMaterials() < 1)
	{
		SkinnedWeaponAttachment.Reset();
		OriginalWeaponAttachmentMaterials.Empty();
		AppliedWeaponAttachmentMaterialParents.Empty();
		WeaponAttachmentSkinMIDs.Empty();
		AppliedWeaponAttachmentSlotMask = 0u;
		bCapturedWeaponAttachmentMaterials = false;
		return;
	}

	if (SkinnedWeaponAttachment.Get() != Attachment)
	{
		SkinnedWeaponAttachment = Attachment;
		OriginalWeaponAttachmentMaterials.Empty();
		AppliedWeaponAttachmentMaterialParents.Empty();
		WeaponAttachmentSkinMIDs.Empty();
		AppliedWeaponAttachmentSlotMask = 0u;
		bCapturedWeaponAttachmentMaterials = false;
	}

	const int32 MaterialSlotCount = FMath::Min(Attachment->Mesh->GetNumMaterials(),
		AUTWeaponFix::MaxWeaponSkinTargetSlots);
	// The attachment carries no skin tag of its own, so the ordinary family mask comes
	// from the equipped weapon CDO. A 3P material derived from M_Invis_Skin overrides
	// that family mask and targets every live attachment slot.
	AUTWeapon* WeaponCDO = (WeaponClass != nullptr)
		? WeaponClass->GetDefaultObject<AUTWeapon>()
		: nullptr;
	AUTWeaponFix* const FixWeaponCDO = Cast<AUTWeaponFix>(WeaponCDO);
	const uint32 TargetSlotMask = AUTWeaponFix::GetResolvedWeaponSkinTargetSlotMask(Skin,
		WeaponCDO != nullptr ? WeaponCDO->WeaponSkinCustomizationTag : NAME_None,
		/*bFirstPersonMesh=*/false, MaterialSlotCount);

	while (Attachment->SavedMeshMaterials.Num() < Attachment->Mesh->GetNumMaterials())
	{
		Attachment->SavedMeshMaterials.Add(
			Attachment->Mesh->GetMaterial(Attachment->SavedMeshMaterials.Num()));
	}
	if (!bCapturedWeaponAttachmentMaterials ||
		OriginalWeaponAttachmentMaterials.Num() != MaterialSlotCount)
	{
		OriginalWeaponAttachmentMaterials.Empty(MaterialSlotCount);
		for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
		{
			OriginalWeaponAttachmentMaterials.Add(
				Attachment->SavedMeshMaterials.IsValidIndex(Slot)
				? Attachment->SavedMeshMaterials[Slot]
				: Attachment->Mesh->GetMaterial(Slot));
		}
		WeaponAttachmentSkinMIDs.Empty();
		AppliedWeaponAttachmentMaterialParents.Empty();
		AppliedWeaponAttachmentSlotMask = 0u;
		bCapturedWeaponAttachmentMaterials = true;
	}

	TArray<UMaterialInterface*> DesiredParents;
	DesiredParents.AddZeroed(MaterialSlotCount);
	if (Skin != nullptr)
	{
		for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
		{
			if (((TargetSlotMask >> Slot) & 0x1u) != 0u)
			{
				DesiredParents[Slot] = (FixWeaponCDO != nullptr)
					? FixWeaponCDO->GetResolvedWeaponSkinMaterialForSlot(
						Skin, /*bFirstPersonMesh=*/false, Slot)
					: Skin->Material;
			}
		}
	}
	const uint32 PreviousSlotMask = AppliedWeaponAttachmentSlotMask;
	bool bMaterialParentsChanged =
		AppliedWeaponAttachmentMaterialParents.Num() != DesiredParents.Num();
	for (int32 Slot = 0; !bMaterialParentsChanged && Slot < MaterialSlotCount; ++Slot)
	{
		bMaterialParentsChanged =
			AppliedWeaponAttachmentMaterialParents[Slot] != DesiredParents[Slot];
	}
	if (bMaterialParentsChanged ||
		TargetSlotMask != AppliedWeaponAttachmentSlotMask ||
		WeaponAttachmentSkinMIDs.Num() != MaterialSlotCount)
	{
		AppliedWeaponAttachmentMaterialParents = DesiredParents;
		WeaponAttachmentSkinMIDs.Empty(MaterialSlotCount);
		WeaponAttachmentSkinMIDs.AddZeroed(MaterialSlotCount);
		for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
		{
			if (((TargetSlotMask >> Slot) & 0x1u) != 0u &&
				DesiredParents[Slot] != nullptr)
			{
				WeaponAttachmentSkinMIDs[Slot] =
					UMaterialInstanceDynamic::Create(DesiredParents[Slot], Attachment->Mesh);
			}
		}
	}
	AppliedWeaponAttachmentSlotMask = TargetSlotMask;

	const uint32 SlotsToUpdate = PreviousSlotMask | TargetSlotMask;
	const bool bBodyOverrideActive = (GetSkin() != nullptr);
	for (int32 Slot = 0; Slot < MaterialSlotCount; ++Slot)
	{
		if (((SlotsToUpdate >> Slot) & 0x1u) == 0u)
		{
			continue;
		}
		const bool bTargetedByNewSkin = ((TargetSlotMask >> Slot) & 0x1u) != 0u;
		UMaterialInterface* const DesiredSlotParent = bTargetedByNewSkin
			? DesiredParents[Slot]
			: nullptr;
		UMaterialInstanceDynamic* const SelectedMID =
			(DesiredSlotParent != nullptr &&
			 WeaponAttachmentSkinMIDs.IsValidIndex(Slot))
			? WeaponAttachmentSkinMIDs[Slot]
			: nullptr;
		UMaterialInterface* const DesiredSlotMaterial =
			(DesiredSlotParent != nullptr)
			? ((SelectedMID != nullptr)
				? Cast<UMaterialInterface>(SelectedMID)
				: DesiredSlotParent)
			: OriginalWeaponAttachmentMaterials[Slot];
		if (Attachment->SavedMeshMaterials.IsValidIndex(Slot))
		{
			Attachment->SavedMeshMaterials[Slot] = DesiredSlotMaterial;
		}
		if (!bBodyOverrideActive && Attachment->Mesh->GetMaterial(Slot) != DesiredSlotMaterial)
		{
			Attachment->Mesh->SetMaterial(Slot, DesiredSlotMaterial);
		}
	}
}


void ATeamArenaCharacter::UpdateWeaponSkin()
{
	if (WeaponClass == nullptr ||
		!WeaponClass->IsChildOf(AUTWeaponFix::StaticClass()))
	{
		SkinnedWeaponAttachment.Reset();
		OriginalWeaponAttachmentMaterials.Empty();
		AppliedWeaponAttachmentMaterialParents.Empty();
		WeaponAttachmentSkinMIDs.Empty();
		AppliedWeaponAttachmentSlotMask = 0u;
		bCapturedWeaponAttachmentMaterials = false;
		Super::UpdateWeaponSkin();
		return;
	}

	UUTWeaponSkin* ReplicatedSkin = ResolveWeaponSkinForClass(WeaponClass);
	if (GetNetMode() != NM_DedicatedServer)
	{
		ApplyWeaponAttachmentSkin(ReplicatedSkin);
	}

	if (AUTWeaponFix* FixWeapon = Cast<AUTWeaponFix>(GetWeapon()))
	{
		FixWeapon->ApplyResolvedWeaponSkin(ReplicatedSkin);
	}
}


void ATeamArenaCharacter::UpdateSkin()
{
	Super::UpdateSkin();
	if (WeaponClass != nullptr && WeaponClass->IsChildOf(AUTWeaponFix::StaticClass()))
	{
		UpdateWeaponSkin();
	}
}


void ATeamArenaCharacter::Tick(float DeltaTime)
{
	// Flush work queued by an earlier ApplyCharacterData before any forced-model apply below can
	// queue another rebuild. This ordering guarantees an ApplyCharacterData reached from this Tick
	// cannot recreate its outline until the following character tick.
	if (bDeferredOutlineUpdatePending)
	{
		FlushDeferredOutlineUpdate();
	}

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
			// Final safety: force-reveal after 500ms if the RevealRttPct timer somehow
			// didn't fire (belt-and-suspenders; normally the timer reveals first).
			if (GetWorld()->GetTimeSeconds() - SpawnHiddenTimestamp > 0.5f)
			{
				RevealAfterPingComp();
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

			// BP-parity apply (visibility-only; also restores when not hidden).
			// See AUTWeaponFix::ApplyWeaponHideState for why not SetHiddenInGame.
			AUTWeaponFix::ApplyWeaponHideState(CurrentWeapon, this, bShouldHide);
		}
		// Clear stale reference if weapon was removed (death, drop)
		if (LastEquippedWeapon && (!CurrentWeapon || CurrentWeapon->IsPendingKillPending()))
		{
			LastEquippedWeapon = nullptr;
		}
	}

	// --- Safety net: clear weapon fire loops that outlive their firing state ---
	// UUTWeaponStateFiring normally exact-clears FireLoopingSound in EndState(). If an
	// abnormal transition misses that cleanup, the replicated character ambient sound
	// otherwise survives indefinitely and permanently consumes an audio voice. Only
	// authority may validate weapon state here: simulated spectator weapon state is
	// reconstructed from separately replicated firing fields and can lag the ambient
	// sound update, misclassifying a legitimate secondary-fire loop as stale.
	if (Role == ROLE_Authority && AmbientSound != nullptr)
	{
		AUTWeapon* ActiveWeapon = GetWeapon();
		USoundBase* CurrentAmbientSound = AmbientSound;
		bool bMatchesWeaponFireLoop = false;
		bool bIsLegitimateActiveLoop = false;

		for (TInventoryIterator<AUTWeapon> It(this); It; ++It)
		{
			AUTWeapon* InventoryWeapon = *It;
			if (InventoryWeapon == nullptr)
			{
				continue;
			}

			for (int32 FireMode = 0; FireMode < InventoryWeapon->FireLoopingSound.Num(); ++FireMode)
			{
				if (InventoryWeapon->FireLoopingSound[FireMode] == CurrentAmbientSound)
				{
					bMatchesWeaponFireLoop = true;
					bIsLegitimateActiveLoop = bIsLegitimateActiveLoop ||
						(InventoryWeapon == ActiveWeapon && InventoryWeapon->IsFiring() &&
						 InventoryWeapon->GetCurrentFireMode() == FireMode);
				}
			}
		}

		if (bMatchesWeaponFireLoop && !bIsLegitimateActiveLoop)
		{
			SetAmbientSound(CurrentAmbientSound, true);
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
	// or beyond this client's cached CharacterOverlayDistance preference. SetVisibility
	// only fires when state changes to avoid redundant MarkRenderStateDirty.
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
				if (FVector::DistSquared(GetActorLocation(), ViewLoc)
					> NCPlusPerformanceSettings::GetCharacterOverlayDistanceSquared())
				{
					bShouldShow = false;
				}
			}
		}

		// ── ForceModels: optional fallback — HIDE the shield-belt overlay (ncp.HideArmorShield 1) ──
		// The event-driven recolour tints the shield to the team skin colour via the "Color" param
		// (the working path, DEFAULT). This hide is the fallback for community models whose shield
		// material bakes the gold and ignores that recolour. Gated on IsEnabled so it also catches
		// transiently-unrecoloured pawns; other overlays (UDamage, etc.) are untouched, and vanilla
		// play keeps the stock shield.
		if (bShouldShow && NCPlusForceModels::IsEnabled() && CVarHideArmorShield.GetValueOnGameThread() != 0)
		{
			UMaterialInstanceDynamic* ShieldMID = Cast<UMaterialInstanceDynamic>(OverlayMesh->GetMaterial(0));
			if (ShieldMID && ShieldMID->Parent && ShieldMID->Parent->GetName().Contains(TEXT("Shield")))
			{
				bShouldShow = false;
			}
		}

		if (OverlayMesh->IsVisible() != bShouldShow)
		{
			OverlayMesh->SetVisibility(bShouldShow, true);
		}
	}

	// Armour overlays are normally rebuilt through UpdateArmorOverlay(), where the Force Models tint
	// is now applied once. Keep only a cheap material-identity guard here for third-party/Blueprint
	// overlay replacement paths that bypass that virtual hook.
	UMaterialInterface* const CurrentArmourOverlayMaterial =
		(OverlayMesh && OverlayMesh->IsRegistered()) ? OverlayMesh->GetMaterial(0) : nullptr;
	if (CurrentArmourOverlayMaterial != ObservedArmourOverlayMaterial.Get())
	{
		ObservedArmourOverlayMaterial = CurrentArmourOverlayMaterial;
		bForcedArmourOverlayDirty = true;
	}
	if (bForcedArmourOverlayDirty)
	{
		RefreshForcedArmourOverlay();
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

		// Resolve the glow colour ONCE for the body hit-flash. Default = stock team colour (unchanged
		// vanilla behaviour); use the ForceModels skin colour when it's recolouring this enemy's body.
		// (The OVERLAY recolour is handled independently above — the armour/shield overlay outlives
		// spawn protection, so it can't be gated on it.)
		FLinearColor GlowColour = GetTeamColor();
		if (bShowGlowToViewer && NCPlusForceModels::IsEnabled())
		{
			const int32 GlowTeam = (int32)GetTeamNum();
			if (GlowTeam != 255)
			{
				const bool bGlowFriendly = (GlowTeam == NCPlusForceModels::GetViewerTeam(GetWorld()));
				const FNCPlusModelSettings& GlowSide = NCPlusForceModels::GetModelSettings(GlowTeam, bGlowFriendly);
				TSubclassOf<AUTCharacterContent> GlowContent = NCPlusForceModels::GetModelClass(GlowSide);
				// Model-or-tint, matching ApplyForcedModel and the overlay recolour above.
				if ((GlowContent && NCPlusForceModels::IsModelAllowed(GlowContent)) || GlowSide.bTint)
				{
					GlowColour = NCPlusForceModels::GetSkinColour(GlowSide);
				}
			}
		}

		// --- PERF: Dirty flag — body-material work only on state change ---
		// Values are constant within each state (glow on vs glow off). The overlay tint above runs
		// every frame; bLastShowGlowState initialized to 0xFF to force first-frame apply.
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
			// Body hit-flash (mostly hidden behind the spawn overlay, but covers the brief post-overlay
			// window and any pawn whose overlay isn't currently active). Uses the same GlowColour resolved
			// above, overbright 20x into the HitFlash param as before.
			const float BrightnessMult = 20.0f;
			const FLinearColor ObviousColor(GlowColour.R * BrightnessMult, GlowColour.G * BrightnessMult, GlowColour.B * BrightnessMult, 1.0f);

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
		// OverheatFactor is locally simulated rather than replicated, so a newly
		// viewed remote Link Gun can carry a bogus spectator-side meter value.
		LinkGun->OverheatFactor = 0.0f;

		// Clear only the stale overheat loop. Clearing the character's ambient
		// slot unconditionally also kills a legitimate secondary-fire beam that
		// was already active when spectating began; the server may not resend an
		// unchanged AmbientSound afterward.
		if (AmbientSound == LinkGun->OverheatSound)
		{
			SetAmbientSound(LinkGun->OverheatSound, true);
		}
	}

	// OnRepWeaponSkin may have run before a remote first-person weapon existed.
	// Re-resolve now so a newly spectated player shows the same skin as third person.
	UpdateWeaponSkin();
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
	SetAmbientSound(nullptr);
	SetStatusAmbientSound(nullptr);
	SetLocalAmbientSound(nullptr);

	if (AmbientSoundComp && AmbientSoundComp->IsPlaying())
	{
		AmbientSoundComp->Stop();
	}

	BeltArmorRemaining = 0;
	LastRegularArmorType = nullptr;

	return Super::Died(EventInstigator, DamageEvent, DamageCauser);
}

void ATeamArenaCharacter::GiveArmor(AUTArmor* InArmorType)
{
	if (InArmorType == nullptr)
	{
		InArmorType = AUTGameMode::StaticClass()->GetDefaultObject<AUTGameMode>()
			->StartingArmorClass.GetDefaultObject();
		if (InArmorType == nullptr)
		{
			return;
		}
	}

	if (IsHelmetArmor(InArmorType))
	{
		// One non-stacking charge: a second helmet before being shot changes
		// nothing, a pickup after a spent block re-arms. Grant is tracked even
		// with ncp.HelmetBlocksHeadshot off (the cvar gates the BLOCK decision),
		// so an admin flipping it mid-match behaves sanely.
		bHeadArmorCharge = true;
		HeadArmorChargeValue = FMath::Max(0, InArmorType->ArmorAmount);
	}

	if (IsArmorPlusBelt(InArmorType))
	{
		// A new belt replaces any mixed stack with a full, pure belt. The pickup
		// remains consumable at any armor value through stock bAllowAllArmorPickups.
		BeltArmorRemaining = FMath::Clamp(InArmorType->ArmorAmount, 0, ArmorPlusMaxTotal);
		LastRegularArmorType = nullptr;
		Super::SetArmorAmount(InArmorType, BeltArmorRemaining);
		return;
	}

	const int32 CurrentTotal = FMath::Clamp(GetArmorAmount(), 0, ArmorPlusMaxTotal);
	const int32 CurrentBelt = FMath::Clamp(BeltArmorRemaining, 0, CurrentTotal);
	const int32 CurrentRegular = FMath::Clamp(CurrentTotal - CurrentBelt, 0, ArmorPlusSoftLimit);
	const int32 PickupAmount = FMath::Max(0, InArmorType->ArmorAmount);
	const int32 NewRegular = FMath::Min(ArmorPlusSoftLimit, CurrentRegular + PickupAmount);
	const int32 NewTotal = FMath::Min(ArmorPlusMaxTotal, CurrentBelt + NewRegular);

	LastRegularArmorType = InArmorType;
	BeltArmorRemaining = CurrentBelt;

	// Preserve the belt class/effects while any 100%-absorb portion remains.
	AUTArmor* DisplayArmorType = (CurrentBelt > 0 && ArmorType != nullptr)
		? ArmorType
		: InArmorType;
	Super::SetArmorAmount(DisplayArmorType, NewTotal);
}

void ATeamArenaCharacter::SetArmorAmount(AUTArmor* InArmorType, int32 Amount)
{
	// A direct set is a full re-spec of the armour state; any helmet charge
	// belonged to the pool being replaced. (GiveArmor is unaffected: it calls
	// Super::SetArmorAmount directly, so a fresh helmet grant survives.)
	bHeadArmorCharge = false;
	HeadArmorChargeValue = 0;

	// Direct setters (starting/player-card armor, dropped armor, BP calls) do not
	// carry a belt/regular split, so treat the supplied type as a pure pool.
	const bool bBelt = IsArmorPlusBelt(InArmorType);
	const int32 Limit = bBelt ? ArmorPlusMaxTotal : ArmorPlusSoftLimit;
	const int32 NewTotal = FMath::Clamp(Amount, 0, Limit);

	BeltArmorRemaining = bBelt ? NewTotal : 0;
	LastRegularArmorType = (!bBelt && NewTotal > 0) ? InArmorType : nullptr;
	Super::SetArmorAmount(InArmorType, NewTotal);
}

void ATeamArenaCharacter::RemoveArmor(int32 Amount)
{
	Super::RemoveArmor(Amount);

	const int32 CurrentTotal = FMath::Max(0, GetArmorAmount());
	BeltArmorRemaining = FMath::Clamp(BeltArmorRemaining, 0, CurrentTotal);

	if (CurrentTotal <= 0)
	{
		BeltArmorRemaining = 0;
		LastRegularArmorType = nullptr;
		// No armour left = no helmet left: the pool the helmet lived in is gone.
		// Prevents a naked 0-armour player carrying a banked block around.
		bHeadArmorCharge = false;
	}
	else if (BeltArmorRemaining == 0 && LastRegularArmorType != nullptr && ArmorType != LastRegularArmorType)
	{
		// ArmorType is replicated; clients run its RepNotify. The authority needs
		// the explicit overlay refresh because it does not receive its own RepNotify.
		ArmorType = LastRegularArmorType;
		UpdateArmorOverlay();
	}
}

void ATeamArenaCharacter::ServerDropArmor_Implementation()
{
	// AUTDroppedArmor serializes only one type and one total, so it cannot represent
	// a mixed belt/regular pool without turning the whole pickup into belt armor.
	// The stock UI path is disabled; reject modified-client calls as well.
}

bool ATeamArenaCharacter::BlockedHeadShot(FVector HitLocation, FVector ShotDirection, float WeaponHeadScaling, bool bConsumeArmor, AUTCharacter* ShotInstigator)
{
	// Stock path first: inventory items implementing PreventHeadShot. Nothing in
	// this build implements it, but a future BP item stays honoured.
	if (Super::BlockedHeadShot(HitLocation, ShotDirection, WeaponHeadScaling, bConsumeArmor, ShotInstigator))
	{
		return true;
	}

	if (CVarHelmetBlocksHeadshot.GetValueOnGameThread() == 0 || !bHeadArmorCharge)
	{
		return false;
	}

	// Server-authoritative: FireInstantHit also runs on the owning client and
	// calls this unguarded. The charge is server-only state, so a client must
	// neither decide nor consume (its copy is always false anyway — this is
	// belt and braces).
	if (Role != ROLE_Authority)
	{
		return false;
	}

	if (bConsumeArmor)
	{
		// One and done: the Epic-era helmet blocked headshots indefinitely; this
		// one dies with the block and only another helmet pickup re-arms it.
		// The helmet's armour points go with it BEFORE the blocked damage
		// resolves against whatever remains — the shot destroys the helmet,
		// UT3-style. Clear the charge first so RemoveArmor's depletion handling
		// never sees a stale one.
		bHeadArmorCharge = false;
		RemoveArmor(HeadArmorChargeValue);
	}
	return true;
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

			// Pickup-time state is authoritative. Clamp defensively for legacy/direct
			// writes, but never infer belt from the combined armor total here.
			BeltArmorRemaining = FMath::Clamp(BeltArmorRemaining, 0, CurrentArmor);
			int32 BeltPortion = BeltArmorRemaining;
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
	// Client ACK'd possession — reveal. Early-out vs the RevealRttPct timer when
	// ExactPing over-read (timer set too long). Guarded against double-fire.
	RevealAfterPingComp();
}

void ATeamArenaCharacter::BeginPingCompensatedSpawnHide()
{
	// PING FLOOR: skip the hide for low-ping spawners. They get possession back
	// almost immediately (no real spawn-kill risk), and their brief hidden window is
	// exactly what makes them appear to "teleport-dodge" off the spawn point on a
	// higher-ping opponent's screen — which is the thing high-ping players complain
	// about. At/above the floor the hide genuinely prevents being seen/shot before
	// you can move. Floor = Mod.ini [NetcodePlus] PingCompSpawnMinPingMs (default
	// 60ms), read via GConfig like the own-footstep setting above. (ExactPing is live
	// for every spawn — measured continuously server-side — so there's no unknown case.)
	float MinPingMs = 60.f;
	float RevealRttPct = 75.f;   // reveal at this % of RTT (server estimate) vs the full round-trip ACK
	const FString ModIniPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	if (GConfig)
	{
		GConfig->GetFloat(TEXT("NetcodePlus"), TEXT("PingCompSpawnMinPingMs"), MinPingMs, ModIniPath);
		GConfig->GetFloat(TEXT("NetcodePlus"), TEXT("PingCompSpawnRevealRttPct"), RevealRttPct, ModIniPath);
	}
	MinPingMs = FMath::Max(0.f, MinPingMs);
	RevealRttPct = FMath::Clamp(RevealRttPct, 0.f, 100.f);

	float Ping = 0.f;
	if (AController* C = GetController())
	{
		if (AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState))
		{
			Ping = PS->ExactPing;   // server-measured true ms (live every spawn, not the compressed Ping)
		}
	}
	if (Ping < MinPingMs)
	{
		return;   // low-ping spawner -> spawn visible immediately, no hide
	}

	bPingCompensatedSpawnPending = true;
	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
	SpawnHiddenTimestamp = GetWorld()->GetTimeSeconds();

	// Reveal on a SERVER ESTIMATE at RevealRttPct% of RTT, rather than waiting for the
	// client's ServerConfirmSpawnReady ACK. The ACK is a FULL round-trip, so it over-
	// hides by ~one one-way trip the spawner doesn't need — and that surplus invisible
	// window is the high-ping spawn edge. The client has control after ~one one-way
	// trip; 75% of RTT reveals a touch after that, leaving a small buffer for client
	// render/orient + jitter. The ACK still reveals early if ExactPing OVER-read
	// (timer set too long), and the 0.5s Tick timeout is a final safety if the timer
	// somehow doesn't fire. Capped at 0.5s to match that timeout.
	const float RevealDelay = FMath::Clamp((RevealRttPct / 100.f) * (Ping / 1000.f), 0.f, 0.5f);
	GetWorldTimerManager().SetTimer(SpawnRevealHandle, this, &ATeamArenaCharacter::RevealAfterPingComp, RevealDelay, false);
}

void ATeamArenaCharacter::RevealAfterPingComp()
{
	if (!bPingCompensatedSpawnPending)
	{
		return;   // already revealed (timer / ACK / timeout race) — no-op
	}
	bPingCompensatedSpawnPending = false;
	SetActorHiddenInGame(false);
	SetActorEnableCollision(true);
	GetWorldTimerManager().ClearTimer(SpawnRevealHandle);
}

// ── Shared client-side iCTF detection ───────────────────────────────────────
bool ATeamArenaCharacter::IsICTFMatch()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return false;
	}

	if (!bIctfModeResolved)
	{
		for (TActorIterator<ACTFStatsReplicator> It(World); It; ++It)
		{
			CachedCTFRep = *It;
			break;
		}

		// ACTFStatsReplicator arrives at match start. The replicated MutInstagibNCP
		// mutator is present during warmup, so it supplies the early iCTF signal.
		if (!bIctfMutatorFound)
		{
			for (TActorIterator<AUTMutator> It(World); It; ++It)
			{
				if (It->GetClass()->GetName().Contains(TEXT("MutInstagibNCP")))
				{
					bIctfMutatorFound = true;
					break;
				}
			}
		}

		// Stop walking actor lists forever outside iCTF. A cached replicator's
		// replicated bool remains live, so a later true update is still observed.
		if (CachedCTFRep.IsValid() || bIctfMutatorFound || (World->GetTimeSeconds() - CreationTime) > 8.f)
		{
			bIctfModeResolved = true;
		}
	}

	return bIctfMutatorFound || (CachedCTFRep.IsValid() && CachedCTFRep->bIsInstagibMatch);
}

// ── Own footstep volume (iCTF) ─────────────────────────────────────────────
// Scale THIS local player's OWN footstep volume by the F5 "Own Footstep Volume" setting. Only the local
// human's own pawn, only in iCTF, only when the setting is below stock (1.0); everything else falls through
// to the stock AUTCharacter::PlayFootstep. UTPlaySound has no volume argument, so the non-stock case is
// played via SpawnSoundAttached with a VolumeMultiplier.
void ATeamArenaCharacter::PlayFootstep(uint8 FootNum, bool bFirstPerson)
{
	if (GetNetMode() != NM_DedicatedServer)
	{
		const float ClutchVolumeScale = GetClutchFootstepVolumeScale();
		if (ClutchVolumeScale < 1.0f)
		{
			PlayFootstepScaled(FootNum, bFirstPerson, ClutchVolumeScale);
			return;
		}
	}

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
			if (IsICTFMatch())
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

float ATeamArenaCharacter::GetClutchFootstepVolumeScale()
{
	if (!CachedClutchFootstepState.IsValid())
	{
		for (TActorIterator<AClutchRoundState> It(GetWorld()); It; ++It)
		{
			CachedClutchFootstepState = *It;
			break;
		}
	}

	AClutchRoundState* State = CachedClutchFootstepState.Get();
	AUTPlayerState* SourceState = Cast<AUTPlayerState>(PlayerState);
	const FClutchRosterEntry* Entry = State && SourceState
		? State->FindEntry(SourceState)
		: nullptr;
	return State && State->IsGameplayPhase() && Entry
		&& Entry->PlayerRole == EClutchRole::Defender
		&& Entry->PlayerStatus == EClutchStatus::Active
		? ClutchDefenderFootstepVolume
		: 1.0f;
}

void ATeamArenaCharacter::PlayFootstepScaled(
	uint8 FootNum, bool bFirstPerson, float VolumeScale)
{
	if ((GetWorld()->TimeSeconds - LastFootstepTime < 0.1f) || bFeigningDeath
		|| IsDead() || bIsCrouched || GetNetMode() == NM_DedicatedServer)
	{
		return;
	}

	AUTPlayerController* OwningPC = Cast<AUTPlayerController>(Controller);
	if (OwningPC && IsLocallyControlled() && !bFirstPerson && !OwningPC->IsBehindView())
	{
		return;
	}

	UParticleSystem* FootstepEffect = nullptr;
	float MaxParticleDistance = 1500.0f;
	USoundBase* SoundToPlay = FootstepSound;
	if (FeetAreInWater())
	{
		SoundToPlay = WaterFootstepSound;
		FootstepEffect = WaterFootstepEffect;
		MaxParticleDistance = 5000.0f;
	}
	else
	{
		AUTPlayerController* LocalViewer = GetLocalViewer();
		const bool bLocalViewer = LocalViewer != nullptr;
		if (bApplyWallSlide)
		{
			if (UTCharacterMovement && UTCharacterMovement->WallRunMaterial)
			{
				const EPhysicalSurface SurfaceType = UPhysicalMaterial::DetermineSurfaceType(
					UTCharacterMovement->WallRunMaterial);
				if (USoundBase* SurfaceSound = GetFootstepSoundForSurfaceType(
					SurfaceType, bLocalViewer))
				{
					SoundToPlay = SurfaceSound;
				}
			}
		}
		else
		{
			static const FName FootstepTraceName(TEXT("ClutchFootstepSurface"));
			FCollisionQueryParams QueryParams(FootstepTraceName, false, this);
			QueryParams.bReturnPhysicalMaterial = true;
			QueryParams.bTraceAsyncScene = true;
			float PawnRadius = 0.0f;
			float PawnHalfHeight = 0.0f;
			GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);
			const FVector TraceStart = GetCapsuleComponent()->GetComponentLocation();
			FHitResult SurfaceHit(1.0f);
			if (GetWorld()->LineTraceSingleByChannel(
				SurfaceHit, TraceStart,
				TraceStart + FVector(0.0f, 0.0f, -(40.0f + PawnHalfHeight)),
				GetCapsuleComponent()->GetCollisionObjectType(), QueryParams)
				&& SurfaceHit.PhysMaterial.IsValid())
			{
				const EPhysicalSurface SurfaceType = UPhysicalMaterial::DetermineSurfaceType(
					SurfaceHit.PhysMaterial.Get());
				if (USoundBase* SurfaceSound = GetFootstepSoundForSurfaceType(
					SurfaceType, bLocalViewer))
				{
					SoundToPlay = SurfaceSound;
				}
			}
		}

		FootstepEffect = GetVelocity().Size() > 500.0f
			&& (!LocalViewer || LocalViewer->IsBehindView())
			? GroundFootstepEffect
			: nullptr;
	}

	VolumeScale = FMath::Clamp(VolumeScale, 0.0f, 1.0f);
	if (SoundToPlay && VolumeScale > 0.0f)
	{
		AUTPlayerController* LocalPC = Cast<AUTPlayerController>(
			GetWorld()->GetFirstPlayerController());
		const bool bViewedSource = LocalPC && LocalPC->GetViewTarget() == this;
		if (bViewedSource)
		{
			UGameplayStatics::SpawnSound2D(this, SoundToPlay,
				VolumeScale * LocalPC->FootStepAmp.OwnVolumeMultiplier,
				LocalPC->FootStepAmp.OwnPitchMultiplier);
		}
		else
		{
			float ListenerVolume = 1.0f;
			float ListenerPitch = 1.0f;
			USoundAttenuation* AttenuationOverride = nullptr;
			bool bSameTeam = false;
			if (LocalPC)
			{
				AUTGameState* GameState = GetWorld()->GetGameState<AUTGameState>();
				bSameTeam = GameState && GameState->OnSameTeam(LocalPC, this);
				if (bSameTeam)
				{
					AttenuationOverride = LocalPC->FootStepAmp.TeammateAttenuation;
					ListenerVolume = LocalPC->FootStepAmp.TeammateVolumeMultiplier;
					ListenerPitch = LocalPC->FootStepAmp.TeammatePitchMultiplier;
				}
				else
				{
					FVector ViewPoint;
					FRotator ViewRotation;
					LocalPC->GetActorEyesViewPoint(ViewPoint, ViewRotation);
					if ((ViewRotation.Vector()
						| (GetActorLocation() - ViewPoint).GetSafeNormal()) < 0.7f)
					{
						ListenerVolume = 3.0f;
					}
				}

				FVector ViewPoint;
				FRotator ViewRotation;
				LocalPC->GetActorEyesViewPoint(ViewPoint, ViewRotation);
				FCollisionQueryParams Params(FName(TEXT("ClutchFootstepLOS")), true, this);
				Params.AddIgnoredActor(LocalPC->GetViewTarget());
				const bool bOccluded = GetWorld()->LineTraceTestByChannel(
					ViewPoint, GetActorLocation(), ECC_Visibility, Params);
				if (bOccluded)
				{
					if (LocalPC->FootStepAmp.OccludedAttenuation)
					{
						AttenuationOverride = LocalPC->FootStepAmp.OccludedAttenuation;
					}
					else
					{
						const float MaxAudibleDistance = SoundToPlay->GetAttenuationSettingsToApply()
							? SoundToPlay->GetAttenuationSettingsToApply()->GetMaxDimension()
							: 4000.0f;
						if ((GetActorLocation() - ViewPoint).Size()
							> MaxAudibleDistance * (bSameTeam ? 0.2f : 0.4f))
						{
							SoundToPlay = nullptr;
						}
						else
						{
							ListenerVolume *= 0.7f;
						}
					}
				}
			}

			if (SoundToPlay)
			{
				UGameplayStatics::SpawnSoundAttached(SoundToPlay, GetRootComponent(),
					NAME_None, FVector::ZeroVector, EAttachLocation::KeepRelativeOffset,
					false, VolumeScale * ListenerVolume, ListenerPitch, 0.0f,
					AttenuationOverride);
			}
		}
	}

	if (FootstepEffect && GetMesh()
		&& GetWorld()->GetTimeSeconds() - GetMesh()->LastRenderTime < 0.05f
		&& (GetLocalViewer() || GetCachedScalabilityCVars().DetailMode != 0))
	{
		AUTWorldSettings* WorldSettings = Cast<AUTWorldSettings>(GetWorld()->GetWorldSettings());
		if (WorldSettings && WorldSettings->EffectIsRelevant(this, GetActorLocation(), true,
			Cast<APlayerController>(GetController()) != nullptr,
			MaxParticleDistance, 0.0f, false))
		{
			FVector EffectLocation = GetActorLocation();
			EffectLocation.Z += 4.0f - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			UGameplayStatics::SpawnEmitterAtLocation(
				GetWorld(), FootstepEffect, EffectLocation, GetActorRotation(), true);
		}
	}

	LastFoot = FootNum;
	LastFootstepTime = GetWorld()->TimeSeconds;
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
