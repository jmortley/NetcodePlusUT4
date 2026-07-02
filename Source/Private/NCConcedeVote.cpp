// NCConcedeVote.cpp — see header. Server-authoritative concede vote.
#include "NCConcedeVote.h"
#include "UnrealTournament.h"
#include "UTGameMode.h"
#include "UTGameState.h"
#include "UTPlayerController.h"
#include "UTBasePlayerController.h"   // ClientSay
#include "UTPlayerState.h"
#include "UTTeamInfo.h"
#include "UTATypes.h"                 // ChatDestinations
#include "NCLeagueDuelGame.h"         // duel-only winner-PS score consistency
#include "Engine/World.h"

namespace
{
	// One vote per world, server-side only. Votes = UniqueIds of losing-team humans who
	// confirmed. The vote pins to the team that was LOSING when it opened; a lead flip
	// voids it (a stale round-1 vote must never end a match the team no longer trails).
	struct FNCConcedeState
	{
		bool bActive = false;
		int32 TeamIdx = INDEX_NONE;
		TSet<FString> Votes;
	};
	TMap<TWeakObjectPtr<UWorld>, FNCConcedeState> GConcede;

	FNCConcedeState& ConcedeStateFor(UWorld* World)
	{
		for (auto It = GConcede.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid()) { It.RemoveCurrent(); }
		}
		return GConcede.FindOrAdd(World);
	}

	FString UidOf(const AUTPlayerState* PS)
	{
		return PS->UniqueId.IsValid() ? PS->UniqueId.ToString() : PS->PlayerName;
	}

	bool IsVotingHuman(const AUTPlayerState* PS)
	{
		return PS && !PS->bOnlySpectator && !PS->bIsABot;
	}

	// '[System]' chat line to one player (stock RPC — renders on any client with a UT HUD).
	void SayTo(APlayerController* PC, const FString& Msg)
	{
		if (AUTBasePlayerController* B = Cast<AUTBasePlayerController>(PC))
		{
			B->ClientSay(nullptr, Msg, ChatDestinations::System);
		}
	}

	// Progress prompt: the LOSING team gets the center banner (packed switch) + an
	// actionable chat line (NCPlusHostPause dual-channel pattern); everyone ELSE gets an
	// informational chat line — the leaders should know a concede is brewing (user call
	// 2026-07-01), they just have no key to press.
	void AnnounceProgress(UWorld* World, int32 TeamIdx, int32 Votes, int32 Needed)
	{
		const int32 Packed = ((Votes & 0xFF) << 8) | (Needed & 0xFF);
		const FString VoterMsg = FString::Printf(
			TEXT("Concede vote: %d/%d — press F1 to confirm, F4 to cancel (or type gg)"), Votes, Needed);
		const FString ObserverMsg = FString::Printf(
			TEXT("The %s team is considering conceding (%d/%d)."),
			(TeamIdx == 0) ? TEXT("RED") : TEXT("BLUE"), Votes, Needed);
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
			if (!PC || !PC->UTPlayerState || PC->UTPlayerState->bIsABot) { continue; }
			if ((int32)PC->UTPlayerState->GetTeamNum() == TeamIdx)
			{
				PC->ClientReceiveLocalizedMessage(UNCConcedeMessage::StaticClass(), Packed);
				SayTo(PC, VoterMsg);
			}
			else
			{
				SayTo(PC, ObserverMsg);
			}
		}
	}

	void AnnounceToAll(UWorld* World, const FString& Msg)
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			SayTo(It->Get(), Msg);
		}
	}
}

// ─── ANCConcedeVote ────────────────────────────────────────────────────────────────

ANCConcedeVote::ANCConcedeVote(const FObjectInitializer& OI)
	: Super(OI)
{
	bReplicates = true;
	bOnlyRelevantToOwner = true;      // only the owning client ever receives it
	PrimaryActorTick.bCanEverTick = false;
	NetUpdateFrequency = 1.f;         // no replicated UPROPERTYs — pure RPC channel
}

bool ANCConcedeVote::ServerConcede_Validate(uint8 /*Action*/) { return true; }
void ANCConcedeVote::ServerConcede_Implementation(uint8 Action)
{
	if (Role != ROLE_Authority) { return; }
	NCConcede::HandleVote(Cast<APlayerController>(GetOwner()), Action);
}

// ─── UNCConcedeMessage ─────────────────────────────────────────────────────────────

UNCConcedeMessage::UNCConcedeMessage(const FObjectInitializer& OI)
	: Super(OI)
{
	bIsStatusAnnouncement = true;
	Lifetime = 3.0f;
	MessageArea = FName(TEXT("Announcements"));
	MessageSlot = FName(TEXT("GameMessages"));
}

