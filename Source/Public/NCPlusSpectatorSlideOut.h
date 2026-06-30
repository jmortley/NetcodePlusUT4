// NCPlusSpectatorSlideOut — spectator slide-out subclass that fixes the
// per-player weapon-stats panel for ElimPlus and instagib CTF.
//
// Two stock problems this corrects (both in
// UUTHUDWidget_SpectatorSlideOut::DrawWeaponStats):
//   1) WEAPON LIST — the stock panel enumerates weapons from map pickups
//      (AUTPickupWeapon) plus ImpactHammer / Enforcer / Translocator. Elim and
//      iCTF maps have no weapon pickups, so the panel listed only those three
//      defaults ("Impact Hammer / Enforcer / Telefrag") at 0%.
//   2) ACCURACY SOURCE — stock reads hits/shots from AUTPlayerState::StatsData
//      via GetWeaponShotsStats/GetWeaponHitsStats, but StatsData is UPROPERTY()
//      with no Replicated specifier (see NCAccuracyStatsReplicator.h), so every
//      value is 0 on a dedicated-server spectator.
//
// This subclass overrides DrawWeaponStats to:
//   - ElimLoadout: list the player's ACTUAL carried weapons (the real BP-defined
//     loadout, read from their pawn inventory — no hardcoded weapon list). Each
//     weapon supplies its own Hits/ShotsStatsName, so NCPlus sniper/shock/rocket/
//     flak and the stock-modified rest all resolve correctly.
//   - CTFAuto: instagib match -> the instagib rifle (also read from inventory,
//     the only accuracy weapon carried); otherwise defer to the stock map-pickup
//     behaviour (normal CTF is left unchanged).
// Accuracy/Shots are sourced from the replicated ANCAccuracyStatsReplicator
// (spawned by every NCPlus gamemode), mirroring NCPlusHUDWidget_Accuracy's
// StatsData->replicator fallback so listen-server and dedicated agree.
#pragma once

#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTHUDWidget_SpectatorSlideOut.h"
#include "NCPlusSpectatorSlideOut.generated.h"

class ANCAccuracyStatsReplicator;

UENUM()
enum class ENCSlideOutWeaponMode : uint8
{
	/** Behave exactly like the stock slide-out. Safe default until the owning
	 *  HUD assigns a real mode in AddSpectatorWidgets. */
	Passthrough,
	/** Always show the fixed Elim ranged loadout. */
	ElimLoadout,
	/** Instagib match -> instagib rifle only; otherwise stock behaviour. */
	CTFAuto
};

UCLASS()
class NETCODEPLUS_API UNCPlusSpectatorSlideOut : public UUTHUDWidget_SpectatorSlideOut
{
	GENERATED_UCLASS_BODY()

	/** Which weapon list to draw. Set by the owning HUD immediately after the
	 *  widget is created (AElimPlusHUD / ANCPlusCTFHUD AddSpectatorWidgets). */
	UPROPERTY(Transient)
	ENCSlideOutWeaponMode WeaponListMode;

	/** When true (stock team panel active), the roster panel is redundant: skip
	 *  rendering it, but stay REGISTERED so ShouldDraw still bootstraps the interactive
	 *  spectator window (cursor / ESC / camera switching). Set by the owning HUD. */
	UPROPERTY(Transient)
	bool bSuppressRosterDraw;

protected:
	/** Suppress only the roster VISUAL when bSuppressRosterDraw — ShouldDraw (the sole
	 *  bootstrap for SUTSpectatorWindow's cursor/ESC/camera input) keeps running. */
	virtual void Draw_Implementation(float DeltaTime) override;

	virtual void DrawWeaponStats(AUTPlayerState* PS, float DeltaTime, float& YPos, float XOffset, float ScoreWidth, float MaxHeight, const FStatsFontInfo& StatsFontInfo) override;

private:
	/** Draw the Shots / Accuracy column headers (Kills/Deaths are intentionally
	 *  dropped — those per-weapon stats are not replicated to spectators, only
	 *  hits/shots are). Advances YPos by one line. */
	void DrawAccuracyHeader(float XOffset, float& YPos, float ScoreWidth, const FStatsFontInfo& StatsFontInfo);

	/** One drawn row: a weapon's display name + the stat NAMEs to look up. */
	struct FNCSlideRow
	{
		FText Label;
		FName HitsStat  = NAME_None;
		FName ShotsStat = NAME_None;
	};

	/** Build the rows from the spectated player's LIVE carried inventory (the
	 *  real BP-defined loadout — each weapon supplies its own Hits/ShotsStatsName).
	 *  Refreshes a per-player cache while alive; reuses that cache when the player
	 *  has no pawn (eliminated). No hardcoded weapon list. */
	void BuildLoadoutRows(AUTPlayerState* PS, TArray<FNCSlideRow>& OutRows);

	/** Last live loadout seen per PlayerId — dead/eliminated-player fallback. */
	TMap<FString, TArray<FNCSlideRow>> CachedLoadoutByPlayer;

	/** Resolve hits+shots for one stat-name pair. Reads PS->GetStatsValue first
	 *  (correct on listen/standalone) then falls back to the per-weapon replicator
	 *  (StatsData is server-only -> 0 on dedicated spectators). */
	void ResolveAccuracy(AUTPlayerState* PS, FName HitsStat, FName ShotsStat, int32& OutHits, int32& OutShots) const;

	/** TActorIterator-cached per-weapon accuracy replicator. Weak so it survives
	 *  match restarts (replicator re-spawns; next query re-caches). */
	UPROPERTY(Transient)
	mutable TWeakObjectPtr<ANCAccuracyStatsReplicator> CachedAccuracyReplicator;
	ANCAccuracyStatsReplicator* GetAccuracyReplicator() const;

	/** True if the current match is instagib, read from the replicated
	 *  ACTFStatsReplicator::bIsInstagibMatch (absent -> false). */
	bool IsInstagibMatch() const;
};
