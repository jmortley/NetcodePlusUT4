// NCPlusCTFScoreboard.h — Custom CTF scoreboard with K/D/Eff/Acc/C/G/R/Ping.
// Accuracy auto-detects instagib vs LG+Sniper.

#pragma once

#include "NetcodePlus.h"
#include "UTCTFScoreboard.h"
#include "NCPlusCTFScoreboard.generated.h"

class ACTFStatsReplicator;

UCLASS()
class NETCODEPLUS_API UNCPlusCTFScoreboard : public UUTCTFScoreboard
{
	GENERATED_UCLASS_BODY()

protected:
	// Per-mode column X positions. Layout depends on bIsInstagibMatch:
	//   non-instagib: Player | K/D | ACC | Caps | Grabs | Returns | Armors | Amp | Ping
	//   instagib:     Player | K/D | EFF | ACC | Caps | Grabs | Returns | Ping
	// Both keep PlayerX past the country-flag glyph (FlagX=0.01, ~6-7% wide).
	struct FCtfColumnLayout
	{
		float PlayerX = 0.09f;
		float KDX     = 0.f;
		float EffX    = 0.f;          // instagib only
		float AccX    = 0.f;
		float CapsX   = 0.f;
		float GrabsX  = 0.f;
		float ReturnsX = 0.f;
		float ArmorsX = 0.f;          // non-instagib only
		float AmpX    = 0.f;          // non-instagib only
		float PingX   = 0.f;
	};
	FCtfColumnLayout NormalLayout;
	FCtfColumnLayout InstagibLayout;

	/** Pick the right layout for the current frame (depends on the
	 *  replicated bIsInstagibMatch flag). Falls back to NormalLayout if
	 *  the replicator isn't found yet. */
	const FCtfColumnLayout& GetActiveLayout();

	// Column header texts
	FText CH_KD;
	FText CH_Eff;
	FText CH_Acc;
	FText CH_Grabs;
	FText CH_Armors;
	FText CH_Amp;

	virtual void PreDraw(float DeltaTime, AUTHUD* InUTHUDOwner, UCanvas* InCanvas, FVector2D InCanvasCenter) override;
	virtual void DrawScoreHeaders(float RenderDelta, float& YOffset) override;
	virtual void DrawPlayerScore(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width, FLinearColor DrawColor) override;
	virtual void DrawPlayerScores(float RenderDelta, float& YOffset) override;
	virtual void DrawReadyText(AUTPlayerState* PlayerState, float XOffset, float YOffset, float Width) override;

	/** Override DrawPlayer so ping/skill draws CENTERED at ColumnHeaderPingX
	 *  rather than right-aligned at the cell's right edge (engine's hardcoded
	 *  0.995). Engine code: UTScoreboard.cpp:792 / 797. Body otherwise mirrors
	 *  engine — same null-guard and mute-block-removed pattern as the duel /
	 *  shaft scoreboards. */
	virtual void DrawPlayer(int32 Index, AUTPlayerState* PlayerState,
		float RenderDelta, float XOffset, float YOffset) override;

private:
	/** Cached replicator reference (found once, reused) */
	UPROPERTY()
	ACTFStatsReplicator* CachedStatsRep = nullptr;

	ACTFStatsReplicator* FindStatsReplicator();

	/** Draw a 4-icon armor row (Belt / Vest / Pads / Helm) centered at
	 *  CenterX with each cell showing icon + count below. Mirrors the duel
	 *  scoreboard's helper. Counts ordering: [Belt, Vest, Pads, Helm]. */
	void DrawArmorIconRow(float CenterX, float CenterY, const uint8* Counts, FLinearColor IconTint);

	/** Draw the amp column: UDamage icon + pickup count + total seconds held.
	 *  CenterX is the column's nominal center; layout is icon-left,
	 *  number-and-time-right. */
	void DrawAmpCell(float CenterX, float CenterY, uint8 AmpCount, int32 AmpTimeS, FLinearColor DrawColor);
};
