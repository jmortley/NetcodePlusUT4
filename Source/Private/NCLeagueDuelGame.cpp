// NCLeagueDuelGame.cpp — fairness-first 1v1 spawn picker + Glicko2 ELO + stats hooks.

#include "NCLeagueDuelGame.h"
#include "UnrealTournament.h"
#include "UTPlayerStart.h"
#include "UTPickupInventory.h"
#include "UTPickupWeapon.h"
#include "UTPlayerState.h"
#include "UTGameInstance.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTHUD.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "StatNames.h"
#include "NCLeagueDuelHUD.h"
#include "NCDuelRatingSystem.h"
#include "NCStatsUploader.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCLeagueDuel, Log, All);

namespace
{
	/** Match weapon class against one of the 6 known league weapons.
	 *  Returns the canonical group key, or NAME_None if not recognized.
	 *  Uses GetName() string-contains so Blueprint subclasses (BP_Sniper_C) match
	 *  the same group as their C++ parents. */
	FName GroupForWeaponClass(UClass* Cls)
	{
		if (!Cls) return NAME_None;
		const FString N = Cls->GetName();
		if (N.Contains(TEXT("Sniper")) || N.Contains(TEXT("Lightning"))) return TEXT("Sniper");
		if (N.Contains(TEXT("ShockRifle")) || N.Contains(TEXT("Shock"))) return TEXT("Shock");
		if (N.Contains(TEXT("Rocket")))                                  return TEXT("Rocket");
		if (N.Contains(TEXT("Flak")))                                    return TEXT("Flak");
		if (N.Contains(TEXT("Minigun")) || N.Contains(TEXT("Stinger")))  return TEXT("Mini");
		if (N.Contains(TEXT("Link")))                                    return TEXT("Link");
		return NAME_None;
	}

	/** Counter-weapon mapping for fair first-spawn pairing. */
	FName CounterGroup(FName G)
	{
		if (G == TEXT("Sniper")) return TEXT("Shock");
		if (G == TEXT("Shock"))  return TEXT("Sniper");
		if (G == TEXT("Rocket")) return TEXT("Flak");
		if (G == TEXT("Flak"))   return TEXT("Rocket");
		if (G == TEXT("Mini"))   return TEXT("Link");
		if (G == TEXT("Link"))   return TEXT("Mini");
		return NAME_None;
	}
}

ANCLeagueDuelGame::ANCLeagueDuelGame(const FObjectInitializer& OI)
	: Super(OI)
{
	DisplayName = NSLOCTEXT("UTGameMode", "NCLeagueDuel", "NetcodePlus League Duel");
	HUDClass = ANCLeagueDuelHUD::StaticClass();
	MinKillerSpawnDistance     = 2500.f;
	MinimumEnemySpawnDistance  = 2400.f;
	ShieldBeltExclusionCount   = 2;
	ShieldBeltPickup           = nullptr;
}

void ANCLeagueDuelGame::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	// Mod.ini overrides — same pattern as ElimPlusGame.
	const FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	GConfig->GetFloat(TEXT("NCLeagueDuel"), TEXT("MinKillerSpawnDistance"),    MinKillerSpawnDistance,    ConfigPath);
	GConfig->GetFloat(TEXT("NCLeagueDuel"), TEXT("MinimumEnemySpawnDistance"), MinimumEnemySpawnDistance, ConfigPath);
	GConfig->GetInt  (TEXT("NCLeagueDuel"), TEXT("ShieldBeltExclusionCount"),  ShieldBeltExclusionCount,  ConfigPath);

	UE_LOG(LogNCLeagueDuel, Log,
		TEXT("InitGame: MinKillerDist=%.0f MinEnemyDist=%.0f BeltExclusions=%d"),
		MinKillerSpawnDistance, MinimumEnemySpawnDistance, ShieldBeltExclusionCount);
}

void ANCLeagueDuelGame::BeginPlay()
{
	Super::BeginPlay();

	if (Role != ROLE_Authority) return;

	ComputeSpawnPairings();
	ComputeShieldBeltExclusions();

	// Reset per-match first-spawn shuffle state. The actual shuffle happens
	// lazily on the first ChoosePlayerStart call once we know WeaponPairs is
	// finalized (deferred so test maps without weapon-anchored starts don't
	// crash with an empty shuffle).
	FirstSpawnShuffleOrder.Reset();
	bFirstSpawnShuffleReady = false;

	RatingSystem = MakeUnique<FNCDuelRatingSystem>();
	FNCDuelRatingSystem::InitDatabase(GetWorld());
}

