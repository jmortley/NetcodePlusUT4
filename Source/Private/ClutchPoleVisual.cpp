#include "ClutchPoleVisual.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"


AClutchPoleVisual::AClutchPoleVisual(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	bNetLoadOnClient = true;
	bReplicateMovement = true;
	bCanBeDamaged = false;
	PrimaryActorTick.bCanEverTick = false;

	PoleMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PoleMesh"));
	RootComponent = PoleMesh;
	PoleMesh->SetMobility(EComponentMobility::Movable);
	PoleMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PoleMesh->SetCollisionResponseToAllChannels(ECR_Ignore);
	PoleMesh->bGenerateOverlapEvents = false;
	PoleMesh->SetRelativeLocation(FVector(0.0f, 0.0f, -3.0f));
	PoleMesh->SetRelativeScale3D(FVector(1.0f, 1.0f, 0.7724762f));

	// Recovered pole mesh intentionally NOT loaded for now (the mesh isn't shipped).
	// Do not resurrect a CDO-time ConstructorHelpers::FObjectFinder for it: the CDO is
	// built during early module startup before the /NetcodePlus/ content mount is ready,
	// so the find fails — and, being a function-static, it caches that failure for the
	// whole session. When the mesh ships, load it at runtime instead (LoadObject in
	// EnsurePoleVisual/BeginPlay, the way the Clutch HUD textures do) and flip
	// AClutchGameMode::bUseRecoveredPoleVisual back on. With no mesh set, HasValidMesh()
	// stays false and the game mode keeps the map's own pole marker.
}


bool AClutchPoleVisual::HasValidMesh() const
{
	return PoleMesh && PoleMesh->GetStaticMesh();
}
