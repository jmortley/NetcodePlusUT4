// NCShaftArenaGame — strict 1v1 link-gun-shaft-only mode with always-on
// vampirism, first-to-N kills win-by-2, persistent ELO + accuracy awards
// in Mods.db (NCRatingShaftArena table).
//
// The loadout weapon is configurable: any AUTWeap_LinkGun_Plus subclass
// (typically a Blueprint) where both fire modes act as the beam. The BP
// owns all the visuals (tracers, beam mesh, sounds) and fire-mode wiring;
// this gamemode just hands it to players via DefaultInventory.
//
// Set the class via:
//   - BP subclass of NCShaftArenaGame (DefaultProperties → ShaftLinkClass), or
//   - Mod.ini  [NCShaftArena]  WeaponClass=/Game/Path/To/BP_ShaftLink.BP_ShaftLink_C
// Mod.ini overrides the BP value when both are set.
//
// The vampirism math is a copy of WipeoutGame's Siphon flow but without the
// powerup-presence check: the gamemode is the powerup, effectively.
#pragma once

#include "NetcodePlus.h"
#include "UTTeamDMGameMode.h"

// Full include needed (not forward-decl) so TUniquePtr<FNCShaftArenaRatingSystem>
// can instantiate its destructor at this header. Must precede .generated.h.
#include "NCShaftArenaRatingSystem.h"

#include "NCShaftArenaGame.generated.h"

class AUTWeap_LinkGun_Plus;
class ANCShaftArenaStatsReplicator;

UCLASS()
class NETCODEPLUS_API ANCShaftArenaGame : public AUTTeamDMGameMode
{
	GENERATED_UCLASS_BODY()

public:
	/** Stock pause permissions + Mod.ini-gated match-host pause ([NetcodePlus]
	 *  bAllowHostPause — see NCPlusHostPause.h). */
	virtual bool AllowPausing(APlayerController* PC) override;

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void HandleMatchHasStarted() override;
	virtual void HandleMatchHasEnded() override;

	// Unlock entitlement-gated cosmetics (boxhat etc.): force the chosen hat via OverrideHatClass so the
	// community master's withheld cosmetic entitlements can't strip it. Server-side, never kicks. See impl
	// (mirrors ANCPlusCTFGameMode / AUWipeoutGame).
	virtual bool ValidateHat(AUTPlayerState* HatOwner, const FString& HatClass) override;

	virtual void ScoreDamage_Implementation(int32 DamageAmount,
		AUTPlayerState* Victim, AUTPlayerState* Attacker) override;
	virtual void ScoreKill_Implementation(AController* Killer, AController* Other,
		APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;

	/** Suppress death-drops. 1v1 shaft is link-only by design — every player
	 *  spawns with the configured ShaftLinkClass, no reason to litter the
	 *  floor with dropped link guns when someone dies. Empty override means
	 *  the held weapon goes with the pawn at destruction; no AUTDroppedPickup
	 *  spawns. */
	virtual void DiscardInventory(APawn* Other, AController* Killer = nullptr) override;

	/** Win condition: first to GoalScore kills (default 10), but only if their
	 *  margin over the next-best player is at least MinWinMargin (default 2).
	 *  Both configurable via Mod.ini [NCShaftArena]. */
	UPROPERTY(EditDefaultsOnly, Category = "NCShaftArena")
	int32 MinWinMargin;

	/** Vampirism heal fraction. Default 0.5 (50%). */
	UPROPERTY(EditDefaultsOnly, Category = "NCShaftArena")
	float SiphonPercent;

	/** Heal cap (overheal allowed up to 199). */
	UPROPERTY(EditDefaultsOnly, Category = "NCShaftArena")
	int32 HealCap;

	/** Loadout weapon. Set via BP subclass DefaultProperties or Mod.ini
	 *  [NCShaftArena] WeaponClass=/Game/Path/To/BP_ShaftLink.BP_ShaftLink_C.
	 *  Players spawn with this in their inventory and nothing else. */
	UPROPERTY(EditDefaultsOnly, Category = "NCShaftArena")
	TSubclassOf<AUTWeap_LinkGun_Plus> ShaftLinkClass;

	/** Bridges StatsData (server-only) and DamageDone (also server-only) to
	 *  clients for the scoreboard. Without this, dedicated-server play shows
	 *  0% accuracy + 0 damage on every row. Same pattern as the duel
	 *  replicator. */
	UPROPERTY()
	ANCShaftArenaStatsReplicator* StatsReplicator = nullptr;

protected:
	TUniquePtr<FNCShaftArenaRatingSystem> RatingSystem;

	/** Idempotency guard: HandleMatchHasEnded is sometimes routed twice by the
	 *  engine state machine. Mirrors AElimPlusGame::bRatingFlushedThisMatch.
	 *  Reset in InitGame; set true after the first RecordMatchResult+Flush+upload. */
	UPROPERTY(Transient)
	bool bRatingFlushedThisMatch = false;

	/** Tracks longest unbroken kill streak this match per PS — fed into the
	 *  awards system at match end. */
	TMap<TWeakObjectPtr<AUTPlayerState>, int32> CurrentStreak;
	TMap<TWeakObjectPtr<AUTPlayerState>, int32> BestStreakThisMatch;

	float ComputeLinkAccuracyPct(AUTPlayerState* PS) const;
	void  BuildMatchSummary(struct FNCMatchSummary& OutSummary) const;
};