FText UNCConcedeMessage::GetText(int32 Switch, bool /*bTargetsPlayerState1*/,
	APlayerState* RelatedPlayerState_1, APlayerState* /*RelatedPlayerState_2*/,
	UObject* /*OptionalObject*/) const
{
	if (Switch == 1)
	{
		FString TeamName = TEXT("The losing team");
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(RelatedPlayerState_1);
		if (UTPS && UTPS->Team)
		{
			TeamName = (UTPS->Team->TeamIndex == 0) ? TEXT("RED") : TEXT("BLUE");
		}
		return FText::FromString(FString::Printf(TEXT("%s CONCEDES"), *TeamName));
	}

	// Switch 0: packed (Votes << 8) | Needed (ClientHitsounds int-packing precedent).
	const int32 Votes  = (Switch >> 8) & 0xFF;
	const int32 Needed = Switch & 0xFF;
	return FText::FromString(FString::Printf(
		TEXT("Concede vote: %d/%d — F1 confirm / F4 cancel"), Votes, Needed));
}

FLinearColor UNCConcedeMessage::GetMessageColor_Implementation(int32 /*MessageIndex*/) const
{
	return FLinearColor::White;
}

// ─── NCConcede namespace ───────────────────────────────────────────────────────────

void NCConcede::SpawnFor(APlayerController* PC)
{
	if (PC == nullptr || !PC->HasAuthority()) { return; }
	AUTPlayerController* UTPC = Cast<AUTPlayerController>(PC);
	if (UTPC && UTPC->UTPlayerState && UTPC->UTPlayerState->bIsABot) { return; }
	// The listen host needs no RPC channel — its console commands call HandleVote directly.
	if (PC->IsLocalController()) { return; }
	UWorld* W = PC->GetWorld();
	if (W == nullptr) { return; }
	FActorSpawnParameters Params;
	Params.Owner = PC;   // drives bOnlyRelevantToOwner routing + legitimizes the client->server RPC
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	W->SpawnActor<ANCConcedeVote>(ANCConcedeVote::StaticClass(), Params);
}

