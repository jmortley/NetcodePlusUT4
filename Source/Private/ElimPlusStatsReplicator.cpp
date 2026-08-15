// ElimPlusStatsReplicator.cpp

#include "ElimPlusStatsReplicator.h"
#include "ElimPlusGame.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
#include "Net/UnrealNetwork.h"

AElimPlusStatsReplicator::AElimPlusStatsReplicator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
	NetUpdateFrequency = 2.0f;
}

void AElimPlusStatsReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AElimPlusStatsReplicator, StatsEntries);
	DOREPLIFETIME(AElimPlusStatsReplicator, bBalanceTeamsActive);
	DOREPLIFETIME(AElimPlusStatsReplicator, Team0ClutchOverlay);
	DOREPLIFETIME(AElimPlusStatsReplicator, Team1ClutchOverlay);
}

void AElimPlusStatsReplicator::SetBalanceTeamsActive(bool bActive)
{
	if (Role != ROLE_Authority) return;
	bBalanceTeamsActive = bActive;
}

FNCClutchOverlayState* AElimPlusStatsReplicator::GetMutableClutchOverlayState(int32 TeamIndex)
{
	return TeamIndex == 0 ? &Team0ClutchOverlay
		: (TeamIndex == 1 ? &Team1ClutchOverlay : nullptr);
}

const FNCClutchOverlayState* AElimPlusStatsReplicator::GetClutchOverlayState(int32 TeamIndex) const
{
	return TeamIndex == 0 ? &Team0ClutchOverlay
		: (TeamIndex == 1 ? &Team1ClutchOverlay : nullptr);
}

float AElimPlusStatsReplicator::GetClutchOverlayServerTime() const
{
	const AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	return GS ? GS->GetServerWorldTimeSeconds()
		: (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f);
}

void AElimPlusStatsReplicator::BeginClutchOverlay(int32 TeamIndex,
	AUTPlayerState* Candidate, int32 EnemiesAlive)
{
	if (Role != ROLE_Authority || !Candidate || EnemiesAlive < 1)
	{
		return;
	}

	FNCClutchOverlayState* State = GetMutableClutchOverlayState(TeamIndex);
	if (!State || State->bActive)
	{
		return;
	}

	const uint32 NextGeneration = State->Generation + 1;
	*State = FNCClutchOverlayState();
	State->Generation = NextGeneration;
	State->bActive = true;
	State->TeamIndex = static_cast<uint8>(TeamIndex);
	State->Candidate = Candidate;
	State->CandidateName = Candidate->PlayerName;
	State->CandidateId = Candidate->UniqueId.IsValid()
		? Candidate->UniqueId.ToString()
		: FString::Printf(TEXT("BOT:%s"), *Candidate->PlayerName);
	State->EnemiesAtStart = EnemiesAlive;
	State->EnemiesRemaining = EnemiesAlive;
	State->StartServerTime = GetClutchOverlayServerTime();
	ForceNetUpdate();
}

void AElimPlusStatsReplicator::CreditClutchOverlayKill(
	AUTPlayerState* KillerPS, AUTPlayerState* VictimPS)
{
	if (Role != ROLE_Authority || !KillerPS || !VictimPS)
	{
		return;
	}

	for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
	{
		FNCClutchOverlayState* State = GetMutableClutchOverlayState(TeamIndex);
		if (State && State->bActive && State->Candidate == KillerPS
			&& static_cast<int32>(VictimPS->GetTeamNum()) == 1 - TeamIndex)
		{
			++State->DirectKills;
			ForceNetUpdate();
			return;
		}
	}
}

void AElimPlusStatsReplicator::UpdateClutchOverlayRemaining(
	int32 AliveTeam0, int32 AliveTeam1)
{
	if (Role != ROLE_Authority)
	{
		return;
	}

	const int32 AliveByTeam[2] = { AliveTeam0, AliveTeam1 };
	bool bChanged = false;
	for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
	{
		FNCClutchOverlayState* State = GetMutableClutchOverlayState(TeamIndex);
		if (State && State->bActive)
		{
			const int32 NewRemaining = FMath::Max(0, AliveByTeam[1 - TeamIndex]);
			if (State->EnemiesRemaining != NewRemaining)
			{
				State->EnemiesRemaining = NewRemaining;
				bChanged = true;
			}
		}
	}
	if (bChanged)
	{
		ForceNetUpdate();
	}
}

void AElimPlusStatsReplicator::EndClutchOverlayForCandidate(
	AUTPlayerState* Candidate, ENCClutchOverlayOutcome Outcome)
{
	if (Role != ROLE_Authority || !Candidate)
	{
		return;
	}

	for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
	{
		FNCClutchOverlayState* State = GetMutableClutchOverlayState(TeamIndex);
		if (State && State->bActive && State->Candidate == Candidate)
		{
			State->bActive = false;
			State->Candidate = nullptr;
			State->Outcome = Outcome;
			State->EndServerTime = GetClutchOverlayServerTime();
			ForceNetUpdate();
			return;
		}
	}
}

