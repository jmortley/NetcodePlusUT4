// NCPCandyLiftGuard.cpp — see NCPCandyLiftGuard.h for the why.
#include "NetcodePlus.h"
#include "NCPCandyLiftGuard.h"
#include "UTLift.h"
#include "UTPickupHealth.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SCS_Node.h"
#include "EngineUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCPCandyGuard, Log, All);

namespace
{
	// Sweep cadence. Hub lifts travel ~400uu/s, so 0.25s ≈ 100uu between
	// looks; the vertical detection margin below stays ahead of that — and
	// even a missed window can only produce a cosmetic pass-through, never a
	// jam, because hardened candies don't block lift sweeps.
	const float SweepInterval = 0.25f;
	// Slop around platform bounds that counts as "the lift interacts here".
	const float LiftMarginXY = 40.f;
	const float LiftMarginZ = 120.f;
	// How far past a hull face an evicted candy is pushed before floor-tracing.
	const float EvictExitPad = 80.f;
	// A candy slower than this (squared) counts as at rest for Home updates.
	const float RestSpeedSq = 25.f;

	FBox ExpandByMargins(const FBox& Box)
	{
		return Box.ExpandBy(FVector(LiftMarginXY, LiftMarginXY, LiftMarginZ));
	}
}

ANCPCandyLiftGuard::ANCPCandyLiftGuard(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
}

ANCPCandyLiftGuard* ANCPCandyLiftGuard::EnsureSpawned(UWorld* World)
{
	if (World == nullptr || World->GetNetMode() == NM_Client)
	{
		return nullptr;
	}
	for (TActorIterator<ANCPCandyLiftGuard> It(World); It; ++It)
	{
		return *It;
	}
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	return World->SpawnActor<ANCPCandyLiftGuard>(SpawnParams);
}

bool ANCPCandyLiftGuard::IsCandy(const AActor* Actor)
{
	return Actor != nullptr
		&& Actor->IsA(AUTPickupHealth::StaticClass())
		&& Actor->GetClass()->GetName().Contains(TEXT("Candy"));
}

void ANCPCandyLiftGuard::HardenPrim(UPrimitiveComponent* Prim)
{
	if (Prim == nullptr || !Prim->IsSimulatingPhysics())
	{
		return;
	}
	// A lift sweep that blocks on this body is what wedges the platform
	// (bMoveWasBlocked) and desyncs its timeline for good — so it must never
	// block. Worst case is a visual pass-through.
	Prim->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Ignore);
	// Not shootable, not splash-shovable (Jeremy 2026-08-06).
	Prim->SetCollisionResponseToChannel(COLLISION_PROJECTILE, ECR_Ignore);
	Prim->SetCollisionResponseToChannel(COLLISION_TRACE_WEAPON, ECR_Ignore);
	Prim->SetCollisionResponseToChannel(COLLISION_TRACE_WEAPONNOCHARACTER, ECR_Ignore);
	Prim->bIgnoreRadialImpulse = true;
	Prim->bIgnoreRadialForce = true;
}

void ANCPCandyLiftGuard::HardenCandy(AActor* Candy)
{
	TInlineComponentArray<UPrimitiveComponent*> Prims(Candy);
	for (UPrimitiveComponent* Prim : Prims)
	{
		HardenPrim(Prim);
	}
}

void ANCPCandyLiftGuard::HardenClassTemplates(UClass* CandyClass)
{
	// Fix the archetypes up the BP chain: SCS component templates plus the
	// templates behind BeginPlay AddComponent nodes (the bouncing orb mesh
	// lives there). Live physics state doesn't exist on templates, so the
	// response/flag writes are plain data edits — every spawn after this is
	// born hardened. Process-lifetime change, re-applied per map by BeginPlay.
	for (UClass* C = CandyClass; C != nullptr; C = C->GetSuperClass())
	{
		UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(C);
		if (BPClass == nullptr)
		{
			break;
		}
		for (UActorComponent* Template : BPClass->ComponentTemplates)
		{
			HardenPrim(Cast<UPrimitiveComponent>(Template));
		}
		if (BPClass->SimpleConstructionScript != nullptr)
		{
			for (USCS_Node* Node : BPClass->SimpleConstructionScript->GetAllNodes())
			{
				if (Node != nullptr)
				{
					HardenPrim(Cast<UPrimitiveComponent>(Node->ComponentTemplate));
				}
			}
		}
	}
}

void ANCPCandyLiftGuard::BeginPlay()
{
	Super::BeginPlay();
	if (Role == ROLE_Authority)
	{
		// Archetype hardening at match start: the guard spawns from
		// HandleMatchHasStarted, so the mutator's candy class package is long
		// loaded and LoadClass is safe here. Instance hardening in Sweep()
		// still covers any candy class this path doesn't name.
		UClass* CandyClass = LoadClass<AUTPickupHealth>(nullptr,
			TEXT("/Game/Blueprints/ElimPlusStuff/CandyPlaceholder.CandyPlaceholder_C"));
		if (CandyClass != nullptr)
		{
			HardenClassTemplates(CandyClass);
			UE_LOG(LogNCPCandyGuard, Log, TEXT("[CandyGuard] Hardened class templates for %s"), *CandyClass->GetName());
		}
		else
		{
			UE_LOG(LogNCPCandyGuard, Log, TEXT("[CandyGuard] CandyPlaceholder class not found — relying on instance hardening"));
		}
	}
}

