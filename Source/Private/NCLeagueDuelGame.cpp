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
	ConsumedPairIndices.Reset();

	// Group PlayerStarts by AssociatedPickup's WeaponType group.
	TMap<FName, TArray<APlayerStart*>> GroupToStarts;
	int32 StartsWithoutAssoc = 0;
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		APlayerStart* PS = *It;
		if (!PS) continue;
		AllPlayerStarts.Add(PS);

		AUTPlayerStart* UTPS = Cast<AUTPlayerStart>(PS);
		if (!UTPS || !UTPS->AssociatedPickup) { ++StartsWithoutAssoc; continue; }

		AUTPickupWeapon* WPickup = Cast<AUTPickupWeapon>(UTPS->AssociatedPickup);
		if (!WPickup || !WPickup->WeaponType) continue;

		const FName Group = GroupForWeaponClass(WPickup->WeaponType);
		if (Group == NAME_None) continue;

		GroupToStarts.FindOrAdd(Group).Add(PS);
	}

	UE_LOG(LogNCLeagueDuel, Log,
		TEXT("ComputeSpawnPairings: %d total starts, %d without AssociatedPickup, %d weapon groups identified"),
		AllPlayerStarts.Num(), StartsWithoutAssoc, GroupToStarts.Num());

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
// First-spawn helper
// =============================================================================

APlayerStart* ANCLeagueDuelGame::SelectPairedSpawnForFirstSpawn(AUTPlayerState* PS)
{
	if (!PS || WeaponPairs.Num() == 0) return nullptr;

	// Find an unconsumed pair (each pair is used at most once per match — once
	// both players have consumed their first spawn, this never fires again).
	TArray<int32> Available;
	Available.Reserve(WeaponPairs.Num());
	for (int32 i = 0; i < WeaponPairs.Num(); ++i)
	{
		if (!ConsumedPairIndices.Contains(i)) Available.Add(i);
	}
	if (Available.Num() == 0) return nullptr;

	const int32 PickedIdx = Available[FMath::RandRange(0, Available.Num() - 1)];
	const FNCLeagueWeaponPair& Pair = WeaponPairs[PickedIdx];

	// Deterministic A/B assignment by team index — keeps red↔blue consistent
	// across both players' first-spawn calls when ChoosePlayerStart fires
	// independently for each.
	APlayerStart* Chosen = nullptr;
	if (PS->Team && PS->Team->TeamIndex == 0)
	{
		Chosen = Pair.StartA;
	}
	else
	{
		Chosen = Pair.StartB;
	}

	// Mark the pair consumed only after BOTH players have spawned. Track via
	// PlayersWhoSpawnedOnce set count: the second call from the matched
	// teammate will trigger consumption. For 1v1, this is just "both players
	// have spawned at least once".
	int32 SpawnedCount = PlayersWhoSpawnedOnce.Num();
	if (SpawnedCount >= 1)
	{
		// This is the second player's first spawn — pair is now fully assigned.
		ConsumedPairIndices.Add(PickedIdx);
	}

	UE_LOG(LogNCLeagueDuel, Log,
		TEXT("First spawn for %s (team %d): assigned pair index %d (%s)"),
		*PS->PlayerName, PS->Team ? PS->Team->TeamIndex : -1,
		PickedIdx,
		Chosen ? *Chosen->GetActorLocation().ToString() : TEXT("nullptr"));

	return Chosen;
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
	if (!PS) return Super::ChoosePlayerStart_Implementation(Player);

	// AUTGameMode populates RespawnChoiceA, then RespawnChoiceB by calling
	// ChoosePlayerStart twice in a row, then later calls it AGAIN to actually
	// spawn the player at whichever they picked (bChosePrimaryRespawnChoice).
	// On that third call, both A and B are already non-null — defer to Super
	// so it returns A or B per the player's pick rather than running our
	// picker fresh and ignoring their choice. (See UTGameMode.cpp:3068.)
	if (bHasRespawnChoices && PS->RespawnChoiceA != nullptr && PS->RespawnChoiceB != nullptr)
	{
		return Super::ChoosePlayerStart_Implementation(Player);
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

	// First-spawn path: paired weapon anchor (only applies on the very first
	// ChoosePlayerStart call for this PS — subsequent calls fall through).
	if (!PlayersWhoSpawnedOnce.Contains(PS) && WeaponPairs.Num() > 0)
	{
		if (APlayerStart* Anchor = SelectPairedSpawnForFirstSpawn(PS))
		{
			PlayersWhoSpawnedOnce.Add(PS);
			if (Anchor != ExcludeStart) return Anchor;
			// Pair anchor collided with already-picked-A — fall through to Tier 1.
		}
	}
	PlayersWhoSpawnedOnce.Add(PS);

	// Tier 1: dynamic scoring within all PlayerStarts (minus belt exclusions
	// and the already-picked choice when filling B).
	APlayerStart* BestSpawn = nullptr;
	float BestScore = -FLT_MAX;
	APlayerStart* FallbackSpawn = nullptr;
	float FallbackEnemyDist = -FLT_MAX;

	for (APlayerStart* Start : AllPlayerStarts)
	{
		if (!Start) continue;
		if (Start == ExcludeStart) continue;
		if (IsExcludedByActiveShieldBelt(Start)) continue;

		float MinEnemyDist = FLT_MAX;
		bool  bHasLOS = false;
		const float ProxAdj = ComputeEnemyProximityScore(Start, Player, MinEnemyDist, bHasLOS);

		if (MinEnemyDist > FallbackEnemyDist)
		{
			FallbackEnemyDist = MinEnemyDist;
			FallbackSpawn = Start;
		}
		if (MinEnemyDist < MinimumEnemySpawnDistance) continue;

		// Killer-distance reject.
		if (const FVector* KillerLoc = LastKillerLocation.Find(Player))
		{
			const float KillerDist = (Start->GetActorLocation() - *KillerLoc).Size2D();
			if (KillerDist < MinKillerSpawnDistance) continue;
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
