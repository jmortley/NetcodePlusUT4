
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
}



void AUTPlusProj_ShockBall::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

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
			// geoUnder=0 with the core continuing to move afterward = a FALSE reveal (invisible-curve);
			// geoUnder=1 = a legitimate wall-stop reveal.
			UE_LOG(LogShockDbg, Warning,
				TEXT("[ShockDbg/CLI] HANDOFF real@%s fake@%s realFakeDist=%.1f vel=%.1f geoUnder=%d tickHz=%d age=%.3f"),
				*GetActorLocation().ToString(), *MyFakeProjectile->GetActorLocation().ToString(),
				FVector::Dist(GetActorLocation(), MyFakeProjectile->GetActorLocation()),
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
	// Relaxed direction check for shock balls. The stock 0.95 dot product threshold
	// is too strict when the cached transactional rotation differs slightly from the
	// server's (sub-frame mouse jitter, network quantization). At 2415 u/s with
	// typically one core in flight, 0.5 (~60 degrees) prevents double-core visuals
	// while still rejecting obviously wrong matches.
	const float Dot = (InFakeProjectile->GetVelocity().GetSafeNormal() | VelDir);
	const float MatchGate = FMath::Clamp(CVarShockMatchFakeDot.GetValueOnGameThread(), -1.f, 1.f);
	const bool bMatch = (Dot > MatchGate);
	if (ShockDbg())
	{
		// A match landing in 0.5-0.95 is one stock (0.95) would have REJECTED — seeds a diverged pair.
		UE_LOG(LogShockDbg, Warning, TEXT("[ShockDbg/%s] CanMatchFake %s dot=%.4f (gate %.2f) fakeRealDist=%.1f"),
			ShockDbgSide(this), bMatch ? TEXT("ACCEPT") : TEXT("reject"), Dot, MatchGate,
			InFakeProjectile ? FVector::Dist(InFakeProjectile->GetActorLocation(), GetActorLocation()) : -1.f);
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
	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
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