void NCConcede::HandleVote(APlayerController* PC, uint8 Action)
{
	if (!PC || !PC->HasAuthority()) { return; }
	UWorld* const World = PC->GetWorld();
	if (!World) { return; }
	AUTGameMode* const GM = Cast<AUTGameMode>(World->GetAuthGameMode());
	AUTGameState* const GS = World->GetGameState<AUTGameState>();
	if (!GM || !GS || GS->Teams.Num() != 2 || !GS->Teams[0] || !GS->Teams[1]) { return; }
	if (!GM->IsMatchInProgress() || GS->HasMatchEnded())
	{
		// ElimPlus round breaks are custom match states (RoundCooldown/Intermission) that
		// IsMatchInProgress rejects — exactly when a just-eliminated team WILL type gg.
		// Ending a match from inside a round transition is deliberately not supported
		// (round-restart timers race EndGame), but a silent no-op reads as "broken":
		// tell the sender to retry. Warmup (match not started) stays silent.
		if (!GS->HasMatchEnded() && GM->HasMatchStarted())
		{
			SayTo(PC, TEXT("Concede: wait for the next round to start, then vote again."));
		}
		return;
	}

	AUTPlayerState* const PS = Cast<AUTPlayerState>(PC->PlayerState);
	if (!IsVotingHuman(PS)) { return; }
	const FString Uid = UidOf(PS);

	FNCConcedeState& V = ConcedeStateFor(World);

	// The CURRENT losing team — strictly lower score; a tie means nobody can concede.
	const int32 Losing = (GS->Teams[0]->Score < GS->Teams[1]->Score) ? 0
	                   : (GS->Teams[1]->Score < GS->Teams[0]->Score) ? 1 : INDEX_NONE;

	// Lead flip since the vote opened -> the pinned vote is void.
	if (V.bActive && V.TeamIdx != Losing)
	{
		V = FNCConcedeState();
	}

	if (Action == kActionCancel)
	{
		if (V.bActive && V.Votes.Remove(Uid) > 0)
		{
			if (V.Votes.Num() == 0)
			{
				// Everyone — the leaders were told the vote was brewing, so they also
				// learn it's off (user call 2026-07-01: leaders should know somehow).
				AnnounceToAll(World, TEXT("Concede vote cancelled."));
				V = FNCConcedeState();
			}
			else
			{
				SayTo(PC, TEXT("Concede: your vote was withdrawn."));
			}
		}
		return;
	}

	if (Losing == INDEX_NONE)
	{
		SayTo(PC, TEXT("Concede: you can only concede while your team is losing."));
		return;
	}
	if ((int32)PS->GetTeamNum() != Losing)
	{
		SayTo(PC, TEXT("Concede: only the losing team can vote to concede."));
		return;
	}
	// F1 confirms an existing vote but never opens one (stray keypress safety).
	if (Action == kActionConfirmOnly && !V.bActive) { return; }

	// Dedup BEFORE any announce: a repeat gg/F1 from someone who already voted must be
	// cheap — the progress announce sends 2 reliable RPCs to the whole losing team, so
	// a key bound to scrollwheel would otherwise amplify into team-wide spam.
	if (V.bActive && V.Votes.Contains(Uid))
	{
		SayTo(PC, TEXT("Concede: you already voted — press F4 to withdraw."));
		return;
	}

	if (!V.bActive)
	{
		V.bActive = true;
		V.TeamIdx = Losing;
	}
	V.Votes.Add(Uid);

	// Threshold over the losing team's CURRENT humans; votes from leavers/team-switchers
	// are dropped here so they can never carry a concede.
	int32 Humans = 0;
	TSet<FString> ValidVotes;
	for (APlayerState* P : GS->PlayerArray)
	{
		AUTPlayerState* U = Cast<AUTPlayerState>(P);
		if (!IsVotingHuman(U) || (int32)U->GetTeamNum() != Losing) { continue; }
		++Humans;
		const FString PUid = UidOf(U);
		if (V.Votes.Contains(PUid)) { ValidVotes.Add(PUid); }
	}
	V.Votes = MoveTemp(ValidVotes);
	if (Humans == 0) { GConcede.Remove(World); return; }

	const int32 Needed = Humans / 2 + 1;   // strictly more than 50%; 1 human (duel) -> 1
	if (V.Votes.Num() < Needed)
	{
		AnnounceProgress(World, Losing, V.Votes.Num(), Needed);
		return;
	}

	// ── Concede passes: forfeit to the other team through the NORMAL end-of-match flow ──
	const int32 W = 1 - Losing;

	// TEAM scores are already consistent by construction — 'Losing' is defined by a
	// strictly lower team score, so the winners lead the team scoreboard as-is.
	AUTPlayerState* Winner = nullptr;
	float TopLoserScore = 0.f;
	for (APlayerState* P : GS->PlayerArray)
	{
		AUTPlayerState* U = Cast<AUTPlayerState>(P);
		if (!U || U->bOnlySpectator) { continue; }
		const int32 Team = (int32)U->GetTeamNum();
		if (Team == W)
		{
			if (!Winner || U->Score > Winner->Score) { Winner = U; }
		}
		else if (Team == Losing)
		{
			TopLoserScore = FMath::Max(TopLoserScore, U->Score);
		}
	}
	if (!Winner)
	{
		SayTo(PC, TEXT("Concede: no opponents remain."));
		GConcede.Remove(World);
		return;
	}
	// ONLY the duel upload derives its winner from the top PLAYER score (NCLeagueDuelGame
	// HandleMatchHasEnded picks top-2 PS->Score) — keep it consistent there. Team-score
	// modes keep real frag counts: bumping PS->Score in ElimPlus/Wipeout would fabricate
	// the winning fragger's score on the final scoreboard/StatSQL (their PPR/ELO never
	// read PS->Score, review-verified).
	if (GM->IsA(ANCLeagueDuelGame::StaticClass()) && Winner->Score <= TopLoserScore)
	{
		Winner->Score = TopLoserScore + 1.f;
		Winner->ForceNetUpdate();
	}

	// Announce, then end. NEVER pass a null winner to EndGame (it would elect the global
	// top scorer regardless of team); the virtual dispatch runs every mode override
	// (ShockDom timer clear, CTF replay gating, rating flush via HandleMatchHasEnded).
	GM->BroadcastLocalized(GM, UNCConcedeMessage::StaticClass(), 1, PS, nullptr, nullptr);
	AnnounceToAll(World, FString::Printf(TEXT("%s team concedes the match."),
		(Losing == 0) ? TEXT("RED") : TEXT("BLUE")));
	UE_LOG(LogTemp, Warning, TEXT("[Concede] team %d concedes (%d/%d votes) — ending match, winner team %d"),
		Losing, Needed, Humans, W);

	GConcede.Remove(World);
	GM->EndGame(Winner, FName(TEXT("Concede")));
}