void AElimPlusStatsReplicator::FinalizeClutchOverlays(int32 WinnerTeamIndex)
{
	if (Role != ROLE_Authority)
	{
		return;
	}

	bool bChanged = false;
	const float ServerNow = GetClutchOverlayServerTime();
	for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
	{
		FNCClutchOverlayState* State = GetMutableClutchOverlayState(TeamIndex);
		if (!State || !State->bActive)
		{
			continue;
		}

		State->bActive = false;
		State->Candidate = nullptr;
		State->EnemiesRemaining = WinnerTeamIndex == TeamIndex ? 0 : State->EnemiesRemaining;
		State->Outcome = WinnerTeamIndex == TeamIndex
			? ENCClutchOverlayOutcome::Clutched
			: (WinnerTeamIndex == INDEX_NONE
				? ENCClutchOverlayOutcome::Cancelled
				: ENCClutchOverlayOutcome::Denied);
		State->EndServerTime = ServerNow;
		bChanged = true;
	}
	if (bChanged)
	{
		ForceNetUpdate();
	}
}

void AElimPlusStatsReplicator::ClearClutchOverlays()
{
	if (Role != ROLE_Authority)
	{
		return;
	}

	for (int32 TeamIndex = 0; TeamIndex < 2; ++TeamIndex)
	{
		FNCClutchOverlayState* State = GetMutableClutchOverlayState(TeamIndex);
		const uint32 NextGeneration = State->Generation + 1;
		*State = FNCClutchOverlayState();
		State->Generation = NextGeneration;
		State->TeamIndex = static_cast<uint8>(TeamIndex);
	}
	ForceNetUpdate();
}

void AElimPlusStatsReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.f;
}

void AElimPlusStatsReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (Role != ROLE_Authority)
	{
		return;
	}
	TimeSinceLastUpdate += DeltaTime;
	if (TimeSinceLastUpdate >= UpdateInterval)
	{
		TimeSinceLastUpdate = 0.f;
		UpdateFromPlayerStates();
	}
}

void AElimPlusStatsReplicator::UpdateFromPlayerStates()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS)
	{
		return;
	}

	StatsEntries.Reset();

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS->bOnlySpectator)
		{
			continue;
		}

		// Bots have invalid UniqueIds — key them with the synthetic "BOT:<name>"
		// shape used everywhere else (rating system, balancer). Lets bot ELOs
		// flow through the same replicator path when bRandomizeBotElo is on.
		FElimPlusStatsEntry Entry;
		Entry.PlayerId = UTPS->UniqueId.IsValid()
			? UTPS->UniqueId.ToString()
			: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);

		// PPR(Current) — pulled from server-only side cache populated by gamemode
		// at end-of-round via SetPlayerPPRCurrent.
		if (const float* PPRPtr = PPRCurrentCache.Find(Entry.PlayerId))
		{
			Entry.PPRCurrent = *PPRPtr;
		}

		// DMG column shows OVERKILL-INCLUSIVE match damage (each hit at full value, incl.
		// the portion beyond victim HP) so the "100 dmg = 1 pt" PPR rule stays exact and
		// the columns line up. Source is the gamemode's per-player accumulator (server-
		// only; this runs on Authority). Engine AUTPlayerState::DamageDone (overkill-
		// stripped, sent to StatSQL) is the fallback for listen-server/standalone where
		// the gamemode cast is unavailable.
		bool bGotOverkillDamage = false;
		if (AElimPlusGame* EG = GetWorld()->GetAuthGameMode<AElimPlusGame>())
		{
			Entry.DamageDone = FMath::RoundToInt(EG->GetMatchDamageForPlayer(UTPS));
			bGotOverkillDamage = true;
		}
		if (!bGotOverkillDamage)
		{
			// Fallback: engine DamageDone via reflection (server-side, not replicated).
			UIntProperty* DmgProp = FindField<UIntProperty>(UTPS->GetClass(), TEXT("DamageDone"));
			if (DmgProp)
			{
				Entry.DamageDone = DmgProp->GetPropertyValue_InContainer(UTPS);
			}
		}

		// ELO: source of truth is FElimPlusRatingSystem on the gamemode. It pushes
		// values via SetPlayerEloAndDelta — but ONLY at HandleMatchHasEnded, not
		// per round. So during a match the cache stays at the snapshotted match-
		// start value (or 1400 baseline for fresh players who haven't loaded yet).
		// We never read AUTPlayerState::TDMRank — defunct in this fork (see
		// feedback_no_epic_mcp_or_tdmrank memory).
		if (const int32* EloPtr = EloCache.Find(Entry.PlayerId))
		{
			Entry.Elo = *EloPtr;
		}
		if (const int32* DeltaPtr = EloDeltaCache.Find(Entry.PlayerId))
		{
			Entry.EloDeltaThisMatch = *DeltaPtr;
		}
		// Global leaderboard rank — pushed by the gamemode/rating system at match
		// start + end (frozen like ELO). 0 stays for bots / unranked players.
		if (const int32* RankPtr = GlobalRankCache.Find(Entry.PlayerId))
		{
			Entry.GlobalRank = *RankPtr;
		}

		// PPR + EloDeltaThisMatch are populated by the gamemode (next phase).
		// For now they stay at defaults — the LG accuracy is computed below.

		// "LG_Acc" is hitscan accuracy. In instagib it's the instagib rifle;
		// otherwise it's the Sniper OR the Lightning Gun. The LG is a Blueprint reskin of
		// AUTPlusSniper but OVERRIDES the stat names in its Class Defaults to
		// LightningRifleHits/LightningRifleShots (NOT SniperHits/Shots) — so we must read BOTH
		// weapons and sum them. A player runs one or the other, so the unused weapon's stats are
		// 0 and the sum is the right per-shot ratio. Auto-detect instagib via NAME_InstagibShots.
		static const FName NAME_LightningRifleHits(TEXT("LightningRifleHits"));
		static const FName NAME_LightningRifleShots(TEXT("LightningRifleShots"));
		float HitscanShots = 0.f;
		float HitscanHits  = 0.f;
		const float InstagibShots = UTPS->GetStatsValue(NAME_InstagibShots);
		if (InstagibShots > 0.f)
		{
			HitscanShots = InstagibShots;
			HitscanHits  = UTPS->GetStatsValue(NAME_InstagibHits);
		}
		else
		{
			// Sniper + Lightning Gun (the LG writes LightningRifle* stats, the Sniper writes Sniper*).
			HitscanHits  = UTPS->GetStatsValue(NAME_SniperHits)  + UTPS->GetStatsValue(NAME_LightningRifleHits);
			HitscanShots = UTPS->GetStatsValue(NAME_SniperShots) + UTPS->GetStatsValue(NAME_LightningRifleShots);
		}
		if (HitscanShots > 0.f)
		{
			const float Acc = (HitscanHits / HitscanShots) * 100.f;
			Entry.LinkGunAccuracyTimes100 = FMath::RoundToInt(FMath::Clamp(Acc, 0.f, 100.f) * 100.f);
		}
		else
		{
			// No shots with the tracked weapon -> sentinel so the scoreboard
			// prints "-" instead of a misleading 0% (matches NCPlusCTFScoreboard).
			Entry.LinkGunAccuracyTimes100 = -1;
		}

		StatsEntries.Add(Entry);
	}

	// Server-side: keep the O(1) lookup in sync (clients refresh via OnRep_StatsEntries).
	RebuildEntryIndex();
}

