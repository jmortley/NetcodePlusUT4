// WipeoutDamageReplicator.cpp
#include "WipeoutDamageReplicator.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "StatNames.h"
#include "WipeoutGame.h"
#include "Net/UnrealNetwork.h"

namespace
{
	// Metadata row carried inside the already-replicated DamageEntries array.
	// The control-prefixed ID plus magic/version fields keep it outside normal
	// platform-ID formats and make accidental matches fail closed. Older 328
	// clients simply ignore it because they only look entries up by a real
	// player's UniqueId. Bit-casting preserves the deadline exactly without
	// adding a replicated property or changing either native layout.
	static const FString WipeoutRoundClockEntryId(TEXT("\x001FNCP:WipeClock:1"));
	static const int32 WipeoutRoundClockMagic = 0x4E435057; // "NCPW"
	static const int32 WipeoutRoundClockVersion = 1;
	static const float MaxReasonableRoundClockSeconds = 86400.f;

	bool IsWipeoutRoundClockEntry(const FReplicatedDamageEntry& Entry)
	{
		return Entry.PlayerId == WipeoutRoundClockEntryId
			&& Entry.BeltPickups == WipeoutRoundClockMagic
			&& Entry.AmpPickups == WipeoutRoundClockVersion;
	}

	int32 EncodeRoundClockDeadline(float Deadline)
	{
		static_assert(sizeof(int32) == sizeof(float), "Round-clock metadata requires 32-bit int and float");
		int32 Encoded = 0;
		FMemory::Memcpy(&Encoded, &Deadline, sizeof(Encoded));
		return Encoded;
	}

	float DecodeRoundClockDeadline(int32 Encoded)
	{
		float Deadline = 0.f;
		FMemory::Memcpy(&Deadline, &Encoded, sizeof(Deadline));
		return Deadline;
	}
}

AWipeoutDamageReplicator::AWipeoutDamageReplicator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f; // Don't need per-frame
	NetUpdateFrequency = 2.0f; // 2 updates/sec is plenty for scoreboard display
}

void AWipeoutDamageReplicator::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWipeoutDamageReplicator, DamageEntries);
	DOREPLIFETIME(AWipeoutDamageReplicator, Team0ClutchOverlay);
	DOREPLIFETIME(AWipeoutDamageReplicator, Team1ClutchOverlay);
}

FNCClutchOverlayState* AWipeoutDamageReplicator::GetMutableClutchOverlayState(int32 TeamIndex)
{
	return TeamIndex == 0 ? &Team0ClutchOverlay
		: (TeamIndex == 1 ? &Team1ClutchOverlay : nullptr);
}

const FNCClutchOverlayState* AWipeoutDamageReplicator::GetClutchOverlayState(int32 TeamIndex) const
{
	return TeamIndex == 0 ? &Team0ClutchOverlay
		: (TeamIndex == 1 ? &Team1ClutchOverlay : nullptr);
}

float AWipeoutDamageReplicator::GetClutchOverlayServerTime() const
{
	const AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	return GS ? GS->GetServerWorldTimeSeconds()
		: (GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f);
}

void AWipeoutDamageReplicator::BeginClutchOverlay(int32 TeamIndex,
	AUTPlayerState* Candidate, int32 EnemiesAlive, bool bSuddenDeath)
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
	State->bRespawnHold = true;
	State->bSuddenDeath = bSuddenDeath;
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

void AWipeoutDamageReplicator::CreditClutchOverlayKill(
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

void AWipeoutDamageReplicator::UpdateClutchOverlayRemaining(
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

void AWipeoutDamageReplicator::PromoteClutchOverlayToSuddenDeath(int32 TeamIndex)
{
	if (Role != ROLE_Authority)
	{
		return;
	}
	FNCClutchOverlayState* State = GetMutableClutchOverlayState(TeamIndex);
	if (State && State->bActive && !State->bSuddenDeath)
	{
		State->bSuddenDeath = true;
		ForceNetUpdate();
	}
}

void AWipeoutDamageReplicator::EndClutchOverlay(int32 TeamIndex,
	ENCClutchOverlayOutcome Outcome, int32 TeammatesRespawned)
{
	if (Role != ROLE_Authority)
	{
		return;
	}
	FNCClutchOverlayState* State = GetMutableClutchOverlayState(TeamIndex);
	if (!State || !State->bActive)
	{
		return;
	}

	State->bActive = false;
	State->Candidate = nullptr;
	State->Outcome = Outcome;
	State->EndServerTime = GetClutchOverlayServerTime();
	State->TeammatesRespawned = FMath::Max(0, TeammatesRespawned);
	ForceNetUpdate();
}

void AWipeoutDamageReplicator::ClearClutchOverlays()
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
		State->bRespawnHold = true;
	}
	ForceNetUpdate();
}

void AWipeoutDamageReplicator::SetRoundClockDeadline(float EndServerTime)
{
	if (Role != ROLE_Authority)
	{
		return;
	}

	if (!FMath::IsFinite(EndServerTime) || EndServerTime <= 0.f)
	{
		ClearRoundClockDeadline();
		return;
	}

	FReplicatedDamageEntry* ClockEntry = DamageEntries.FindByPredicate(
		[](const FReplicatedDamageEntry& Entry)
		{
			return IsWipeoutRoundClockEntry(Entry);
		});
	if (!ClockEntry)
	{
		const int32 ClockEntryIndex = DamageEntries.AddDefaulted();
		ClockEntry = &DamageEntries[ClockEntryIndex];
		ClockEntry->PlayerId = WipeoutRoundClockEntryId;
		ClockEntry->BeltPickups = WipeoutRoundClockMagic;
		ClockEntry->AmpPickups = WipeoutRoundClockVersion;
	}
	ClockEntry->DamageDone = EncodeRoundClockDeadline(EndServerTime);
	ForceNetUpdate();
}

