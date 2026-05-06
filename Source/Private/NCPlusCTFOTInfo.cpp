// NCPlusCTFOTInfo.cpp
#include "NCPlusCTFOTInfo.h"
#include "Net/UnrealNetwork.h"

ANCPlusCTFOTInfo::ANCPlusCTFOTInfo(const FObjectInitializer& OI)
	: Super(OI)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	NetUpdateFrequency = 1.f;       // value rarely changes (once per match at OT entry)
	OvertimeStartElapsed = -1;
}

void ANCPlusCTFOTInfo::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ANCPlusCTFOTInfo, OvertimeStartElapsed);
}
