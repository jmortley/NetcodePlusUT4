#include "ClutchRoundState.h"
#include "UTPlayerState.h"
#include "Net/UnrealNetwork.h"


const uint8 AClutchRoundState::NoTeam;
const uint8 AClutchRoundState::UnassignedSlot;


FClutchRosterEntry::FClutchRosterEntry()
	: PlayerState(nullptr)
	, PlayerIdFallback(INDEX_NONE)
	, RosterSlot(AClutchRoundState::UnassignedSlot)
	, TeamIndex(AClutchRoundState::NoTeam)
	, PlayerRole(EClutchRole::None)
	, PlayerStatus(EClutchStatus::Queued)
	, HitsTaken(0)
{
}


AClutchRoundState::AClutchRoundState(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	bNetLoadOnClient = true;
	bReplicateMovement = false;
	NetUpdateFrequency = 10.0f;
	PrimaryActorTick.bCanEverTick = false;

	Phase = EClutchRoundPhase::Waiting;
	ActiveAttacker = nullptr;
	AttackingTeamIndex = NoTeam;
	RoundNumber = 0;
	RoundStartServerTime = 0.0f;
	PoleUnlockServerTime = 0.0f;
	RoundEndServerTime = 0.0f;
	PoleProgress = 0.0f;
	ScoreGoal = 9;
	MaxAttackerHits = 3;
	LastWinningTeamIndex = NoTeam;
	LastEndReason = NAME_None;
}


void AClutchRoundState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(AClutchRoundState, Phase);
	DOREPLIFETIME(AClutchRoundState, Roster);
	DOREPLIFETIME(AClutchRoundState, ActiveAttacker);
	DOREPLIFETIME(AClutchRoundState, AttackingTeamIndex);
	DOREPLIFETIME(AClutchRoundState, RoundNumber);
	DOREPLIFETIME(AClutchRoundState, RoundStartServerTime);
	DOREPLIFETIME(AClutchRoundState, PoleUnlockServerTime);
	DOREPLIFETIME(AClutchRoundState, RoundEndServerTime);
	DOREPLIFETIME(AClutchRoundState, PoleProgress);
	DOREPLIFETIME(AClutchRoundState, ScoreGoal);
	DOREPLIFETIME(AClutchRoundState, MaxAttackerHits);
	DOREPLIFETIME(AClutchRoundState, LastWinningTeamIndex);
	DOREPLIFETIME(AClutchRoundState, LastEndReason);
}


FString AClutchRoundState::BuildStablePlayerId(const AUTPlayerState* PlayerState)
{
	if (!PlayerState)
	{
		return FString();
	}

	if (PlayerState->UniqueId.IsValid())
	{
		return FString::Printf(TEXT("uid:%s"), *PlayerState->UniqueId.ToString());
	}

	if (!PlayerState->PlayerName.IsEmpty())
	{
		return FString::Printf(TEXT("name:%s"), *PlayerState->PlayerName.ToLower());
	}

	if (PlayerState->PlayerId >= 0)
	{
		return FString::Printf(TEXT("pid:%d"), PlayerState->PlayerId);
	}

	return FString();
}


const FClutchRosterEntry* AClutchRoundState::FindEntry(const AUTPlayerState* PlayerState) const
{
	if (!PlayerState)
	{
		return nullptr;
	}

	// Actor identity is strongest while both references are live.
	for (const FClutchRosterEntry& Entry : Roster)
	{
		if (Entry.PlayerState == PlayerState)
		{
			return &Entry;
		}
	}

	return FindEntryByIdentity(BuildStablePlayerId(PlayerState), PlayerState->PlayerId);
}


const FClutchRosterEntry* AClutchRoundState::FindEntryByIdentity(
	const FString& StablePlayerId, int32 PlayerIdFallback) const
{
	if (!StablePlayerId.IsEmpty())
	{
		for (const FClutchRosterEntry& Entry : Roster)
		{
			if (!Entry.StablePlayerId.IsEmpty() && Entry.StablePlayerId == StablePlayerId)
			{
				return &Entry;
			}
		}

		// A real online identity was supplied and did not match. Never let a
		// session-local PlayerId reuse hijack a different user's roster row.
		if (StablePlayerId.StartsWith(TEXT("uid:")))
		{
			return nullptr;
		}
	}

	if (PlayerIdFallback >= 0)
	{
		for (const FClutchRosterEntry& Entry : Roster)
		{
			if (Entry.PlayerIdFallback == PlayerIdFallback
				&& !Entry.StablePlayerId.StartsWith(TEXT("uid:")))
			{
				return &Entry;
			}
		}
	}

	return nullptr;
}


