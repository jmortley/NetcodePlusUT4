// WipeoutDamageReplicator.h — Lightweight replicated actor that broadcasts
// per-player damage totals from server to all clients.
// DamageDone on AUTPlayerState is not replicated by Epic, so this actor
// periodically snapshots it and replicates via a parallel array.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Info.h"
#include "NCClutchOverlay.h"
#include "WipeoutDamageReplicator.generated.h"

/** Single entry: maps a player's UniqueId string to their total damage */
USTRUCT()
struct FReplicatedDamageEntry
{
	GENERATED_BODY()

	UPROPERTY()
	FString PlayerId;

	UPROPERTY()
	int32 DamageDone = 0;

	UPROPERTY()
	int32 BeltPickups = 0;

	UPROPERTY()
	int32 AmpPickups = 0;

	UPROPERTY()
	int32 VestPickups = 0;

	/** Siphon powerup pickups. SiphonPowerup deliberately reuses the stock
	 *  Berserk stat names (SiphonPowerup.cpp) to avoid an engine edit, so this
	 *  reads NAME_BerserkCount. */
	UPROPERTY()
	int32 SiphonPickups = 0;

	/** Health given to teammates this match. Lives on the GameMode
	 *  (AUWipeoutGame::HealingDoneThisMatch, credited by CreditHealing) — server
	 *  only, hence the trip through this replicator like DamageDone. */
	UPROPERTY()
	int32 HealingDone = 0;
};

UCLASS(NotPlaceable)
class NETCODEPLUS_API AWipeoutDamageReplicator : public AInfo
{
	GENERATED_UCLASS_BODY()

public:

	/** Replicated damage snapshot — server writes, clients read */
	UPROPERTY(Replicated, Transient)
	TArray<FReplicatedDamageEntry> DamageEntries;

	/** Sparse, event-driven last-alive state. Two named fields avoid old-UHT
	 *  fixed-array edge cases and preserve simultaneous 1v1 attempts. */
	UPROPERTY(Replicated, Transient)
	FNCClutchOverlayState Team0ClutchOverlay;

	UPROPERTY(Replicated, Transient)
	FNCClutchOverlayState Team1ClutchOverlay;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Look up a player's full replicated snapshot by UniqueNetId string. */
	const FReplicatedDamageEntry* FindEntry(const FString& UniqueIdStr) const;

	/** Look up individual values. Missing players return 0. Safe on clients. */
	int32 GetDamageForPlayer(const FString& UniqueIdStr) const;
	int32 GetBeltsForPlayer(const FString& UniqueIdStr) const;
	int32 GetAmpsForPlayer(const FString& UniqueIdStr) const;
	int32 GetVestsForPlayer(const FString& UniqueIdStr) const;
	int32 GetSiphonsForPlayer(const FString& UniqueIdStr) const;
	int32 GetHealingForPlayer(const FString& UniqueIdStr) const;

	/** Client-safe team-slot accessor. Invalid indexes return null. */
	const FNCClutchOverlayState* GetClutchOverlayState(int32 TeamIndex) const;

	/** Sparse authority-only clutch/hold state mutations. */
	void BeginClutchOverlay(int32 TeamIndex, AUTPlayerState* Candidate,
		int32 EnemiesAlive, bool bSuddenDeath);
	void CreditClutchOverlayKill(AUTPlayerState* KillerPS, AUTPlayerState* VictimPS);
	void UpdateClutchOverlayRemaining(int32 AliveTeam0, int32 AliveTeam1);
	void PromoteClutchOverlayToSuddenDeath(int32 TeamIndex);
	void EndClutchOverlay(int32 TeamIndex, ENCClutchOverlayOutcome Outcome,
		int32 TeammatesRespawned = 0);
	void ClearClutchOverlays();

	/** Authority-only event updates for the exact round-clock anchor. The
	 *  deadline rides as a reserved metadata row in the existing DamageEntries
	 *  replication stream, preserving the actor and struct layouts. */
	void SetRoundClockDeadline(float EndServerTime);
	void ClearRoundClockDeadline();

	/** Client-safe exact round time. -1 means no active replicated anchor, so
	 *  callers should fall back to the legacy BP/stock clock. */
	int32 GetRoundClockSecondsRemaining() const;

	/** Server-only: refresh DamageEntries from all PlayerStates */
	void UpdateFromPlayerStates();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

private:
	FNCClutchOverlayState* GetMutableClutchOverlayState(int32 TeamIndex);
	float GetClutchOverlayServerTime() const;

	/** How often to snapshot damage (seconds) */
	float UpdateInterval = 1.0f;
	float TimeSinceLastUpdate = 0.0f;
};
