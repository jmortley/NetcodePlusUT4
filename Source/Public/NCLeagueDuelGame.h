// NCLeagueDuelGame — AUTDuelGame with a fairness-first spawn picker.
//
// Behavior summary:
//   - At InitGame (server only, before any player joins), walk every
//     AUTPickupWeapon actor in the world and claim its closest PlayerStart
//     within 2500uu as that weapon's anchor. Independent of the map's
//     AssociatedPickup field (which most maps don't bother to set).
//     Build 3 weapon pairs: Sniper↔Shock, Rocket↔Flak, Mini↔Link, each with
//     the maximum-distance start-pair on multi-pickup maps. Then shuffle the
//     pair index list and roll a 50/50 bRedGetsPrimarySide flag — the
//     entire "preset spawn choices" table is FROZEN at this point.
//   - PostLogin: each joining player gets RespawnChoiceA = first pair's
//     team-side, RespawnChoiceB = second pair's team-side (read straight
//     from the pre-built shuffle order). Zero per-join computation.
//   - On respawn after death, RatePlayerStart enforces ≥MinKillerSpawnDistance
//     (2500uu default) from the killer's last known location, and the
//     inter-A/B 2500uu rule prevents the player from being cornered.
//   - While the Shield Belt pickup is active, the closest 2 PlayerStarts to
//     the belt are excluded from spawn selection (don't let a player snowball
//     belt control).
//
// Server-only state: WeaponPairs / WeaponAnchoredStarts / FirstSpawnShuffleOrder
// / bRedGetsPrimarySide / ShieldBeltPickup all live on the GameMode (we don't
// subclass AUTGameState — ABI mismatch). 1v1 ELO via FNCDuelRatingSystem
// (Glicko2). Stats upload stubs hit FNCStatsUploader.
#pragma once

#include "NetcodePlus.h"
#include "UTDuelGame.h"

// Full include needed (not forward-decl): TUniquePtr<FNCDuelRatingSystem>
// instantiates its destructor at this header — `delete` requires the complete
// type. Must come BEFORE the .generated.h (UHT requires .generated.h to be
// the last include).
#include "NCDuelRatingSystem.h"

#include "NCLeagueDuelGame.generated.h"

class AUTPickupInventory;

USTRUCT()
struct FNCLeagueWeaponPair
{
	GENERATED_BODY()

	UPROPERTY() TSubclassOf<class AUTWeapon> WeaponClass;
	UPROPERTY() class APlayerStart* StartA = nullptr;
	UPROPERTY() class APlayerStart* StartB = nullptr;
	UPROPERTY() TSubclassOf<class AUTWeapon> CounterClass;     // the class StartB anchors
	/** Group keys matching GroupForWeaponClass output (Sniper/Shock/Rocket/...).
	 *  Used to prevent weapon-class collisions between Choice A and B when one
	 *  pair is consumed and the fallback has to scan WeaponAnchoredStarts. */
	FName GroupA = NAME_None;
	FName GroupB = NAME_None;
};

UCLASS()
class NETCODEPLUS_API ANCLeagueDuelGame : public AUTDuelGame
{
	GENERATED_UCLASS_BODY()

public:
	/** Stock pause permissions + Mod.ini-gated match-host pause ([NetcodePlus]
	 *  bAllowHostPause — see NCPlusHostPause.h). */
	virtual bool AllowPausing(APlayerController* PC) override;

	/** Defer a host/rcon unpause behind a short server-only resume countdown
	 *  (see NCPlusHostPause::DeferUnpauseForCountdown). */
	virtual bool ClearPause() override;

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void EndPlayerIntro() override;
	virtual void HandleMatchHasStarted() override;
	virtual void HandleMatchHasEnded() override;

	// Unlock entitlement-gated cosmetics (boxhat etc.): force the chosen hat via OverrideHatClass so the
	// community master's withheld cosmetic entitlements can't strip it. Server-side, never kicks. See impl
	// (mirrors ANCPlusCTFGameMode / AUWipeoutGame).
	virtual bool ValidateHat(AUTPlayerState* HatOwner, const FString& HatClass) override;

	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual AActor* FindPlayerStart_Implementation(AController* Player, const FString& IncomingName = TEXT("")) override;
	virtual float   RatePlayerStart(class APlayerStart* P, AController* Player) override;

