#include "ClutchRoundState.h"
#include "UTPlayerState.h"
#include "Net/UnrealNetwork.h"


const uint8 AClutchRoundState::NoTeam;
const uint8 AClutchRoundState::UnassignedSlot;


FClutchRosterEntry::FClutchRosterEntry()
	: PlayerState(nullptr)
	, PlayerIdFallback(INDEX_NONE)
	, RosterSlot(AClutchRoundState::UnassignedSlot)
	, AttackOrderIndex(AClutchRoundState::UnassignedSlot)
	, bAttackOrderSelector(false)
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
	AttackOrderLockedMask = 0;
	AttackOrderDeadlineServerTime = 0.0f;
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
	DOREPLIFETIME(AClutchRoundState, AttackOrderLockedMask);
	DOREPLIFETIME(AClutchRoundState, AttackOrderDeadlineServerTime);
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


bool AClutchRoundState::IsAttackOrderLocked(uint8 TeamIndex) const
{
	return TeamIndex <= 1
		&& (AttackOrderLockedMask & static_cast<uint8>(1 << TeamIndex)) != 0;
}


bool AClutchRoundState::AreAttackOrdersLocked() const
{
	return (AttackOrderLockedMask & 0x03) == 0x03;
}


bool AClutchRoundState::IsPlayerAttackOrderSelector(AUTPlayerState* PlayerState) const
{
	const FClutchRosterEntry* Entry = FindEntry(PlayerState);
	return Entry && Entry->bAttackOrderSelector;
}