void AWipeoutDamageReplicator::ClearRoundClockDeadline()
{
	if (Role != ROLE_Authority)
	{
		return;
	}

	const int32 Removed = DamageEntries.RemoveAll(
		[](const FReplicatedDamageEntry& Entry)
		{
			return IsWipeoutRoundClockEntry(Entry);
		});
	if (Removed > 0)
	{
		ForceNetUpdate();
	}
}

int32 AWipeoutDamageReplicator::GetRoundClockSecondsRemaining() const
{
	const FReplicatedDamageEntry* ClockEntry = DamageEntries.FindByPredicate(
		[](const FReplicatedDamageEntry& Entry)
		{
			return IsWipeoutRoundClockEntry(Entry);
		});
	if (!ClockEntry)
	{
		return -1;
	}
	const float RoundClockEndServerTime = DecodeRoundClockDeadline(ClockEntry->DamageDone);
	if (!FMath::IsFinite(RoundClockEndServerTime) || RoundClockEndServerTime <= 0.f)
	{
		return -1;
	}

	const AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (!GS)
	{
		return -1;
	}

	const float Remaining = RoundClockEndServerTime - GS->GetServerWorldTimeSeconds();
	if (!FMath::IsFinite(Remaining) || Remaining > MaxReasonableRoundClockSeconds)
	{
		return -1;
	}
	return FMath::Max(0, FMath::CeilToInt(FMath::Max(0.f, Remaining)));
}

void AWipeoutDamageReplicator::BeginPlay()
{
	Super::BeginPlay();
	TimeSinceLastUpdate = 0.0f;
}

void AWipeoutDamageReplicator::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// Server only: periodically snapshot damage from PlayerStates
	if (Role != ROLE_Authority)
	{
		return;
	}

	TimeSinceLastUpdate += DeltaTime;
	if (TimeSinceLastUpdate >= UpdateInterval)
	{
		TimeSinceLastUpdate = 0.0f;
		UpdateFromPlayerStates();
	}
}

void AWipeoutDamageReplicator::UpdateFromPlayerStates()
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS)
	{
		return;
	}

	// Server-only path, so the authoritative GameMode is reachable here. Null in
	// any non-Wipeout mode that somehow spawns this replicator — healing simply
	// stays 0 rather than failing the whole refresh.
	AUWipeoutGame* WipeoutGame = GetWorld()->GetAuthGameMode<AUWipeoutGame>();

	// Preserve the reserved clock row while rebuilding the player snapshot.
	// This keeps the event-driven deadline alive through the 1 Hz stats refresh
	// without adding state to the actor itself.
	FReplicatedDamageEntry RoundClockEntry;
	bool bHasRoundClockEntry = false;
	if (const FReplicatedDamageEntry* ExistingClock = DamageEntries.FindByPredicate(
		[](const FReplicatedDamageEntry& Entry)
		{
			return IsWipeoutRoundClockEntry(Entry);
		}))
	{
		RoundClockEntry = *ExistingClock;
		bHasRoundClockEntry = true;
	}

	DamageEntries.Reset();

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || !UTPS->UniqueId.IsValid())
		{
			continue;
		}

		FReplicatedDamageEntry Entry;
		Entry.PlayerId = UTPS->UniqueId.ToString();

		// DamageDone is tracked server-side on AUTPlayerState but not replicated
		UIntProperty* DmgProp = FindField<UIntProperty>(UTPS->GetClass(), TEXT("DamageDone"));
		if (DmgProp)
		{
			Entry.DamageDone = DmgProp->GetPropertyValue_InContainer(UTPS);
		}

		// Belt and Amp pickup counts — server-side stats
		Entry.BeltPickups = UTPS->GetStatsValue(NAME_ShieldBeltCount);
		Entry.AmpPickups = UTPS->GetStatsValue(NAME_UDamageCount);

		// Vest, and Siphon (which reuses the Berserk stat names — see
		// SiphonPowerup.cpp — so no engine-side stat had to be added).
		Entry.VestPickups = UTPS->GetStatsValue(NAME_ArmorVestCount);
		Entry.SiphonPickups = UTPS->GetStatsValue(NAME_BerserkCount);

		// Healing is accumulated on the GameMode, not the PlayerState.
		if (WipeoutGame != nullptr)
		{
			Entry.HealingDone = WipeoutGame->GetHealingDoneForPlayer(UTPS);
		}

		DamageEntries.Add(Entry);
	}

	if (bHasRoundClockEntry)
	{
		DamageEntries.Add(MoveTemp(RoundClockEntry));
	}
}

const FReplicatedDamageEntry* AWipeoutDamageReplicator::FindEntry(const FString& UniqueIdStr) const
{
	for (const FReplicatedDamageEntry& Entry : DamageEntries)
	{
		if (Entry.PlayerId == UniqueIdStr)
		{
			return &Entry;
		}
	}
	return nullptr;
}

int32 AWipeoutDamageReplicator::GetDamageForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->DamageDone : 0;
}

int32 AWipeoutDamageReplicator::GetBeltsForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->BeltPickups : 0;
}

int32 AWipeoutDamageReplicator::GetAmpsForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->AmpPickups : 0;
}

int32 AWipeoutDamageReplicator::GetVestsForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->VestPickups : 0;
}

int32 AWipeoutDamageReplicator::GetSiphonsForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->SiphonPickups : 0;
}

int32 AWipeoutDamageReplicator::GetHealingForPlayer(const FString& UniqueIdStr) const
{
	const FReplicatedDamageEntry* Entry = FindEntry(UniqueIdStr);
	return Entry ? Entry->HealingDone : 0;
}
