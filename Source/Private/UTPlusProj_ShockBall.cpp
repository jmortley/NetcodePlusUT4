
#include "UTPlusProj_ShockBall.h"
#include "UTPlusShockRifle.h"
#include "Particles/ParticleSystemComponent.h"
#include "Engine/DemoNetDriver.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Components/SphereComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "Sound/SoundBase.h"
#include "UTKillcamPlayback.h"
#include "UTLocalPlayer.h"

// =========================================================================
// SHOCK-CORE DIAGNOSTICS (ncp.ShockDebug)
// Event-gated (NEVER per-tick — projectile tick is 120-720Hz) logging to pin down
// the two long-standing shock-core symptoms that don't repro offline/standalone:
//   - stuck-in-mesh: a live core embeds + "spins" in a wall, still comboable
//   - invisible-bounce/curve: a core seems to collide with / curve off nothing
// One cvar, registered in BOTH the client and dedicated-server process. Set it on
// whichever process you want logs from: client-side for pairing/handoff/reveal/snap
// events ([ShockDbg/CLI]), server-side for stuck/embed/combo/proj-vs-proj events
// ([ShockDbg/SRV]). Default 0 = inert (a single cvar read + branch per event).
// =========================================================================
DEFINE_LOG_CATEGORY_STATIC(LogShockDbg, Log, All);

static TAutoConsoleVariable<int32> CVarShockDebug(
	TEXT("ncp.ShockDebug"), 0,
	TEXT("NetcodePlus shock-core diagnostics. 0=off, 1=event logs (pairing/anchor/recovery/handoff/stuck/combo/collision/lifecycle). Use ncp.ShockDump for an on-demand live/audio inventory."),
	ECVF_Default);

