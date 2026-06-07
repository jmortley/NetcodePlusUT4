// WarmupRoamMutator.h - warmup-only "roam" mode for NCPlusCTF.
//
// `mutate warmup` toggles a per-player roam state during warmup that makes you
// invulnerable and disables your firing, so you can learn the map without dying
// or fragging. Auto-added by NCPlusCTFGameMode (all NCPlusCTF, incl. iCTF).
//
// STRICTLY warmup: roam is stripped from everyone the instant the match leaves
// MatchState::WaitingToStart, so it can never carry into live play. Effects use
// engine hooks - AUTCharacter::bDamageHurtsHealth (server-side damage gate) and
// DisallowWeaponFiring() (weapon fire paths check IsFiringDisabled()).
#pragma once

#include "NetcodePlus.h"   // PCH preamble - required for unity-build PCH bootstrap.
#include "CoreMinimal.h"
#include "UTMutator.h"
#include "WarmupRoamMutator.generated.h"

class AUTCharacter;

UCLASS()
class NETCODEPLUS_API AWarmupRoamMutator : public AUTMutator
{
	GENERATED_UCLASS_BODY()

public:
	/** `mutate warmup` toggles roam for the sender. Warmup-only; rejected otherwise. */
	virtual void Mutate_Implementation(const FString& MutateString, APlayerController* Sender) override;

	/** Re-assert roam on a respawned pawn (firing/invuln reset to defaults each spawn). */
	virtual void ModifyPlayer_Implementation(APawn* Other, bool bIsNewSpawn) override;

	/** Strip roam from everyone the moment warmup ends - the fairness guarantee. */
	virtual void NotifyMatchStateChange_Implementation(FName NewState) override;

protected:
	/** Controllers currently roaming (server-only, match-scoped). */
	UPROPERTY(Transient)
	TArray<TWeakObjectPtr<AController>> RoamingControllers;

	/** True only while the match is in MatchState::WaitingToStart. */
	bool IsWarmup() const;

	bool IsRoaming(AController* C) const;
	void SetRoaming(AController* C, bool bRoam);

	/** Toggle invulnerability + firing-disable on a pawn. */
	void ApplyRoam(AUTCharacter* Char, bool bEnable);

	/** Strip roam from all tracked pawns + clear the list. */
	void ClearAll();
};