void ANCLeagueDuelGame::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	// Per feedback_postlogin_before_plugin_check.md: PostLogin fires before the
	// plugin-compat kick, so a player who later gets kicked still appears here.
	// LoadPlayerFromDB just preloads career ELO into the cache — harmless if the
	// player ends up being rejected.
	if (Role != ROLE_Authority || !RatingSystem || !NewPlayer) return;
	AUTPlayerState* PS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (!PS) return;
	const FString UniqueId = PS->StatsID.IsEmpty() ? PS->PlayerName : PS->StatsID;
	RatingSystem->LoadPlayerFromDB(GetWorld(), UniqueId);
}

void ANCLeagueDuelGame::HandleMatchHasStarted()
{
	Super::HandleMatchHasStarted();
	if (Role == ROLE_Authority && RatingSystem)
	{
		RatingSystem->SnapshotMatchStart();
	}
}

void ANCLeagueDuelGame::HandleMatchHasEnded()
{
	Super::HandleMatchHasEnded();
	if (Role != ROLE_Authority || !RatingSystem || !UTGameState) return;

	// Identify the two duelists. With more than 2 players (server testing /
	// spectators), use the top-2 scorers — duel intends 1v1.
	AUTPlayerState* P1 = nullptr;
	AUTPlayerState* P2 = nullptr;
	for (APlayerState* APS : UTGameState->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS);
		if (!UTPS || UTPS->bOnlySpectator) continue;
		if (!P1 || UTPS->Score > P1->Score)      { P2 = P1; P1 = UTPS; }
		else if (!P2 || UTPS->Score > P2->Score) { P2 = UTPS; }
	}

	if (P1 && P2)
	{
		const bool bDraw = (P1->Score == P2->Score);
		const FString WinnerId = P1->StatsID.IsEmpty() ? P1->PlayerName : P1->StatsID;
		const FString LoserId  = P2->StatsID.IsEmpty() ? P2->PlayerName : P2->StatsID;
		RatingSystem->ProcessMatchResult(WinnerId, LoserId, bDraw);
		RatingSystem->Flush(GetWorld());
	}

	FNCMatchSummary Summary;
	BuildMatchSummary(Summary);
	FNCStatsUploader::PostToUT4Stats(GetWorld(), Summary);
	FNCStatsUploader::PostToStatSQL(GetWorld(), Summary);
}

// =============================================================================
// Spawn precompute
// =============================================================================