// =========================================================================
// DIAGNOSTIC ISOLATION TOGGLES (defaults = current shipped behaviour)
// Flip these live (set on whichever process you want — server for the authoritative
// core's stuck/slide behaviour, client for the fake/pairing visual) to isolate which
// NetcodePlus delta drives the stuck-in-wall / curving symptoms vs stock. None of these
// change replicated state, UPROPERTYs, or NETCODE_PLUGIN_VERSION — pure behaviour gates.
// =========================================================================
static TAutoConsoleVariable<int32> CVarShockDriftCorrect(
	TEXT("ncp.ShockDriftCorrect"), 1,
	TEXT("Per-tick heading re-assert (drift correction). 1=on (shipped), 0=off (stock-like: no re-aim, PMC owns velocity)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockMatchFakeDot(
	TEXT("ncp.ShockMatchFakeDot"), 0.5f,
	TEXT("CanMatchFake direction gate (dot). 0.5=shipped (~60deg), 0.95=stock (~18deg: rejects divergent fake/real pairs)."),
	ECVF_Default);

// FAKE-THEFT FIX (2026-07-08). Captured live: a foreign core 3498u away paired with the local
// fake at dot 0.8571 (gate 0.5), was teleported onto it and rewrote its velocity — the
// long-reported "curve/swoosh"; the correct real arrived 57ms later, found its fake stolen,
// and stayed visible (double core / stuck-then-vanished family). Root: the stock pairing loop
// (UTProjectile.cpp:290-322) gates on class + CanMatchFake only, and foreign cores fall into
// the LOCAL player's fake list because a remote InstigatorController never exists on this
// client (UTProjectile.cpp:275 falls back to the first local PC). Two new gates, individually
// kill-switchable, client-side only, no version bump.
static TAutoConsoleVariable<int32> CVarShockMatchFakeInstigator(
	TEXT("ncp.ShockMatchFakeInstigator"), 1,
	TEXT("CanMatchFake instigator gate. 1=on (default): a replicated core may only pair with a fake fired by the SAME pawn; rejects (fail-closed) when either Instigator is null - a null on the real means the shooter's pawn hasn't resolved on this client, i.e. a foreign core, exactly the fake-theft population. 0=off (pre-fix behaviour: any local fake can be stolen)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockMatchFakeMaxDist(
	TEXT("ncp.ShockMatchFakeMaxDist"), 1250.f,
	TEXT("CanMatchFake max fake<->real distance in units, measured post-CatchupTick (healthy own pairs measure 21-26u). A LOOSE backstop to the instigator gate (gate 2 is the real theft defense); mainly rejects same-instigator stale/ghost fakes at long range. Default 1250 covers the documented legit retransmit band (after burst loss the RELIABLE ServerStartFireFixed channel delivers a legit real ~480-1200u behind its OWN fake; the proactive +40/+80ms resends only cover ~100-220u) - raised from 1000, which rejected the 1000-1200u top of that band and reintroduced double cores on lossy links. NOT a cross-pair boundary: the gate measures real<->fake, and a real lagging its own fake by S units sits (1401-S)u from the NEXT shot's fake (consecutive fakes are 0.58s*2415=1401u apart), so for S>700 the next fake is CLOSER and the stock closest-match loop cross-pairs it at any gate value - that residual (own real adopting the next shot's fake) needs shot-identity to fix, not distance (pre-existing; stock's dot-only gate cross-paired too). Raising 1000->1250 does NOT worsen it (cross-pairs need S>700, where the wrong fake is <701u, already inside the old 1000 gate). Bias LOW when tuning: a missed pair is a brief double core, a cross-pair is a teleporting fake. <=0 disables this gate AND the no-progress staleness gate."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockMatchStoppedFakeGrace(
	TEXT("ncp.ShockMatchStoppedFakeGrace"), 0.25f,
	TEXT("Seconds after firing that a cleanly-stopped fake may match using its cached fire direction instead of zero current velocity. Same-instigator, distance, and staleness gates still apply. Default 0.25 for dogfood; <=0 disables the fallback."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockOrphanStoppedTimeout(
	TEXT("ncp.ShockOrphanStoppedTimeout"), 1.5f,
	TEXT("Seconds an unpaired local shock fake may remain stopped before quiet client-only removal. Stops audio and destroys without an explosion; <=0 disables the reaper. Default 1.5."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarShockServerTickHz(
	TEXT("ncp.ShockServerTickHz"), 0,
	TEXT("Server shock-core tick rate. 0=240Hz (shipped); >0=that Hz (clamped 30..720); <0=unset/tick-every-frame (stock-like). Read at spawn — set BEFORE firing."),
	ECVF_Default);

// CLIENT rollback levers (default 1 = current shipped behaviour; flip to 0 live to kill the feature
// without a rebuild). Both are client-side cosmetic fake/real reconciliation — no replication, no version bump.
static TAutoConsoleVariable<int32> CVarShockConverge(
	TEXT("ncp.ShockConverge"), 1,
	TEXT("Client fake->authoritative-estimate convergence. 1=on (dogfood default: bounded soft phase correction, never a mid-flight backward snap), 0=off (fake renders its own predicted path; legacy >120u movement-update snap remains as a safety fallback)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockConvergeTime(
	TEXT("ncp.ShockConvergeTime"), 0.25f,
	TEXT("Desired time in seconds for client shock-core phase error to converge toward the server estimate (clamped 0.10..1.00). Correction speed is also bounded by ncp.ShockConvergeMaxSpeed and 75% of current travel speed so the visible core cannot reverse."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockConvergeMaxSpeed(
	TEXT("ncp.ShockConvergeMaxSpeed"), 1200.f,
	TEXT("Maximum client shock-core phase-correction speed in uu/s (clamped 100..2000). Default 1200 keeps a 2415uu/s core moving forward even while correcting backward."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockConvergeMaxDist(
	TEXT("ncp.ShockConvergeMaxDist"), 500.f,
	TEXT("Largest paired fake<->server-estimate error eligible for soft in-flight phase convergence (uu; clamped 60..2000). Default 500 covers normal high-ping phase error while avoiding aggressive correction of loss-delayed/cross-paired outliers."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarShockHandoff(
	TEXT("ncp.ShockHandoff"), 1,
	TEXT("Client confirmed-stop handoff: after a replicated server stop, destroy the fake + reveal the real at raw replicated truth. 1=on (dogfood default), 0=off."),
	ECVF_Default);

// COMMIT 2: owning-client local-stop contradiction recovery. Default ON for dogfood; 0 is a live
// kill switch back to the old local-stop handoff behaviour. Pure client reconciliation, no net fields.
static TAutoConsoleVariable<int32> CVarShockLocalStopRecovery(
	TEXT("ncp.ShockLocalStopRecovery"), 1,
	TEXT("Recover a paired owning-client core from a local-only WorldStatic stop using the collision-free server-anchor estimate. 1=on (dogfood default), 0=off (old handoff behaviour)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockRecoveryCorrectionSpeed(
	TEXT("ncp.ShockRecoveryCorrectionSpeed"), 1200.f,
	TEXT("Maximum positional correction speed toward the authoritative estimate during local-stop recovery (uu/s; clamped 0..10000)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarShockRecoveryMaxTime(
	TEXT("ncp.ShockRecoveryMaxTime"), 0.35f,
	TEXT("Maximum local-stop recovery duration in seconds before collision is restored and recovery ends (clamped 0.05..1.0)."),
	ECVF_Default);

static FORCEINLINE bool ShockDbg()
{
	return CVarShockDebug.GetValueOnGameThread() > 0;
}

static FORCEINLINE const TCHAR* ShockDbgSide(const AActor* A)
{
	return (A && A->Role == ROLE_Authority) ? TEXT("SRV") : TEXT("CLI");
}

// Zero-length WorldStatic sphere sweep at the core's location — true if embedded in
// static geometry. Shared by diagnostics, server stuck cleanup, and short client recovery.
static bool ShockWorldStaticUnder(const AActor* Self, USphereComponent* Collision)
{
	if (!Self || !Collision || !Self->GetWorld())
	{
		return false;
	}
	FHitResult H;
	const float R = FMath::Max(2.f, Collision->GetScaledSphereRadius());
	return Self->GetWorld()->SweepSingleByChannel(H, Self->GetActorLocation(), Self->GetActorLocation(),
		FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(R),
		FCollisionQueryParams(TEXT("ShockDbgGeo"), false, Self));
}

static void ShockDumpAll()
{
	int32 WorldCount = 0;
	int32 CoreCount = 0;
	if (GEngine != nullptr)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World == nullptr || !World->IsGameWorld())
			{
				continue;
			}
			WorldCount++;

			UDemoNetDriver* DemoDriver = World->DemoNetDriver;
			const bool bReplayPlayback = DemoDriver != nullptr && DemoDriver->IsPlaying();
			const bool bReplayFastForward = bReplayPlayback && DemoDriver->IsFastForwarding();
			const bool bWorldPaused = World->IsPaused();

			APlayerController* ListenerPC = nullptr;
			for (FConstPlayerControllerIterator PCIt = World->GetPlayerControllerIterator(); PCIt; ++PCIt)
			{
				APlayerController* Candidate = PCIt->Get();
				if (Candidate != nullptr && Candidate->IsLocalPlayerController())
				{
					ListenerPC = Candidate;
					break;
				}
			}

			FVector ListenerLocation = FVector::ZeroVector;
			FRotator ListenerRotation = FRotator::ZeroRotator;
			const bool bHasListener = ListenerPC != nullptr;
			if (bHasListener)
			{
				ListenerPC->GetPlayerViewPoint(ListenerLocation, ListenerRotation);
			}

			AActor* ViewTarget = ListenerPC != nullptr ? ListenerPC->GetViewTarget() : nullptr;
			UE_LOG(LogShockDbg, Warning,
				TEXT("[ShockDbg/DUMP] WORLD name=%s path=%s type=%d netMode=%d replay=%d paused=%d fastForward=%d worldTime=%.3f realTime=%.3f listener=%s viewTarget=%s listenerLoc=%s listenerRot=%s"),
				*World->GetName(), *World->GetPathName(), (int32)Context.WorldType, (int32)World->GetNetMode(),
				bReplayPlayback ? 1 : 0, bWorldPaused ? 1 : 0, bReplayFastForward ? 1 : 0,
				World->GetTimeSeconds(), World->GetRealTimeSeconds(),
				ListenerPC ? *ListenerPC->GetName() : TEXT("none"),
				ViewTarget ? *ViewTarget->GetName() : TEXT("none"),
				bHasListener ? *ListenerLocation.ToString() : TEXT("none"),
				bHasListener ? *ListenerRotation.ToString() : TEXT("none"));

			for (TActorIterator<AUTPlusProj_ShockBall> It(World); It; ++It)
			{
				AUTPlusProj_ShockBall* Core = *It;
				if (Core == nullptr)
				{
					continue;
				}
				CoreCount++;
				TArray<UAudioComponent*> AudioComponents;
				Core->GetComponents<UAudioComponent>(AudioComponents);
				const float CoreListenerDistance = bHasListener
					? FVector::Dist(Core->GetActorLocation(), ListenerLocation)
					: -1.f;
				UE_LOG(LogShockDbg, Warning,
					TEXT("[ShockDbg/DUMP] world=%s replay=%d paused=%d core=%s class=%s age=%.3f life=%.3f role=%d hidden=%d fake=%d master=%s pairedFake=%s exploded=%d pendingKill=%d loc=%s listenerDist=%.1f vel=%s audio=%d"),
					*World->GetName(), bReplayPlayback ? 1 : 0, bWorldPaused ? 1 : 0,
					*Core->GetName(), *Core->GetClass()->GetName(), Core->GetGameTimeSinceCreation(), Core->GetLifeSpan(),
					(int32)Core->Role, Core->bHidden ? 1 : 0, Core->bFakeClientProjectile ? 1 : 0,
					Core->MasterProjectile ? *Core->MasterProjectile->GetName() : TEXT("none"),
					Core->MyFakeProjectile ? *Core->MyFakeProjectile->GetName() : TEXT("none"),
					Core->bExploded ? 1 : 0, Core->IsPendingKillPending() ? 1 : 0,
					*Core->GetActorLocation().ToString(), CoreListenerDistance,
					*Core->GetVelocity().ToString(), AudioComponents.Num());
				for (UAudioComponent* Audio : AudioComponents)
				{
					if (Audio != nullptr)
					{
						const FVector AudioLocation = Audio->GetComponentLocation();
						const float AudioListenerDistance = bHasListener
							? FVector::Dist(AudioLocation, ListenerLocation)
							: -1.f;
						USceneComponent* AttachParent = Audio->GetAttachParent();
						UE_LOG(LogShockDbg, Warning,
							TEXT("[ShockDbg/DUMP]   audioComp=%s playing=%d active=%d sound=%s duration=%.3f owner=%s attachParent=%s loc=%s listenerDist=%.1f"),
							*Audio->GetName(), Audio->IsPlaying() ? 1 : 0, Audio->IsActive() ? 1 : 0,
							Audio->Sound ? *Audio->Sound->GetName() : TEXT("none"),
							Audio->Sound ? Audio->Sound->GetDuration() : 0.f,
							Audio->GetOwner() ? *Audio->GetOwner()->GetName() : TEXT("none"),
							AttachParent ? *AttachParent->GetName() : TEXT("none"),
							*AudioLocation.ToString(), AudioListenerDistance);
					}
				}
			}
		}
	}
	UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/DUMP] complete worlds=%d cores=%d"), WorldCount, CoreCount);
}

static FAutoConsoleCommand CmdShockDump(
	TEXT("ncp.ShockDump"),
	TEXT("Dump every live NetcodePlus shock core and its audio components in all current game/replay worlds."),
	FConsoleCommandDelegate::CreateStatic(&ShockDumpAll));

enum class ENCPKillcamWorldState : uint8
{
	NotKillcam,
	Hidden,
	Visible
};

/** Distinguish UT's retained instant-replay world from ordinary demo playback. The replay driver
 *  alone cannot do that: both worlds report IsPlaying(). Matching the local player's killcam manager
 *  also avoids touching a normal replay, and IsEnabled tracks the stock viewport handoff. */
static ENCPKillcamWorldState GetNCPKillcamWorldState(const UWorld* World)
{
	if (World == nullptr || World->DemoNetDriver == nullptr || !World->DemoNetDriver->IsPlaying())
	{
		return ENCPKillcamWorldState::NotKillcam;
	}

	UGameInstance* GameInstance = World->GetGameInstance();
	if (GameInstance == nullptr)
	{
		return ENCPKillcamWorldState::NotKillcam;
	}

	for (auto PlayerIt = GameInstance->GetLocalPlayerIterator(); PlayerIt; ++PlayerIt)
	{
		const UUTLocalPlayer* LocalPlayer = Cast<UUTLocalPlayer>(*PlayerIt);
		UUTKillcamPlayback* Playback = LocalPlayer ? LocalPlayer->GetKillcamPlaybackManager() : nullptr;
		if (Playback != nullptr && Playback->GetKillcamWorld() == World)
		{
			return Playback->IsEnabled() ? ENCPKillcamWorldState::Visible : ENCPKillcamWorldState::Hidden;
		}
	}

	return ENCPKillcamWorldState::NotKillcam;
}



AUTPlusProj_ShockBall::AUTPlusProj_ShockBall(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	FlightEffectVisual = nullptr;
	bVisualInitialized = false; // Start as false
	VisualInterpSpeed = 100.0f;

	// Curve-diagnostic state is initialised HERE, not in BeginPlay: on a replicated real,
	// PostNetReceiveVelocity fires before BeginPlay (the initial-bunch rep-notify precedes
	// PostNetInit->BeginPlay), so BeginPlay-init would erase the FirstRepVelDir baseline that the
	// pre-BeginPlay PNRV call records. FireLineOrigin is the exception (it needs the spawn location).
	NextCurveLatLog = 20.f;
	NextCurveVelDegLog = 3.f;
	ConvergePullAccum = FVector::ZeroVector;
	NextConvergePullLog = 25.f;
	FirstRepVelDir = FVector::ZeroVector;
	bLoggedFirstRepVel = false;

	// Behavioural pairing state.
	ExpectedDispAccum = 0.f;
	OrphanStoppedStartTime = -1.f;
	bServerConfirmedStop = false;
	AuthEstimateLocation = FVector::ZeroVector;
	AuthEstimateVelocity = FVector::ZeroVector;
	bAuthEstimateValid = false;
	AuthEstimateSampleTime = 0.f;
	bLocalStopRecovery = false;
	bRecoveryIgnoringWorldStatic = false;
	LocalStopRecoveryStartTime = 0.f;
	LocalStopRecoveryStartLocation = FVector::ZeroVector;
	bKillcamAudioSuppressed = false;
}

void AUTPlusProj_ShockBall::NotifyClientSideHit(AUTPlayerController* InstigatedBy, FVector HitLocation, AActor* DamageCauser, int32 Damage)
{
	// BARE MINIMUM VALIDATION
	// If the core is marked for deletion or has already exploded on the server, deny the combo.
	if (IsPendingKillPending() || bExploded)
	{
		return;
	}

	// If it's still alive, pass control back to Epic's standard logic
	Super::NotifyClientSideHit(InstigatedBy, HitLocation, DamageCauser, Damage);
}


void AUTPlusProj_ShockBall::PerformCombo(class AController* InstigatedBy, class AActor* DamageCauser)
{
	if (ShockDbg() && Role == ROLE_Authority)
	{
		UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/SRV] COMBO @%s vel=%.1f geoUnder=%d age=%.3f"),
			*GetActorLocation().ToString(), ProjectileMovement ? ProjectileMovement->Velocity.Size() : -1.f,
			ShockWorldStaticUnder(this, CollisionComp) ? 1 : 0, GetWorld()->GetTimeSeconds() - CreationTime);
	}

	// Consume extra ammo for the combo
	if (Role == ROLE_Authority)
	{
		AUTGameMode* GameMode = GetWorld()->GetAuthGameMode<AUTGameMode>();
		AUTWeapon* Weapon = Cast<AUTWeapon>(DamageCauser);
		if (Weapon && (!GameMode || GameMode->bAmmoIsLimited || (Weapon->Ammo > 9)))
		{
			Weapon->AddAmmo(-ComboAmmoCost);
		}

		// This gets called before server startfire(). bPlayComboEffects = true will send the FireExtra when fired
		AUTCharacter* UTC = (InstigatedBy != nullptr) ? Cast<AUTCharacter>(InstigatedBy->GetPawn()) : nullptr;

		// CHANGED: Look for UTPlusShockRifle instead of UTWeap_ShockRifle
		AUTPlusShockRifle* ShockRifle = (UTC != nullptr) ? Cast<AUTPlusShockRifle>(UTC->GetWeapon()) : nullptr;
		if (ShockRifle != nullptr)
		{
			ShockRifle->bPlayComboEffects = true;
		}
	}

	// The player who combos gets the credit
	InstigatorController = InstigatedBy;

	// Replicate combo and execute locally
	bComboExplosion = true;
	OnRep_ComboExplosion();
	Explode(GetActorLocation(), FVector(1.0f, 0.0f, 0.0f));
}


bool AUTPlusProj_ShockBall::ShouldIgnoreHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp)
{
	// Fake projectiles should NEVER process hits locally — the server handles
	// all collision. Without this, the fake shock core hits a wall on the client,
	// stops/embeds without exploding (fakes don't have authority to explode),
	// and remains alive + combo-able. The combo RPC reaches the server where
	// the real projectile may still be in flight (due to latency), giving a
	// free wall-combo that shouldn't exist.
	if (bFakeClientProjectile)
	{
		return true;
	}
	return Super::ShouldIgnoreHit_Implementation(OtherActor, OtherComp);
}

void AUTPlusProj_ShockBall::DamageImpactedActor_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp, const FVector& HitLocation, const FVector& HitNormal)
{
	// ACCURACY FIX (server-authoritative): the shock core has no ProcessHit override, so hook the
	// direct-impact credit here. AUTProjectile::DamageImpactedActor credits a full accuracy hit
	// (StatsHitCredit defaults to 1.0, no pawn check) for any non-null OtherActor — a core that hits
	// or embeds in a static-mesh wall inflates ShockRifleHits. Zero the credit for non-pawn impacts
	// so only player hits count. Pawn hits keep the default credit; the combo/radial path in Explode
	// resets StatsHitCredit itself, so this affects only the buggy direct-impact line.
	if (Role == ROLE_Authority && Cast<APawn>(OtherActor) == nullptr)
	{
		StatsHitCredit = 0.f;
	}
	Super::DamageImpactedActor_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
}

void AUTPlusProj_ShockBall::SetOriginalFireDirection(const FVector& Dir)
{
	OriginalFireDirection = Dir;
	bHasCachedFireDirection = true;
}

void AUTPlusProj_ShockBall::CaptureRawAuthoritativeAnchor()
{
	if (GetNetMode() != NM_Client || bFakeClientProjectile || GetWorld() == nullptr)
	{
		return;
	}

	const FVector RawLocation = UTProjReplicatedMovement.Location;
	const FVector RawVelocity = UTProjReplicatedMovement.LinearVelocity;
	float PredictionTime = 0.f;
	AUTPlayerController* MyPlayer = Cast<AUTPlayerController>(InstigatorController
		? InstigatorController : GEngine->GetFirstLocalPlayerController(GetWorld()));
	if (MyPlayer != nullptr && !RawVelocity.IsNearlyZero(2.f))
	{
		PredictionTime = FMath::Clamp(MyPlayer->GetPredictionTime(), 0.f, 0.5f);
	}

	// UT shock cores are straight, constant-velocity projectiles between the sparse spawn/stop/explode
	// movement samples. Project the RAW packet to client "server now" without running collision; stock's
	// normal PostNetReceive path is deliberately masked to the fake while paired.
	AuthEstimateLocation = RawLocation + RawVelocity * PredictionTime * CustomTimeDilation;
	AuthEstimateVelocity = RawVelocity;
	AuthEstimateSampleTime = GetWorld()->GetTimeSeconds();
	bAuthEstimateValid = true;

	if (ShockDbg() && MyFakeProjectile != nullptr)
	{
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/CLI] AUTH-ANCHOR core=%s raw=%s projected=%s vel=%s prediction=%.3f fakeDist=%.1f age=%.3f"),
			*GetName(), *RawLocation.ToString(), *AuthEstimateLocation.ToString(), *RawVelocity.ToString(),
			PredictionTime, FVector::Dist(AuthEstimateLocation, MyFakeProjectile->GetActorLocation()),
			GetWorld()->GetTimeSeconds() - CreationTime);
	}
}

void AUTPlusProj_ShockBall::AdvanceAuthoritativeEstimate(float DeltaTime)
{
	if (!bAuthEstimateValid || bServerConfirmedStop || AuthEstimateVelocity.IsNearlyZero(2.f))
	{
		return;
	}
	AuthEstimateLocation += AuthEstimateVelocity * DeltaTime;
	AuthEstimateSampleTime = GetWorld() ? GetWorld()->GetTimeSeconds() : AuthEstimateSampleTime + DeltaTime;
}

FVector AUTPlusProj_ShockBall::GetAuthoritativeTarget() const
{
	return bAuthEstimateValid ? AuthEstimateLocation : FVector(UTProjReplicatedMovement.Location);
}

void AUTPlusProj_ShockBall::BeginFakeProjectileSynch(AUTProjectile* InFakeProjectile)
{
	const bool bFakeStoppedBeforePair = InFakeProjectile != nullptr
		&& InFakeProjectile->ProjectileMovement != nullptr
		&& InFakeProjectile->ProjectileMovement->Velocity.IsNearlyZero(2.f);

	// BeginPlay already CatchupTick'd the replicated real. Prefer the collision-free raw anchor captured
	// by the initial movement rep-notify; fall back to the caught-up real only if that notify did not run.
	if (GetNetMode() == NM_Client && !bFakeClientProjectile && !bAuthEstimateValid && GetWorld() != nullptr)
	{
		AuthEstimateLocation = GetActorLocation();
		AuthEstimateVelocity = GetVelocity();
		AuthEstimateSampleTime = GetWorld()->GetTimeSeconds();
		bAuthEstimateValid = !AuthEstimateVelocity.IsNearlyZero(2.f);
	}
	if (ShockDbg() && InFakeProjectile != nullptr && bAuthEstimateValid && GetWorld() != nullptr)
	{
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/CLI] AUTH-PAIR core=%s anchor=%s vel=%s fake=%s fake@%s error=%.1f age=%.3f"),
			*GetName(), *AuthEstimateLocation.ToString(), *AuthEstimateVelocity.ToString(),
			*InFakeProjectile->GetName(), *InFakeProjectile->GetActorLocation().ToString(),
			FVector::Dist(AuthEstimateLocation, InFakeProjectile->GetActorLocation()),
			GetWorld()->GetTimeSeconds() - CreationTime);
	}
	Super::BeginFakeProjectileSynch(InFakeProjectile);

	// A near-muzzle client-only wall hit can stop the fake before the replicated real arrives. Once the
	// tightly-guarded CanMatchFake grace path pairs it, recover immediately; waiting for the master's
	// zero-velocity detector cannot work because the server real is still moving normally.
	if (bFakeStoppedBeforePair && GetNetMode() == NM_Client && !bFakeClientProjectile
		&& MyFakeProjectile == InFakeProjectile && !bServerConfirmedStop
		&& bAuthEstimateValid && !AuthEstimateVelocity.IsNearlyZero(2.f))
	{
		const bool bStarted = BeginLocalStopRecovery(TEXT("pair-stopped-fake"));
		if (!bStarted && ShockDbg())
		{
			UE_LOG(LogShockDbg, Warning,
				TEXT("[ShockDbg/CLI] RECOVERY-BLOCKED core=%s trigger=pair-stopped-fake anchor=%d anchorVel=%.1f age=%.3f"),
				*GetName(), bAuthEstimateValid ? 1 : 0, AuthEstimateVelocity.Size(),
				GetWorld() ? GetWorld()->GetTimeSeconds() - CreationTime : -1.f);
		}
	}
}

void AUTPlusProj_ShockBall::OnRep_UTProjReplicatedMovement()
{
	// Capture before Super copies the raw packet into ReplicatedMovement and the shock ball's
	// bMoveFakeToReplicatedPos=false path substitutes the fake's contaminated local position.
	CaptureRawAuthoritativeAnchor();
	Super::OnRep_UTProjReplicatedMovement();
}

void AUTPlusProj_ShockBall::RestartRecoveryProjectile(AUTProjectile* Projectile, const FVector& Location, const FVector& Velocity)
{
	if (Projectile == nullptr || Projectile->IsPendingKillPending())
	{
		return;
	}
	Projectile->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	if (Projectile->ProjectileMovement != nullptr && Projectile->CollisionComp != nullptr)
	{
		// UProjectileMovementComponent::StopSimulating clears UpdatedComponent. Velocity alone cannot
		// restart it; rebind the collision root and explicitly re-enable component ticking.
		Projectile->ProjectileMovement->SetUpdatedComponent(Projectile->CollisionComp);
		Projectile->ProjectileMovement->SetActive(true);
		Projectile->ProjectileMovement->SetComponentTickEnabled(true);
		Projectile->ProjectileMovement->Velocity = Velocity;
		Projectile->ProjectileMovement->UpdateComponentVelocity();
	}
}

void AUTPlusProj_ShockBall::SetRecoveryWorldStaticIgnored(bool bIgnore)
{
	AUTProjectile* Fake = (MyFakeProjectile != nullptr && !MyFakeProjectile->IsPendingKillPending())
		? MyFakeProjectile : nullptr;
	if (bIgnore && !bRecoveryIgnoringWorldStatic)
	{
		RecoveryCollisionComponents.Reset();
		RecoveryCollisionResponses.Reset();
		TArray<UPrimitiveComponent*> Components;
		GetComponents<UPrimitiveComponent>(Components);
		if (Fake != nullptr)
		{
			TArray<UPrimitiveComponent*> FakeComponents;
			Fake->GetComponents<UPrimitiveComponent>(FakeComponents);
			Components.Append(FakeComponents);
		}
		for (UPrimitiveComponent* Component : Components)
		{
			if (Component != nullptr)
			{
				RecoveryCollisionComponents.Add(Component);
				RecoveryCollisionResponses.Add(Component->GetCollisionResponseToChannel(ECC_WorldStatic));
				Component->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Ignore);
			}
		}
		bRecoveryIgnoringWorldStatic = true;
	}
	else if (!bIgnore && bRecoveryIgnoringWorldStatic)
	{
		const int32 RestoreCount = FMath::Min(RecoveryCollisionComponents.Num(), RecoveryCollisionResponses.Num());
		for (int32 Index = 0; Index < RestoreCount; ++Index)
		{
			if (RecoveryCollisionComponents[Index].IsValid())
			{
				RecoveryCollisionComponents[Index]->SetCollisionResponseToChannel(ECC_WorldStatic, RecoveryCollisionResponses[Index]);
			}
		}
		RecoveryCollisionComponents.Reset();
		RecoveryCollisionResponses.Reset();
		bRecoveryIgnoringWorldStatic = false;
	}
}

bool AUTPlusProj_ShockBall::BeginLocalStopRecovery(const TCHAR* Trigger)
{
	if (bLocalStopRecovery || CVarShockLocalStopRecovery.GetValueOnGameThread() <= 0
		|| GetNetMode() != NM_Client || bFakeClientProjectile || bServerConfirmedStop
		|| !bAuthEstimateValid || AuthEstimateVelocity.IsNearlyZero(2.f)
		|| MyFakeProjectile == nullptr || MyFakeProjectile->IsPendingKillPending() || GetWorld() == nullptr)
	{
		return false;
	}

	bLocalStopRecovery = true;
	LocalStopRecoveryStartTime = GetWorld()->GetTimeSeconds();
	LocalStopRecoveryStartLocation = MyFakeProjectile->GetActorLocation();
	SetRecoveryWorldStaticIgnored(true);
	if (AUTPlusProj_ShockBall* FakeShock = Cast<AUTPlusProj_ShockBall>(MyFakeProjectile))
	{
		// The fake's own drift-correction tick reasserts OriginalFireDirection. Rebase that direction
		// to the replicated server velocity so it cannot undo recovery between master ticks.
		FakeShock->SetOriginalFireDirection(AuthEstimateVelocity.GetSafeNormal());
	}

	// Start from the visible location (normally <1u from the hidden real), not from a possibly distant
	// estimate. The bounded correction below closes error without a visible teleport.
	RestartRecoveryProjectile(this, LocalStopRecoveryStartLocation, AuthEstimateVelocity);
	RestartRecoveryProjectile(MyFakeProjectile, LocalStopRecoveryStartLocation, AuthEstimateVelocity);

	if (ShockDbg())
	{
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/CLI] RECOVERY-BEGIN core=%s trigger=%s at=%s estimate=%s error=%.1f vel=%s age=%.3f"),
			*GetName(), Trigger, *LocalStopRecoveryStartLocation.ToString(), *AuthEstimateLocation.ToString(),
			FVector::Dist(LocalStopRecoveryStartLocation, AuthEstimateLocation), *AuthEstimateVelocity.ToString(),
			GetWorld()->GetTimeSeconds() - CreationTime);
	}
	return true;
}

void AUTPlusProj_ShockBall::EndLocalStopRecovery(const TCHAR* Reason)
{
	if (!bLocalStopRecovery && !bRecoveryIgnoringWorldStatic)
	{
		return;
	}
	SetRecoveryWorldStaticIgnored(false);
	if (ShockDbg() && GetWorld() != nullptr)
	{
		const float RemainingError = (MyFakeProjectile && !MyFakeProjectile->IsPendingKillPending())
			? FVector::Dist(MyFakeProjectile->GetActorLocation(), GetAuthoritativeTarget()) : -1.f;
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/CLI] RECOVERY-END core=%s reason=%s error=%.1f elapsed=%.3f age=%.3f"),
			*GetName(), Reason, RemainingError, GetWorld()->GetTimeSeconds() - LocalStopRecoveryStartTime,
			GetWorld()->GetTimeSeconds() - CreationTime);
	}
	bLocalStopRecovery = false;
}

