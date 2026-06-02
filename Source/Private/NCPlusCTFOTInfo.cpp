// NCPlusCTFOTInfo.cpp
#include "NCPlusCTFOTInfo.h"
#include "Net/UnrealNetwork.h"

ANCPlusCTFOTInfo::ANCPlusCTFOTInfo(const FObjectInitializer& OI)
	: Super(OI)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	NetUpdateFrequency = 1.f;       // values change at most a few times per match
	OvertimeStartElapsed = -1;
	AdvantageStartElapsed = -1;
	bHasHalftime = false;
}

void ANCPlusCTFOTInfo::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCPlusCTFOTInfo, OvertimeStartElapsed);
	DOREPLIFETIME(ANCPlusCTFOTInfo, AdvantageStartElapsed);
	DOREPLIFETIME(ANCPlusCTFOTInfo, bHasHalftime);
}