void ANCLeagueDuelGame::ComputeSpawnPairings()
{
	WeaponPairs.Reset();
	AllPlayerStarts.Reset();
	WeaponAnchoredStarts.Reset();
	ConsumedPairIndices.Reset();

	// Cache every PlayerStart up front.
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		if (APlayerStart* PS = *It) AllPlayerStarts.Add(PS);
	}

	// Build group→starts map by WALKING THE WEAPON PICKUPS, not the PlayerStarts.
	// Many UT4 maps don't fill AUTPlayerStart::AssociatedPickup (it's optional
	// metadata the mapper has to set manually). Going pickup-first means we
	// don't depend on that field — every spawned AUTPickupWeapon contributes
	// regardless of how the map was authored. For each weapon, claim the
	// closest unclaimed PlayerStart within MaxAssocDist; that becomes the
	// "anchor" for that weapon group.
	TMap<FName, TArray<APlayerStart*>> GroupToStarts;
	TSet<APlayerStart*> ClaimedStarts;
	const float MaxAssocDist = 2500.f;  // reasonable "near" radius; weapons
	                                    // beyond this are not credibly an anchor.

	int32 PickupsScanned = 0;
	int32 PickupsClaimed = 0;
	for (TActorIterator<AUTPickupWeapon> It(GetWorld()); It; ++It)
	{
		AUTPickupWeapon* Pickup = *It;
		if (!Pickup || !Pickup->WeaponType) continue;
		++PickupsScanned;

		const FName Group = GroupForWeaponClass(Pickup->WeaponType);
		const FVector PickupLoc = Pickup->GetActorLocation();

		// Find nearest unclaimed PlayerStart.
		APlayerStart* NearestStart = nullptr;
		float NearestDistSq = MaxAssocDist * MaxAssocDist;
		for (APlayerStart* Cand : AllPlayerStarts)
		{
			if (!Cand || ClaimedStarts.Contains(Cand)) continue;
			const float DSq = FVector::DistSquared(Cand->GetActorLocation(), PickupLoc);
			if (DSq < NearestDistSq)
			{
				NearestDistSq = DSq;
				NearestStart = Cand;
			}
		}
		if (!NearestStart) continue;

		ClaimedStarts.Add(NearestStart);
		WeaponAnchoredStarts.Add(NearestStart);
		++PickupsClaimed;

		if (Group != NAME_None)
		{
			GroupToStarts.FindOrAdd(Group).Add(NearestStart);
		}
		UE_LOG(LogNCLeagueDuel, Log,
			TEXT("Anchored weapon %s -> PlayerStart at %s (%.0fuu, group %s)"),
			*Pickup->WeaponType->GetName(),
			*NearestStart->GetActorLocation().ToString(),
			FMath::Sqrt(NearestDistSq),
			*Group.ToString());
	}

	UE_LOG(LogNCLeagueDuel, Log,
		TEXT("ComputeSpawnPairings: %d total starts, %d weapon pickups (%d anchored), %d weapon groups identified"),
		AllPlayerStarts.Num(), PickupsScanned, PickupsClaimed, GroupToStarts.Num());

	// For each canonical pair group (handle each pair once via a "consumed" set
	// so we don't emit Sniper↔Shock and Shock↔Sniper as separate pairs).
	TSet<FName> ConsumedGroups;
	auto MakePair = [&](FName GroupA, FName GroupB)
	{
		if (ConsumedGroups.Contains(GroupA) || ConsumedGroups.Contains(GroupB)) return;
		const TArray<APlayerStart*>* StartsA = GroupToStarts.Find(GroupA);
		const TArray<APlayerStart*>* StartsB = GroupToStarts.Find(GroupB);
		if (!StartsA || !StartsB || StartsA->Num() == 0 || StartsB->Num() == 0)
		{
			// Missing one side — skip this pair (player decision: use fairest available).
			return;
		}

		// Pick (a, b) with maximum 2D distance — DM-Deck duplicates each weapon,
		// and a player stuck near the closest counter-weapon spawn isn't fair.
		APlayerStart* BestA = nullptr;
		APlayerStart* BestB = nullptr;
		float BestDist = -1.f;
		for (APlayerStart* A : *StartsA)
		{
			if (!A) continue;
			for (APlayerStart* B : *StartsB)
			{
				if (!B || A == B) continue;
				const float D = (A->GetActorLocation() - B->GetActorLocation()).Size2D();
				if (D > BestDist) { BestDist = D; BestA = A; BestB = B; }
			}
		}
		if (!BestA || !BestB) return;

		FNCLeagueWeaponPair Pair;
		Pair.StartA = BestA;
		Pair.StartB = BestB;
		Pair.GroupA = GroupA;
		Pair.GroupB = GroupB;
		WeaponPairs.Add(Pair);
		ConsumedGroups.Add(GroupA);
		ConsumedGroups.Add(GroupB);

		UE_LOG(LogNCLeagueDuel, Log,
			TEXT("Paired %s ↔ %s: distance %.0fuu (chose maximum-separation combo)"),
			*GroupA.ToString(), *GroupB.ToString(), BestDist);
	};

	MakePair(TEXT("Sniper"), TEXT("Shock"));
	MakePair(TEXT("Rocket"), TEXT("Flak"));
	MakePair(TEXT("Mini"),   TEXT("Link"));

	UE_LOG(LogNCLeagueDuel, Log, TEXT("ComputeSpawnPairings: %d pairs ready for first-spawn assignment"), WeaponPairs.Num());
}

void ANCLeagueDuelGame::ComputeShieldBeltExclusions()
{
	ShieldBeltPickup = nullptr;
	ShieldBeltExclusions.Reset();

	for (TActorIterator<AUTPickupInventory> It(GetWorld()); It; ++It)
	{
		AUTPickupInventory* P = *It;
		if (!P || !P->GetInventoryType()) continue;
		// Same string-contains pattern WipeoutGame and ShockDomGameMode use.
		if (P->GetInventoryType()->GetName().Contains(TEXT("ShieldBelt")))
		{
			ShieldBeltPickup = P;
			break;
		}
	}

	if (!ShieldBeltPickup)
	{
		UE_LOG(LogNCLeagueDuel, Log, TEXT("ComputeShieldBeltExclusions: no ShieldBelt on map — exclusion disabled"));
		return;
	}

	const FVector BeltLoc = ShieldBeltPickup->GetActorLocation();
	TArray<APlayerStart*> Sorted = AllPlayerStarts;
	Sorted.Sort([&](const APlayerStart& A, const APlayerStart& B)
	{
		return (A.GetActorLocation() - BeltLoc).Size2D()
		     < (B.GetActorLocation() - BeltLoc).Size2D();
	});

	const int32 N = FMath::Min(ShieldBeltExclusionCount, Sorted.Num());
	for (int32 i = 0; i < N; ++i)
	{
		ShieldBeltExclusions.Add(Sorted[i]);
	}

	UE_LOG(LogNCLeagueDuel, Log,
		TEXT("ComputeShieldBeltExclusions: belt at %s, excluded %d nearest PlayerStarts while belt active"),
		*BeltLoc.ToString(), ShieldBeltExclusions.Num());
}