void ANCPCandyLiftGuard::RelocateCandy(AActor* Candy, const FVector& Target)
{
	Candy->SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);

	// Simulating components (the bouncing orb mesh) do not follow a kinematic
	// root — move and still them explicitly or the visual stays behind.
	TInlineComponentArray<UPrimitiveComponent*> Prims(Candy);
	for (UPrimitiveComponent* Prim : Prims)
	{
		if (Prim != nullptr && Prim->IsSimulatingPhysics())
		{
			Prim->SetWorldLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
			Prim->SetPhysicsLinearVelocity(FVector::ZeroVector);
			Prim->SetPhysicsAngularVelocity(FVector::ZeroVector);
		}
	}

	// Pickups can be net-dormant; without a flush clients keep the old spot.
	Candy->FlushNetDormancy();
}

bool ANCPCandyLiftGuard::InsideAnyHull(const FVector& Loc, const TArray<FLiftInfo>& Lifts)
{
	for (const FLiftInfo& Info : Lifts)
	{
		if (Info.Hull.IsInside(Loc))
		{
			return true;
		}
	}
	return false;
}

FVector ANCPCandyLiftGuard::PickEvictionTarget(UWorld* World, const FVector& Seed,
	const FVector* Home, const TArray<FLiftInfo>& Lifts) const
{
	if (Home != nullptr && !InsideAnyHull(*Home, Lifts))
	{
		return *Home;
	}

	FCollisionQueryParams Params(FName(TEXT("CandyEvict")), false);
	for (const FLiftInfo& Info : Lifts)
	{
		Params.AddIgnoredActor(Info.Lift);
	}

	const FVector Base = (Home != nullptr) ? *Home : Seed;
	const FLiftInfo* Container = nullptr;
	for (const FLiftInfo& Info : Lifts)
	{
		if (Info.Hull.IsInside(Base))
		{
			Container = &Info;
			break;
		}
	}

	if (Container != nullptr)
	{
		// Candidate exits out each vertical face of the hull, nearest first —
		// the orb should land on the floor BESIDE the lift, not in its shaft.
		struct FExit { float Dist; FVector Spot; };
		TArray<FExit> Exits;
		Exits.Add({ Base.X - Container->Hull.Min.X, FVector(Container->Hull.Min.X - EvictExitPad, Base.Y, Base.Z) });
		Exits.Add({ Container->Hull.Max.X - Base.X, FVector(Container->Hull.Max.X + EvictExitPad, Base.Y, Base.Z) });
		Exits.Add({ Base.Y - Container->Hull.Min.Y, FVector(Base.X, Container->Hull.Min.Y - EvictExitPad, Base.Z) });
		Exits.Add({ Container->Hull.Max.Y - Base.Y, FVector(Base.X, Container->Hull.Max.Y + EvictExitPad, Base.Z) });
		Exits.Sort([](const FExit& A, const FExit& B) { return A.Dist < B.Dist; });

		for (const FExit& Exit : Exits)
		{
			FHitResult Floor;
			if (World->LineTraceSingleByChannel(Floor, Exit.Spot + FVector(0, 0, 150.f),
				Exit.Spot - FVector(0, 0, 4000.f), ECC_WorldStatic, Params))
			{
				return Floor.Location + FVector(0, 0, 40.f);
			}
		}
	}

	// No usable side exit (enclosed shaft): straight down, ignoring lifts.
	// Hardened candies can't jam the lift from there regardless.
	FHitResult Floor;
	if (World->LineTraceSingleByChannel(Floor, Base + FVector(0, 0, 200.f),
		Base - FVector(0, 0, 5000.f), ECC_WorldStatic, Params))
	{
		return Floor.Location + FVector(0, 0, 40.f);
	}
	return Base;
}

void ANCPCandyLiftGuard::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	SweepAccumulator += DeltaTime;
	if (SweepAccumulator >= SweepInterval)
	{
		SweepAccumulator = 0.f;
		Sweep();
	}
}