void AUTPlusProj_ShockBall::TickLocalStopRecovery(float DeltaTime)
{
	if (!bLocalStopRecovery || GetWorld() == nullptr)
	{
		return;
	}
	if (bServerConfirmedStop || MyFakeProjectile == nullptr || MyFakeProjectile->IsPendingKillPending())
	{
		EndLocalStopRecovery(bServerConfirmedStop ? TEXT("confirmed-stop") : TEXT("fake-gone"));
		return;
	}

	// A second local stop can occur immediately after collision is restored. Re-enter the short
	// WorldStatic-ignore phase and fully rebind both stopped movement components.
	if ((ProjectileMovement && ProjectileMovement->Velocity.IsNearlyZero(2.f))
		|| (MyFakeProjectile->ProjectileMovement && MyFakeProjectile->ProjectileMovement->Velocity.IsNearlyZero(2.f)))
	{
		SetRecoveryWorldStaticIgnored(true);
		RestartRecoveryProjectile(this, MyFakeProjectile->GetActorLocation(), AuthEstimateVelocity);
		RestartRecoveryProjectile(MyFakeProjectile, MyFakeProjectile->GetActorLocation(), AuthEstimateVelocity);
	}

	const FVector FakeLocation = MyFakeProjectile->GetActorLocation();
	const FVector Error = GetAuthoritativeTarget() - FakeLocation;
	const float CorrectionSpeed = FMath::Clamp(CVarShockRecoveryCorrectionSpeed.GetValueOnGameThread(), 0.f, 10000.f);
	const FVector Correction = Error.GetClampedToMaxSize(CorrectionSpeed * DeltaTime);
	const FVector CorrectedLocation = FakeLocation + Correction;
	RestartRecoveryProjectile(this, CorrectedLocation, AuthEstimateVelocity);
	RestartRecoveryProjectile(MyFakeProjectile, CorrectedLocation, AuthEstimateVelocity);

	if (bRecoveryIgnoringWorldStatic)
	{
		const float Radius = CollisionComp ? CollisionComp->GetScaledSphereRadius() : 8.f;
		const float ClearanceDistance = FMath::Max(16.f, 2.f * Radius + 2.f);
		const bool bTravelledClearance = FVector::Dist(LocalStopRecoveryStartLocation, CorrectedLocation) >= ClearanceDistance;
		if (bTravelledClearance && !ShockWorldStaticUnder(MyFakeProjectile, MyFakeProjectile->CollisionComp))
		{
			SetRecoveryWorldStaticIgnored(false);
			if (ShockDbg())
			{
				UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] RECOVERY-CLEAR core=%s travelled=%.1f error=%.1f age=%.3f"),
					*GetName(), FVector::Dist(LocalStopRecoveryStartLocation, CorrectedLocation), Error.Size(),
					GetWorld()->GetTimeSeconds() - CreationTime);
			}
		}
	}

	const float Elapsed = GetWorld()->GetTimeSeconds() - LocalStopRecoveryStartTime;
	const float MaxTime = FMath::Clamp(CVarShockRecoveryMaxTime.GetValueOnGameThread(), 0.05f, 1.f);
	if ((!bRecoveryIgnoringWorldStatic && Error.SizeSquared() <= FMath::Square(10.f)) || Elapsed >= MaxTime)
	{
		EndLocalStopRecovery(Elapsed >= MaxTime ? TEXT("timeout") : TEXT("converged"));
	}
}