bool ANCLeagueDuelGame::IsExcludedByActiveShieldBelt(APlayerStart* PS) const
{
	return ShieldBeltPickup
	    && ShieldBeltPickup->State.bActive
	    && ShieldBeltExclusions.Contains(PS);
}

// =============================================================================
// First-spawn helpers (BP port: shuffle pairs once per match, take 2 as A/B)
// =============================================================================

void ANCLeagueDuelGame::EnsureFirstSpawnShuffle()
{
	if (bFirstSpawnShuffleReady) return;

	FirstSpawnShuffleOrder.Reset();
	FirstSpawnShuffleOrder.Reserve(WeaponPairs.Num());
	for (int32 i = 0; i < WeaponPairs.Num(); ++i)
	{
		FirstSpawnShuffleOrder.Add(i);
	}

	// Fisher-Yates shuffle. Mirrors the BP "Shuffle" node on the spawn-pairs array.
	for (int32 i = FirstSpawnShuffleOrder.Num() - 1; i > 0; --i)
	{
		const int32 j = FMath::RandRange(0, i);
		FirstSpawnShuffleOrder.Swap(i, j);
	}

	// 50/50: which team gets StartA side? Mirrors the BP "Red Is Player 1" branch
	// that flipped which side of each pair red got. Without this, a meta-dominant
	// weapon (e.g. Sniper) would always go to red — bRedGetsPrimarySide swaps
	// half the time so weapon-meta fairness holds across matches.
	bRedGetsPrimarySide = (FMath::RandRange(0, 1) == 0);
	bFirstSpawnShuffleReady = true;

	FString OrderStr;
	for (int32 i = 0; i < FirstSpawnShuffleOrder.Num(); ++i)
	{
		const FNCLeagueWeaponPair& P = WeaponPairs[FirstSpawnShuffleOrder[i]];
		OrderStr += FString::Printf(TEXT("[%d:%s<->%s] "),
			FirstSpawnShuffleOrder[i], *P.GroupA.ToString(), *P.GroupB.ToString());
	}
	UE_LOG(LogNCLeagueDuel, Log,
		TEXT("First-spawn shuffle: order=%s | red gets %s side"),
		*OrderStr, bRedGetsPrimarySide ? TEXT("StartA") : TEXT("StartB"));
}

FName ANCLeagueDuelGame::GetWeaponGroupForStart(APlayerStart* Start) const
{
	if (!Start) return NAME_None;
	for (const FNCLeagueWeaponPair& P : WeaponPairs)
	{
		if (P.StartA == Start) return P.GroupA;
		if (P.StartB == Start) return P.GroupB;
	}
	// Fall back: read it directly off the AssociatedPickup. Same group keys as
	// ComputeSpawnPairings produces (so canonical-pair checks stay consistent).
	if (AUTPlayerStart* UTPS = Cast<AUTPlayerStart>(Start))
	{
		if (AUTPickupWeapon* WPickup = Cast<AUTPickupWeapon>(UTPS->AssociatedPickup))
		{
			if (WPickup->WeaponType)
			{
				return GroupForWeaponClass(WPickup->WeaponType);
			}
		}
	}
	return NAME_None;
}

