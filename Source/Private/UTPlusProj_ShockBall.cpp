
#include "UTPlusProj_ShockBall.h"
#include "UTPlusShockRifle.h"
#include "Particles/ParticleSystemComponent.h"
#include "Engine/DemoNetDriver.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "Components/SphereComponent.h"

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
	TEXT("NetcodePlus shock-core diagnostics. 0=off, 1=event logs (pairing/handoff/reveal/stuck/combo/collision)."),
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

static TAutoConsoleVariable<int32> CVarShockServerTickHz(
	TEXT("ncp.ShockServerTickHz"), 0,
	TEXT("Server shock-core tick rate. 0=240Hz (shipped); >0=that Hz (clamped 30..720); <0=unset/tick-every-frame (stock-like). Read at spawn — set BEFORE firing."),
	ECVF_Default);

// CLIENT rollback levers (default 1 = current shipped behaviour; flip to 0 live to kill the feature
// without a rebuild). Both are client-side cosmetic fake/real reconciliation — no replication, no version bump.
static TAutoConsoleVariable<int32> CVarShockConverge(
	TEXT("ncp.ShockConverge"), 1,
	TEXT("Client fake->real convergence interp (the ~700ms, 60u-capped pull of the rendered fake toward the real). 1=on (shipped), 0=off (fake renders its own predicted path, no pull)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarShockHandoff(
	TEXT("ncp.ShockHandoff"), 1,
	TEXT("Client stuck-ball handoff: when the real stops, destroy the fake + reveal the real so the shooter sees the stop. 1=on (shipped), 0=off (no reveal; fake persists until the explode/pairing cleanup)."),
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
// static geometry. Used only inside ShockDbg()-gated blocks (no cost when off).
static bool ShockDbgGeoUnder(const AActor* Self, USphereComponent* Collision)
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
	bServerConfirmedStop = false;
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
			ShockDbgGeoUnder(this, CollisionComp) ? 1 : 0, GetWorld()->GetTimeSeconds() - CreationTime);
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

void AUTPlusProj_ShockBall::BeginPlay()
{
	Super::BeginPlay();

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

	// CURVE diagnostics (ncp.ShockDebug, FAKE only — the core the shooter actually sees).
	// Two independent signals, each event-gated by a doubling threshold so a straight core
	// logs NOTHING and a bending one traces its bend in a handful of lines:
	//  - CURVE-LAT: perpendicular distance of the fake from the original fire LINE. The
	//    convergence pull below moves the fake via AddActorWorldOffset — position only,
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
	if (Role == ROLE_Authority && CollisionComp && !bExploded)
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
	if (Role == ROLE_Authority && CollisionComp && !bExploded)
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

	// Smooth-converge the fake to the real's position over ~700ms while both
	// are in flight. Without this, small position errors at fake/real pairing
	// time (shooter movement parallax, server fast-forward imperfection, sub-
	// frame spawn timing) leave the two visibly offset for the entire flight,
	// producing a "venn diagram" look from oblique viewing angles. Even though
	// the engine hides the real on pairing, residual visual cues from the real
	// (or just the offset itself in cases where pairing partially fails) make
	// the desync noticeable on shock balls because they're spheres — no shape
	// ambiguity to hide behind.
	//
	// Position-based correction so it doesn't conflict with the velocity-based
	// drift correction above (which keeps Velocity locked to OriginalFireDirection
	// on the fake). The fake's velocity vector is untouched; only its actor
	// location shifts gradually toward the real. Time constant 0.7s matches
	// UTComp's NewNet shock projectile INTERP_TIME.
	//
	// Skipped when the real has stopped — the handoff path below handles that
	// by snapping the fake to the real's location and destroying it.
	if (CVarShockConverge.GetValueOnGameThread() > 0
		&& GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile
		&& !MyFakeProjectile->IsPendingKillPending()
		&& ProjectileMovement && !ProjectileMovement->Velocity.IsNearlyZero(2.0f))
	{
		const FVector RealLoc = GetActorLocation();
		const FVector FakeLoc = MyFakeProjectile->GetActorLocation();
		const FVector Delta = RealLoc - FakeLoc;
		const float DeltaSize = Delta.Size();

		// Apply only in the small-drift range. The 120u snap path in
		// PostNetReceiveVelocity catches anything larger (combo, server
		// teleport, replication hiccup) — let it handle those.
		if (DeltaSize > 1.f && DeltaSize < 60.f)
		{
			constexpr float ConvergeTime = 0.7f;
			const float Alpha = FMath::Clamp(DeltaTime / ConvergeTime, 0.f, 1.f);
			const FVector Correction = Delta * Alpha;
			MyFakeProjectile->AddActorWorldOffset(Correction, false, nullptr, ETeleportType::TeleportPhysics);

			// CURVE diagnostics: total pull the convergence has applied to the fake this
			// flight. A sustained one-sided pull (real walking away on a diverged heading,
			// converge dragging the fake after it) is the leading unlogged bend mechanism —
			// pairs with the fake's CURVE-LAT lines. Doubling threshold, same as the rest.
			if (ShockDbg())
			{
				ConvergePullAccum += Correction;
				const float Pulled = ConvergePullAccum.Size();
				if (Pulled >= NextConvergePullLog)
				{
					UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] CONVERGE-PULL accum=%.1f pullDir=%s realFakeDist=%.1f fake@%s age=%.3f"),
						Pulled, *ConvergePullAccum.GetSafeNormal().ToString(), DeltaSize,
						*MyFakeProjectile->GetActorLocation().ToString(),
						GetWorld()->GetTimeSeconds() - CreationTime);
					while (Pulled >= NextConvergePullLog) { NextConvergePullLog *= 2.f; }
				}
			}
		}
	}

	// Stuck-ball handoff: when the real stops (bio goo, wall) but the fake is
	// still the rendering authority (bMoveFakeToReplicatedPos = false), the
	// fake's flight particle stops emitting → invisible to the shooter.
	// Detect the stop and hand rendering back to the real so the shooter
	// sees the same "stuck ball" visual as everyone else.
	if (CVarShockHandoff.GetValueOnGameThread() > 0
		&& GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile
		&& !MyFakeProjectile->IsPendingKillPending()
		&& ProjectileMovement && ProjectileMovement->Velocity.IsNearlyZero(2.0f))
	{
		if (ShockDbg())
		{
			// stopSrc=local-only + geoUnder=0 = the false-handoff population (the real's OWN PMC
			// stopped locally, no replicated stop). replTruthDist = how far this local reveal position
			// is from the last server snapshot (UTProjReplicatedMovement.Location) — the teleport
			// magnitude commit 2 removes by revealing at server truth instead of local sim.
			// geoUnder=1 = a legitimate wall-stop reveal.
			UE_LOG(LogShockDbg, Warning,
				TEXT("[ShockDbg/CLI] HANDOFF stopSrc=%s real@%s fake@%s realFakeDist=%.1f replTruth@%s replTruthDist=%.1f vel=%.1f geoUnder=%d tickHz=%d age=%.3f"),
				bServerConfirmedStop ? TEXT("replicated") : TEXT("local-only"),
				*GetActorLocation().ToString(), *MyFakeProjectile->GetActorLocation().ToString(),
				FVector::Dist(GetActorLocation(), MyFakeProjectile->GetActorLocation()),
				*UTProjReplicatedMovement.Location.ToString(),
				FVector::Dist(GetActorLocation(), (FVector)UTProjReplicatedMovement.Location),
				ProjectileMovement ? ProjectileMovement->Velocity.Size() : -1.f,
				ShockDbgGeoUnder(this, CollisionComp) ? 1 : 0, AUTWeaponFix::GetTargetProjectileTickRate(),
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
	// early-return so it is a pure "did the server tell us it stopped" signal. Log-only in this build;
	// commit 2 gates the HANDOFF / stop-snap reveals on it so the false-handoff population (the real's
	// OWN local PMC stop, with no replicated stop) routes through a contradiction reveal instead of
	// teleporting the fake to a local-sim position.
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
		if (ShockDbg())
		{
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV stop-snap real@%s fake@%s dist=%.1f"),
				*GetActorLocation().ToString(), *MyFakeProjectile->GetActorLocation().ToString(),
				FVector::Dist(GetActorLocation(), MyFakeProjectile->GetActorLocation()));
		}
		// Snap + stop fake
		MyFakeProjectile->SetActorLocation(GetActorLocation(), false, nullptr, ETeleportType::TeleportPhysics);

		if (MyFakeProjectile->ProjectileMovement)
		{
			MyFakeProjectile->ProjectileMovement->StopMovementImmediately();
			MyFakeProjectile->ProjectileMovement->UpdateComponentVelocity();
		}
		return;
	}

	// Optional fail-safe: only if drift is huge (don't fight prediction)
	const float MaxDrift = 120.f;
	if (FVector::DistSquared(MyFakeProjectile->GetActorLocation(), GetActorLocation()) > FMath::Square(MaxDrift))
	{
		if (ShockDbg())
		{
			UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/CLI] PNRV 120u failsafe snap dist=%.1f newVel=%.1f"),
				FVector::Dist(MyFakeProjectile->GetActorLocation(), GetActorLocation()), NewVelocity.Size());
		}
		MyFakeProjectile->SetActorLocation(GetActorLocation(), false, nullptr, ETeleportType::TeleportPhysics);
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
	// CRITICAL FIX: If this is a combo, DO NOT touch the fake projectile.
	bool bIsCombo = bComboExplosion || (MyDamageType && MyDamageType->GetName().Contains(TEXT("Combo")));

	if (!bIsCombo && GetNetMode() == NM_Client && !bFakeClientProjectile && MyFakeProjectile && !MyFakeProjectile->IsPendingKillPending())
	{
		MyFakeProjectile->SetActorLocation(HitLocation, false, nullptr, ETeleportType::TeleportPhysics);

		if (MyFakeProjectile->ProjectileMovement)
		{
			MyFakeProjectile->ProjectileMovement->Velocity = FVector::ZeroVector;
		}
	}

	Super::Explode_Implementation(HitLocation, HitNormal, HitComp);
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

	// GATE 1 — direction. Relaxed 0.5 (~60deg) vs stock 0.95 for sub-frame rotation and
	// quantization differences between the cached transactional rotation and the server's.
	// Tertiary now that gates 2/3 exist; healthy pairs measure dot=1.0000, so tightening
	// toward 0.95 is a live-cvar experiment before any default change.
	const float Dot = (InFakeProjectile->GetVelocity().GetSafeNormal() | VelDir);
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
	// NOTE this build does NOT reject a CLEANLY-stopped fake (velocity 0 -> AUTProjectile::GetVelocity
	// returns 0): its accumulator froze near its real displacement so the ratio ~1 and 3b passes it by
	// design — a stopped fake is rejected here only incidentally by gate 1 (dot=0), which holds while
	// ncp.ShockMatchFakeDot >= 0 (strict '>'; a negative gate deliberately reopens stopped-fake matching).
	// Structural stopped-fake handling needs the real's motion state and is deferred (commit 2). The age
	// floor keeps every fresh pair untouched; shares the MaxDist kill-switch (<=0 disables gate 3 AND 3b).
	bool bStaleOK = true;
	float FakeAge = 0.f;
	float FakeDisp = -1.f;
	float FakeExpDisp = -1.f;
	const AUTPlusProj_ShockBall* FakeShock = Cast<AUTPlusProj_ShockBall>(InFakeProjectile);
	if (FakeShock != nullptr && GetWorld() != nullptr)
	{
		FakeAge = GetWorld()->GetTimeSeconds() - InFakeProjectile->CreationTime;
		FakeDisp = FVector::Dist(InFakeProjectile->GetActorLocation(), FakeShock->FireLineOrigin);
		FakeExpDisp = FakeShock->ExpectedDispAccum;
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
			TEXT("[ShockDbg/%s] CanMatchFake %s dot=%.4f (gate %.2f %s) dist=%.1f (max %.0f %s) inst=%s real=%s fake=%s fakeAge=%.3f fakeDisp=%.1f fakeExpDisp=%.1f stale=%s fakeProj=%s"),
			ShockDbgSide(this), bMatch ? TEXT("ACCEPT") : TEXT("reject"),
			Dot, MatchGate, bDotOK ? TEXT("ok") : TEXT("FAIL"),
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

	// STUCK-VANISH INSTRUMENTATION (ncp.ShockDebug, log-only). The suspected dominant "stuck then
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
	const bool bDiagPairedReal = ShockDbg() && GetNetMode() == NM_Client && !bFakeClientProjectile
		&& !bExploded && bHasSpawnedFully && OtherActor != nullptr && OtherActor != this && OtherComp != nullptr
		&& (OtherActor != Instigator || Instigator == nullptr || bCanHitInstigator)
		&& MyFakeProjectile != nullptr && !MyFakeProjectile->IsPendingKillPending();
	bool bPreSelfPK = false;
	bool bReachDestroy = false;
	if (bDiagPairedReal)
	{
		bPreSelfPK = IsPendingKillPending();
		const bool bIgnore = ShouldIgnoreHit(OtherActor, OtherComp);
		const bool bPawn = (Cast<APawn>(OtherActor) != nullptr);
		bReachDestroy = !bIgnore && !bPawn; // non-pawn + non-ignored + (outer/fake already checked) => stock reaches Destroy()
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
		UE_LOG(LogShockDbg, Warning,
			TEXT("[ShockDbg/CLI] PROC-HIT/real POST selfPK %d->%d destroy=%s fakeValid=%d"),
			bPreSelfPK ? 1 : 0, bPostSelfPK ? 1 : 0,
			bPostSelfPK ? TEXT("took-effect") : TEXT("REFUSED"),
			bFakeValid ? 1 : 0);
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
