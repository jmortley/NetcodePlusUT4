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

	static ConstructorHelpers::FObjectFinder<UStaticMesh> RecoveredPoleMesh(
		TEXT("StaticMesh'/NetcodePlus/Clutch/Objective/SM_DarkHell_Pole.SM_DarkHell_Pole'"));
	if (RecoveredPoleMesh.Succeeded())
	{
		PoleMesh->SetStaticMesh(RecoveredPoleMesh.Object);
	}
}


bool AClutchPoleVisual::HasValidMesh() const
{
	return PoleMesh && PoleMesh->GetStaticMesh();
}