APlayerStart* ANCLeagueDuelGame::SelectPairedSpawnForFirstSpawn(AUTPlayerState* PS,
	int32 ChoiceIdx, APlayerStart* ExcludeStart)
{
	if (!PS) return nullptr;
	const int32 TeamIdx = (PS->Team) ? PS->Team->TeamIndex : 0;

	EnsureFirstSpawnShuffle();

	// Tier A — direct read from the per-match shuffle order. Choice A and B
	// always come from DIFFERENT pairs, and both players hit the same pair
	// for each ChoiceIdx (red gets one side, blue gets the other → mutual
	// fairness). Mirrors the BP shuffle+GET 0/1 flow.
	if (FirstSpawnShuffleOrder.IsValidIndex(ChoiceIdx))
	{
		const int32 PairIdx = FirstSpawnShuffleOrder[ChoiceIdx];
		const FNCLeagueWeaponPair& Pair = WeaponPairs[PairIdx];

		// Side selection: bRedGetsPrimarySide flips which TeamIndex maps to StartA.
		const bool bUseStartA = (TeamIdx == 0) ? bRedGetsPrimarySide : !bRedGetsPrimarySide;
		APlayerStart* Chosen = bUseStartA ? Pair.StartA : Pair.StartB;
		if (Chosen)
		{
			UE_LOG(LogNCLeagueDuel, Log,
				TEXT("First-spawn pick for %s (team %d, choice %c): pair %d (%s<->%s) -> %s side at %s"),
				*PS->PlayerName, TeamIdx, ChoiceIdx == 0 ? TCHAR('A') : TCHAR('B'),
				PairIdx, *Pair.GroupA.ToString(), *Pair.GroupB.ToString(),
				bUseStartA ? TEXT("StartA") : TEXT("StartB"),
				*Chosen->GetActorLocation().ToString());
			return Chosen;
		}
	}

	// Tier B — fallback for sparse maps (fewer than 2 canonical pairs available).
	// Pick any weapon-anchored start, but exclude anything whose weapon GROUP
	// matches ExcludeStart (so we don't get "Rocket + Rocket" when the map has
	// duplicate Rocket pickups but no Sniper/Mini pairs).
	const FName ExcludeGroup = GetWeaponGroupForStart(ExcludeStart);

	TArray<APlayerStart*> Eligible;
	Eligible.Reserve(WeaponAnchoredStarts.Num());
	for (APlayerStart* Cand : WeaponAnchoredStarts)
	{
		if (!Cand || Cand == ExcludeStart) continue;
		if (ExcludeGroup != NAME_None && GetWeaponGroupForStart(Cand) == ExcludeGroup) continue;
		Eligible.Add(Cand);
	}
	if (Eligible.Num() == 0)
	{
		// Last-resort: drop the group filter so we still hand back a weapon spot.
		for (APlayerStart* Cand : WeaponAnchoredStarts)
		{
			if (!Cand || Cand == ExcludeStart) continue;
			Eligible.Add(Cand);
		}
		if (Eligible.Num() == 0) return nullptr;
	}

	APlayerStart* Picked = Eligible[FMath::RandRange(0, Eligible.Num() - 1)];
	UE_LOG(LogNCLeagueDuel, Log,
		TEXT("First-spawn pick for %s (team %d, choice %c): weapon-anchored fallback -> %s [excluded group %s]"),
		*PS->PlayerName, TeamIdx, ChoiceIdx == 0 ? TCHAR('A') : TCHAR('B'),
		Picked ? *Picked->GetActorLocation().ToString() : TEXT("nullptr"),
		*ExcludeGroup.ToString());
	return Picked;
}

// =============================================================================
// RatePlayerStart + ChoosePlayerStart
// =============================================================================

float ANCLeagueDuelGame::ComputeEnemyProximityScore(APlayerStart* P, AController* Player,
	float& OutMinEnemyDist, bool& bOutHasLOS) const
{
	OutMinEnemyDist = FLT_MAX;
	bOutHasLOS = false;
	if (!P || !Player) return 0.f;

	const FVector StartLoc = P->GetActorLocation();
	static FName NAME_NCDuelSpawnLos(TEXT("NCDuelSpawnLOS"));

	float Penalty = 0.f;
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* C = It->Get();
		if (!C || C == Player || !C->GetPawn()) continue;
		AUTCharacter* EnemyChar = Cast<AUTCharacter>(C->GetPawn());
		if (!EnemyChar || EnemyChar->IsDead()) continue;

		const FVector EnemyLoc = EnemyChar->GetActorLocation();
		const float Dist = (StartLoc - EnemyLoc).Size();
		OutMinEnemyDist = FMath::Min(OutMinEnemyDist, Dist);

		// LOS penalty — same eye-to-eye pattern as ShockDomGameMode.
		const FVector SpawnEye = StartLoc + FVector(0.f, 0.f, 64.f);
		const FVector EnemyEye = EnemyLoc + FVector(0.f, 0.f, EnemyChar->BaseEyeHeight);
		if (!GetWorld()->LineTraceTestByChannel(
			SpawnEye, EnemyEye,
			COLLISION_TRACE_WEAPONNOCHARACTER,
			FCollisionQueryParams(NAME_NCDuelSpawnLos, false)))
		{
			bOutHasLOS = true;
			Penalty -= 200.f;
		}
	}
	return Penalty;
}

float ANCLeagueDuelGame::RatePlayerStart(APlayerStart* P, AController* Player)
{
	float Score = Super::RatePlayerStart(P, Player);
	if (Score <= 0.f || !P || !Player) return Score;

	if (IsExcludedByActiveShieldBelt(P)) return -1.f;

	if (const FVector* KillerLoc = LastKillerLocation.Find(Player))
	{
		const float Dist = (P->GetActorLocation() - *KillerLoc).Size2D();
		if (Dist < MinKillerSpawnDistance) return 0.f;
		Score += FMath::Min(Dist, 5000.f) * 0.2f;
	}

	float MinEnemyDist = FLT_MAX;
	bool bHasLOS = false;
	Score += ComputeEnemyProximityScore(P, Player, MinEnemyDist, bHasLOS);

	return FMath::Max(Score, 0.2f);
}

