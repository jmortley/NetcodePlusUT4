#include "ShockDomControlPoint.h"
#include "UnrealTournament.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "UTGameState.h"
#include "ShockDomGameMode.h"
#include "Net/UnrealNetwork.h"


AShockDomControlPoint::AShockDomControlPoint(const FObjectInitializer& OI)
	: Super(OI)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	NetUpdateFrequency = 5.f;

	OwningTeamIndex = 255;
	PointIndex = 0;
	LastCaptureTime = 0.f;
	CaptureRadius = 200.f;
	DynMaterial = nullptr;

	// Capture volume — root component
	CaptureVolume = CreateDefaultSubobject<USphereComponent>(TEXT("CaptureVolume"));
	CaptureVolume->InitSphereRadius(CaptureRadius);
	CaptureVolume->SetCollisionProfileName(TEXT("Trigger"));
	CaptureVolume->bGenerateOverlapEvents = true;
	RootComponent = CaptureVolume;

	// Visual marker mesh — mesh assigned by BP subclass
	VisualMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualMesh"));
	VisualMesh->SetupAttachment(RootComponent);
	VisualMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// Team-colored light
	TeamLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("TeamLight"));
	TeamLight->SetupAttachment(RootComponent);
	TeamLight->SetRelativeLocation(FVector(0.f, 0.f, 100.f));
	TeamLight->SetIntensity(3000.f);
	TeamLight->SetAttenuationRadius(500.f);
	TeamLight->SetLightColor(FLinearColor(0.5f, 0.5f, 0.5f));
}


void AShockDomControlPoint::BeginPlay()
{
	Super::BeginPlay();

	// Bind overlap delegate here — not in constructor (CDO has no world)
	if (CaptureVolume)
	{
		CaptureVolume->OnComponentBeginOverlap.AddDynamic(this, &AShockDomControlPoint::OnCaptureVolumeOverlap);
	}
}


void AShockDomControlPoint::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AShockDomControlPoint, OwningTeamIndex);
	DOREPLIFETIME(AShockDomControlPoint, PointIndex);
	DOREPLIFETIME(AShockDomControlPoint, PointLabel);
	DOREPLIFETIME(AShockDomControlPoint, LastCaptureTime);
}


void AShockDomControlPoint::OnCaptureVolumeOverlap(UPrimitiveComponent* OverlappedComp,
	AActor* OtherActor, UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& SweepResult)
{
	if (!HasAuthority()) return;

	AUTCharacter* UTChar = Cast<AUTCharacter>(OtherActor);
	if (!UTChar || UTChar->IsDead()) return;

	AUTPlayerState* PS = Cast<AUTPlayerState>(UTChar->PlayerState);
	if (!PS || !PS->Team) return;

	uint8 NewTeam = PS->Team->TeamIndex;
	if (NewTeam != OwningTeamIndex)
	{
		CapturePoint(NewTeam, UTChar);
	}
}


void AShockDomControlPoint::CapturePoint(uint8 NewTeamIndex, AUTCharacter* Capturer)
{
	OwningTeamIndex = NewTeamIndex;
	LastCaptureTime = GetWorld()->GetTimeSeconds();
	ForceNetUpdate();
	UpdateVisuals();

	// Notify game mode
	AShockDomGameMode* GM = GetWorld()->GetAuthGameMode<AShockDomGameMode>();
	if (GM)
	{
		GM->OnPointCaptured(this, NewTeamIndex, Capturer);
	}
}


FLinearColor AShockDomControlPoint::GetOwnerColor() const
{
	if (OwningTeamIndex == 255)
	{
		return FLinearColor(0.4f, 0.4f, 0.4f, 1.f);
	}

	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (GS && GS->Teams.IsValidIndex(OwningTeamIndex) && GS->Teams[OwningTeamIndex])
	{
		return GS->Teams[OwningTeamIndex]->TeamColor;
	}

	return (OwningTeamIndex == 0)
		? FLinearColor(1.f, 0.05f, 0.05f, 1.f)
		: FLinearColor(0.05f, 0.15f, 1.f, 1.f);
}


void AShockDomControlPoint::OnRep_OwningTeamIndex()
{
	UpdateVisuals();
}


void AShockDomControlPoint::UpdateVisuals()
{
	FLinearColor Color = GetOwnerColor();

	if (TeamLight)
	{
		TeamLight->SetLightColor(Color);
	}

	if (VisualMesh)
	{
		if (!DynMaterial)
		{
			UMaterialInterface* BaseMat = VisualMesh->GetMaterial(0);
			if (BaseMat)
			{
				DynMaterial = VisualMesh->CreateAndSetMaterialInstanceDynamic(0);
			}
		}
		if (DynMaterial)
		{
			DynMaterial->SetVectorParameterValue(TEXT("Color"), Color);
			DynMaterial->SetVectorParameterValue(TEXT("BaseColor"), Color);
		}
	}
}