void ANCPCandyLiftGuard::Sweep()
{
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}

	// --- Lift platforms: bounds, swept-path hull, moved-since-last-sweep ---
	TArray<FLiftInfo> Lifts;
	for (TActorIterator<AUTLift> It(World); It; ++It)
	{
		UPrimitiveComponent* Comp = It->GetEncroachComponent();
		if (Comp == nullptr)
		{
			continue;
		}
		const FVector CompLoc = Comp->GetComponentLocation();
		FVector* LastLoc = LiftCompLastPos.Find(Comp);
		const bool bMoving = LastLoc != nullptr && !CompLoc.Equals(*LastLoc, 2.f);
		LiftCompLastPos.Add(Comp, CompLoc);

		FLiftInfo Info;
		Info.Lift = *It;
		Info.Box = Comp->Bounds.GetBox();
		Info.Detect = ExpandByMargins(Info.Box);

		// Swept path: replay the platform box at every stop, offset from the
		// stop nearest the platform's current center. GetStops() is the lift
		// BP's own data (Generic_Lift implements it for navmesh export); a
		// lift class without it degrades to current-bounds detection, which
		// the moving/embedded checks below still cover.
		FBox Hull = Info.Box;
		const TArray<FVector> Stops = It->GetStops();
		if (Stops.Num() > 0)
		{
			const FVector Center = Info.Box.GetCenter();
			FVector Ref = Stops[0];
			for (const FVector& Stop : Stops)
			{
				if (FVector::DistSquared(Stop, Center) < FVector::DistSquared(Ref, Center))
				{
					Ref = Stop;
				}
			}
			for (const FVector& Stop : Stops)
			{
				Hull += Info.Box.ShiftBy(Stop - Ref);
			}
		}
		Info.Hull = ExpandByMargins(Hull);
		Info.bMoving = bMoving;
		Lifts.Add(Info);
	}

	const float KillZ = World->GetWorldSettings() ? World->GetWorldSettings()->KillZ : -HALF_WORLD_MAX;

	// --- Candies ---
	for (TActorIterator<AUTPickupHealth> It(World); It; ++It)
	{
		AUTPickupHealth* Candy = *It;
		if (Candy->IsPendingKillPending() || !IsCandy(Candy))
		{
			continue;
		}

		const bool bFirstSight = !HardenedCandies.Contains(Candy);
		if (bFirstSight)
		{
			HardenedCandies.Add(Candy);
			HardenCandy(Candy);
		}

		const FVector Loc = Candy->GetActorLocation();
		FVector* Home = CandyHome.Find(Candy);

		// Fell out of the world: bring it back rather than losing the orb.
		if (Loc.Z < KillZ + 100.f)
		{
			const FVector Target = PickEvictionTarget(World, Loc, Home, Lifts);
			if (!Target.Equals(Loc))
			{
				UE_LOG(LogNCPCandyGuard, Log, TEXT("[CandyGuard] %s below KillZ — restoring to %s"),
					*Candy->GetName(), *Target.ToString());
				RelocateCandy(Candy, Target);
			}
			continue;
		}

		// Eviction: near the current platform while it moves or with the orb
		// embedded in it, or freshly spawned anywhere in a lift's swept path
		// (the died-mid-lift case — evict BEFORE the lift is next triggered).
		bool bEvict = false;
		const TCHAR* Why = TEXT("");
		for (const FLiftInfo& Info : Lifts)
		{
			if (Info.Detect.IsInside(Loc) && (Info.bMoving || Info.Box.IsInside(Loc)))
			{
				bEvict = true;
				Why = Info.bMoving ? TEXT("moving platform") : TEXT("embedded in platform");
				break;
			}
		}
		if (!bEvict && bFirstSight && InsideAnyHull(Loc, Lifts))
		{
			bEvict = true;
			Why = TEXT("spawned in lift path");
		}
		if (bEvict)
		{
			const FVector Target = PickEvictionTarget(World, Loc, Home, Lifts);
			UE_LOG(LogNCPCandyGuard, Log, TEXT("[CandyGuard] %s: %s — relocating to %s"),
				*Candy->GetName(), Why, *Target.ToString());
			RelocateCandy(Candy, Target);
			continue;
		}

		// Home upkeep: at rest, grounded, and clear of every lift's swept
		// path — so a lift platform or shaft spot can never become Home.
		if (!InsideAnyHull(Loc, Lifts) && Candy->GetVelocity().SizeSquared() < RestSpeedSq)
		{
			FCollisionQueryParams Params(FName(TEXT("CandyGround")), false, Candy);
			FHitResult Ground;
			if (World->LineTraceSingleByChannel(Ground, Loc, Loc - FVector(0, 0, 150.f), ECC_WorldStatic, Params))
			{
				CandyHome.Add(Candy, Loc);
			}
		}
		else if (Home == nullptr && !InsideAnyHull(Loc, Lifts))
		{
			// Provisional home for a brand-new orb still settling; refined by
			// the rest-update above. In-hull spawns get no Home until evicted.
			CandyHome.Add(Candy, Loc);
		}
	}

	// Drop entries for consumed/destroyed candies and removed lifts.
	for (auto It = CandyHome.CreateIterator(); It; ++It)
	{
		if (!It->Key.IsValid())
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = HardenedCandies.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
	for (auto It = LiftCompLastPos.CreateIterator(); It; ++It)
	{
		if (!It->Key.IsValid())
		{
			It.RemoveCurrent();
		}
	}
}
