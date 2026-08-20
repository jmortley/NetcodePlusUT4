// WipeoutHUD — FlagRun-style portrait strip for Wipeout game mode
#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "WipeoutHUD.generated.h"

class AWipeoutDamageReplicator;
class AUTGameState;

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

	/** Mockup-style pip restyle (2026-08): health-over-armor stacked large on the
	 *  tile face (including the local player) and viewer-relative team/enemy alias
	 *  resolution. AClutchHUD reuses DrawPlayerIcon for its own small roster tiles
	 *  and sets this false in its ctor to keep the legacy compact "HP/AR" line and
	 *  fixed red/blue aliases there. */
	bool bPortraitMockupRestyle = true;

	virtual void DrawPlayerIcon(AUTPlayerState* PlayerState, float LiveScaling, float XOffset, float YOffset, float IconSize);
	virtual void GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers);

	/** Layout alias for one team's portrait strip. Red/blue by default; with
	 *  [NetcodePlus] ViewerRelativePortraits on, resolves portrait_team /
	 *  portrait_enemy against the local viewer's team instead (teamless viewer
	 *  buckets red as "ours", matching NCPlusForceModels::GetViewerTeam). */
	FName PortraitAliasFor(uint8 TeamIdx) const;

	/** Compact score/clock/score module — replaces bpHW_TeamGameClock while
	 *  preserving TeamSkins colors and the shared scorebar F5 alias. */
	virtual void DrawTeamScoreBar(AUTGameState* GS);

	/** "NOW WATCHING <player>" spectator banner, ported from iCTF (ANCPlusCTFHUD).
	 *  Canonical banner for dead players and true spectators; the stock duplicate is frame-suppressed. */
	void DrawSpectatorTarget();

	/** Force game-only input when dead but match in progress — prevents mouse escaping viewport */
	virtual EInputMode::Type GetInputMode_Implementation() const override;

	/** Take high-res screenshot when match ends (if enabled in NCP settings) */
	virtual void NotifyMatchStateChange() override;

private:
	AWipeoutDamageReplicator* FindDamageReplicator(UWorld* World);
	TWeakObjectPtr<UWorld> CachedDamageReplicatorWorld;
	TWeakObjectPtr<AUTGameState> CachedDamageReplicatorGameState;
	TWeakObjectPtr<AWipeoutDamageReplicator> CachedDamageReplicator;
	float NextDamageReplicatorRetryTime = 0.f;

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
		// Health and armor render as two separately-styled stacked lines on the
		// portrait (health over armor, mockup style), so each caches its own FText.
		const UFont* HpFont = nullptr;
		int32 HpKeyHP = MIN_int32;
		FText  HpText;
		float  HpWidth  = 0.f;
		float  HpHeight = 0.f;
		int32 ArKeyAR = MIN_int32;
		FText  ArText;
		float  ArWidth  = 0.f;
		float  ArHeight = 0.f;
		const UFont* CountdownFont = nullptr;
		int32 CountdownSeconds = MIN_int32;
		FText CountdownText;
		float CountdownWidth = 0.f;
		float CountdownHeight = 0.f;
	};
	TMap<TWeakObjectPtr<AUTPlayerState>, FWipeoutPipCache> PipCacheByPS;

	// Retained for native-instance layout compatibility on the 328 RC. The
	// compact portrait treatment no longer needs a framed-section pre-pass, so
	// these caches are intentionally dormant rather than removed this late.
	const UFont* NameRowFontCache[2] = { nullptr, nullptr };
	float NameRowScaleCache[2] = { -1.f, -1.f };
	float NameRowHeightCache[2] = { 0.f, 0.f };

};
