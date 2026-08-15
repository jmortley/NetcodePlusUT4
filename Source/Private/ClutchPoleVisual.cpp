#include "ClutchPoleVisual.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/UObjectGlobals.h"


DEFINE_LOG_CATEGORY_STATIC(LogClutchPoleVisual, Log, All);


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

	// Load the mesh in BeginPlay rather than through ConstructorHelpers here.
	// Native CDOs can be constructed before optional /Game content paks mount;
	// caching a failed constructor-time lookup would keep the pole invisible.
}


void AClutchPoleVisual::BeginPlay()
{
	Super::BeginPlay();

	if (!PoleMesh || PoleMesh->GetStaticMesh())
	{
		return;
	}

	static const TCHAR* PoleMeshPath =
		TEXT("/Game/NetcodePlusOptional/SM_DarkHell_Pole.SM_DarkHell_Pole");
	UStaticMesh* RecoveredPoleMesh = LoadObject<UStaticMesh>(nullptr, PoleMeshPath);
	if (RecoveredPoleMesh)
	{
		PoleMesh->SetStaticMesh(RecoveredPoleMesh);
		PoleMesh->SetVisibility(true, true);
		PoleMesh->SetHiddenInGame(false, true);
		UE_LOG(LogClutchPoleVisual, Log,
			TEXT("Loaded recovered Clutch pole mesh '%s'"), PoleMeshPath);
	}
	else
	{
		UE_LOG(LogClutchPoleVisual, Warning,
			TEXT("Unable to load recovered Clutch pole mesh '%s'"), PoleMeshPath);
	}
}


bool AClutchPoleVisual::HasValidMesh() const
{
	return PoleMesh && PoleMesh->GetStaticMesh();
}