FClutchRosterEntry* AClutchRoundState::FindMutableEntry(const AUTPlayerState* PlayerState)
{
	if (!PlayerState)
	{
		return nullptr;
	}

	for (FClutchRosterEntry& Entry : Roster)
	{
		if (Entry.PlayerState == PlayerState)
		{
			return &Entry;
		}
	}

	return FindMutableEntryByIdentity(BuildStablePlayerId(PlayerState), PlayerState->PlayerId);
}


FClutchRosterEntry* AClutchRoundState::FindMutableEntryByIdentity(
	const FString& StablePlayerId, int32 PlayerIdFallback)
{
	if (!StablePlayerId.IsEmpty())
	{
		for (FClutchRosterEntry& Entry : Roster)
		{
			if (!Entry.StablePlayerId.IsEmpty() && Entry.StablePlayerId == StablePlayerId)
			{
				return &Entry;
			}
		}

		if (StablePlayerId.StartsWith(TEXT("uid:")))
		{
			return nullptr;
		}
	}

	if (PlayerIdFallback >= 0)
	{
		for (FClutchRosterEntry& Entry : Roster)
		{
			if (Entry.PlayerIdFallback == PlayerIdFallback
				&& !Entry.StablePlayerId.StartsWith(TEXT("uid:")))
			{
				return &Entry;
			}
		}
	}

	return nullptr;
}


bool AClutchRoundState::GetEntryForPlayer(
	AUTPlayerState* PlayerState, FClutchRosterEntry& OutEntry) const
{
	const FClutchRosterEntry* Entry = FindEntry(PlayerState);
	if (!Entry)
	{
		OutEntry = FClutchRosterEntry();
		return false;
	}

	OutEntry = *Entry;
	return true;
}


bool AClutchRoundState::GetEntryByIdentity(const FString& StablePlayerId,
	int32 PlayerIdFallback, FClutchRosterEntry& OutEntry) const
{
	const FClutchRosterEntry* Entry = FindEntryByIdentity(StablePlayerId, PlayerIdFallback);
	if (!Entry)
	{
		OutEntry = FClutchRosterEntry();
		return false;
	}

	OutEntry = *Entry;
	return true;
}


bool AClutchRoundState::IsGameplayPhase() const
{
	return Phase == EClutchRoundPhase::Combat || Phase == EClutchRoundPhase::Capture;
}


bool AClutchRoundState::IsEntryRoundActive(const FClutchRosterEntry& Entry) const
{
	return IsGameplayPhase() && Entry.PlayerStatus == EClutchStatus::Active;
}


bool AClutchRoundState::ShouldEntrySpectate(const FClutchRosterEntry& Entry) const
{
	return !IsEntryRoundActive(Entry);
}


bool AClutchRoundState::EntryUsesAttackerWeapon(const FClutchRosterEntry& Entry) const
{
	return IsEntryRoundActive(Entry) && Entry.PlayerRole == EClutchRole::Attacker;
}


bool AClutchRoundState::EntryUsesDefenderWeapon(const FClutchRosterEntry& Entry) const
{
	return IsEntryRoundActive(Entry) && Entry.PlayerRole == EClutchRole::Defender;
}


int32 AClutchRoundState::GetEntryArmorRemaining(const FClutchRosterEntry& Entry) const
{
	if (Entry.PlayerRole != EClutchRole::Attacker)
	{
		return 0;
	}

	return FMath::Max(0, static_cast<int32>(MaxAttackerHits) - static_cast<int32>(Entry.HitsTaken));
}


bool AClutchRoundState::IsPlayerRoundActive(AUTPlayerState* PlayerState) const
{
	const FClutchRosterEntry* Entry = FindEntry(PlayerState);
	return Entry && IsEntryRoundActive(*Entry);
}


bool AClutchRoundState::ShouldPlayerSpectate(AUTPlayerState* PlayerState) const
{
	const FClutchRosterEntry* Entry = FindEntry(PlayerState);
	return !Entry || ShouldEntrySpectate(*Entry);
}