void AUTPlusProj_ShockBall::BeginPlay()
{
	Super::BeginPlay();
	UpdateKillcamAudioSuppression();

	// During instant replay / demo playback, the real projectile was hidden by
	// BeginFakeProjectileSynch in the original game (fake was rendering authority).
	// No fake exists in demo playback, so the real must become the visual — undo
	// the hidden state that was recorded into the demo stream.
	UWorld* W = GetWorld();
	const bool bInReplay = (W && W->DemoNetDriver && W->DemoNetDriver->IsPlaying());
	if (bInReplay)
	{
		SetActorHiddenInGame(false);
		TArray<USceneComponent*> Components;
		GetComponents<USceneComponent>(Components);
		for (USceneComponent* Comp : Components)
		{
			if (Comp)
			{
				Comp->SetVisibility(true);
			}
		}
	}

	// Cache the original fire direction for drift correction at high fps
	bHasCachedFireDirection = false;
	StuckTime = 0.f;
	LastStuckProgressLoc = GetActorLocation();

	// Spawn origin for CanMatchFake gate 3b + curve diagnostics (needs actor location, so BeginPlay
	// not constructor). The curve-diagnostic LATCH members are constructor-init'd on purpose (see the
	// constructor) and must NOT be reset here, or a replicated real's pre-BeginPlay PNRV baseline is
	// wiped. ExpectedDispAccum resets per spawn (harmless; also constructor-zeroed).
	FireLineOrigin = GetActorLocation();
	ExpectedDispAccum = 0.f;
	if (ProjectileMovement && !ProjectileMovement->Velocity.IsNearlyZero())
	{
		OriginalFireDirection = ProjectileMovement->Velocity.GetSafeNormal();
		bHasCachedFireDirection = true;
	}

	if (Role == ROLE_Authority)
	{
		// Server tick rate. Shipped = fixed 240Hz. Override via ncp.ShockServerTickHz:
		//   0  = 240Hz (shipped);  >0 = that Hz (clamped 30..720);  <0 = unset (tick every frame, stock-like).
		const int32 HzOverride = CVarShockServerTickHz.GetValueOnGameThread();
		float ServerInterval;
		if (HzOverride < 0)       ServerInterval = 0.f;                                                       // every frame (stock-like)
		else if (HzOverride == 0) ServerInterval = 1.f / 240.f;                                               // shipped
		else                      ServerInterval = 1.f / static_cast<float>(FMath::Clamp(HzOverride, 30, 720));
		PrimaryActorTick.TickInterval = ServerInterval;
		if (ProjectileMovement) ProjectileMovement->PrimaryComponentTick.TickInterval = ServerInterval;
	}
	else if (GetNetMode() != NM_DedicatedServer)
	{
		// CLIENT: Ask the Weapon Class for the rate
		int32 TargetHz = AUTWeaponFix::GetTargetProjectileTickRate();

		float ClientInterval = 1.f / static_cast<float>(TargetHz);

		PrimaryActorTick.TickInterval = ClientInterval;
		if (ProjectileMovement)
		{
			ProjectileMovement->PrimaryComponentTick.TickInterval = ClientInterval;
			// Force update immediately
			ProjectileMovement->SetComponentTickInterval(ClientInterval);
		}
	}

	// Deferred PNRV paired comparison (ncp.ShockDebug). The first replicated velocity (the server's
	// true fire heading) is captured by PostNetReceiveVelocity BEFORE this BeginPlay runs, when the
	// fake isn't paired yet. Now that Super::BeginPlay() has run the pairing loop, emit the aim-mismatch
	// comparison against the paired fake's fire line — the measurement the old inline log could never
	// produce (it always saw MyFakeProjectile == null and printed the -2 sentinel).
	if (ShockDbg() && GetNetMode() == NM_Client && !bFakeClientProjectile && bLoggedFirstRepVel)
	{
		const AUTPlusProj_ShockBall* Fake = Cast<AUTPlusProj_ShockBall>(MyFakeProjectile);
		if (Fake != nullptr && Fake->bHasCachedFireDirection)
		{
			const float FakeDot = FirstRepVelDir | Fake->OriginalFireDirection;
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV paired-cmp dotVsFakeFireDir=%.4f deg=%.2f repDir=%s fakeFireDir=%s age=%.3f"),
				FakeDot, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FakeDot, -1.f, 1.f))),
				*FirstRepVelDir.ToString(), *Fake->OriginalFireDirection.ToString(),
				GetWorld()->GetTimeSeconds() - CreationTime);
		}
		else
		{
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV paired-cmp unpaired (no fake at BeginPlay) repDir=%s age=%.3f"),
				*FirstRepVelDir.ToString(), GetWorld()->GetTimeSeconds() - CreationTime);
		}
	}
}



