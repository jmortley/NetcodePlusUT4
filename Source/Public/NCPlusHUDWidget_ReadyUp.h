// NCPlusHUDWidget_ReadyUp - warmup-only F5 ready-up prompt.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget.h"
#include "NCPlusHUDWidget_ReadyUp.generated.h"

class ANCReadyUpState;

/**
 * Bottom-center status for servers using UTComp-style player ready-up.
 * The replicated state actor is also the feature-presence signal, so this
 * widget is a complete no-op on host-controlled servers.
 */
UCLASS()
class NETCODEPLUS_API UNCPlusHUDWidget_ReadyUp : public UUTHUDWidget
{
	GENERATED_UCLASS_BODY()

	virtual void Draw_Implementation(float DeltaTime) override;
	virtual bool ShouldDraw_Implementation(bool bShowScores) override;

private:
	UPROPERTY(Transient)
	mutable TWeakObjectPtr<ANCReadyUpState> CachedReadyUpState;

	ANCReadyUpState* GetReadyUpState() const;
};