void AElimPlusStatsReplicator::RebuildEntryIndex()
{
	EntryIndexByPlayerId.Reset();
	for (int32 i = 0; i < StatsEntries.Num(); ++i)
	{
		// Keep first-match semantics (matches the old linear scan) if two entries ever
		// share a PlayerId (e.g. two bots with the same name → same "BOT:<name>" key).
		if (!EntryIndexByPlayerId.Contains(StatsEntries[i].PlayerId))
		{
			EntryIndexByPlayerId.Add(StatsEntries[i].PlayerId, i);
		}
	}
}

void AElimPlusStatsReplicator::OnRep_StatsEntries()
{
	RebuildEntryIndex();
}

const FElimPlusStatsEntry* AElimPlusStatsReplicator::FindEntry(const FString& UniqueIdStr) const
{
	if (const int32* Idx = EntryIndexByPlayerId.Find(UniqueIdStr))
	{
		if (StatsEntries.IsValidIndex(*Idx))
		{
			return &StatsEntries[*Idx];
		}
	}
	return nullptr;
}

int32 AElimPlusStatsReplicator::GetDamageForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return E ? E->DamageDone : 0;
}

float AElimPlusStatsReplicator::GetPPRCurrentForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return E ? E->PPRCurrent : 0.f;
}

int32 AElimPlusStatsReplicator::GetEloForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return E ? E->Elo : 1400;
}

int32 AElimPlusStatsReplicator::GetEloDeltaForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return E ? E->EloDeltaThisMatch : 0;
}

float AElimPlusStatsReplicator::GetLinkGunAccuracyForPlayer(const FString& UniqueIdStr) const
{
	const FElimPlusStatsEntry* E = FindEntry(UniqueIdStr);
	return (E && E->LinkGunAccuracyTimes100 >= 0) ? (static_cast<float>(E->LinkGunAccuracyTimes100) / 100.f) : -1.f;
}

void AElimPlusStatsReplicator::SetPlayerPPRCurrent(const FString& UniqueIdStr, float Value)
{
	if (Role != ROLE_Authority) return;
	PPRCurrentCache.FindOrAdd(UniqueIdStr) = Value;
}

void AElimPlusStatsReplicator::SetPlayerEloAndDelta(const FString& UniqueIdStr, int32 NewElo, int32 DeltaThisMatch)
{
	if (Role != ROLE_Authority) return;
	EloCache.FindOrAdd(UniqueIdStr) = NewElo;
	EloDeltaCache.FindOrAdd(UniqueIdStr) = DeltaThisMatch;
}

void AElimPlusStatsReplicator::SetPlayerGlobalRank(const FString& UniqueIdStr, int32 Rank)
{
	if (Role != ROLE_Authority) return;
	GlobalRankCache.FindOrAdd(UniqueIdStr) = Rank;
}