void AUTPlusProj_ShockBall::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	UpdateKillcamAudioSuppression();

	if (GetNetMode() == NM_Client && !bFakeClientProjectile)
	{
		// Keep estimate advancement restricted to a live paired fake, but always service an
		// active recovery. TickLocalStopRecovery owns the null/pending-kill guard and calls
		// EndLocalStopRecovery("fake-gone"), restoring the real's saved WorldStatic response.
		// Previously the outer fake-validity gate made that cleanup branch unreachable.
		if (MyFakeProjectile != nullptr && !MyFakeProjectile->IsPendingKillPending())
		{
			AdvanceAuthoritativeEstimate(DeltaTime);
		}
		TickLocalStopRecovery(DeltaTime);
	}

	// Gate 3b input (behavioural, NOT diagnostics — runs regardless of ncp.ShockDebug). Integrate the
	// fake's own expected travel with the actor tick delta, which is already scaled by CustomTimeDilation.
	// Unlike currentSpeed*wallAge this stays correct under Slomo and FREEZES when the PMC stops, so a
	// penetrating ghost (drift-correct re-asserts full speed while wedged and not displacing) accumulates
	// far past its actual displacement and fails gate 3b, while a legit dilated or late-retransmit fake
	// tracks its displacement and passes. Fake-only; server cores and the client real never accumulate.
	if (bFakeClientProjectile && ProjectileMovement)
	{
		ExpectedDispAccum += ProjectileMovement->Velocity.Size() * DeltaTime;
	}

	// An owning client can create a fake whose server shot never arrives (rejected/unmatched fire,
	// death, or round transition), or whose delayed real cannot pass the pairing gates after the
	// fake has already stopped on geometry. With no master, the stock projectile lifespan leaves
	// that fake visibly rolling in the wall (and playing flight audio) for roughly eight seconds.
	// Reap only the exact orphan signature: local predicted fake, no master, unexploded, and
	// continuously stopped for the configured WORLD-time interval. World time keeps the timeout
	// independent of projectile Slomo. Do not Explode(): the server never confirmed an impact there.
	if (GetNetMode() == NM_Client && bFakeClientProjectile && MasterProjectile == nullptr
		&& !bExploded && !IsPendingKillPending() && ProjectileMovement != nullptr && GetWorld() != nullptr)
	{
		const float Timeout = CVarShockOrphanStoppedTimeout.GetValueOnGameThread();
		if (Timeout > 0.f && ProjectileMovement->Velocity.IsNearlyZero(2.f))
		{
			const float Now = GetWorld()->GetTimeSeconds();
			if (OrphanStoppedStartTime < 0.f)
			{
				OrphanStoppedStartTime = Now;
			}
			else if (Now - OrphanStoppedStartTime >= Timeout)
			{
				AUTPlayerController* MyPlayer = Cast<AUTPlayerController>(InstigatorController
					? InstigatorController
					: (GEngine != nullptr ? GEngine->GetFirstLocalPlayerController(GetWorld()) : nullptr));
				if (MyPlayer != nullptr)
				{
					MyPlayer->FakeProjectiles.Remove(this);
				}

				TArray<UAudioComponent*> AudioComponents;
				GetComponents<UAudioComponent>(AudioComponents);
				for (UAudioComponent* Audio : AudioComponents)
				{
					if (Audio != nullptr)
					{
						Audio->Stop();
					}
				}

				if (ShockDbg())
				{
					UE_LOG(LogShockDbg, Warning,
						TEXT("[ShockDbg/CLI] ORPHAN-REAP fake=%s stoppedFor=%.3f age=%.3f loc=%s"),
						*GetName(), Now - OrphanStoppedStartTime, Now - CreationTime,
						*GetActorLocation().ToString());
				}
				Destroy();
				return;
			}
		}
		else
		{
			// Only continuous stopped time counts. A resumed fake gets a fresh grace interval.
			OrphanStoppedStartTime = -1.f;
		}
	}
	else
	{
		OrphanStoppedStartTime = -1.f;
	}

	// CURVE diagnostics (ncp.ShockDebug, FAKE only — the core the shooter actually sees).
	// Two independent signals, each event-gated by a doubling threshold so a straight core
	// logs NOTHING and a bending one traces its bend in a handful of lines:
	//  - CURVE-LAT: perpendicular distance of the fake from the original fire LINE. The
	//    convergence pull below moves the fake via collision-free SetActorLocation — position only,
	//    velocity untouched — so no velocity check can ever see it. This is the signal
	//    that catches a convergence-driven swoosh.
	//  - CURVE-VEL: heading deviation of the fake's VELOCITY from OriginalFireDirection.
	//    Only meaningful while ncp.ShockDriftCorrect=0 (bisect mode): with the re-assert
	//    on, velocity is snapped back to the fire line every tick and this can only ever
	//    see a single tick of PMC float error.
	if (ShockDbg() && bFakeClientProjectile && bHasCachedFireDirection && ProjectileMovement && !bExploded)
	{
		const FVector Rel = GetActorLocation() - FireLineOrigin;
		const float Fwd = Rel | OriginalFireDirection;
		const float Lat = (Rel - Fwd * OriginalFireDirection).Size();
		if (Lat >= NextCurveLatLog)
		{
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] CURVE-LAT fake lat=%.1f fwd=%.1f @%s fireDir=%s converge=%d driftCorrect=%d age=%.3f"),
				Lat, Fwd, *GetActorLocation().ToString(), *OriginalFireDirection.ToString(),
				CVarShockConverge.GetValueOnGameThread(), CVarShockDriftCorrect.GetValueOnGameThread(),
				GetWorld()->GetTimeSeconds() - CreationTime);
			while (Lat >= NextCurveLatLog) { NextCurveLatLog *= 2.f; }
		}
		if (!ProjectileMovement->Velocity.IsNearlyZero())
		{
			const float VelDot = ProjectileMovement->Velocity.GetSafeNormal() | OriginalFireDirection;
			const float VelDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(VelDot, -1.f, 1.f)));
			if (VelDeg >= NextCurveVelDegLog)
			{
				UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] CURVE-VEL fake deg=%.2f velDir=%s fireDir=%s driftCorrect=%d age=%.3f"),
					VelDeg, *ProjectileMovement->Velocity.GetSafeNormal().ToString(), *OriginalFireDirection.ToString(),
					CVarShockDriftCorrect.GetValueOnGameThread(), GetWorld()->GetTimeSeconds() - CreationTime);
				while (VelDeg >= NextCurveVelDegLog) { NextCurveVelDegLog *= 2.f; }
			}
		}
	}

	// Drift correction: floating-point accumulation in ProjectileMovementComponent
	// rotates the velocity direction slightly per tick; over a slow shock ball's long
	// flight (1000+ ticks) it visibly curves. Re-assert the heading along the original
	// fire line every tick, preserving current speed. Only zero-gravity projectiles need
	// this (shock balls have no arc). Rockets/flak have NO Tick override — they set
	// heading once at spawn (SpawnNetPredictedProjectile enforces Velocity = SpawnRotation
	// * InitialSpeed) and coast ballistically, because they're too short-lived to drift.
	//
	// UNCONDITIONAL on heading: the old `dot > 0.9998` (~1.1 deg) latch was a one-way gate
	// that, once an FPS hitch kicked the heading past threshold, never re-engaged → permanent
	// off-aim cores. Re-asserting every tick fixed that.
	//
	// GATED on WHO runs it (and WHEN). Only the client FAKE (client-authoritative aim) and the
	// SERVER real integrate the full flight via the movement component and accumulate float drift —
	// AND both have their OriginalFireDirection seeded from the TRUE fire line (SetOriginalFireDirection,
	// called only in SpawnNetPredictedProjectile). The client's REPLICATED real is a SimulatedProxy
	// that never goes through that spawn path, so BeginPlay seeds its OriginalFireDirection from the
	// FIRST *quantized* replicated velocity — a slightly-wrong axis. Re-asserting drift on it locks the
	// (still-collidable, later-revealed) real onto that wrong axis = off-aim curve. So skip the client
	// real and let it follow the authoritative replicated path. Also skip on the server when the core
	// is embedded in static geometry, so drift stops re-injecting velocity INTO the wall — that
	// re-inflation is exactly what kept Speed high and starved the old velocity-gated stuck check.
	bool bServerEmbedded = false;
	FHitResult StuckHit;
	if (Role == ROLE_Authority && GetNetMode() != NM_Client && CollisionComp && !bExploded)
	{
		const FVector Loc = GetActorLocation();
		const float ProbeRadius = FMath::Max(2.f, CollisionComp->GetScaledSphereRadius());
		// Zero-length sweep — true only if overlapping static world geometry (BSP, static meshes),
		// not actors like bio globs or movers.
		bServerEmbedded = GetWorld()->SweepSingleByChannel(StuckHit, Loc, Loc,
			FQuat::Identity, ECC_WorldStatic, FCollisionShape::MakeSphere(ProbeRadius),
			FCollisionQueryParams(TEXT("ShockStuckCheck"), false, this));
	}

	if (CVarShockDriftCorrect.GetValueOnGameThread() > 0
		&& bHasCachedFireDirection && ProjectileMovement
		&& !ProjectileMovement->Velocity.IsNearlyZero()
		&& FMath::IsNearlyZero(ProjectileMovement->ProjectileGravityScale)
		&& (bFakeClientProjectile || Role == ROLE_Authority)
		&& !bServerEmbedded)
	{
		const float Speed = ProjectileMovement->Velocity.Size();
		ProjectileMovement->Velocity = OriginalFireDirection * Speed;
	}

	// Stuck-ball detection (server). A core that reaches a wall in flight can end up with its centre
	// PENETRATING the surface; from inside an overlap the movement component stops reporting a clean
	// blocking hit, so it never runs OnStop -> Explode and sits embedded, visibly jittering/"spins",
	// alive and still combo-able. Detect it directly: force-explode when the core is (a) embedded in
	// static geometry AND (b) not making net progress. Both are required so a fast core skimming past
	// a wall (still moving) or a slomo core hovering in open space (not touching geometry) is never
	// wrongly detonated. Velocity-INDEPENDENT on purpose — the drift correction above keeps Speed high
	// on an embedded core, so the previous IsNearlyZero(5.0) velocity gate never fired.
	if (Role == ROLE_Authority && GetNetMode() != NM_Client && CollisionComp && !bExploded)
	{
		const FVector Loc = GetActorLocation();
		const bool bNotTravelling = FVector::DistSquared(Loc, LastStuckProgressLoc)
			< FMath::Square(StuckProgressThreshold);
		if (bServerEmbedded && bNotTravelling)
		{
			if (ShockDbg() && StuckTime == 0.f)
			{
				UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/SRV] STUCK enter @%s vel=%.1f age=%.3f"),
					*Loc.ToString(), ProjectileMovement ? ProjectileMovement->Velocity.Size() : -1.f,
					GetWorld()->GetTimeSeconds() - CreationTime);
			}
			StuckTime += DeltaTime;
			if (StuckTime >= StuckExplodeDelay)
			{
				if (ShockDbg())
				{
					UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/SRV] STUCK force-explode @%s heldFor=%.3f age=%.3f"),
						*Loc.ToString(), StuckTime, GetWorld()->GetTimeSeconds() - CreationTime);
				}
				Explode(Loc, StuckHit.ImpactNormal.IsNearlyZero() ? FVector(0.f, 0.f, 1.f) : StuckHit.ImpactNormal);
				return;
			}
		}
		else
		{
			if (ShockDbg() && StuckTime > 0.f)
			{
				// Rapid enter/reset alternation = embed detection flickering (the old sawtooth).
				UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/SRV] STUCK reset embedded=%d travelling=%d heldFor=%.3f vel=%.1f"),
					bServerEmbedded ? 1 : 0, bNotTravelling ? 0 : 1, StuckTime,
					ProjectileMovement ? ProjectileMovement->Velocity.Size() : -1.f);
			}
			StuckTime = 0.f;
			LastStuckProgressLoc = Loc;
		}
	}

	// In-flight convergence targets the independent collision-free server-anchor estimate,
	// not GetActorLocation() on the hidden real. bMoveFakeToReplicatedPos=false makes stock substitute
	// the fake's position into that real, so the old target was a contaminated local ghost. Keep real
	// and fake co-located after every correction: fake TakeDamage forwards to the master's location for
	// combo reporting, and a visual-only correction would report the wrong point to the server.
	//
	// Correct PHASE rather than snapping position. At high ping a fresh projected server sample can sit
	// behind the locally-fired fake; the former 120u failsafe teleported the visible core backward, after
	// which normal projectile movement sent it forward again. Bound correction to 75% of travel speed,
	// so a behind-target correction can only slow the core — never reverse its visible direction.
	if (CVarShockConverge.GetValueOnGameThread() > 0
		&& GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile
		&& !MyFakeProjectile->IsPendingKillPending()
		&& !bLocalStopRecovery && bAuthEstimateValid
		&& ProjectileMovement && !ProjectileMovement->Velocity.IsNearlyZero(2.0f))
	{
		const FVector FakeLoc = MyFakeProjectile->GetActorLocation();
		const FVector Delta = AuthEstimateLocation - FakeLoc;
		const float DeltaSize = Delta.Size();
		const FVector FakeVelocity = MyFakeProjectile->GetVelocity();
		const float TravelSpeed = FakeVelocity.Size();
		const FVector TravelDirection = FakeVelocity.GetSafeNormal();
		const float PhaseError = Delta | TravelDirection;
		const float PhaseErrorSize = FMath::Abs(PhaseError);

		const float MaxConvergeDist = FMath::Clamp(CVarShockConvergeMaxDist.GetValueOnGameThread(), 60.f, 2000.f);
		if (PhaseErrorSize > 1.f && DeltaSize <= MaxConvergeDist && !TravelDirection.IsNearlyZero())
		{
			const float ConvergeTime = FMath::Clamp(CVarShockConvergeTime.GetValueOnGameThread(), 0.10f, 1.00f);
			const float ConfigMaxSpeed = FMath::Clamp(CVarShockConvergeMaxSpeed.GetValueOnGameThread(), 100.f, 2000.f);
			const float NoReverseMaxSpeed = TravelSpeed * 0.75f;
			const float DesiredCorrectionSpeed = PhaseErrorSize / ConvergeTime;
			const float CorrectionSpeed = FMath::Min3(DesiredCorrectionSpeed, ConfigMaxSpeed, NoReverseMaxSpeed);
			// Move only along the fake's current flight line. Correcting the full 3D delta can bend the
			// rendered core toward a slightly different server aim line — phase correction must not curve it.
			const FVector PhaseTarget = FakeLoc + TravelDirection * PhaseError;
			const FVector CorrectedLocation = FMath::VInterpConstantTo(FakeLoc, PhaseTarget, DeltaTime, CorrectionSpeed);
			const FVector Correction = CorrectedLocation - FakeLoc;
			SetActorLocation(CorrectedLocation, false, nullptr, ETeleportType::TeleportPhysics);
			MyFakeProjectile->SetActorLocation(CorrectedLocation, false, nullptr, ETeleportType::TeleportPhysics);

			// Diagnostics: total phase correction applied this flight. This correction is constrained
			// to the fake's flight line, so any CURVE-LAT event must have another cause.
			if (ShockDbg())
			{
				ConvergePullAccum += Correction;
				const float Pulled = ConvergePullAccum.Size();
				if (Pulled >= NextConvergePullLog)
				{
					UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] CONVERGE-PULL accum=%.1f pullDir=%s estimateDist=%.1f phaseError=%.1f fake@%s age=%.3f"),
						Pulled, *ConvergePullAccum.GetSafeNormal().ToString(), DeltaSize, PhaseError,
						*MyFakeProjectile->GetActorLocation().ToString(),
						GetWorld()->GetTimeSeconds() - CreationTime);
					while (Pulled >= NextConvergePullLog) { NextConvergePullLog *= 2.f; }
				}
			}
		}
	}

	// A local-only stop is a prediction contradiction, not permission to reveal at the local wall.
	// Recover both representations immediately. Only a replicated zero velocity may hand rendering
	// back to the real, and that handoff uses raw replicated truth rather than the contaminated actor.
	if (GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile
		&& !MyFakeProjectile->IsPendingKillPending()
		&& ProjectileMovement && ProjectileMovement->Velocity.IsNearlyZero(2.0f))
	{
		if (!bServerConfirmedStop && CVarShockLocalStopRecovery.GetValueOnGameThread() > 0)
		{
			const bool bStarted = BeginLocalStopRecovery(TEXT("tick-zero"));
			if (!bStarted && ShockDbg())
			{
				UE_LOG(LogShockDbg, Warning,
					TEXT("[ShockDbg/CLI] RECOVERY-BLOCKED core=%s active=%d anchor=%d anchorVel=%.1f fake=%d age=%.3f"),
					*GetName(), bLocalStopRecovery ? 1 : 0, bAuthEstimateValid ? 1 : 0,
					AuthEstimateVelocity.Size(), MyFakeProjectile ? 1 : 0,
					GetWorld()->GetTimeSeconds() - CreationTime);
			}
			// Recovery-on is fail-closed: never reveal at a client-only wall merely because an anchor
			// was unavailable. Wait for replicated stop/explode/destruction instead.
			return;
		}
		if (CVarShockHandoff.GetValueOnGameThread() <= 0)
		{
			return;
		}

		const FVector HandoffLocation = bServerConfirmedStop ? GetAuthoritativeTarget() : GetActorLocation();
		if (bServerConfirmedStop)
		{
			EndLocalStopRecovery(TEXT("confirmed-handoff"));
			SetActorLocation(HandoffLocation, false, nullptr, ETeleportType::TeleportPhysics);
			MyFakeProjectile->SetActorLocation(HandoffLocation, false, nullptr, ETeleportType::TeleportPhysics);
		}
		if (ShockDbg())
		{
			UE_LOG(LogShockDbg, Warning,
				TEXT("[ShockDbg/CLI] HANDOFF core=%s stopSrc=%s real@%s fake@%s target@%s realFakeDist=%.1f rawTruth@%s vel=%.1f geoUnder=%d tickHz=%d age=%.3f"),
				*GetName(),
				bServerConfirmedStop ? TEXT("replicated") : TEXT("local-only"),
				*GetActorLocation().ToString(), *MyFakeProjectile->GetActorLocation().ToString(),
				*HandoffLocation.ToString(),
				FVector::Dist(GetActorLocation(), MyFakeProjectile->GetActorLocation()),
				*UTProjReplicatedMovement.Location.ToString(),
				ProjectileMovement ? ProjectileMovement->Velocity.Size() : -1.f,
				ShockWorldStaticUnder(this, CollisionComp) ? 1 : 0, AUTWeaponFix::GetTargetProjectileTickRate(),
				GetWorld()->GetTimeSeconds() - CreationTime);
		}
		SetActorHiddenInGame(false);
		// BeginFakeProjectileSynch also set every USceneComponent visibility to false —
		// SetActorHiddenInGame alone doesn't undo that.
		TArray<USceneComponent*> Components;
		GetComponents<USceneComponent>(Components);
		for (int32 i = 0; i < Components.Num(); i++)
		{
			Components[i]->SetVisibility(true);
		}
		MyFakeProjectile->Destroy();
		MyFakeProjectile = nullptr;
	}
}