bool AClutchRoundState::PlayerUsesAttackerWeapon(AUTPlayerState* PlayerState) const
{
	const FClutchRosterEntry* Entry = FindEntry(PlayerState);
	return Entry && EntryUsesAttackerWeapon(*Entry);
}


bool AClutchRoundState::PlayerUsesDefenderWeapon(AUTPlayerState* PlayerState) const
{
	const FClutchRosterEntry* Entry = FindEntry(PlayerState);
	return Entry && EntryUsesDefenderWeapon(*Entry);
}


int32 AClutchRoundState::GetPlayerArmorRemaining(AUTPlayerState* PlayerState) const
{
	const FClutchRosterEntry* Entry = FindEntry(PlayerState);
	return Entry ? GetEntryArmorRemaining(*Entry) : 0;
}


bool AClutchRoundState::ResetForMatch(int32 InScoreGoal, uint8 InMaxAttackerHits)
{
	if (!HasAuthority())
	{
		return false;
	}

	Phase = EClutchRoundPhase::Waiting;
	Roster.Reset();
	ActiveAttacker = nullptr;
	AttackingTeamIndex = NoTeam;
	RoundNumber = 0;
	RoundStartServerTime = 0.0f;
	PoleUnlockServerTime = 0.0f;
	RoundEndServerTime = 0.0f;
	PoleProgress = 0.0f;
	ScoreGoal = FMath::Max(1, InScoreGoal);
	MaxAttackerHits = static_cast<uint8>(FMath::Clamp<int32>(InMaxAttackerHits, 1, 255));
	LastWinningTeamIndex = NoTeam;
	LastEndReason = NAME_None;

	MarkStateDirty();
	return true;
}


bool AClutchRoundState::BeginRound(uint8 InAttackingTeamIndex,
	AUTPlayerState* InActiveAttacker, int32 InRoundNumber,
	float InRoundStartServerTime, float InPoleUnlockServerTime,
	float InRoundEndServerTime)
{
	if (!HasAuthority() || InAttackingTeamIndex > 1 || !InActiveAttacker)
	{
		return false;
	}

	Phase = EClutchRoundPhase::Intermission;
	ActiveAttacker = InActiveAttacker;
	AttackingTeamIndex = InAttackingTeamIndex;
	RoundNumber = FMath::Max(1, InRoundNumber);
	RoundStartServerTime = FMath::Max(0.0f, InRoundStartServerTime);
	PoleUnlockServerTime = FMath::Max(RoundStartServerTime, InPoleUnlockServerTime);
	RoundEndServerTime = FMath::Max(PoleUnlockServerTime, InRoundEndServerTime);
	PoleProgress = 0.0f;
	LastWinningTeamIndex = NoTeam;
	LastEndReason = NAME_None;

	if (FClutchRosterEntry* AttackerEntry = FindMutableEntry(InActiveAttacker))
	{
		AttackerEntry->PlayerState = InActiveAttacker;
		AttackerEntry->PlayerRole = EClutchRole::Attacker;
		AttackerEntry->PlayerStatus = EClutchStatus::Active;
		AttackerEntry->HitsTaken = 0;
	}

	MarkStateDirty();
	return true;
}


bool AClutchRoundState::StartRoundClock(float InRoundStartServerTime,
	float InPoleUnlockServerTime, float InRoundEndServerTime)
{
	if (!HasAuthority() || !ActiveAttacker || AttackingTeamIndex > 1)
	{
		return false;
	}

	Phase = EClutchRoundPhase::Combat;
	RoundStartServerTime = FMath::Max(0.0f, InRoundStartServerTime);
	PoleUnlockServerTime = FMath::Max(RoundStartServerTime, InPoleUnlockServerTime);
	RoundEndServerTime = FMath::Max(PoleUnlockServerTime, InRoundEndServerTime);
	MarkStateDirty();
	return true;
}


bool AClutchRoundState::SetPhase(EClutchRoundPhase NewPhase)
{
	if (!HasAuthority())
	{
		return false;
	}

	if (Phase != NewPhase)
	{
		Phase = NewPhase;
		MarkStateDirty();
	}
	return true;
}


