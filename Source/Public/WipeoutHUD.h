// WipeoutHUD — FlagRun-style portrait strip for Wipeout game mode
#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "WipeoutHUD.generated.h"

UCLASS()
class NETCODEPLUS_API AWipeoutHUD : public AUTHUD
{
	GENERATED_UCLASS_BODY()

	virtual void DrawHUD() override;
	virtual FLinearColor GetBaseHUDColor() override;

	// Portrait atlas icons — same UV coords as AUTFlagRunHUD
	UPROPERTY(EditAnywhere, BlueprintReadWrite, NoClear)
	FCanvasIcon RedTeamIcon;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, NoClear)
	FCanvasIcon BlueTeamIcon;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, NoClear)
	FCanvasIcon RedTeamOverlay;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, NoClear)
	FCanvasIcon BlueTeamOverlay;

	int32 RedPlayerCount;
	int32 BluePlayerCount;

	virtual void DrawPlayerIcon(AUTPlayerState* PlayerState, float LiveScaling, float XOffset, float YOffset, float IconSize);
	virtual void GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers);
};
