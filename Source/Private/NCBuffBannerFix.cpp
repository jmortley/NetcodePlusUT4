// Keep the Wipeout healing banner on the authoritative actor root.
//
// BP_BuffBanner_SpawnHealthRegenNP has a non-physical DefaultSceneRoot and a
// physics-simulating PowerupBase child. Actor movement replication corrects the
// root, not that child body, so the base can detach and roll differently on each
// machine. This exact-class spawn hook runs after Blueprint construction on both
// authority and clients, before the first physics tick: every role makes the
// visual base kinematic again, while only authority chooses the floor position.
#include "NetcodePlus.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"

namespace
{

static const TCHAR* GNCBuffBannerClassPath =
	TEXT("/Game/RestrictedAssets/Pickups/Powerups/BuffStations/BP_BuffBanner_SpawnHealthRegenNP.BP_BuffBanner_SpawnHealthRegenNP_C");
static const FName GNCBuffBannerClassName(TEXT("BP_BuffBanner_SpawnHealthRegenNP_C"));
static const TCHAR* GNCBuffBannerBasePrefix = TEXT("PowerupBase");

static const float GNCBuffBannerTraceUp = 25.f;
static const float GNCBuffBannerTraceDown = 2000.f;
static const float GNCBuffBannerFloorClearance = 2.f;
static const float GNCBuffBannerMinFloorNormalZ = 0.5f;

static FDelegateHandle GNCBuffBannerWorldInitHandle;
static FDelegateHandle GNCBuffBannerWorldCleanupHandle;
static TMap<TWeakObjectPtr<UWorld>, FDelegateHandle> GNCBuffBannerSpawnHandles;

static bool NCIsHealthRegenBanner(const AActor* Actor)
{
	return Actor != nullptr
		&& Actor->GetClass() != nullptr
		// This hook sees every spawned projectile and effect. Avoid constructing
		// a full object path unless the cheap class-name filter matches first.
		&& Actor->GetClass()->GetFName() == GNCBuffBannerClassName
		&& Actor->GetClass()->GetPathName().Equals(GNCBuffBannerClassPath, ESearchCase::CaseSensitive);
}

/**
 * Stop the Blueprint's detached child-body simulation locally. Physics state is
 * not replicated, so this must execute independently on the server and clients.
 * Returns the lowest pre-settle world-space bound for authority floor placement.
 */
static bool NCStabilizeBannerComponents(AActor* Banner, float& OutBottomZ)
{
	OutBottomZ = Banner != nullptr ? Banner->GetActorLocation().Z : 0.f;
	if (Banner == nullptr)
	{
		return false;
	}

	USceneComponent* Root = Banner->GetRootComponent();
	TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents;
	Banner->GetComponents(PrimitiveComponents);

	bool bFoundBase = false;
	for (UPrimitiveComponent* Primitive : PrimitiveComponents)
	{
		if (Primitive == nullptr || Primitive == Root)
		{
			continue;
		}

		// This actor's healing overlap and projectile/damage behavior live on
		// other primitives. Touch only the known movable base.
		const bool bNamedPowerupBase = Primitive->GetName().StartsWith(GNCBuffBannerBasePrefix);
		if (!bNamedPowerupBase)
		{
			continue;
		}
		const bool bSimulatingChild = Primitive->BodyInstance.bSimulatePhysics
			|| Primitive->IsSimulatingPhysics();

		const float ComponentBottom = Primitive->Bounds.Origin.Z - Primitive->Bounds.BoxExtent.Z;
		OutBottomZ = bFoundBase ? FMath::Min(OutBottomZ, ComponentBottom) : ComponentBottom;
		bFoundBase = true;

		if (bSimulatingChild)
		{
			Primitive->SetAllPhysicsLinearVelocity(FVector::ZeroVector, false);
			Primitive->SetAllPhysicsAngularVelocity(FVector::ZeroVector, false);
		}
		Primitive->SetEnableGravity(false);
		Primitive->SetSimulatePhysics(false);
		// Preserve the Blueprint collision profile: the station is shootable and
		// destroys itself after enough damage. Ignore only pawns so the now-
		// kinematic base cannot snag the player standing at its spawn XY.
		Primitive->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);

		// Enabling physics detaches a child component in UE4. Reattach it after
		// disabling simulation so future replicated root movement carries it.
		if (Root != nullptr && Primitive->GetAttachParent() == nullptr)
		{
			Primitive->AttachToComponent(Root, FAttachmentTransformRules::KeepWorldTransform);
		}
	}

	return bFoundBase;
}