bool AClutchRoundState::SetPoleProgress(float NewProgress)
{
	if (!HasAuthority())
	{
		return false;
	}

	const float ClampedProgress = FMath::Clamp(NewProgress, 0.0f, 100.0f);
	if (!FMath::IsNearlyEqual(PoleProgress, ClampedProgress))
	{
		PoleProgress = ClampedProgress;
		// Pole progress can change every server tick. Let NetUpdateFrequency cap
		// it at 10 Hz instead of bypassing the cap with ForceNetUpdate each frame.
		MarkStateDirty(false);
	}
	return true;
}


bool AClutchRoundState::FinishRound(uint8 WinningTeamIndex, FName EndReason, bool bMatchEnded)
{
	if (!HasAuthority() || (WinningTeamIndex > 1 && WinningTeamIndex != NoTeam))
	{
		return false;
	}

	Phase = bMatchEnded ? EClutchRoundPhase::MatchEnd : EClutchRoundPhase::RoundEnd;
	LastWinningTeamIndex = WinningTeamIndex;
	LastEndReason = EndReason;
	MarkStateDirty();
	return true;
}


bool AClutchRoundState::UpsertPlayer(AUTPlayerState* PlayerState, uint8 TeamIndex,
	uint8 RosterSlot, EClutchRole PlayerRole, EClutchStatus PlayerStatus)
{
	if (!HasAuthority() || !PlayerState || (TeamIndex > 1 && TeamIndex != NoTeam))
	{
		return false;
	}

	const FString StableId = BuildStablePlayerId(PlayerState);
	FClutchRosterEntry* Entry = FindMutableEntry(PlayerState);
	if (!Entry)
	{
		FClutchRosterEntry NewEntry;
		Roster.Add(NewEntry);
		Entry = &Roster.Last();
	}

	AUTPlayerState* PreviousPlayerState = Entry->PlayerState;
	const bool bWasActiveAttacker = ActiveAttacker == PreviousPlayerState
		|| (Entry->PlayerRole == EClutchRole::Attacker
			&& Entry->PlayerStatus == EClutchStatus::Active);

	Entry->PlayerState = PlayerState;
	if (!StableId.IsEmpty())
	{
		Entry->StablePlayerId = StableId;
	}
	Entry->PlayerIdFallback = PlayerState->PlayerId;
	Entry->PlayerNameFallback = PlayerState->PlayerName;
	Entry->TeamIndex = TeamIndex;
	Entry->RosterSlot = RosterSlot;
	Entry->PlayerRole = PlayerRole;
	Entry->PlayerStatus = PlayerStatus;
	if (PlayerRole != EClutchRole::Attacker)
	{
		Entry->HitsTaken = 0;
	}

	if (PlayerRole == EClutchRole::Attacker && PlayerStatus == EClutchStatus::Active)
	{
		ActiveAttacker = PlayerState;
	}
	else if (bWasActiveAttacker)
	{
		ActiveAttacker = nullptr;
	}

	MarkStateDirty();
	return true;
}


bool AClutchRoundState::DetachPlayer(AUTPlayerState* PlayerState)
{
	if (!HasAuthority() || !PlayerState)
	{
		return false;
	}

	FClutchRosterEntry* Entry = FindMutableEntry(PlayerState);
	if (!Entry)
	{
		return false;
	}

	if (ActiveAttacker == PlayerState)
	{
		ActiveAttacker = nullptr;
	}

	// Preserve StablePlayerId, PlayerIdFallback, name, team, and rotation slot.
	Entry->PlayerState = nullptr;
	Entry->PlayerRole = EClutchRole::None;
	Entry->PlayerStatus = EClutchStatus::Disconnected;
	Entry->HitsTaken = 0;

	MarkStateDirty();
	return true;
}


bool AClutchRoundState::ClearRoster()
{
	if (!HasAuthority())
	{
		return false;
	}

	Roster.Reset();
	ActiveAttacker = nullptr;
	MarkStateDirty();
	return true;
}


bool AClutchRoundState::SetPlayerRoundState(AUTPlayerState* PlayerState,
	EClutchRole PlayerRole, EClutchStatus PlayerStatus, uint8 InHitsTaken)
{
	if (!HasAuthority() || !PlayerState)
	{
		return false;
	}

	FClutchRosterEntry* Entry = FindMutableEntry(PlayerState);
	if (!Entry)
	{
		return false;
	}

	const bool bWasActiveAttacker = ActiveAttacker == Entry->PlayerState;
	Entry->PlayerState = PlayerState;
	Entry->PlayerRole = PlayerRole;
	Entry->PlayerStatus = PlayerStatus;
	Entry->HitsTaken = (PlayerRole == EClutchRole::Attacker)
		? static_cast<uint8>(FMath::Min<int32>(InHitsTaken, MaxAttackerHits))
		: 0;

	if (PlayerRole == EClutchRole::Attacker && PlayerStatus == EClutchStatus::Active)
	{
		ActiveAttacker = PlayerState;
	}
	else if (bWasActiveAttacker)
	{
		ActiveAttacker = nullptr;
	}

	MarkStateDirty();
	return true;
}


