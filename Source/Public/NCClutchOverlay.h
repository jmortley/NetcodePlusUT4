// NCClutchOverlay.h - shared replicated clutch/last-alive state and client HUD renderer.
#pragma once

#include "NetcodePlus.h"
#include "NCClutchOverlay.generated.h"

class AUTPlayerState;
class AUTHUD;
class UCanvas;

/** Terminal presentation state for one clutch/hold attempt. */
UENUM()
enum class ENCClutchOverlayOutcome : uint8
{
	None,
	Clutched,
	Held,
	Denied,
	Cancelled
};

/**
 * Compact event-driven state for one team's active or recently completed
 * clutch attempt. Replicators keep two entries (red/blue), which also handles
 * the legitimate case where both players in a 1v1 have an open attempt.
 */
USTRUCT()
struct FNCClutchOverlayState
{
	GENERATED_BODY()

	/** Incremented whenever this team starts or clears an attempt. */
	UPROPERTY()
	uint32 Generation = 0;

	UPROPERTY()
	bool bActive = false;

	/** Wipeout hold semantics. False means an Elim-style clutch. */
	UPROPERTY()
	bool bRespawnHold = false;

	/** A Wipeout hold becomes a true clutch when respawns are permanently off. */
	UPROPERTY()
	bool bSuddenDeath = false;

	UPROPERTY()
	uint8 TeamIndex = 255;

	/** Live pointer when available; captured text below survives disconnect/destruction. */
	UPROPERTY()
	AUTPlayerState* Candidate = nullptr;

	UPROPERTY()
	FString CandidateName;

	UPROPERTY()
	FString CandidateId;

	UPROPERTY()
	int32 EnemiesAtStart = 0;

	UPROPERTY()
	int32 EnemiesRemaining = 0;

	UPROPERTY()
	int32 DirectKills = 0;

	/** Wipeout-only: teammates who returned to end a successful hold. */
	UPROPERTY()
	int32 TeammatesRespawned = 0;

	/** AGameStateBase::GetServerWorldTimeSeconds() on open/close. */
	UPROPERTY()
	float StartServerTime = 0.f;

	UPROPERTY()
	float EndServerTime = 0.f;

	UPROPERTY()
	ENCClutchOverlayOutcome Outcome = ENCClutchOverlayOutcome::None;
};

namespace NCClutchOverlay
{
	/** Client-local [NetcodePlus] ShowClutchOverlay preference; defaults on. */
	NETCODEPLUS_API bool IsEnabled();

	/** Persist and immediately publish the client-local visibility preference. */
	NETCODEPLUS_API void SetEnabled(bool bEnabled);

	/**
	 * Draw the two replicated team states for true spectators/casters only.
	 * Also honors the movable/hideable `clutch_overlay` nchud draw-call alias.
	 */
	NETCODEPLUS_API void Draw(AUTHUD* HUD, UCanvas* Canvas,
		const FNCClutchOverlayState& Team0State,
		const FNCClutchOverlayState& Team1State);
}