static bool NCFindBannerFloor(const AActor* Banner, FHitResult& OutHit)
{
	UWorld* World = Banner != nullptr ? Banner->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return false;
	}

	const FVector SpawnLocation = Banner->GetActorLocation();
	const FVector TraceStart = SpawnLocation + FVector(0.f, 0.f, GNCBuffBannerTraceUp);
	const FVector TraceEnd = SpawnLocation - FVector(0.f, 0.f, GNCBuffBannerTraceDown);

	FCollisionQueryParams QueryParams(FName(TEXT("NCBuffBannerFloor")), false, Banner);
	if (Banner->GetInstigator() != nullptr)
	{
		QueryParams.AddIgnoredActor(Banner->GetInstigator());
	}

	const FCollisionObjectQueryParams ObjectParams(ECC_WorldStatic);
	return World->LineTraceSingleByObjectType(OutHit, TraceStart, TraceEnd, ObjectParams, QueryParams)
		&& OutHit.bBlockingHit
		&& !OutHit.bStartPenetrating
		&& OutHit.ImpactNormal.Z >= GNCBuffBannerMinFloorNormalZ;
}

static void NCHandleBuffBannerSpawned(AActor* Actor)
{
	if (!NCIsHealthRegenBanner(Actor))
	{
		return;
	}

	float BaseBottomZ = Actor->GetActorLocation().Z;
	NCStabilizeBannerComponents(Actor, BaseBottomZ);

	// Clients never choose or predict placement. Their kinematic presentation
	// follows the replicated root transform selected below by authority.
	if (!Actor->HasAuthority())
	{
		return;
	}

	Actor->SetReplicates(true);
	Actor->SetReplicateMovement(true);
	Actor->bOnlyRelevantToOwner = false;
	Actor->bAlwaysRelevant = true;

	FHitResult FloorHit;
	const bool bFloorSettled = NCFindBannerFloor(Actor, FloorHit);
	if (bFloorSettled)
	{
		FVector SettledLocation = Actor->GetActorLocation();
		SettledLocation.Z += (FloorHit.ImpactPoint.Z + GNCBuffBannerFloorClearance) - BaseBottomZ;
		Actor->SetActorLocation(SettledLocation, false, nullptr, ETeleportType::TeleportPhysics);
	}

	Actor->ForceNetUpdate();

	UE_LOG(LogGameMode, Verbose, TEXT("[BuffBannerFix] stabilized %s at %s%s"),
		*Actor->GetName(), *Actor->GetActorLocation().ToCompactString(),
		bFloorSettled ? TEXT(" (floor-settled)") : TEXT(" (no floor hit)"));
}

static void NCRegisterBuffBannerWorld(UWorld* World)
{
	const TWeakObjectPtr<UWorld> WorldKey(World);
	if (World == nullptr || !World->IsGameWorld() || GNCBuffBannerSpawnHandles.Contains(WorldKey))
	{
		return;
	}

	const FDelegateHandle Handle = World->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateStatic(&NCHandleBuffBannerSpawned));
	GNCBuffBannerSpawnHandles.Add(WorldKey, Handle);
}

static void NCOnBuffBannerWorldInitialized(UWorld* World, const UWorld::InitializationValues)
{
	NCRegisterBuffBannerWorld(World);
}

static void NCOnBuffBannerWorldCleanup(UWorld* World, bool /*bSessionEnded*/, bool /*bCleanupResources*/)
{
	const TWeakObjectPtr<UWorld> WorldKey(World);
	FDelegateHandle* Handle = GNCBuffBannerSpawnHandles.Find(WorldKey);
	if (World != nullptr && Handle != nullptr && Handle->IsValid())
	{
		World->RemoveOnActorSpawnedHandler(*Handle);
	}
	GNCBuffBannerSpawnHandles.Remove(WorldKey);
}

} // anonymous namespace

void RegisterNCBuffBannerFix()
{
	if (!GNCBuffBannerWorldInitHandle.IsValid())
	{
		GNCBuffBannerWorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddStatic(
			&NCOnBuffBannerWorldInitialized);
	}
	if (!GNCBuffBannerWorldCleanupHandle.IsValid())
	{
		GNCBuffBannerWorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddStatic(
			&NCOnBuffBannerWorldCleanup);
	}

	// Also cover a game world that already exists during a hot reload.
	if (GEngine != nullptr)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			NCRegisterBuffBannerWorld(Context.World());
		}
	}
}

void UnregisterNCBuffBannerFix()
{
	if (GNCBuffBannerWorldInitHandle.IsValid())
	{
		FWorldDelegates::OnPostWorldInitialization.Remove(GNCBuffBannerWorldInitHandle);
		GNCBuffBannerWorldInitHandle.Reset();
	}
	if (GNCBuffBannerWorldCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(GNCBuffBannerWorldCleanupHandle);
		GNCBuffBannerWorldCleanupHandle.Reset();
	}

	for (const TPair<TWeakObjectPtr<UWorld>, FDelegateHandle>& Pair : GNCBuffBannerSpawnHandles)
	{
		UWorld* World = Pair.Key.Get();
		if (World != nullptr && Pair.Value.IsValid())
		{
			World->RemoveOnActorSpawnedHandler(Pair.Value);
		}
	}
	GNCBuffBannerSpawnHandles.Empty();
}