bool AClutchRoundState::SetPlayerHitCount(AUTPlayerState* PlayerState, uint8 InHitsTaken)
{
	if (!HasAuthority() || !PlayerState)
	{
		return false;
	}

	FClutchRosterEntry* Entry = FindMutableEntry(PlayerState);
	if (!Entry || Entry->PlayerRole != EClutchRole::Attacker)
	{
		return false;
	}

	const uint8 ClampedHits = static_cast<uint8>(FMath::Min<int32>(InHitsTaken, MaxAttackerHits));
	if (Entry->HitsTaken != ClampedHits)
	{
		Entry->HitsTaken = ClampedHits;
		MarkStateDirty();
	}
	return true;
}


uint8 AClutchRoundState::AddAttackerHit(AUTPlayerState* PlayerState)
{
	if (!HasAuthority() || !PlayerState)
	{
		return 0;
	}

	FClutchRosterEntry* Entry = FindMutableEntry(PlayerState);
	if (!Entry || Entry->PlayerRole != EClutchRole::Attacker)
	{
		return 0;
	}

	const uint8 NewHitCount = static_cast<uint8>(FMath::Min<int32>(
		static_cast<int32>(Entry->HitsTaken) + 1,
		static_cast<int32>(MaxAttackerHits)));

	if (Entry->HitsTaken != NewHitCount)
	{
		Entry->HitsTaken = NewHitCount;
		MarkStateDirty();
	}

	return Entry->HitsTaken;
}


int32 AClutchRoundState::SelectNextRotationSlot(
	const TArray<int32>& EligibleSlots, int32 PreviousSlot)
{
	TArray<int32> SortedSlots;
	SortedSlots.Reserve(EligibleSlots.Num());
	for (int32 Slot : EligibleSlots)
	{
		if (Slot >= 0)
		{
			SortedSlots.AddUnique(Slot);
		}
	}

	SortedSlots.Sort();
	if (SortedSlots.Num() == 0)
	{
		return INDEX_NONE;
	}

	for (int32 Slot : SortedSlots)
	{
		if (Slot > PreviousSlot)
		{
			return Slot;
		}
	}

	return SortedSlots[0];
}


uint8 AClutchRoundState::GetDefendingTeam(uint8 InAttackingTeamIndex)
{
	return InAttackingTeamIndex <= 1
		? static_cast<uint8>(1 - InAttackingTeamIndex)
		: NoTeam;
}


uint8 AClutchRoundState::ResolveRoundWinner(uint8 InAttackingTeamIndex,
	bool bTimedOut, int32 DefendersAlive, bool bAttackerAlive, bool bPoleCaptured)
{
	const uint8 DefendingTeam = GetDefendingTeam(InAttackingTeamIndex);
	if (DefendingTeam == NoTeam)
	{
		return NoTeam;
	}

	// Keep this priority centralized so timeout/kill/capture events in the same
	// server frame always resolve identically.
	if (bTimedOut)
	{
		return DefendingTeam;
	}
	if (DefendersAlive <= 0)
	{
		return InAttackingTeamIndex;
	}
	if (!bAttackerAlive)
	{
		return DefendingTeam;
	}
	if (bPoleCaptured)
	{
		return InAttackingTeamIndex;
	}

	return NoTeam;
}


bool AClutchRoundState::HasWonMatch(int32 WinnerScore, int32 LoserScore,
	int32 GoalScore, bool bWinByTwo, int32 MinimumWinMargin)
{
	if (WinnerScore < FMath::Max(1, GoalScore))
	{
		return false;
	}
	return !bWinByTwo
		|| WinnerScore - LoserScore >= FMath::Max(1, MinimumWinMargin);
}


void AClutchRoundState::MarkStateDirty(bool bForceImmediate)
{
	if (HasAuthority() && bForceImmediate)
	{
		ForceNetUpdate();
	}
}
