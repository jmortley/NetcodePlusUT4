// ElimPlusHUD — FlagRun-style portrait strip for ElimPlus (TeamArena) game mode.
// Mirror of AWipeoutHUD with elim-specific adaptations:
//   - No respawn countdown overlay on dead portraits (no mid-round respawns)
//   - No next-to-spawn gold border (irrelevant in elim)
//   - Always-dim overlay on dead portraits (no sweep animation)
//   - Optional ELO chip per portrait (read from ElimPlusStatsReplicator)
#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUD.h"
#include "ElimPlusHUD.generated.h"

class AElimPlusStatsReplicator;

/** One player's client-presentation state. UObject references are weak because
 * remote state may now survive for one 120 Hz sample interval; nothing is replicated. */
struct FElimPlusHUDPlayerSnapshot
{
	TWeakObjectPtr<AUTPlayerState> PlayerState;
	TWeakObjectPtr<AUTCharacter> Character;
	uint8 TeamNum = 255;
	bool bAlive = false;
	int32 Health = 0;
	int32 Armor = 0;
	uint8 PortraitSortKey = 0;
};

/** Reusable per-HUD roster storage shared by all three ElimPlus roster renderers. */
struct FElimPlusHUDSnapshot
{
	TArray<FElimPlusHUDPlayerSnapshot> Players;
	TArray<int32> TeamPlayerIndices[2];
	TArray<int32> PortraitPlayerIndices;
	AUTPlayerState* ScorerPS = nullptr;
	AUTPlayerState* LocalPS = nullptr;
	int32 AliveCountTeam[2] = { 0, 0 };
	TWeakObjectPtr<AUTPlayerState> SoleSurvivor[2];

	void ResetFrame(int32 ExpectedPlayers)
	{
		Players.Reset(ExpectedPlayers);
		TeamPlayerIndices[0].Reset(ExpectedPlayers);
		TeamPlayerIndices[1].Reset(ExpectedPlayers);
		PortraitPlayerIndices.Reset(ExpectedPlayers);
		ScorerPS = nullptr;
		LocalPS = nullptr;
		AliveCountTeam[0] = AliveCountTeam[1] = 0;
		SoleSurvivor[0] = nullptr;
		SoleSurvivor[1] = nullptr;
	}
};

UCLASS()
class NETCODEPLUS_API AElimPlusHUD : public AUTHUD
{
	GENERATED_UCLASS_BODY()

	virtual void BeginPlay() override;
	virtual void DrawHUD() override;
	virtual bool ShouldDrawMinimap() override;
	virtual FLinearColor GetBaseHUDColor() override;

	/** Swap the stock spectator slide-out for UNCPlusSpectatorSlideOut so the
	 *  weapon-stats panel lists the Elim loadout with replicated accuracy. */
	virtual void AddSpectatorWidgets() override;

	// Portrait atlas icons — same UV coords as AUTFlagRunHUD / AWipeoutHUD
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

	virtual void DrawPlayerIcon(AUTPlayerState* PlayerState, bool bPlayerAlive, float XOffset, float YOffset, float IconSize);
	virtual void GetPlayerListForIcons(TArray<AUTPlayerState*>& SortedPlayers);

	/** Custom team score bar — dynamic team colors via TeamSkins, plus round clock. */
	virtual void DrawTeamScoreBar(AUTGameState* GS);

	/** "NOW WATCHING <player>" spectator banner, ported from iCTF (ANCPlusCTFHUD).
	 *  Canonical banner for dead players and true spectators; the stock duplicate is frame-suppressed. */
	void DrawSpectatorTarget();

	/** Force game-only input when dead but match in progress — prevents mouse escaping viewport */
	virtual EInputMode::Type GetInputMode_Implementation() const override;

	/** Take high-res screenshot when match ends (if enabled in NCP settings) */
	virtual void NotifyMatchStateChange() override;

	/** Custom Canvas overlay shown during PlayerIntro listing both team rosters
	 *  with names + ELOs and team-strength totals. Fades out when state
	 *  transitions to CountdownToBegin (lines up with the "3" countdown
	 *  announcement), then disappears entirely. Drawn from DrawHUD after Super. */
	void DrawPreMatchTeamPreview();

private:
	/** Cache roster/order and remote presentation at 120 Hz. The local player is
	 * refreshed every render frame, as are all Canvas animation/draw calculations. */
	void BuildPlayerSnapshot(AUTGameState* GS, AUTPlayerState* ScorerPS);
	AElimPlusStatsReplicator* FindStatsReplicator(UWorld* World);
	void DrawPlayerIconFromSnapshot(AUTPlayerState* PlayerState, bool bPlayerAlive,
		float XOffset, float YOffset, float IconSize, const FElimPlusHUDPlayerSnapshot* FramePlayer);

	struct FPortraitOrderSignature
	{
		TWeakObjectPtr<AUTPlayerState> PlayerState;
		uint8 TeamNum = 255;
		uint8 SortKey = 0;
	};

