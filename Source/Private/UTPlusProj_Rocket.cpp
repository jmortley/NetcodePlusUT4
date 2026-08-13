// UTPlusProj_Rocket.cpp
// Enhanced rocket with client-notify projectile rewind support

#include "UTPlusProj_Rocket.h"
#include "UTWeaponFix.h"
#include "UTCharacter.h"
#include "HAL/IConsoleManager.h"

// Diagnostic category (Warning survives Shipping). Side labels distinguish the owning client's
// local-authority fake from a real server projectile; actor IDs connect pairing and lifecycle events.
DEFINE_LOG_CATEGORY_STATIC(LogRocketDbg, Log, All);

// Primary-rocket fake-theft protection. Defaults ON for the dogfood branch; both gates are
// client-only pairing policy and add no replicated fields or server/client ordering requirement.
static TAutoConsoleVariable<int32> CVarRocketMatchFakeInstigator(
	TEXT("ncp.RocketMatchFakeInstigator"), 1,
	TEXT("Primary rocket fake-pairing instigator gate. 1=on (dogfood default, fail-closed on null); 0=disable this gate (live/direction/distance checks remain)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarRocketMatchFakeMaxDist(
	TEXT("ncp.RocketMatchFakeMaxDist"), 1250.f,
	TEXT("Maximum primary rocket real<->fake pairing distance in units after catchup. Default 1250; <=0 disables the distance gate."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarRocketPairDebug(
	TEXT("ncp.RocketPairDebug"), 0,
	TEXT("Event-only primary rocket pairing/lifecycle diagnostics. 0=off (default); 1=candidate, selected-pair, shutdown, and destruction logs."),
	ECVF_Default);

// Owning-client straight-primary presentation. UTComp uses the same high-level model: preserve the
// immediately-fired fake position when the real arrives, then repay the phase error gradually. Keep
// these separately kill-switchable from the theft gates so dogfood can A/B feel without changing
// matching or server behavior.
static TAutoConsoleVariable<int32> CVarRocketSoftSync(
	TEXT("ncp.RocketSoftSync"), 1,
	TEXT("Preserve straight primary/spread rocket fake position at pairing and softly converge phase toward the caught-up server estimate. 1=on (dogfood default), 0=stock hard snap."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarRocketSoftSyncTime(
	TEXT("ncp.RocketSoftSyncTime"), 0.50f,
	TEXT("Desired primary rocket phase-convergence time in seconds (clamped 0.10..1.00). Default 0.50 mirrors UTComp's rocket interpolation window."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarRocketSoftSyncMaxSpeed(
	TEXT("ncp.RocketSoftSyncMaxSpeed"), 1200.f,
	TEXT("Maximum primary rocket phase-correction speed in uu/s (clamped 100..2000 and additionally capped to 75% of travel speed so the rocket cannot reverse)."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarRocketSoftSyncMaxDist(
	TEXT("ncp.RocketSoftSyncMaxDist"), 500.f,
	TEXT("Maximum pre-pair real<->fake distance eligible for primary rocket soft sync (uu; clamped 60..1250). Larger loss/outlier gaps keep stock pairing behavior."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarRocketServerFirstExplosionVisual(
	TEXT("ncp.RocketServerFirstExplosionVisual"), 1,
	TEXT("Guarantee a paired rocket fake plays the authoritative explosion when its server real resolves first, including the stock exploded-master -> fake ShutDown fallback path. 1=on (dogfood default), 0=stock silent shutdown."),
	ECVF_Default);

static FORCEINLINE bool RocketPairDbg()
{
	return CVarRocketPairDebug.GetValueOnGameThread() > 0;
}

static FORCEINLINE const TCHAR* RocketDbgSide(const AUTProjectile* Rocket)
{
	if (Rocket != nullptr && Rocket->GetNetMode() == NM_Client)
	{
		// SpawnNetPredictedProjectile assigns bFakeClientProjectile after SpawnActor returns, which is
		// after BeginPlay. ROLE_Authority in a client process is nevertheless unambiguously the local fake.
		return (Rocket->bFakeClientProjectile || Rocket->Role == ROLE_Authority) ? TEXT("FAKE") : TEXT("CLI");
	}
	return TEXT("SRV");
}

static bool RocketStraightSoftSyncEligible(const AUTPlusProj_Rocket* Real, const AUTProjectile* Fake)
{
	const AUTProj_Rocket* FakeRocket = Cast<AUTProj_Rocket>(Fake);
	return Real != nullptr && FakeRocket != nullptr
		&& Real->GetNetMode() == NM_Client && !Real->bFakeClientProjectile
		&& Real->TargetActor == nullptr && FakeRocket->TargetActor == nullptr
		&& Real->ProjectileMovement != nullptr && Fake->ProjectileMovement != nullptr
		&& FMath::IsNearlyZero(Real->ProjectileMovement->ProjectileGravityScale)
		&& FMath::IsNearlyZero(Fake->ProjectileMovement->ProjectileGravityScale);
}

AUTPlusProj_Rocket::AUTPlusProj_Rocket(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bPrimarySoftSyncActive = false;
	PrimarySyncEstimateLocation = FVector::ZeroVector;
	PrimarySyncEstimateVelocity = FVector::ZeroVector;
	PrimarySyncCorrectionSpeed = 0.f;
	bForcingShutdownExplosion = false;
}

bool AUTPlusProj_Rocket::CanMatchFake(AUTProjectile* InFakeProjectile, const FVector& VelDir) const
{
	if (InFakeProjectile == nullptr)
	{
		return false;
	}

	// The stock caller removes the selected fake before BeginFakeProjectileSynch verifies it. Rejecting
	// a corpse here prevents the real from consuming its only pairing chance on an unusable fake.
	const bool bLiveFake = !InFakeProjectile->IsPendingKillPending() && !InFakeProjectile->bExploded;
	const float Dot = InFakeProjectile->GetVelocity().GetSafeNormal() | VelDir;
	const bool bDirectionOK = bLiveFake && Super::CanMatchFake(InFakeProjectile, VelDir);

	// Foreign replicated projectiles fall back to the first local player controller in stock
	// AUTProjectile::BeginPlay, so class+direction alone lets them search the local player's fake list.
	// Same-instigator equality is the theft fix; unresolved/null ownership fails closed.
	const bool bInstGateOn = CVarRocketMatchFakeInstigator.GetValueOnGameThread() != 0;
	const bool bInstigatorOK = !bInstGateOn
		|| (Instigator != nullptr && InFakeProjectile->Instigator == Instigator);

	const float MaxDist = CVarRocketMatchFakeMaxDist.GetValueOnGameThread();
	const float Dist = FVector::Dist(GetActorLocation(), InFakeProjectile->GetActorLocation());
	const bool bDistanceOK = MaxDist <= 0.f || Dist <= MaxDist;
	const bool bMatch = bLiveFake && bDirectionOK && bInstigatorOK && bDistanceOK;

	if (RocketPairDbg())
	{
		UE_LOG(LogRocketDbg, Warning,
			TEXT("[RocketDbg/%s] CanMatchFake %s real=%s fake=%s live=%d dot=%.4f dir=%s dist=%.1f max=%.0f distGate=%s instGate=%s realInst=%s fakeInst=%s age=%.3f"),
			RocketDbgSide(this), bMatch ? TEXT("ACCEPT") : TEXT("reject"), *GetName(), *InFakeProjectile->GetName(),
			bLiveFake ? 1 : 0, Dot, bDirectionOK ? TEXT("ok") : TEXT("FAIL"),
			Dist, MaxDist, bDistanceOK ? TEXT("ok") : TEXT("FAIL"),
			!bInstGateOn ? TEXT("off") : (bInstigatorOK ? TEXT("ok") : TEXT("FAIL")),
			Instigator ? *Instigator->GetName() : TEXT("null"),
			InFakeProjectile->Instigator ? *InFakeProjectile->Instigator->GetName() : TEXT("null"),
			GetWorld() ? GetWorld()->GetTimeSeconds() - InFakeProjectile->CreationTime : -1.f);
	}

	return bMatch;
}

void AUTPlusProj_Rocket::BeginFakeProjectileSynch(AUTProjectile* InFakeProjectile)
{
	const FVector FakeLocationBefore = InFakeProjectile ? InFakeProjectile->GetActorLocation() : FVector::ZeroVector;
	const FRotator FakeRotationBefore = InFakeProjectile ? InFakeProjectile->GetActorRotation() : FRotator::ZeroRotator;
	const FVector FakeVelocityBefore = InFakeProjectile ? InFakeProjectile->GetVelocity() : FVector::ZeroVector;
	const FVector CaughtUpRealLocation = GetActorLocation();
	const FVector CaughtUpRealVelocity = GetVelocity();
	const float PrePairDistance = InFakeProjectile
		? FVector::Dist(CaughtUpRealLocation, FakeLocationBefore) : -1.f;
	const float SoftSyncWindow = FMath::Clamp(CVarRocketSoftSyncMaxDist.GetValueOnGameThread(), 60.f, 1250.f);
	const bool bUseSoftSync = CVarRocketSoftSync.GetValueOnGameThread() > 0
		&& PrePairDistance >= 0.f && PrePairDistance <= SoftSyncWindow
		&& RocketStraightSoftSyncEligible(this, InFakeProjectile);

	Super::BeginFakeProjectileSynch(InFakeProjectile);
	const bool bPaired = MyFakeProjectile == InFakeProjectile && InFakeProjectile != nullptr;
	const float StockPostPairDistance = bPaired
		? FVector::Dist(GetActorLocation(), InFakeProjectile->GetActorLocation()) : -1.f;

	bPrimarySoftSyncActive = false;
	if (bPaired && bUseSoftSync)
	{
		// Super performs all stock ownership/lifespan/hide bookkeeping, including its hard fake->real
		// teleport. Restore the position the player actually saw before this function returns (so the
		// stock snap never reaches a rendered frame), then repay only longitudinal phase over time.
		InFakeProjectile->SetActorLocation(FakeLocationBefore, false, nullptr, ETeleportType::TeleportPhysics);
		InFakeProjectile->SetActorRotation(FakeRotationBefore, ETeleportType::TeleportPhysics);
		if (InFakeProjectile->ProjectileMovement)
		{
			InFakeProjectile->ProjectileMovement->Velocity = FakeVelocityBefore;
			InFakeProjectile->ProjectileMovement->UpdateComponentVelocity();
		}
		PrimarySyncEstimateLocation = CaughtUpRealLocation;
		PrimarySyncEstimateVelocity = CaughtUpRealVelocity;
		const float SyncTime = FMath::Clamp(CVarRocketSoftSyncTime.GetValueOnGameThread(), 0.10f, 1.00f);
		const float ConfigMaxSpeed = FMath::Clamp(CVarRocketSoftSyncMaxSpeed.GetValueOnGameThread(), 100.f, 2000.f);
		const float FakeTravelSpeed = FakeVelocityBefore.Size();
		const FVector FakeTravelDirection = FakeVelocityBefore.GetSafeNormal();
		const float InitialPhaseError = FMath::Abs((CaughtUpRealLocation - FakeLocationBefore) | FakeTravelDirection);
		PrimarySyncCorrectionSpeed = FMath::Min3(InitialPhaseError / SyncTime, ConfigMaxSpeed, FakeTravelSpeed * 0.75f);
		bPrimarySoftSyncActive = !PrimarySyncEstimateVelocity.IsNearlyZero(2.f)
			&& !FakeTravelDirection.IsNearlyZero() && InitialPhaseError > 1.f
			&& PrimarySyncCorrectionSpeed > 0.f;
	}

	if (RocketPairDbg() && GetNetMode() == NM_Client)
	{
		UE_LOG(LogRocketDbg, Warning,
			TEXT("[RocketDbg/%s] PAIR %s real=%s fake=%s realInst=%s fakeInst=%s preDist=%.1f stockDist=%.1f finalDist=%.1f softSync=%d window=%.1f"),
			RocketDbgSide(this), bPaired ? TEXT("selected") : TEXT("FAILED"), *GetName(),
			InFakeProjectile ? *InFakeProjectile->GetName() : TEXT("null"),
			Instigator ? *Instigator->GetName() : TEXT("null"),
			(InFakeProjectile && InFakeProjectile->Instigator) ? *InFakeProjectile->Instigator->GetName() : TEXT("null"),
			PrePairDistance, StockPostPairDistance,
			bPaired ? FVector::Dist(GetActorLocation(), InFakeProjectile->GetActorLocation()) : -1.f,
			bPrimarySoftSyncActive ? 1 : 0, SoftSyncWindow);
	}
}

void AUTPlusProj_Rocket::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (!bPrimarySoftSyncActive || GetNetMode() != NM_Client || bFakeClientProjectile
		|| MyFakeProjectile == nullptr || MyFakeProjectile->IsPendingKillPending()
		|| MyFakeProjectile->bExploded || bExploded)
	{
		bPrimarySoftSyncActive = false;
		return;
	}

	// Seeking can begin after initial replication; immediately stop straight-line correction if it does.
	if (!RocketStraightSoftSyncEligible(this, MyFakeProjectile))
	{
		bPrimarySoftSyncActive = false;
		return;
	}

	PrimarySyncEstimateLocation += PrimarySyncEstimateVelocity * DeltaTime;
	const FVector FakeLocation = MyFakeProjectile->GetActorLocation();
	const FVector FakeVelocity = MyFakeProjectile->GetVelocity();
	const float TravelSpeed = FakeVelocity.Size();
	const FVector TravelDirection = FakeVelocity.GetSafeNormal();
	const FVector EstimateDelta = PrimarySyncEstimateLocation - FakeLocation;
	const float EstimateDistance = EstimateDelta.Size();
	const float PhaseError = EstimateDelta | TravelDirection;
	const float PhaseErrorSize = FMath::Abs(PhaseError);
	const float MaxDistance = FMath::Clamp(CVarRocketSoftSyncMaxDist.GetValueOnGameThread(), 60.f, 1250.f);

	if (TravelDirection.IsNearlyZero() || EstimateDistance > MaxDistance)
	{
		if (RocketPairDbg())
		{
			UE_LOG(LogRocketDbg, Warning,
				TEXT("[RocketDbg/CLI] SOFTSYNC stop real=%s fake=%s reason=%s estimateDist=%.1f phaseError=%.1f"),
				*GetName(), *MyFakeProjectile->GetName(),
				TravelDirection.IsNearlyZero() ? TEXT("zero-velocity") : TEXT("outlier"),
				EstimateDistance, PhaseError);
		}
		bPrimarySoftSyncActive = false;
		return;
	}

	if (PhaseErrorSize <= 1.f)
	{
		if (RocketPairDbg())
		{
			UE_LOG(LogRocketDbg, Warning,
				TEXT("[RocketDbg/CLI] SOFTSYNC complete real=%s fake=%s estimateDist=%.1f phaseError=%.1f"),
				*GetName(), *MyFakeProjectile->GetName(), EstimateDistance, PhaseError);
		}
		bPrimarySoftSyncActive = false;
		return;
	}

	// Fixed speed is intentional: initialPhase/syncTime completes the ordinary correction in the
	// configured window, matching UTComp. Recomputing remainingPhase/syncTime each tick would create
	// an exponential tail that takes several times longer than the advertised 0.5 seconds.
	const float CorrectionSpeed = FMath::Min(PrimarySyncCorrectionSpeed, TravelSpeed * 0.75f);
	const FVector PhaseTarget = FakeLocation + TravelDirection * PhaseError;
	const FVector CorrectedLocation = FMath::VInterpConstantTo(FakeLocation, PhaseTarget, DeltaTime, CorrectionSpeed);
	MyFakeProjectile->SetActorLocation(CorrectedLocation, false, nullptr, ETeleportType::TeleportPhysics);
}

void AUTPlusProj_Rocket::Explode_Implementation(const FVector& HitLocation, const FVector& HitNormal,
	UPrimitiveComponent* HitComp)
{
	bPrimarySoftSyncActive = false;
	if (CVarRocketServerFirstExplosionVisual.GetValueOnGameThread() > 0
		&& GetNetMode() == NM_Client && !bFakeClientProjectile
		&& MyFakeProjectile != nullptr && !MyFakeProjectile->IsPendingKillPending()
		&& !MyFakeProjectile->bExploded)
	{
		AUTProjectile* VisualFake = MyFakeProjectile;
		VisualFake->SetActorLocation(HitLocation, false, nullptr, ETeleportType::TeleportPhysics);
		if (VisualFake->ProjectileMovement)
		{
			VisualFake->ProjectileMovement->Velocity = FVector::ZeroVector;
			VisualFake->ProjectileMovement->UpdateComponentVelocity();
		}
		if (RocketPairDbg())
		{
			UE_LOG(LogRocketDbg, Warning,
				TEXT("[RocketDbg/CLI] SERVER-FIRST fake-explode real=%s fake=%s hit=%s"),
				*GetName(), *VisualFake->GetName(), *HitLocation.ToString());
		}
		// The paired real suppresses its own effect. Let the visible fake play exactly one effect at
		// authoritative truth; fake projectile explosion remains cosmetic and applies no client damage.
		VisualFake->Explode(HitLocation, HitNormal, HitComp);
	}

	Super::Explode_Implementation(HitLocation, HitNormal, HitComp);
}

void AUTPlusProj_Rocket::ShutDown()
{
	bPrimarySoftSyncActive = false;
	if (RocketPairDbg() && GetNetMode() == NM_Client)
	{
		UE_LOG(LogRocketDbg, Warning,
			TEXT("[RocketDbg/%s] SHUTDOWN rocket=%s fake=%d exploded=%d master=%s myFake=%s hidden=%d loc=%s age=%.3f"),
			RocketDbgSide(this), *GetName(), bFakeClientProjectile ? 1 : 0, bExploded ? 1 : 0,
			MasterProjectile ? *MasterProjectile->GetName() : TEXT("null"),
			MyFakeProjectile ? *MyFakeProjectile->GetName() : TEXT("null"),
			bHidden ? 1 : 0, *GetActorLocation().ToString(),
			GetWorld() ? GetWorld()->GetTimeSeconds() - CreationTime : -1.f);
	}

	// Final visual guarantee. Stock AUTProjectile::Explode suppresses the hidden real's effect whenever
	// MyFakeProjectile exists, then delegates only ShutDown() to that fake. Our real-side Explode hook
	// normally converts the fake first, but Blueprint/lifecycle variants can reach this delegate without
	// executing that hook. If this is the still-unexploded visible fake and its paired master is already
	// exploded, play exactly one cosmetic explosion now instead of silently disappearing. Each rocket in
	// a loaded spread owns its own master/fake pair, so the guarantee applies independently to the volley.
	if (CVarRocketServerFirstExplosionVisual.GetValueOnGameThread() > 0
		&& GetNetMode() == NM_Client && bFakeClientProjectile && !bExploded && !bForcingShutdownExplosion
		&& MasterProjectile != nullptr && !MasterProjectile->IsPendingKillPending()
		&& MasterProjectile->bExploded)
	{
		const FVector ExplosionLocation = MasterProjectile->GetActorLocation();
		FVector ImpactNormal = -GetVelocity().GetSafeNormal();
		if (ImpactNormal.IsNearlyZero())
		{
			ImpactNormal = FVector(0.f, 0.f, 1.f);
		}
		SetActorLocation(ExplosionLocation, false, nullptr, ETeleportType::TeleportPhysics);
		if (ProjectileMovement)
		{
			ProjectileMovement->Velocity = FVector::ZeroVector;
			ProjectileMovement->UpdateComponentVelocity();
		}
		if (RocketPairDbg())
		{
			UE_LOG(LogRocketDbg, Warning,
				TEXT("[RocketDbg/FAKE] SHUTDOWN-FALLBACK explode fake=%s master=%s hit=%s"),
				*GetName(), *MasterProjectile->GetName(), *ExplosionLocation.ToString());
		}
		bForcingShutdownExplosion = true;
		Explode(ExplosionLocation, ImpactNormal, nullptr);
		bForcingShutdownExplosion = false;
		return;
	}
	Super::ShutDown();
}

void AUTPlusProj_Rocket::Destroyed()
{
	bPrimarySoftSyncActive = false;
	if (RocketPairDbg() && GetNetMode() == NM_Client)
	{
		UE_LOG(LogRocketDbg, Warning,
			TEXT("[RocketDbg/%s] DESTROYED rocket=%s fake=%d exploded=%d master=%s myFake=%s hidden=%d loc=%s age=%.3f"),
			RocketDbgSide(this), *GetName(), bFakeClientProjectile ? 1 : 0, bExploded ? 1 : 0,
			MasterProjectile ? *MasterProjectile->GetName() : TEXT("null"),
			MyFakeProjectile ? *MyFakeProjectile->GetName() : TEXT("null"),
			bHidden ? 1 : 0, *GetActorLocation().ToString(),
			GetWorld() ? GetWorld()->GetTimeSeconds() - CreationTime : -1.f);
	}
	Super::Destroyed();
}

void AUTPlusProj_Rocket::BeginPlay()
{
	Super::BeginPlay();
	// DIAGNOSTIC: ROLE_Authority means a true server rocket in a server process, but it also means
	// a locally-spawned fake in a client process. Label the side explicitly so client logs never claim
	// that their own fake was a server actor.
	if (Role == ROLE_Authority && RocketPairDbg())
	{
		AUTCharacter* OwnerChar = Cast<AUTCharacter>(GetInstigator());
		UE_LOG(LogRocketDbg, Warning, TEXT("[RocketDbg/%s] SPAWN rocket=%s fake=%d owner=%s at=%s"),
			RocketDbgSide(this), *GetName(), (bFakeClientProjectile || (GetNetMode() == NM_Client && Role == ROLE_Authority)) ? 1 : 0,
			OwnerChar ? *OwnerChar->GetName() : TEXT("?"),
			*GetActorLocation().ToString());
	}
}

void AUTPlusProj_Rocket::ProcessHit_Implementation(AActor* OtherActor, UPrimitiveComponent* OtherComp,
	const FVector& HitLocation, const FVector& HitNormal)
{
	// CLIENT-SIDE HIT: Notify weapon so server can validate with rewind.
	// This fires on the CLIENT when the replicated (real) rocket overlaps an enemy
	// on the client's local pawn positions. The server may disagree because its
	// capsule positions are different — the RPC gives it a second chance with rewind.
	// Role != ROLE_Authority means we're on the client viewing the replicated rocket.
	if (Role != ROLE_Authority && OtherActor && !bFakeClientProjectile)
	{
		AUTCharacter* HitChar = Cast<AUTCharacter>(OtherActor);
		if (HitChar)
		{
			AUTCharacter* OwnerChar = Cast<AUTCharacter>(GetInstigator());
			// Only the SHOOTER's own client can route the claim — the Server RPC needs the
			// weapon's owning connection. This same ProcessHit also runs on our client for a
			// bot's / remote player's replicated rocket, where GetWeapon() resolves a weapon we
			// don't own → the engine drops the RPC with "No owning connection" spam. Gate to the
			// locally-controlled shooter so only routable claims are sent.
			if (OwnerChar && OwnerChar->IsLocallyControlled())
			{
				// The launcher that FIRED this rocket, not whatever is held now: a mid-flight
				// weapon switch would otherwise route the claim to a weapon that never tracked it.
				AUTWeaponFix* Weapon = AUTWeaponFix::FindFiringWeaponForProjectile(OwnerChar, this);
				if (Weapon)
				{
					Weapon->NotifyFakeProjectileHit(HitChar, HitLocation, 0, this); // FireMode 0 = primary (rockets)
				}
			}
		}
	}

	// SERVER-SIDE: snapshot final state into the weapon's grace buffer BEFORE Super explodes/
	// destroys this projectile, so a claim arriving after the rocket is gone (close-range timing
	// race) can still rewind-rescue. The pawn we directly hit (or null = geometry/whiff) is passed
	// so the grace path won't double-damage a target that already took the present-time hit.
	if (Role == ROLE_Authority)
	{
		// DIAGNOSTIC: what this authority-role rocket hit. RocketDbgSide distinguishes a real server
		// projectile from the owning client's local-authority fake, and the actor ID joins the event
		// to pairing/shutdown/destruction lines.
		if (RocketPairDbg())
		{
			UE_LOG(LogRocketDbg, Warning, TEXT("[RocketDbg/%s] HIT rocket=%s fake=%d other=%s (%s) at=%s"),
				RocketDbgSide(this), *GetName(), bFakeClientProjectile ? 1 : 0,
				OtherActor ? *OtherActor->GetName() : TEXT("none/whiff"),
				Cast<APawn>(OtherActor) ? TEXT("PAWN") : TEXT("non-pawn"),
				*HitLocation.ToString());
		}

		AUTCharacter* OwnerChar = Cast<AUTCharacter>(GetInstigator());
		// Resolve the launcher that fired this rocket, not the weapon held when it explodes.
		// The tracked grace entry lives on the firing weapon, matching the client claim route.
		AUTWeaponFix* Weapon = AUTWeaponFix::FindFiringWeaponForProjectile(OwnerChar, this);
		if (Weapon)
		{
			Weapon->OnTrackedProjectileResolved(this, Cast<AUTCharacter>(OtherActor));
		}
	}

	// ACCURACY FIX (server-authoritative): a direct impact on world geometry (a static-mesh actor;
	// BSP reports a null OtherActor and is already exempt) credits a full accuracy hit in
	// AUTProjectile::DamageImpactedActor — StatsHitCredit defaults to 1.0 with no pawn check, so a
	// rocket detonating against a wall inflates RocketHits. Zero the credit for non-pawn impacts so
	// only player hits count. Pawn hits keep the default credit; the radial/splash path in Explode
	// resets StatsHitCredit itself, so this affects only the buggy direct-impact line.
	if (Role == ROLE_Authority && Cast<APawn>(OtherActor) == nullptr)
	{
		StatsHitCredit = 0.f;
	}

	// Standard processing: damage (server only), explode, etc.
	Super::ProcessHit_Implementation(OtherActor, OtherComp, HitLocation, HitNormal);
}