void AUTPlusProj_ShockBall::UpdateKillcamAudioSuppression()
{
	const ENCPKillcamWorldState KillcamState = GetNCPKillcamWorldState(GetWorld());
	if (KillcamState == ENCPKillcamWorldState::NotKillcam)
	{
		return;
	}

	if (KillcamState == ENCPKillcamWorldState::Hidden)
	{
		TArray<UAudioComponent*> AudioComponents;
		GetComponents<UAudioComponent>(AudioComponents);

		int32 StoppedThisCall = 0;
		for (UAudioComponent* AudioComponent : AudioComponents)
		{
			if (AudioComponent != nullptr && AudioComponent->IsPlaying())
			{
				KillcamAudioComponentsToResume.AddUnique(TWeakObjectPtr<UAudioComponent>(AudioComponent));
				AudioComponent->Stop();
				++StoppedThisCall;
			}
		}

		if (!bKillcamAudioSuppressed)
		{
			bKillcamAudioSuppressed = true;
			if (ShockDbg())
			{
				UE_LOG(LogShockDbg, Warning,
					TEXT("[ShockDbg/REPLAY-AUDIO] suppress core=%s components=%d world=%s"),
					*GetName(), StoppedThisCall, *GetWorld()->GetPathName());
			}
		}
		return;
	}

	if (bKillcamAudioSuppressed)
	{
		int32 Resumed = 0;
		for (const TWeakObjectPtr<UAudioComponent>& WeakAudioComponent : KillcamAudioComponentsToResume)
		{
			UAudioComponent* AudioComponent = WeakAudioComponent.Get();
			if (AudioComponent != nullptr && AudioComponent->Sound != nullptr && !AudioComponent->IsPlaying())
			{
				AudioComponent->Play();
				++Resumed;
			}
		}

		KillcamAudioComponentsToResume.Reset();
		bKillcamAudioSuppressed = false;
		if (ShockDbg())
		{
			UE_LOG(LogShockDbg, Warning,
				TEXT("[ShockDbg/REPLAY-AUDIO] resume core=%s components=%d world=%s"),
				*GetName(), Resumed, *GetWorld()->GetPathName());
		}
	}
}

// 1. SHIELD / SLOWMO FIX (Replicated Variable)
void AUTPlusProj_ShockBall::OnRep_Slomo()
{
	Super::OnRep_Slomo();

	if (GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile && !MyFakeProjectile->IsPendingKillPending())
	{
		MyFakeProjectile->Slomo = Slomo;
		MyFakeProjectile->OnRep_Slomo();
	}
}



