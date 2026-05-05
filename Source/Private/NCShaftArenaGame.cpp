// NCShaftArenaGame.cpp — 1v1 shaft-only with vampirism, win-by-2, ELO + awards.

#include "NCShaftArenaGame.h"
#include "UnrealTournament.h"
#include "UTPlayerState.h"
#include "UTGameState.h"
#include "UTCharacter.h"
#include "UTPlayerController.h"
#include "UTHUD.h"
#include "Engine/World.h"
#include "StatNames.h"
#include "NCShaftArenaHUD.h"
#include "NCShaftArenaRatingSystem.h"
#include "NCStatsUploader.h"
#include "UTWeap_LinkGun_Plus.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCShaftArena, Log, All);

ANCShaftArenaGame::ANCShaftArenaGame(const FObjectInitializer& OI)
	: Super(OI)
{
	DisplayName     = NSLOCTEXT("UTGameMode", "NCShaftArena", "NetcodePlus Shaft Arena");
	HUDClass        = ANCShaftArenaHUD::StaticClass();
	GoalScore       = 10;
	TimeLimit       = 0;
	MinWinMargin    = 2;
	SiphonPercent   = 0.5f;
	HealCap         = 199;
	// ShaftLinkClass: nullptr by default — user supplies it via a BP subclass
	// of NCShaftArenaGame, or via Mod.ini [NCShaftArena] WeaponClass=...
	// DefaultInventory is left untouched here; InitGame configures it after
	// resolving the class so a Mod.ini override can replace the BP value.
	ShaftLinkClass = nullptr;
}

void ANCShaftArenaGame::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);

	const FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	int32 IniGoal = GoalScore;
	GConfig->GetInt  (TEXT("NCShaftArena"), TEXT("GoalScore"),     IniGoal,        ConfigPath);
	GConfig->GetInt  (TEXT("NCShaftArena"), TEXT("MinWinMargin"),  MinWinMargin,   ConfigPath);
	GConfig->GetFloat(TEXT("NCShaftArena"), TEXT("SiphonPercent"), SiphonPercent,  ConfigPath);
	GConfig->GetInt  (TEXT("NCShaftArena"), TEXT("HealCap"),       HealCap,        ConfigPath);
	GoalScore = IniGoal;

	// Mod.ini WeaponClass overrides the BP-set ShaftLinkClass when present.
	// Path format mirrors blueprint asset references:
	//   /Game/Mods/NetcodePlus/Weapons/BP_ShaftLink.BP_ShaftLink_C
	FString WeaponPath;
	if (GConfig->GetString(TEXT("NCShaftArena"), TEXT("WeaponClass"), WeaponPath, ConfigPath)
		&& !WeaponPath.IsEmpty())
	{
		if (UClass* Loaded = LoadClass<AUTWeap_LinkGun_Plus>(nullptr, *WeaponPath))
		{
			ShaftLinkClass = Loaded;
			UE_LOG(LogNCShaftArena, Log, TEXT("InitGame: loaded ShaftLinkClass from Mod.ini: %s"), *WeaponPath);
		}
		else
		{
			UE_LOG(LogNCShaftArena, Warning,
				TEXT("InitGame: Mod.ini WeaponClass='%s' did not resolve to an AUTWeap_LinkGun_Plus subclass"),
				*WeaponPath);
		}
	}

	if (ShaftLinkClass)
	{
		// Loadout: ONLY the configured shaft link. Base AUTGameMode::GiveDefaultInventory
		// iterates DefaultInventory at SetPlayerDefaults time.
		DefaultInventory.Empty();
		DefaultInventory.Add(ShaftLinkClass);
	}
	else
	{
		UE_LOG(LogNCShaftArena, Warning,
			TEXT("InitGame: ShaftLinkClass not set — players will spawn with stock inventory. ")
			TEXT("Configure via BP subclass or Mod.ini [NCShaftArena] WeaponClass=..."));
	}

	UE_LOG(LogNCShaftArena, Log,
		TEXT("InitGame: GoalScore=%d MinWinMargin=%d Siphon=%.2f HealCap=%d ShaftLinkClass=%s"),
		GoalScore, MinWinMargin, SiphonPercent, HealCap,
		ShaftLinkClass ? *ShaftLinkClass->GetPathName() : TEXT("(none)"));
}

void ANCShaftArenaGame::BeginPlay()
{
	Super::BeginPlay();

	if (Role != ROLE_Authority) return;

	RatingSystem = MakeUnique<FNCShaftArenaRatingSystem>();
	FNCShaftArenaRatingSystem::InitDatabase(GetWorld());
}

void ANCShaftArenaGame::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	if (Role != ROLE_Authority || !RatingSystem || !NewPlayer) return;
	AUTPlayerState* PS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (!PS) return;
	const FString UniqueId = PS->StatsID.IsEmpty() ? PS->PlayerName : PS->StatsID;
	RatingSystem->LoadPlayerFromDB(GetWorld(), UniqueId);
}

void ANCShaftArenaGame::HandleMatchHasStarted()
{
	Super::HandleMatchHasStarted();
	if (Role == ROLE_Authority && RatingSystem)
	{
		RatingSystem->SnapshotMatchStart();
	}
}