AActor* ANCLeagueDuelGame::ChoosePlayerStart_Implementation(AController* Player)
{
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS)
	{
		UE_LOG(LogNCLeagueDuel, Warning,
			TEXT("ChoosePlayerStart: no AUTPlayerState on controller %s — deferring to Super"),
			Player ? *Player->GetName() : TEXT("(null)"));
		return Super::ChoosePlayerStart_Implementation(Player);
	}

	// Diagnostic: log every entry with the PlayerState's current spawn-choice
	// state. The expected sequence per InitNewPlayer (UTGameMode.cpp:3056-3057)
	// is ChoosePlayerStart called twice — first populates RespawnChoiceA (we
	// see A=null on entry), second populates RespawnChoiceB (we see A=set,
	// B=null). A third call from RestartPlayer→FindPlayerStart sees both set
	// and is deferred to Super. If you see fewer than 3 lines with this prefix
	// per match, something is intercepting calls before they reach us (BP
	// override, mutator, etc.).
	UE_LOG(LogNCLeagueDuel, Log,
		TEXT("ChoosePlayerStart entry: PS=%s team=%d bHasRespawnChoices=%d RespawnChoiceA=%s RespawnChoiceB=%s bChosePrimary=%d"),
		*PS->PlayerName, PS->Team ? PS->Team->TeamIndex : -1,
		bHasRespawnChoices ? 1 : 0,
		PS->RespawnChoiceA ? *PS->RespawnChoiceA->GetActorLocation().ToString() : TEXT("(null)"),
		PS->RespawnChoiceB ? *PS->RespawnChoiceB->GetActorLocation().ToString() : TEXT("(null)"),
		PS->bChosePrimaryRespawnChoice ? 1 : 0);

	// AUTGameMode populates RespawnChoiceA, then RespawnChoiceB by calling
	// ChoosePlayerStart twice in a row, then later calls it AGAIN to actually
	// spawn the player at whichever they picked (bChosePrimaryRespawnChoice).
	// On that third call, both A and B are already non-null — defer to Super
	// so it returns A or B per the player's pick rather than running our
	// picker fresh and ignoring their choice. (See UTGameMode.cpp:3068.)
	if (bHasRespawnChoices && PS->RespawnChoiceA != nullptr && PS->RespawnChoiceB != nullptr)
	{
		AActor* SuperPick = Super::ChoosePlayerStart_Implementation(Player);
		UE_LOG(LogNCLeagueDuel, Log,
			TEXT("ChoosePlayerStart spawn-actual: deferring to Super — returned %s (chose %s)"),
			SuperPick ? *SuperPick->GetActorLocation().ToString() : TEXT("(null)"),
			PS->bChosePrimaryRespawnChoice ? TEXT("A") : TEXT("B"));
		return SuperPick;
	}

	// We're being asked to populate either A or B with a fresh pick. If A is
	// already set we're filling B — exclude A from candidates so the two
	// choices are guaranteed distinct (the random tiebreak otherwise often
	// returns the same top-scoring start twice).
	APlayerStart* ExcludeStart = nullptr;
	if (bHasRespawnChoices && PS->RespawnChoiceA != nullptr && PS->RespawnChoiceB == nullptr)
	{
		ExcludeStart = PS->RespawnChoiceA;
	}

	// First-spawn path. Both populate-A (ExcludeStart=null) and populate-B
	// (ExcludeStart=A's pick) hit SelectPairedSpawnForFirstSpawn so each
	// choice comes from a distinct pre-shuffled pair — A from shuffle[0],
	// B from shuffle[1]. The shuffle is per-match, so red and blue see the
	// same two pairs; the bRedGetsPrimarySide flag swaps which side each
	// team gets (mutual fairness). PlayersWhoSpawnedOnce is marked when
	// filling B so the eventual third "actual spawn" call falls through
	// to Super (which returns A or B per bChosePrimaryRespawnChoice).
	if (!PlayersWhoSpawnedOnce.Contains(PS) && WeaponPairs.Num() > 0)
	{
		const int32 ChoiceIdx = (ExcludeStart == nullptr) ? 0 : 1;
		if (APlayerStart* Anchor = SelectPairedSpawnForFirstSpawn(PS, ChoiceIdx, ExcludeStart))
		{
			if (ExcludeStart != nullptr)
			{
				PlayersWhoSpawnedOnce.Add(PS);
			}
			return Anchor;
		}
	}
	PlayersWhoSpawnedOnce.Add(PS);

	// Tier 1: dynamic scoring within all PlayerStarts (minus belt exclusions
	// and the already-picked choice when filling B).
	APlayerStart* BestSpawn = nullptr;
	float BestScore = -FLT_MAX;
	APlayerStart* FallbackSpawn = nullptr;
	float FallbackEnemyDist = -FLT_MAX;

	// Distance filters (inter-A/B + killer-distance + min-enemy-distance) only
	// apply once the player has been killed at least once. The very first
	// spawn of the game is pure pair/weapon-anchored selection — fairness is
	// positional equivalence to the opponent, not "far from a killer that
	// doesn't exist yet."
	const bool bIsFirstSpawn = !LastKillerLocation.Contains(Player);

	for (APlayerStart* Start : AllPlayerStarts)
	{
		if (!Start) continue;
		if (Start == ExcludeStart) continue;
		if (IsExcludedByActiveShieldBelt(Start)) continue;

		// Inter-A/B distance: when filling B post-death, reject candidates
		// within MinKillerSpawnDistance of the already-picked A so the player
		// can't be cornered with two close-together choices. Skip on first
		// spawn — both choices being near each other isn't a tactical issue
		// when nobody's hunting the player yet.
		if (ExcludeStart && !bIsFirstSpawn)
		{
			const float DistFromA = (Start->GetActorLocation() - ExcludeStart->GetActorLocation()).Size2D();
			if (DistFromA < MinKillerSpawnDistance) continue;
		}

		float MinEnemyDist = FLT_MAX;
		bool  bHasLOS = false;
		const float ProxAdj = ComputeEnemyProximityScore(Start, Player, MinEnemyDist, bHasLOS);

		if (MinEnemyDist > FallbackEnemyDist)
		{
			FallbackEnemyDist = MinEnemyDist;
			FallbackSpawn = Start;
		}
		// Min-enemy-distance hard reject — also skipped on first spawn so a
		// tiny map with weapons close to spawn points doesn't wedge the picker.
		if (!bIsFirstSpawn && MinEnemyDist < MinimumEnemySpawnDistance) continue;

		// Killer-distance reject (no-op if bIsFirstSpawn since the Find above
		// is what defines bIsFirstSpawn — but we keep the lookup explicit so
		// the intent is obvious to readers).
		if (!bIsFirstSpawn)
		{
			if (const FVector* KillerLoc = LastKillerLocation.Find(Player))
			{
				const float KillerDist = (Start->GetActorLocation() - *KillerLoc).Size2D();
				if (KillerDist < MinKillerSpawnDistance) continue;
			}
		}

		float Score = (MinEnemyDist < FLT_MAX ? FMath::Min(MinEnemyDist, 5000.f) * 0.5f : 0.f)
		            + ProxAdj
		            + FMath::FRandRange(0.f, 200.f);
		if (Score > BestScore) { BestScore = Score; BestSpawn = Start; }
	}

	if (BestSpawn) return BestSpawn;

	// Tier 2: best-of-bad from full set (still respects belt + exclusion,
	// drops enemy-distance hard reject — keep killer-distance reject).
	if (FallbackSpawn && FallbackSpawn != ExcludeStart) return FallbackSpawn;

	// Tier 3: full fallback to engine picker.
	return Super::ChoosePlayerStart_Implementation(Player);
}