// 2. WALL / STOP FIX (Velocity Replication)
void AUTPlusProj_ShockBall::PostNetReceiveVelocity(const FVector& NewVelocity)
{
	Super::PostNetReceiveVelocity(NewVelocity);

	// Record a server-confirmed stop (replicated velocity ~0). Sticky, and set BEFORE the pairing
	// early-return so it is a pure "did the server tell us it stopped" signal.
	if (GetNetMode() == NM_Client && !bFakeClientProjectile && NewVelocity.IsNearlyZero(2.0f))
	{
		bServerConfirmedStop = true;
	}

	// CURVE diagnostics: the SERVER's heading as replicated (shock movement replicates at
	// spawn/stop/explode only, so this is cheap). Runs before the pairing early-return on
	// purpose — an unpaired real (e.g. a CanMatchFake reject at ncp.ShockMatchFakeDot 0.95)
	// must still log. First non-stop update = the heading the server actually fired on;
	// compared against the fake's TRUE fire line this measures client/server aim mismatch
	// directly (velocity quantization cannot produce whole degrees at 2415u/s). A later
	// non-stop update on a different heading = the server core itself bent mid-flight —
	// should never happen; if that line ever fires the cause is server-side.
	if (ShockDbg() && GetNetMode() == NM_Client && !bFakeClientProjectile && !NewVelocity.IsNearlyZero(2.0f))
	{
		const FVector NewDir = NewVelocity.GetSafeNormal();
		if (!bLoggedFirstRepVel)
		{
			// DEFERRED comparison: on the initial replicated bunch this runs before the pairing loop
			// (rep-notify precedes PostNetInit->BeginPlay), so MyFakeProjectile is null here. Record
			// the server's true fire heading now; BeginPlay emits 'PNRV paired-cmp' once the fake is
			// known. Constructor-init + no BeginPlay reset keep this baseline intact until then.
			bLoggedFirstRepVel = true;
			FirstRepVelDir = NewDir;
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV first-vel repDir=%s paired=%d age=%.3f (cmp deferred to BeginPlay)"),
				*NewDir.ToString(), MyFakeProjectile ? 1 : 0, GetWorld()->GetTimeSeconds() - CreationTime);
		}
		else
		{
			const float ChangeDot = NewDir | FirstRepVelDir;
			if (ChangeDot < 0.99939f) // > ~2deg heading change between replicated updates
			{
				UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV heading-change deg=%.2f prev=%s new=%s age=%.3f"),
					FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(ChangeDot, -1.f, 1.f))),
					*FirstRepVelDir.ToString(), *NewDir.ToString(), GetWorld()->GetTimeSeconds() - CreationTime);
				// Re-baseline so a sustained bend logs once per change, not once per update.
				FirstRepVelDir = NewDir;
			}
		}
	}

	if (GetNetMode() != NM_Client || bFakeClientProjectile || !MyFakeProjectile || MyFakeProjectile->IsPendingKillPending())
	{
		return;
	}

	const float StopThresh = 2.0f;

	// Server says "I stopped"
	if (NewVelocity.IsNearlyZero(StopThresh))
	{
		EndLocalStopRecovery(TEXT("replicated-stop"));
		const FVector TruthLocation = GetAuthoritativeTarget();
		if (ShockDbg())
		{
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV stop-snap core=%s real@%s fake@%s truth@%s dist=%.1f"),
				*GetName(),
				*GetActorLocation().ToString(), *MyFakeProjectile->GetActorLocation().ToString(),
				*TruthLocation.ToString(),
				FVector::Dist(GetActorLocation(), MyFakeProjectile->GetActorLocation()));
		}
		// Snap BOTH representations to raw replicated truth. GetActorLocation() is not truth here:
		// bMoveFakeToReplicatedPos=false may already have substituted the fake's local position.
		SetActorLocation(TruthLocation, false, nullptr, ETeleportType::TeleportPhysics);
		MyFakeProjectile->SetActorLocation(TruthLocation, false, nullptr, ETeleportType::TeleportPhysics);

		if (ProjectileMovement)
		{
			ProjectileMovement->StopMovementImmediately();
			ProjectileMovement->UpdateComponentVelocity();
		}
		if (MyFakeProjectile->ProjectileMovement)
		{
			MyFakeProjectile->ProjectileMovement->StopMovementImmediately();
			MyFakeProjectile->ProjectileMovement->UpdateComponentVelocity();
		}
		return;
	}

	// A fresh nonzero movement update is authoritative evidence that the core is still travelling, but
	// its projected position can be behind the owning client's fake at high ping. With convergence on,
	// queue that error for bounded phase correction in Tick instead of teleporting backward. Keep the
	// legacy snap only behind the convergence kill-switch as a safety fallback.
	const float MaxDrift = 120.f;
	const FVector TruthLocation = GetAuthoritativeTarget();
	if (FVector::DistSquared(MyFakeProjectile->GetActorLocation(), TruthLocation) > FMath::Square(MaxDrift))
	{
		const float Drift = FVector::Dist(MyFakeProjectile->GetActorLocation(), TruthLocation);
		if (CVarShockConverge.GetValueOnGameThread() > 0)
		{
			if (ShockDbg())
			{
				const float SoftWindow = FMath::Clamp(CVarShockConvergeMaxDist.GetValueOnGameThread(), 60.f, 2000.f);
				UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV soft-resync core=%s dist=%.1f eligible=%d window=%.1f target=%s newVel=%.1f"),
					*GetName(), Drift, Drift <= SoftWindow ? 1 : 0, SoftWindow,
					*TruthLocation.ToString(), NewVelocity.Size());
			}
		}
		else
		{
			if (ShockDbg())
			{
				UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV legacy 120u failsafe snap core=%s dist=%.1f truth=%s newVel=%.1f"),
					*GetName(), Drift, *TruthLocation.ToString(), NewVelocity.Size());
			}
			SetActorLocation(TruthLocation, false, nullptr, ETeleportType::TeleportPhysics);
			MyFakeProjectile->SetActorLocation(TruthLocation, false, nullptr, ETeleportType::TeleportPhysics);
		}
		if (ProjectileMovement)
		{
			ProjectileMovement->Velocity = NewVelocity;
			ProjectileMovement->UpdateComponentVelocity();
		}
		if (MyFakeProjectile->ProjectileMovement)
		{
			MyFakeProjectile->ProjectileMovement->Velocity = NewVelocity;
			MyFakeProjectile->ProjectileMovement->UpdateComponentVelocity();
		}
	}
}

// 3. MID-AIR COLLISION / CANCELLATION FIX (Explosion Replication)
void AUTPlusProj_ShockBall::Explode_Implementation(const FVector& HitLocation, const FVector& HitNormal, UPrimitiveComponent* HitComp)
{
	EndLocalStopRecovery(TEXT("explode"));
	// CRITICAL FIX: If this is a combo, DO NOT touch the fake projectile.
	bool bIsCombo = bComboExplosion || (MyDamageType && MyDamageType->GetName().Contains(TEXT("Combo")));

	if (!bIsCombo && GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile && !MyFakeProjectile->IsPendingKillPending())
	{
		AUTProjectile* VisualFake = MyFakeProjectile;
		VisualFake->SetActorLocation(HitLocation, false, nullptr, ETeleportType::TeleportPhysics);

		if (VisualFake->ProjectileMovement)
		{
			VisualFake->ProjectileMovement->Velocity = FVector::ZeroVector;
		}

		// The replicated master suppresses its own effect while a fake is paired.
		// Let the visible fake play the single wall-impact effect at confirmed truth.
		if (!VisualFake->bExploded)
		{
			if (ShockDbg())
			{
				UE_LOG(LogShockDbg, Warning,
					TEXT("[ShockDbg/CLI] FAKE-EXPLODE confirmed X=%.1f Y=%.1f Z=%.1f"),
					HitLocation.X, HitLocation.Y, HitLocation.Z);
			}
			VisualFake->Explode(HitLocation, HitNormal, HitComp);
		}
	}

	Super::Explode_Implementation(HitLocation, HitNormal, HitComp);
}

void AUTPlusProj_ShockBall::ShutDown()
{
	if (ShockDbg())
	{
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/%s] SHUTDOWN core=%s class=%s fake=%d hidden=%d loc=%s age=%.3f"),
			ShockDbgSide(this), *GetName(), *GetClass()->GetName(), bFakeClientProjectile ? 1 : 0,
			bHidden ? 1 : 0, *GetActorLocation().ToString(), GetWorld() ? GetWorld()->GetTimeSeconds() - CreationTime : -1.f);
	}
	EndLocalStopRecovery(TEXT("shutdown"));
	Super::ShutDown();
}

void AUTPlusProj_ShockBall::Destroyed()
{
	if (ShockDbg())
	{
		TArray<UAudioComponent*> AudioComponents;
		GetComponents<UAudioComponent>(AudioComponents);
		int32 PlayingAudio = 0;
		for (UAudioComponent* Audio : AudioComponents)
		{
			PlayingAudio += (Audio != nullptr && Audio->IsPlaying()) ? 1 : 0;
		}
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/%s] DESTROYED core=%s class=%s fake=%d hidden=%d exploded=%d audioPlaying=%d/%d loc=%s age=%.3f"),
			ShockDbgSide(this), *GetName(), *GetClass()->GetName(), bFakeClientProjectile ? 1 : 0,
			bHidden ? 1 : 0, bExploded ? 1 : 0, PlayingAudio, AudioComponents.Num(),
			*GetActorLocation().ToString(), GetWorld() ? GetWorld()->GetTimeSeconds() - CreationTime : -1.f);
	}
	EndLocalStopRecovery(TEXT("destroyed"));
	Super::Destroyed();
}

