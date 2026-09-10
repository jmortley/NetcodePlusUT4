#pragma once

#include "NetcodePlus.h"
#include "UTHUD.h"
#include "NCPlusXTDMHUD.generated.h"

class ANCPlusXTDMReplicator;

/** Four-team instagib presentation. The game state remains a stock-parent Blueprint. */
UCLASS()
class NETCODEPLUS_API ANCPlusXTDMHUD : public AUTHUD
{
	GENERATED_UCLASS_BODY()
public:
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	virtual void AddSpectatorWidgets() override;
	virtual void Destroyed() override;
	virtual bool ShouldDrawMinimap() override { return false; }
	virtual FLinearColor GetBaseHUDColor() override;
	virtual EInputMode::Type GetInputMode_Implementation() const override;
	virtual void ReceiveLocalMessage(TSubclassOf<UUTLocalMessage> MessageClass,
		APlayerState* Player1, APlayerState* Player2, uint32 MessageIndex,
		FText LocalMessageText, UObject* OptionalObject = nullptr) override;

	void GetTeamRoster(uint8 Team, TArray<AUTPlayerState*>& OutPlayers);
	void ViewTeamSlot(uint8 Team, int32 Slot);
	uint8 KeyboardSpectatorTeam = 0;
	static FLinearColor TeamColor(const AUTGameState* GS, uint8 Team);
	static FString TeamLabel(uint8 Team);
	static void Text(UCanvas* InCanvas, UFont* Font, const FString& Value,
		float X, float Y, float Scale, FLinearColor Color, bool bCentered = false);
	static void Tile(UCanvas* InCanvas, float X, float Y, float W, float H, FLinearColor Color);
	FString PlayerStatus(AUTPlayerState* PS, ANCPlusXTDMReplicator* Rep) const;

private:
	void DrawScoreStrip(AUTGameState* GS);
	void DrawTeammates(AUTGameState* GS);
	void UpdateSpectatorInput();
	float NextRosterRefresh = -1.f;
	TWeakObjectPtr<AUTGameState> RosterGameState;
	TArray<TWeakObjectPtr<AUTPlayerState>> TeamRosters[4];
	int32 CachedScores[4];
	FString ScoreStrings[4];
	int32 CachedClock = MIN_int32;
	FString ClockString;
	bool bSpectatorInputActive = false;
	bool bPostMatchScreenshotTaken = false;
	float PostMatchScreenshotStable = -1.f;
};
