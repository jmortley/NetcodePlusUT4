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

	virtual void BeginPlay() override;
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

	/** Subclass override: set false in your HUD ctor to skip the portrait
	 *  strips entirely (NCLeagueDuel sets this false — 1v1 doesn't benefit
	 *  from a team-portrait row). Layout entries for portrait_red /
	 *  portrait_blue are still respected when this is true. */
	UPROPERTY(EditDefaultsOnly, Category = "WipeoutHUD")
	bool bShouldDrawPortraits = true;

	virtual void DrawPlayerIcon(AUTPlayerState* PlayerState, float LiveScaling, float XOffset, float YOffset, float IconSize);
	virtual void GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers);

	/** Custom team score bar — replaces bpHW_TeamGameClock to respect TeamSkins colors.
	 *  Shows "Phayder" (red) vs "Liandri" (blue) when custom team colors are detected. */
	virtual void DrawTeamScoreBar(AUTGameState* GS);

	/** "NOW WATCHING <player>" spectator banner, ported from iCTF (ANCPlusCTFHUD).
	 *  Canonical banner for dead players and true spectators; the stock duplicate is frame-suppressed. */
	void DrawSpectatorTarget();

	/** Force game-only input when dead but match in progress — prevents mouse escaping viewport */
	virtual EInputMode::Type GetInputMode_Implementation() const override;

	/** Take high-res screenshot when match ends (if enabled in NCP settings) */
	virtual void NotifyMatchStateChange() override;

private:
	// Post-match screenshot state — serviced by NCPlusHUDDrawCall::ServicePostMatchScreenshot from DrawHUD.
	bool bPostMatchScreenshotTaken = false;
	float PostMatchScreenshotStable = -1.f;

	/** Portrait ordering only changes with roster/scorer/spectating-order state.
	 *  Keep that order between frames while the draw path still reads volatile
	 *  pawn, health, armor, and respawn state every frame. */
	struct FPortraitOrderSignature
	{
		TWeakObjectPtr<AUTPlayerState> PlayerState;
		uint8 TeamNum = 255;
		int32 SortKey = 0;
	};
	TArray<AUTPlayerState*> PortraitRenderPlayers;
	TArray<FPortraitOrderSignature> CurrentPortraitSignature;
	TArray<FPortraitOrderSignature> CachedPortraitSignature;
	TWeakObjectPtr<UWorld> CachedPortraitWorld;
	TWeakObjectPtr<AUTGameState> CachedPortraitGameState;

	/** Per-PlayerState HP readout cache for the portrait strip: last-rendered FText +
	 *  measured extents keyed on (HP, armor, font), so an unchanged value skips the
	 *  Printf + StrLen + FText::FromString each frame. Keyed weakly; self-expires with
	 *  the PS. (Fitted names cached separately by NCPlusHUDDrawCall::ResolveFittedName.) */
	struct FWipeoutPipCache
	{
		const UFont* HpFont = nullptr;
		int32 HpKeyHP = MIN_int32;
		int32 HpKeyAR = MIN_int32;
		FText  HpText;
		float  HpWidth  = 0.f;
		float  HpHeight = 0.f;
		const UFont* CountdownFont = nullptr;
		int32 CountdownSeconds = MIN_int32;
		FText CountdownText;
		float CountdownWidth = 0.f;
		float CountdownHeight = 0.f;
	};
	TMap<TWeakObjectPtr<AUTPlayerState>, FWipeoutPipCache> PipCacheByPS;
};