bool AUTPlusProj_ShockBall::CanMatchFake(AUTProjectile* InFakeProjectile, const FVector& VelDir) const
{
	// The call site null-checks every candidate (UTProjectile.cpp:293) — stay defensive anyway.
	if (InFakeProjectile == nullptr)
	{
		return false;
	}

	// Never match an exploded-but-not-yet-destroyed fake: the caller consumes the pairing
	// (RemoveAt, UTProjectile.cpp:325) BEFORE BeginFakeProjectileSynch bails on a corpse
	// (:373-377), so accepting one burns the real's only pairing chance on a dead fake and
	// leaves the real permanently visible.
	if (InFakeProjectile->bExploded)
	{
		if (ShockDbg())
		{
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/%s] CanMatchFake reject fakeProj=%s bExploded"),
				ShockDbgSide(this), *InFakeProjectile->GetName());
		}
		return false;
	}

	const AUTPlusProj_ShockBall* FakeShock = Cast<AUTPlusProj_ShockBall>(InFakeProjectile);
	float FakeAge = -1.f;
	float FakeDisp = -1.f;
	float FakeExpDisp = -1.f;
	if (FakeShock != nullptr && GetWorld() != nullptr)
	{
		FakeAge = GetWorld()->GetTimeSeconds() - InFakeProjectile->CreationTime;
		FakeDisp = FVector::Dist(InFakeProjectile->GetActorLocation(), FakeShock->FireLineOrigin);
		FakeExpDisp = FakeShock->ExpectedDispAccum;
	}

	// GATE 1 — direction. A clean local wall stop zeroes the fake's PMC velocity. If that happens
	// before the real arrives, dot(currentVelocity, serverVelocity) becomes 0 and rejects the correct
	// pair. For a tightly-bounded fresh interval use the direction cached at fake spawn instead; the
	// instigator/distance/staleness gates below still have to pass. Old stopped ghosts retain zero as
	// their match direction and remain rejected at the default non-negative dot gate.
	const FVector FakeCurrentVelocity = InFakeProjectile->GetVelocity();
	const bool bFakeStopped = FakeCurrentVelocity.IsNearlyZero(2.f);
	const float StoppedFakeGrace = FMath::Max(0.f, CVarShockMatchStoppedFakeGrace.GetValueOnGameThread());
	const bool bUseStoppedFakeDirection = bFakeStopped && StoppedFakeGrace > 0.f
		&& FakeAge >= 0.f && FakeAge <= StoppedFakeGrace
		&& FakeShock != nullptr && FakeShock->bHasCachedFireDirection;
	const FVector FakeMatchDirection = bUseStoppedFakeDirection
		? FakeShock->OriginalFireDirection
		: FakeCurrentVelocity.GetSafeNormal();
	const float Dot = (FakeMatchDirection | VelDir);
	const float MatchGate = FMath::Clamp(CVarShockMatchFakeDot.GetValueOnGameThread(), -1.f, 1.f);
	const bool bDotOK = (Dot > MatchGate);

	// GATE 2 — instigator equality, FAIL-CLOSED on null (the fake-theft fix, see the cvar
	// comment block up top). AActor::Instigator is replicated and applied before the
	// BeginPlay pairing loop runs, so equality against the fake's Instigator (always the
	// local pawn: Params.Instigator = UTOwner at spawn) is decidable here. A null
	// real-Instigator means the shooter's pawn hasn't resolved on this client — that IS
	// the foreign-theft population, so null rejects. null==null also rejects: a GC-nulled
	// fake (owner died in flight) meeting an unresolved foreign core is not a pair. Own
	// reals can't arrive null outside shooter-already-dead edges, where one transient
	// unpaired-but-visible core is the correct conservative outcome.
	const bool bInstGateOn = (CVarShockMatchFakeInstigator.GetValueOnGameThread() != 0);
	const bool bInstOK = !bInstGateOn
		|| (Instigator != nullptr && InFakeProjectile->Instigator == Instigator);

	// GATE 3 — max distance, post-CatchupTick (UTProjectile.cpp:279-283 runs before the
	// pairing loop). Backstop for same-instigator stale fakes gate 2 is blind to:
	// server-rejected fires leave ghost fakes flying their full lifespan kilounits downrange.
	const float MaxDist = CVarShockMatchFakeMaxDist.GetValueOnGameThread();
	const float Dist = FVector::Dist(InFakeProjectile->GetActorLocation(), GetActorLocation());
	const bool bDistOK = (MaxDist <= 0.f) || (Dist <= MaxDist);

	// GATE 3b — staleness/progress. Rejects a PENETRATING ghost fake that defeats gates 1-3: same
	// instigator, and while it is wedged (embedded via overlap, never a clean blocking hit, so
	// StopSimulating never ran) drift-correct keeps re-asserting full speed at dot~1.0 and it sits
	// against the very wall the player refires at, so direction+distance pass. Its tell: expected
	// travel (integrated over the flight) races far past its actual displacement-from-spawn. Expected
	// travel comes from the fake's own ExpectedDispAccum (|vel|*dt each Tick), NOT currentSpeed*wallAge:
	// the accumulator is dilation-correct (Slomo) and FREEZES on a PMC stop.
	// A cleanly-stopped fake's accumulator freezes near its displacement, so this ratio remains ~1.
	// Gate 1 rejects it after the short cached-direction grace; during that grace, pairing is intentional
	// and BeginFakeProjectileSynch immediately starts authoritative local-stop recovery. The age floor
	// keeps every fresh pair untouched; shares the MaxDist kill-switch (<=0 disables gate 3 AND 3b).
	bool bStaleOK = true;
	if (FakeShock != nullptr && FakeAge >= 0.f)
	{
		if (MaxDist > 0.f && FakeAge > 0.3f)
		{
			bStaleOK = (FakeDisp >= 0.25f * FakeExpDisp);
		}
	}

	const bool bMatch = bDotOK && bInstOK && bDistOK && bStaleOK;

	if (ShockDbg())
	{
		// Per-gate verdicts + fake age/displacement. The residual wrong-pair classes
		// (late-retransmit real vs stale ghost) read identical on dot/dist/inst and differ
		// ONLY on age/progress, so captures need both fields to be attributable.
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/%s] CanMatchFake %s dot=%.4f (gate %.2f %s src=%s stopped=%d) dist=%.1f (max %.0f %s) inst=%s real=%s fake=%s fakeAge=%.3f fakeDisp=%.1f fakeExpDisp=%.1f stale=%s fakeProj=%s"),
			ShockDbgSide(this), bMatch ? TEXT("ACCEPT") : TEXT("reject"),
			Dot, MatchGate, bDotOK ? TEXT("ok") : TEXT("FAIL"),
			bUseStoppedFakeDirection ? TEXT("cached-stopped") : TEXT("current"), bFakeStopped ? 1 : 0,
			Dist, MaxDist, bDistOK ? TEXT("ok") : TEXT("FAIL"),
			!bInstGateOn ? TEXT("off") : (bInstOK ? TEXT("ok") : TEXT("FAIL")),
			Instigator ? *Instigator->GetName() : TEXT("null"),
			InFakeProjectile->Instigator ? *InFakeProjectile->Instigator->GetName() : TEXT("null"),
			FakeAge, FakeDisp, FakeExpDisp, bStaleOK ? TEXT("ok") : TEXT("FAIL"),
			*InFakeProjectile->GetName());
	}
	return bMatch;
}

void AUTPlusProj_ShockBall::ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp, const FVector& HitLocation, const FVector& HitNormal)
{
	// DIAGNOSTIC ONLY (ncp.ShockDebug) — behaviour unchanged (Super does all the work). Logs when a
	// core's collision resolves against another projectile or a HIDDEN actor: the prime candidate for
	// "bounces off something invisible". The hidden REAL of any paired core keeps its shootable
	// collision (pairing only hides visuals, never disables collision), so a core can collide with an
	// invisible, position-divergent body that the shooter can't see.
	if (ShockDbg() && OtherActor && OtherActor != this)
	{
		AUTProjectile* OtherProj = Cast<AUTProjectile>(OtherActor);
		if (OtherProj != nullptr || (OtherActor->bHidden != 0))
		{
			UE_LOG(LogShockDbg, Warning,
				TEXT("[ShockDbg/%s] PROC-HIT other=%s(%s) otherHidden=%d otherFake=%d hit@%s self@%s selfFake=%d"),
				ShockDbgSide(this), *OtherActor->GetName(), *OtherActor->GetClass()->GetName(),
				(OtherActor->bHidden != 0) ? 1 : 0, (OtherProj && OtherProj->bFakeClientProjectile) ? 1 : 0,
				*HitLocation.ToString(), *GetActorLocation().ToString(), bFakeClientProjectile ? 1 : 0);
		}
	}

	// STUCK-VANISH path and immediate recovery trigger. The dominant "stuck then
	// silently vanished" path: the HIDDEN client real (collision live; pairing hid only visuals) takes
	// a non-ignored LOCAL blocking hit — a wall (the 'Projectile' profile blocks WorldStatic) or another
	// shock core (ShouldIgnoreHit un-ignores shock-vs-shock). Stock ProcessHit then runs the paired
	// branch (UTProjectile.cpp:824-828): forward the hit to the fake, then Destroy() self. On a CLIENT
	// that Destroy() is REFUSED (UWorld::DestroyActor rejects a non-authority, non-net-forced,
	// non-net-temporary actor; AUTProjectile sets bNetTemporary=false), and the forwarded hit is a no-op
	// because our fake's ShouldIgnoreHit returns true — so nothing explodes, the real survives hidden
	// with its PMC stopped, and the velocity-only HANDOFF next Tick is what actually destroys the fake
	// and reveals the real. A wall hit emits none of the existing HANDOFF/REVEAL-SNAP/PROC-HIT lines,
	// which is why it was invisible until now.
	//
	// bReachDestroy MIRRORS stock's gating so destroy=REFUSED is only claimed when stock GENUINELY
	// reaches the Destroy() call: the outer predicate (:807), a non-ignored hit, and a live paired fake.
	// It excludes PAWN hits (the only case with stock's team-ignore early-return at :819-823), so the
	// verdict never speaks for a path that returned before Destroy(); the investigation targets wall /
	// core hits anyway, and the PRE line's other-class makes any pawn hit obvious. ShouldIgnoreHit is a
	// BlueprintNativeEvent but is side-effect-free across this hierarchy (pure casts+reads; the shipped
	// shock BP does not override it), so the one extra evaluation keeps ShockDebug behaviour-neutral.
	const bool bPairedClientReal = GetNetMode() == NM_Client && !bFakeClientProjectile
		&& !bExploded && bHasSpawnedFully && OtherActor != nullptr && OtherActor != this && OtherComp != nullptr
		&& (OtherActor != Instigator || Instigator == nullptr || bCanHitInstigator)
		&& MyFakeProjectile != nullptr && !MyFakeProjectile->IsPendingKillPending();
	const bool bIgnore = bPairedClientReal ? ShouldIgnoreHit(OtherActor, OtherComp) : true;
	const bool bPawn = bPairedClientReal && (Cast<APawn>(OtherActor) != nullptr);
	const bool bReachDestroy = bPairedClientReal && !bIgnore && !bPawn;
	const bool bLocalWorldStaticHit = bReachDestroy && OtherComp->GetCollisionObjectType() == ECC_WorldStatic;
	const bool bPreSelfPK = IsPendingKillPending();
	if (ShockDbg() && bPairedClientReal)
	{
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/CLI] PROC-HIT/real PRE other=%s(%s) ignore=%d pawn=%d reachDestroy=%d selfPK=%d fakePK=%d vel=%.1f tearOff=%d netTemp=%d role=%d hit@%s self@%s age=%.3f"),
			*OtherActor->GetName(), *OtherActor->GetClass()->GetName(),
			bIgnore ? 1 : 0, bPawn ? 1 : 0, bReachDestroy ? 1 : 0, bPreSelfPK ? 1 : 0,
			MyFakeProjectile->IsPendingKillPending() ? 1 : 0,
			ProjectileMovement ? ProjectileMovement->Velocity.Size() : -1.f,
			bTearOff ? 1 : 0, bNetTemporary ? 1 : 0, (int32)Role,
			*HitLocation.ToString(), *GetActorLocation().ToString(),
			GetWorld()->GetTimeSeconds() - CreationTime);
	}

	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);

	// Verdict only where stock genuinely reached Destroy(). Reading members after a same-frame Destroy()
	// is safe (pending-kill defers actual teardown). selfPK 0->0 == the engine refused the destroy.
	if (bReachDestroy)
	{
		const bool bPostSelfPK = IsPendingKillPending();
		const bool bFakeValid = (MyFakeProjectile != nullptr && !MyFakeProjectile->IsPendingKillPending());
		if (ShockDbg())
		{
			UE_LOG(LogShockDbg, Warning,
				TEXT("[ShockDbg/CLI] PROC-HIT/real POST core=%s selfPK %d->%d destroy=%s fakeValid=%d"),
				*GetName(), bPreSelfPK ? 1 : 0, bPostSelfPK ? 1 : 0,
				bPostSelfPK ? TEXT("took-effect") : TEXT("REFUSED"),
				bFakeValid ? 1 : 0);
		}

		// The engine refused Destroy() on this simulated proxy. Recover in the collision callback rather
		// than waiting a frame, when the estimator error is still only a few units and no hitch is visible.
		if (bLocalWorldStaticHit && !bPostSelfPK && bFakeValid && !bServerConfirmedStop)
		{
			BeginLocalStopRecovery(TEXT("process-hit-worldstatic"));
		}
	}
}

void AUTPlusProj_ShockBall::PostNetReceiveLocationAndRotation()
{
	// DIAGNOSTIC ONLY (ncp.ShockDebug) — behaviour unchanged (Super does all the work). Once the fake
	// is gone, the stock implementation snaps the real to its true server position and forward-predicts
	// by GetPredictionTime() (the suspected "reveal teleport"/curve). Log only a large jump on the
	// client real so steady-state updates stay quiet.
	const FVector PreLoc = GetActorLocation();
	const bool bHadFake = (MyFakeProjectile != nullptr);
	Super::PostNetReceiveLocationAndRotation();
	if (ShockDbg() && !bHadFake && !bFakeClientProjectile && Role != ROLE_Authority)
	{
		const float Jump = FVector::Dist(PreLoc, GetActorLocation());
		if (Jump > 20.f)
		{
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] REVEAL-SNAP jump=%.1f %s -> %s age=%.3f"),
				Jump, *PreLoc.ToString(), *GetActorLocation().ToString(), GetWorld()->GetTimeSeconds() - CreationTime);
		}
	}
}