void ANCShaftArenaGame::HandleMatchHasEnded()
{
	Super::HandleMatchHasEnded();
	if (Role != ROLE_Authority || !RatingSystem || !UTGameState) return;

	// Resolve top two scorers (FFA, but should be 1v1 in practice).
	AUTPlayerState* Winner = nullptr;
	AUTPlayerState* Loser  = nullptr;
	for (APlayerState* APS : UTGameState->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS);
		if (!UTPS || UTPS->bOnlySpectator) continue;
		if (!Winner || UTPS->Score > Winner->Score)      { Loser = Winner; Winner = UTPS; }
		else if (!Loser || UTPS->Score > Loser->Score)   { Loser = UTPS; }
	}

	if (Winner && Loser)
	{
		const FString WinnerId = Winner->StatsID.IsEmpty() ? Winner->PlayerName : Winner->StatsID;
		const FString LoserId  = Loser->StatsID.IsEmpty()  ? Loser->PlayerName  : Loser->StatsID;
		const float WinnerAcc = ComputeLinkAccuracyPct(Winner);
		const float LoserAcc  = ComputeLinkAccuracyPct(Loser);
		const int32 WinnerStreak = BestStreakThisMatch.FindRef(Winner);
		const int32 LoserStreak  = BestStreakThisMatch.FindRef(Loser);

		RatingSystem->RecordMatchResult(
			WinnerId, LoserId,
			Winner->Score, Loser->Score,
			WinnerAcc, LoserAcc,
			WinnerStreak, LoserStreak);
		RatingSystem->Flush(GetWorld());
	}

	FNCMatchSummary Summary;
	BuildMatchSummary(Summary);
	FNCStatsUploader::PostToUT4Stats(GetWorld(), Summary);
	FNCStatsUploader::PostToStatSQL(GetWorld(), Summary);
}

// =============================================================================
// Vampirism (always-on)
// =============================================================================

void ANCShaftArenaGame::ScoreDamage_Implementation(int32 DamageAmount,
	AUTPlayerState* Victim, AUTPlayerState* Attacker)
{
	Super::ScoreDamage_Implementation(DamageAmount, Victim, Attacker);

	if (!Attacker || !Victim || Attacker == Victim || DamageAmount <= 0) return;
	AUTCharacter* AttackerChar = Attacker->GetUTCharacter();
	if (!AttackerChar || AttackerChar->IsDead() || AttackerChar->IsPendingKillPending()) return;

	const int32 Heal  = FMath::CeilToInt(float(DamageAmount) * SiphonPercent);
	const int32 NewHP = FMath::Min<int32>(AttackerChar->Health + Heal, HealCap);
	if (NewHP > AttackerChar->Health)
	{
		AttackerChar->Health = NewHP;
		AttackerChar->OnHealthUpdated();
	}
}

// =============================================================================
// Streak tracking + win-by-2
// =============================================================================

void ANCShaftArenaGame::ScoreKill_Implementation(AController* Killer, AController* Other,
	APawn* KilledPawn, TSubclassOf<UDamageType> DamageType)
{
	Super::ScoreKill_Implementation(Killer, Other, KilledPawn, DamageType);

	AUTPlayerState* KillerPS = Killer ? Cast<AUTPlayerState>(Killer->PlayerState) : nullptr;
	AUTPlayerState* VictimPS = Other  ? Cast<AUTPlayerState>(Other->PlayerState)  : nullptr;

	if (KillerPS && KillerPS != VictimPS)
	{
		int32& Streak = CurrentStreak.FindOrAdd(KillerPS);
		++Streak;
		int32& Best = BestStreakThisMatch.FindOrAdd(KillerPS);
		Best = FMath::Max(Best, Streak);
	}
	if (VictimPS)
	{
		// Death resets the victim's current streak.
		CurrentStreak.FindOrAdd(VictimPS) = 0;
	}
}

bool ANCShaftArenaGame::CheckScore_Implementation(AUTPlayerState* Scorer)
{
	if (!Scorer || !UTGameState) return false;

	// Find leader's score and runner-up's score.
	int32 LeaderScore  = -1;
	int32 RunnerScore  = -1;
	AUTPlayerState* Leader = nullptr;
	for (APlayerState* APS : UTGameState->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS);
		if (!UTPS || UTPS->bOnlySpectator) continue;
		const int32 S = int32(UTPS->Score);
		if (S > LeaderScore) { RunnerScore = LeaderScore; LeaderScore = S; Leader = UTPS; }
		else if (S > RunnerScore) { RunnerScore = S; }
	}

	// First-to-GoalScore AND margin >= MinWinMargin.
	if (Leader && LeaderScore >= GoalScore && (LeaderScore - FMath::Max(0, RunnerScore)) >= MinWinMargin)
	{
		UE_LOG(LogNCShaftArena, Log,
			TEXT("CheckScore: ending match — %s reached %d (margin %d)"),
			*Leader->PlayerName, LeaderScore, LeaderScore - RunnerScore);
		EndGame(Leader, FName(TEXT("fraglimit")));
		return true;
	}
	return false;
}

// =============================================================================
// Helpers
// =============================================================================

float ANCShaftArenaGame::ComputeLinkAccuracyPct(AUTPlayerState* PS) const
{
	if (!PS) return 0.f;
	const int32 Hits  = PS->GetStatsValue(NAME_LinkHits);
	const int32 Shots = PS->GetStatsValue(NAME_LinkShots);
	return (Shots > 0) ? float(Hits) / float(Shots) * 100.f : 0.f;
}

void ANCShaftArenaGame::BuildMatchSummary(FNCMatchSummary& Out) const
{
	Out.GameMode = TEXT("NCShaftArena");
	if (UTGameState)
	{
		Out.MapName    = GetWorld() ? GetWorld()->GetMapName() : FString();
		Out.ServerName = UTGameState->ServerName;
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
		P.WeaponAccuracy.Add(FName(TEXT("LinkGun")),
			FIntPoint(UTPS->GetStatsValue(NAME_LinkShots), UTPS->GetStatsValue(NAME_LinkHits)));

		if (RatingSystem)
		{
			P.PreMatchElo  = RatingSystem->GetPreMatchElo(P.UniqueId);
			P.PostMatchElo = RatingSystem->GetCachedElo(P.UniqueId);
		}

		Out.Players.Add(P);
	}
}