	FElimPlusHUDSnapshot PlayerSnapshot;
	FElimPlusHUDPlayerSnapshot ActiveFramePlayer;
	bool bHasActiveFramePlayer = false;
	bool bUsePreparedPlayerSnapshot = false;
	TArray<AUTPlayerState*> PortraitRenderPlayers;
	TArray<FPortraitOrderSignature> CachedPortraitSignature;
	TArray<int32> CachedPortraitOrder;
	TWeakObjectPtr<UWorld> CachedSnapshotWorld;
	TWeakObjectPtr<AUTGameState> CachedSnapshotGameState;
	TWeakObjectPtr<AUTPlayerState> CachedSnapshotScorer;
	int32 CachedSnapshotPlayerArrayNum = INDEX_NONE;
	float NextSnapshotSampleTime = 0.f;
	static constexpr float SnapshotSampleInterval = 1.f / 120.f;

	// -1 preserves the complete-card public path. The portrait strip selects
	// 0..4 to submit one resource/layer pass for every card.
	int32 ActivePortraitDrawPass = -1;
	struct FElimPortraitPassState
	{
		FCanvasIcon CharIcon;
		FName Alias;
		uint8 TeamNum = 255;
		float Opacity = 1.f;
		float X = 0.f;
		float Y = 0.f;
		float Width = 0.f;
		float Height = 0.f;
		TWeakObjectPtr<AUTGameState> GameState;
		FLinearColor TeamColor = FLinearColor::White;
		float TextScale = 1.f;
		UFont* PipFont = nullptr;
		float PipFontExtra = 1.f;
	};
	TMap<TWeakObjectPtr<AUTPlayerState>, FElimPortraitPassState> PortraitPassStateByPS;

	// Per-HUD lookup state avoids live/replay or split-screen worlds evicting one
	// another from a translation-unit static replicator cache.
	TWeakObjectPtr<UWorld> CachedStatsWorld;
	TWeakObjectPtr<AUTGameState> CachedStatsGameState;
	TWeakObjectPtr<AElimPlusStatsReplicator> CachedStatsReplicator;
	float NextStatsReplicatorRetryTime = 0.f;

	// Per-HUD KDA strings avoid split-screen scorers invalidating a shared one-entry cache.
	TWeakObjectPtr<AUTPlayerState> CachedKdaPS;
	int32 CachedKdaScore = MAX_int32;
	int32 CachedKdaKills = MAX_int32;
	int32 CachedKdaDeaths = MAX_int32;
	int32 CachedKdaAssists = MAX_int32;
	FString CachedScoreString;
	FString CachedKdaString;

	// Post-match screenshot state — serviced by NCPlusHUDDrawCall::ServicePostMatchScreenshot from DrawHUD.
	bool bPostMatchScreenshotTaken = false;
	float PostMatchScreenshotStable = -1.f;

	/** Per-player ELO chip animation state. Triggered the first frame the
	 *  replicator's EloDeltaThisMatch transitions from 0 to non-zero (= match end);
	 *  cleared automatically when Delta returns to 0 (next match's frame-1 push). */
	struct FElimPlusEloAnim
	{
		float StartTime  = 0.f;
		int32 FromElo    = 0;
		int32 ToElo      = 0;
		int32 FinalDelta = 0;
	};
	// Keyed on the PlayerState (weak) rather than the UniqueId string — no per-pip
	// FString hashing. Stale weak keys are pruned when the roster signature changes.
	TMap<TWeakObjectPtr<AUTPlayerState>, FElimPlusEloAnim> EloAnimByPlayerId;
	static constexpr float EloAnimDurationSec = 4.0f;

	/** Per-PlayerState portrait-strip caches: the stable stats lookup key,
	 *  and the last-rendered ELO-chip / HP FText + measured width keyed on the values they
	 *  display, so an unchanged frame skips the Printf + StrLen + FText::FromString. Keyed
	 *  weakly and explicitly pruned with the roster. (Fitted names are cached separately by
	 *  NCPlusHUDDrawCall::ResolveFittedName.) */
	struct FElimPipCache
	{
		FString UidStr;
		FString UidSourceName;
		bool    bUidValid = false;
		bool    bUidFromOnlineId = false;

		int32 EloKeyElo   = MIN_int32;
		int32 EloKeyDelta = MIN_int32;
		FText  EloText;
		float  EloWidth   = 0.f;
		float  EloHeight  = 0.f;

		const UFont* HpFont = nullptr;
		int32 HpKeyHP = MIN_int32;
		int32 HpKeyAR = MIN_int32;
		FText  HpText;
		float  HpWidth  = 0.f;
		float  HpHeight = 0.f;

		// Beta cards render health and armor as separately styled lines. Keep
		// their unscaled extents so fitting is arithmetic-only on unchanged frames.
		const UFont* BetaVitalsFont = nullptr;
		int32 BetaHpKey = MIN_int32;
		int32 BetaArKey = MIN_int32;
		FText BetaHpText;
		FText BetaArText;
		float BetaHpWidth = 0.f;
		float BetaHpHeight = 0.f;
		float BetaArWidth = 0.f;
		float BetaArHeight = 0.f;
	};
	TMap<TWeakObjectPtr<AUTPlayerState>, FElimPipCache> PipCacheByPS;

	/** Client-side timestamp captured the first frame the gamestate reports
	 *  CountdownToBegin. DrawPreMatchTeamPreview fades the overlay alpha from
	 *  1 → 0 over PreviewFadeDurationSec starting at this time. Reset to -1
	 *  while in PlayerIntro so the next match's countdown re-arms the fade. */
	float CountdownStartTimeSeconds = -1.f;
	static constexpr float PreviewFadeDurationSec = 0.8f;
};