void AClutchRoundState::GetTeamAttackOrderSlots(
	uint8 TeamIndex, TArray<int32>& OutSlots, bool bIncludeDisconnected) const
{
	OutSlots.Reset();
	if (TeamIndex > 1)
	{
		return;
	}

	// A live entry is a connected player. A disconnected entry keeps its preserved
	// TeamIndex/RosterSlot/AttackOrderIndex (see DetachPlayer) and is only counted when
	// the caller wants the full rotation order — used to advance past a departed
	// previous attacker without collapsing the sequence back to the front.
	auto EntryCounts = [bIncludeDisconnected](const FClutchRosterEntry& Entry) -> bool
	{
		const bool bLive = Entry.PlayerState != nullptr
			&& Entry.PlayerStatus != EClutchStatus::Disconnected;
		const bool bReserved = bIncludeDisconnected
			&& Entry.PlayerStatus == EClutchStatus::Disconnected;
		return bLive || bReserved;
	};

	// First honor every assigned order index. The second pass appends a newly
	// joined or otherwise unassigned player in stable roster-slot order.
	for (int32 OrderIndex = 0; OrderIndex < 6; ++OrderIndex)
	{
		for (const FClutchRosterEntry& Entry : Roster)
		{
			if (EntryCounts(Entry)
				&& Entry.TeamIndex == TeamIndex
				&& Entry.RosterSlot != UnassignedSlot
				&& Entry.AttackOrderIndex == OrderIndex)
			{
				OutSlots.AddUnique(static_cast<int32>(Entry.RosterSlot));
			}
		}
	}

	for (int32 RosterSlot = 0; RosterSlot < 6; ++RosterSlot)
	{
		for (const FClutchRosterEntry& Entry : Roster)
		{
			if (EntryCounts(Entry)
				&& Entry.TeamIndex == TeamIndex
				&& Entry.RosterSlot == RosterSlot)
			{
				OutSlots.AddUnique(RosterSlot);
				break;
			}
		}
	}
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
	AttackOrderLockedMask = 0;
	AttackOrderDeadlineServerTime = 0.0f;
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


bool AClutchRoundState::BeginAttackOrderSelection(float InDeadlineServerTime)
{
	if (!HasAuthority())
	{
		return false;
	}

	Phase = EClutchRoundPhase::OrderSelection;
	ActiveAttacker = nullptr;
	AttackingTeamIndex = NoTeam;
	AttackOrderLockedMask = 0;
	AttackOrderDeadlineServerTime = FMath::Max(0.0f, InDeadlineServerTime);
	RoundStartServerTime = 0.0f;
	PoleUnlockServerTime = 0.0f;
	RoundEndServerTime = 0.0f;
	PoleProgress = 0.0f;
	for (FClutchRosterEntry& Entry : Roster)
	{
		if (Entry.PlayerState && Entry.PlayerStatus != EClutchStatus::Disconnected)
		{
			Entry.PlayerRole = EClutchRole::None;
			Entry.PlayerStatus = EClutchStatus::Queued;
			Entry.HitsTaken = 0;
		}
	}

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
	AttackOrderDeadlineServerTime = 0.0f;
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
	const uint8 PreviousTeamIndex = Entry->TeamIndex;
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
	if (PreviousTeamIndex != TeamIndex)
	{
		Entry->AttackOrderIndex = UnassignedSlot;
		Entry->bAttackOrderSelector = false;
	}
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

	// Preserve identity, team, rotation slot, and chosen attack-order position so
	// a reconnect can reclaim the same place. A disconnected player cannot select.
	Entry->PlayerState = nullptr;
	Entry->PlayerRole = EClutchRole::None;
	Entry->PlayerStatus = EClutchStatus::Disconnected;
	Entry->bAttackOrderSelector = false;
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
	AttackOrderLockedMask = 0;
	AttackOrderDeadlineServerTime = 0.0f;
	MarkStateDirty();
	return true;
}


bool AClutchRoundState::SetTeamAttackOrder(uint8 TeamIndex,
	const TArray<int32>& OrderedRosterSlots, bool bLockOrder)
{
	if (!HasAuthority() || TeamIndex > 1)
	{
		return false;
	}

	TArray<int32> EligibleSlots;
	GetTeamAttackOrderSlots(TeamIndex, EligibleSlots);
	if (!IsValidAttackOrder(EligibleSlots, OrderedRosterSlots))
	{
		return false;
	}

	for (FClutchRosterEntry& Entry : Roster)
	{
		if (Entry.TeamIndex == TeamIndex)
		{
			Entry.AttackOrderIndex = UnassignedSlot;
		}
	}
	for (int32 OrderIndex = 0; OrderIndex < OrderedRosterSlots.Num(); ++OrderIndex)
	{
		for (FClutchRosterEntry& Entry : Roster)
		{
			if (Entry.TeamIndex == TeamIndex
				&& Entry.RosterSlot == OrderedRosterSlots[OrderIndex])
			{
				Entry.AttackOrderIndex = static_cast<uint8>(OrderIndex);
				break;
			}
		}
	}

	if (bLockOrder)
	{
		AttackOrderLockedMask |= static_cast<uint8>(1 << TeamIndex);
	}
	MarkStateDirty();
	return true;
}


bool AClutchRoundState::SetAttackOrderLocked(uint8 TeamIndex, bool bLocked)
{
	if (!HasAuthority() || TeamIndex > 1)
	{
		return false;
	}

	const uint8 TeamBit = static_cast<uint8>(1 << TeamIndex);
	const uint8 NewMask = bLocked
		? static_cast<uint8>(AttackOrderLockedMask | TeamBit)
		: static_cast<uint8>(AttackOrderLockedMask & ~TeamBit);
	if (NewMask != AttackOrderLockedMask)
	{
		AttackOrderLockedMask = NewMask;
		MarkStateDirty();
	}
	return true;
}


bool AClutchRoundState::SetAttackOrderSelectors(
	AUTPlayerState* Team0Selector, AUTPlayerState* Team1Selector)
{
	if (!HasAuthority())
	{
		return false;
	}

	for (FClutchRosterEntry& Entry : Roster)
	{
		Entry.bAttackOrderSelector = Entry.PlayerState
			&& ((Entry.TeamIndex == 0 && Entry.PlayerState == Team0Selector)
				|| (Entry.TeamIndex == 1 && Entry.PlayerState == Team1Selector));
	}
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


bool AClutchRoundState::IsValidAttackOrder(
	const TArray<int32>& EligibleSlots, const TArray<int32>& ProposedSlots)
{
	TArray<int32> UniqueEligibleSlots;
	for (int32 Slot : EligibleSlots)
	{
		if (Slot >= 0)
		{
			UniqueEligibleSlots.AddUnique(Slot);
		}
	}
	if (UniqueEligibleSlots.Num() == 0
		|| ProposedSlots.Num() != UniqueEligibleSlots.Num())
	{
		return false;
	}

	TArray<int32> UniqueProposedSlots;
	for (int32 Slot : ProposedSlots)
	{
		if (!UniqueEligibleSlots.Contains(Slot)
			|| UniqueProposedSlots.Contains(Slot))
		{
			return false;
		}
		UniqueProposedSlots.Add(Slot);
	}
	return true;
}


int32 AClutchRoundState::SelectNextOrderedSlot(
	const TArray<int32>& FullOrder, const TArray<int32>& ConnectedSlots, int32 PreviousSlot)
{
	TArray<int32> Order;
	for (int32 Slot : FullOrder)
	{
		if (Slot >= 0)
		{
			Order.AddUnique(Slot);
		}
	}
	if (Order.Num() == 0 || ConnectedSlots.Num() == 0)
	{
		return INDEX_NONE;
	}

	// Anchor on the previous attacker's position within the FULL order. Because a
	// disconnected previous attacker's slot is preserved there, it is still found and
	// we advance to the next teammate rather than resetting to the front. When it is
	// absent (first selection, or a slot never assigned), start just before the front
	// so the earliest connected slot in order is chosen. Walk forward, skipping any
	// disconnected slots, and return the first slot that is still connected.
	const int32 PreviousIndex = Order.Find(PreviousSlot);
	const int32 Anchor = (PreviousIndex == INDEX_NONE) ? -1 : PreviousIndex;
	const int32 Count = Order.Num();
	for (int32 Step = 1; Step <= Count; ++Step)
	{
		const int32 Candidate = Order[(Anchor + Step) % Count];
		if (ConnectedSlots.Contains(Candidate))
		{
			return Candidate;
		}
	}

	// No connected slot appears in FullOrder (defensive — connected slots are normally
	// all present). Fall back to the first supplied connected slot.
	return ConnectedSlots[0];
}


uint8 AClutchRoundState::GetDefendingTeam(uint8 InAttackingTeamIndex)
{
	return InAttackingTeamIndex <= 1
		? static_cast<uint8>(1 - InAttackingTeamIndex)
		: NoTeam;
}


bool AClutchRoundState::CanSpectateRosterEntry(uint8 ViewerTeamIndex,
	const FClutchRosterEntry& TargetEntry, bool bTargetAlive)
{
	return bTargetAlive
		&& ViewerTeamIndex <= 1
		&& TargetEntry.TeamIndex == ViewerTeamIndex
		&& TargetEntry.PlayerStatus == EClutchStatus::Active;
}


float AClutchRoundState::AdvancePoleProgress(float CurrentProgress,
	float DeltaSeconds, bool bAttackerPresent, bool bDefenderPresent,
	float CaptureSeconds, float DecaySeconds)
{
	float Result = FMath::Clamp(CurrentProgress, 0.0f, 100.0f);
	if (DeltaSeconds <= 0.0f)
	{
		return Result;
	}

	if (bAttackerPresent && !bDefenderPresent)
	{
		Result += DeltaSeconds * (100.0f / FMath::Max(0.1f, CaptureSeconds));
	}
	else if (!bAttackerPresent)
	{
		Result -= DeltaSeconds * (100.0f / FMath::Max(0.1f, DecaySeconds));
	}
	// Attacker + defender is contested and deliberately freezes progress.

	return FMath::Clamp(Result, 0.0f, 100.0f);
}


int32 AClutchRoundState::AdvanceAmmoRegen(float& AccumulatorSeconds,
	float DeltaSeconds, int32 CurrentAmmo, int32 MagazineSize, float RegenInterval)
{
	if (MagazineSize <= 0 || RegenInterval <= 0.0f || CurrentAmmo >= MagazineSize)
	{
		// A full clip (or a disabled feature) parks the timer at zero so the refill
		// clock always restarts from the shot that drops it below capacity.
		AccumulatorSeconds = 0.0f;
		return 0;
	}

	if (DeltaSeconds > 0.0f)
	{
		AccumulatorSeconds += DeltaSeconds;
	}

	int32 Grants = FMath::FloorToInt(AccumulatorSeconds / RegenInterval);
	if (Grants <= 0)
	{
		return 0;
	}

	Grants = FMath::Min(Grants, MagazineSize - CurrentAmmo);
	AccumulatorSeconds -= Grants * RegenInterval;
	if (CurrentAmmo + Grants >= MagazineSize)
	{
		AccumulatorSeconds = 0.0f;
	}
	return Grants;
}


int32 AClutchRoundState::ResolveRoleDamage(EClutchRole DamageDealerRole,
	EClutchRole VictimRole, int32 DefenderDamage, int32 AttackerDamage)
{
	if (DamageDealerRole == EClutchRole::Defender
		&& VictimRole == EClutchRole::Attacker)
	{
		return FMath::Max(0, DefenderDamage);
	}
	if (DamageDealerRole == EClutchRole::Attacker
		&& VictimRole == EClutchRole::Defender)
	{
		return FMath::Max(0, AttackerDamage);
	}
	return 0;
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