AActor* ANCLeagueDuelGame::FindPlayerStart_Implementation(AController* Player, const FString& IncomingName)
{
	// Mirror the original BP's FindPlayerStart override: when both
	// RespawnChoiceA and RespawnChoiceB are populated and the player has
	// committed to one (bChosePrimaryRespawnChoice), short-circuit and return
	// that PlayerStart directly. Bypasses any engine path that would otherwise
	// re-route through ChoosePlayerStart (or skip it entirely on
	// IncomingName-matched lookups), guaranteeing the player spawns at the
	// pick they selected in the spawn-choice UI.
	//
	// Why both ChoosePlayerStart AND FindPlayerStart need overriding (the BP
	// did this for a reason): ChoosePlayerStart populates the choices,
	// FindPlayerStart is the engine's actual "give me where to spawn" hook
	// and can be called via paths that don't reach ChoosePlayerStart (named-
	// PlayerStart matches, alternate spawn flows from mutators/replays, etc.).
	if (AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr)
	{
		if (bHasRespawnChoices && PS->RespawnChoiceA && PS->RespawnChoiceB)
		{
			APlayerStart* Picked = PS->bChosePrimaryRespawnChoice
				? PS->RespawnChoiceA
				: PS->RespawnChoiceB;
			if (Picked)
			{
				UE_LOG(LogNCLeagueDuel, Log,
					TEXT("FindPlayerStart short-circuit for %s (team %d): chose %s -> %s | A=%s B=%s"),
					*PS->PlayerName, PS->Team ? PS->Team->TeamIndex : -1,
					PS->bChosePrimaryRespawnChoice ? TEXT("A") : TEXT("B"),
					*Picked->GetActorLocation().ToString(),
					*PS->RespawnChoiceA->GetActorLocation().ToString(),
					*PS->RespawnChoiceB->GetActorLocation().ToString());
				return Picked;
			}
		}
	}

	// No committed pick yet — fall back to Super, which will route through
	// ChoosePlayerStart (and our override) for the populate calls.
	AActor* Result = Super::FindPlayerStart_Implementation(Player, IncomingName);
	if (AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr)
	{
		UE_LOG(LogNCLeagueDuel, Log,
			TEXT("FindPlayerStart fallthrough for %s (team %d): Super returned %s | A=%s B=%s bChosePrimary=%d"),
			*PS->PlayerName, PS->Team ? PS->Team->TeamIndex : -1,
			Result ? *Result->GetActorLocation().ToString() : TEXT("(null)"),
			PS->RespawnChoiceA ? *PS->RespawnChoiceA->GetActorLocation().ToString() : TEXT("(null)"),
			PS->RespawnChoiceB ? *PS->RespawnChoiceB->GetActorLocation().ToString() : TEXT("(null)"),
			PS->bChosePrimaryRespawnChoice ? 1 : 0);
	}
	return Result;
}