	virtual void ScoreKill_Implementation(AController* Killer, AController* Other,
		APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;

	/** Closest distance (uu) the spawn picker enforces between a respawn and
	 *  the player's most recent killer. Mod.ini key: [NCLeagueDuel] MinKillerSpawnDistance. */
	UPROPERTY(EditDefaultsOnly, Category = "NCLeagueDuel")
	float MinKillerSpawnDistance;

	/** Hard-reject any spawn this close to a living enemy (Tier 1). Mod.ini:
	 *  [NCLeagueDuel] MinimumEnemySpawnDistance. */
	UPROPERTY(EditDefaultsOnly, Category = "NCLeagueDuel")
	float MinimumEnemySpawnDistance;

	/** Number of PlayerStarts nearest the Shield Belt to exclude while the
	 *  belt is active. Default 2. */
	UPROPERTY(EditDefaultsOnly, Category = "NCLeagueDuel")
	int32 ShieldBeltExclusionCount;

protected:
	UPROPERTY() TArray<FNCLeagueWeaponPair> WeaponPairs;
	UPROPERTY() TArray<class APlayerStart*> AllPlayerStarts;
	/** Every PlayerStart whose AssociatedPickup is a weapon (any weapon, not
	 *  just the canonical pair list). First-spawn picker falls back to this
	 *  pool when canonical pairs run out so the player still lands at a gun
	 *  rather than a generic spot near a bio bottle or health vial. */
	UPROPERTY() TArray<class APlayerStart*> WeaponAnchoredStarts;
	UPROPERTY() AUTPickupInventory*         ShieldBeltPickup;
	UPROPERTY() TArray<class APlayerStart*> ShieldBeltExclusions;

	/** Killer location captured in ScoreKill — drives the 2500uu respawn rule. */
	TMap<TWeakObjectPtr<AController>, FVector> LastKillerLocation;

	/** Pairs that have already been assigned this match (skip on next first-spawn). */
	TSet<int32> ConsumedPairIndices;

	/** Per-match shuffle of WeaponPairs indices. Choice A = WeaponPairs[FirstSpawnShuffleOrder[0]],
	 *  Choice B = WeaponPairs[FirstSpawnShuffleOrder[1]]. Mirrors the BP "shuffle pairs, take 2"
	 *  flow so both players see the SAME two pairs as their A/B options (mutual fairness). */
	TArray<int32> FirstSpawnShuffleOrder;

	/** 50/50 random per match — when true, team 0 (red) gets the StartA side of each pair
	 *  and team 1 (blue) gets StartB; when false, sides are swapped. Adds weapon-meta
	 *  fairness (e.g. if Sniper is OP, red has 50% chance to start at it instead of always). */
	bool bRedGetsPrimarySide = true;

	/** True once FirstSpawnShuffleOrder + bRedGetsPrimarySide have been initialized for this
	 *  match. Reset in BeginPlay. */
	bool bFirstSpawnShuffleReady = false;

	/** Server-only Glicko2 ELO. Loaded from Mods.db on PostLogin, persisted
	 *  in HandleMatchHasEnded. */
	TUniquePtr<FNCDuelRatingSystem> RatingSystem;

	/** Idempotency guard: HandleMatchHasEnded is sometimes routed twice by the
	 *  engine state machine. Mirrors AElimPlusGame::bRatingFlushedThisMatch.
	 *  Reset in InitGame; set true after the first ProcessMatchResult+Flush+upload. */
	UPROPERTY(Transient)
	bool bRatingFlushedThisMatch = false;

	/** Replicates per-player hitscan accuracy to clients. AUTPlayerState's
	 *  StatsData TMap (which holds NAME_LinkHits / NAME_ShockRifleHits etc.)
	 *  is server-only — without this replicator the duel scoreboard's Acc
	 *  column always reads 0 on remote clients. Spawned in InitGame, ticked
	 *  on the authority. */
	UPROPERTY()
	class ANCLeagueDuelStatsReplicator* StatsReplicator = nullptr;

	void  ComputeSpawnPairings();
	void  ComputeShieldBeltExclusions();
	bool  IsExcludedByActiveShieldBelt(class APlayerStart* PS) const;
	/** Lazily build FirstSpawnShuffleOrder + bRedGetsPrimarySide. No-op once ready. */
	void  EnsureFirstSpawnShuffle();
	/** Get the canonical weapon group key (Sniper/Shock/Rocket/Flak/Mini/Link/None)
	 *  for whatever weapon a PlayerStart's AssociatedPickup carries. Used by the
	 *  Tier-B fallback so Choice B can avoid the same weapon FAMILY as A. */
	FName GetWeaponGroupForStart(class APlayerStart* Start) const;
	/** Pick a paired-weapon anchor for the player's team. ChoiceIdx 0 -> Choice A,
	 *  1 -> Choice B; reads from the per-match shuffle order so both players' A's
	 *  come from the SAME pair (mutual fairness). ExcludeStart triggers the Tier-B
	 *  fallback path when WeaponPairs is too sparse. */
	class APlayerStart* SelectPairedSpawnForFirstSpawn(class AUTPlayerState* PS,
	                                                    int32 ChoiceIdx,
	                                                    class APlayerStart* ExcludeStart);
	float ComputeEnemyProximityScore(class APlayerStart* P, AController* Player,
	                                 float& OutMinEnemyDist, bool& bOutHasLOS) const;
	void  BuildMatchSummary(struct FNCMatchSummary& OutSummary) const;
};
