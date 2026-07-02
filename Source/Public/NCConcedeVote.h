// NCConcedeVote — "gg" concede vote: the LOSING team can vote to forfeit the match.
// Port of zo's CustomHUD BP concede (type gg in console -> vote opens; F1 confirm /
// F4 cancel, keys honoured from the old ElimZoHUD Game.ini section; more than 50% of
// the losing team's HUMANS agree -> the match ends immediately with the leaders the
// winner — in a duel that's just the losing player). Server-authoritative; votes pin
// to the team that was losing when the vote opened and void on a lead flip.
#pragma once
#include "NetcodePlus.h"
#include "GameFramework/Info.h"
#include "UTLocalMessage.h"
#include "NCConcedeVote.generated.h"

/** Per-player owned RPC channel for the concede vote (the version-gate pattern:
 *  owner-only relevancy, no replicated state, no tick). Spawned server-side at
 *  PostLogin for every human remote player; the client console commands
 *  (gg / concedeconfirm / concedecancel) find their own instance and RPC through it.
 *  The listen host skips the actor — its commands call NCConcede::HandleVote directly. */
UCLASS()
class NETCODEPLUS_API ANCConcedeVote : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	/** Client -> server vote action. 0 = start-or-confirm (gg), 1 = confirm ONLY if a
	 *  vote is already live (F1 — never opens a vote, so a stray keypress can't),
	 *  2 = retract (F4). */
	UFUNCTION(Reliable, Server, WithValidation)
	void ServerConcede(uint8 Action);
};

/** Concede announcements. Switch 0 = vote progress to the losing team, packed
 *  (Votes << 8) | Needed; Switch 1 = "RED/BLUE CONCEDES" to everyone (team read from
 *  RelatedPlayerState_1, ShockDomMessage-style). */
UCLASS()
class NETCODEPLUS_API UNCConcedeMessage : public UUTLocalMessage
{
	GENERATED_UCLASS_BODY()

public:
	virtual FText GetText(int32 Switch, bool bTargetsPlayerState1, APlayerState* RelatedPlayerState_1,
		APlayerState* RelatedPlayerState_2, UObject* OptionalObject) const override;

	virtual FLinearColor GetMessageColor_Implementation(int32 MessageIndex) const override;
};

namespace NCConcede
{
	/** Vote actions (the ServerConcede/HandleVote uint8). */
	static const uint8 kActionStartOrConfirm = 0;   // gg
	static const uint8 kActionConfirmOnly    = 1;   // F1
	static const uint8 kActionCancel         = 2;   // F4

	/** Spawn the per-player vote channel (server, PostLogin — call right after the
	 *  version gate's SpawnFor). Self-guards: authority only, skips bots and the
	 *  listen host. */
	NETCODEPLUS_API void SpawnFor(class APlayerController* PC);

	/** Server-side vote handling — actions above. Safe to call against any game mode;
	 *  no-ops unless a 2-team UT team game is InProgress and the sender is a human on
	 *  the losing team (cancel is always honoured). Ends the match through the mode's
	 *  virtual EndGame so every override (rating flush, stats upload, timers, lineup)
	 *  runs exactly as a normal win. */
	NETCODEPLUS_API void HandleVote(class APlayerController* PC, uint8 Action);
}