void ANCLeagueDuelGame::ScoreKill_Implementation(AController* Killer, AController* Other,
	APawn* KilledPawn, TSubclassOf<UDamageType> DamageType)
{
	Super::ScoreKill_Implementation(Killer, Other, KilledPawn, DamageType);

	// Capture the killer's pawn location so the victim's next spawn picker can
	// enforce MinKillerSpawnDistance.
	if (Killer && Killer->GetPawn())
	{
		LastKillerLocation.FindOrAdd(Other) = Killer->GetPawn()->GetActorLocation();
	}
}

// =============================================================================
// Stats summary builder
// =============================================================================

void ANCLeagueDuelGame::BuildMatchSummary(FNCMatchSummary& Out) const
{
	Out.GameMode  = TEXT("NCLeagueDuel");
	if (UTGameState)
	{
		Out.MapName    = GetWorld() ? GetWorld()->GetMapName() : FString();
		Out.ServerName = UTGameState->ServerName;
		Out.RedScore   = UTGameState->Teams.IsValidIndex(0) && UTGameState->Teams[0] ? UTGameState->Teams[0]->Score : 0;
		Out.BlueScore  = UTGameState->Teams.IsValidIndex(1) && UTGameState->Teams[1] ? UTGameState->Teams[1]->Score : 0;
	}

	if (!UTGameState) return;
	for (APlayerState* APS : UTGameState->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS);
		if (!UTPS || UTPS->bOnlySpectator) continue;

		FNCPlayerSummary P;
		P.UniqueId   = UTPS->StatsID.IsEmpty() ? UTPS->PlayerName : UTPS->StatsID;
		P.PlayerName = UTPS->PlayerName;
		P.Score      = UTPS->Score;
		P.Kills      = UTPS->Kills;
		P.Deaths     = UTPS->Deaths;
		P.Ping       = UTPS->Ping;
		P.Team       = UTPS->Team ? UTPS->Team->TeamIndex : 0;

		// Per-weapon accuracy from the engine stats system.
		// FIntPoint stores Shots in X and Hits in Y.
		P.WeaponAccuracy.Add(FName(TEXT("LinkGun")),
			FIntPoint(UTPS->GetStatsValue(NAME_LinkShots), UTPS->GetStatsValue(NAME_LinkHits)));
		P.WeaponAccuracy.Add(FName(TEXT("ShockRifle")),
			FIntPoint(UTPS->GetStatsValue(NAME_ShockRifleShots), UTPS->GetStatsValue(NAME_ShockRifleHits)));
		P.WeaponAccuracy.Add(FName(TEXT("SniperRifle")),
			FIntPoint(UTPS->GetStatsValue(NAME_SniperShots), UTPS->GetStatsValue(NAME_SniperHits)));

		if (RatingSystem)
		{
			P.PreMatchElo  = RatingSystem->GetPreMatchElo(P.UniqueId);
			P.PostMatchElo = RatingSystem->GetCachedElo(P.UniqueId);
		}

		Out.Players.Add(P);
	}
}
