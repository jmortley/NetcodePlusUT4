// NCShaftArenaGame — strict 1v1 link-gun-shaft-only mode with always-on
// vampirism, first-to-N kills win-by-2, persistent ELO + accuracy awards
// in Mods.db (NCRatingShaftArena table).
//
// Both fire modes of the loadout weapon (AUTWeap_ShaftLink) act as the beam.
// The vampirism math is a copy of WipeoutGame's Siphon flow but without the
// powerup-presence check: the gamemode is the powerup, effectively.
#pragma once

#include "NetcodePlus.h"
#include "UTDMGameMode.h"

// Full include needed (not forward-decl) so TUniquePtr<FNCShaftArenaRatingSystem>
// can instantiate its destructor at this header. Must precede .generated.h.
#include "NCShaftArenaRatingSystem.h"

#include "NCShaftArenaGame.generated.h"

class AUTWeap_ShaftLink;

UCLASS()
class NETCODEPLUS_API ANCShaftArenaGame : public AUTDMGameMode
{
	GENERATED_UCLASS_BODY()

public:
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void HandleMatchHasStarted() override;
	virtual void HandleMatchHasEnded() override;

	virtual void ScoreDamage_Implementation(int32 DamageAmount,
		AUTPlayerState* Victim, AUTPlayerState* Attacker) override;
	virtual void ScoreKill_Implementation(AController* Killer, AController* Other,
		APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;

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

	/** Loadout weapon — defaults to AUTWeap_ShaftLink in the constructor. */
	UPROPERTY(EditDefaultsOnly, Category = "NCShaftArena")
	TSubclassOf<AUTWeap_ShaftLink> ShaftLinkClass;

protected:
	TUniquePtr<FNCShaftArenaRatingSystem> RatingSystem;

	/** Tracks longest unbroken kill streak this match per PS — fed into the
	 *  awards system at match end. */
	TMap<TWeakObjectPtr<AUTPlayerState>, int32> CurrentStreak;
	TMap<TWeakObjectPtr<AUTPlayerState>, int32> BestStreakThisMatch;

	float ComputeLinkAccuracyPct(AUTPlayerState* PS) const;
	void  BuildMatchSummary(struct FNCMatchSummary& OutSummary) const;
};
