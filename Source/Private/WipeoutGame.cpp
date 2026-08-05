#include "WipeoutGame.h"
#include "NCPlusRoundSpectate.h"      // late-joiner / reconnect free-camera lock
#include "NCHybridSpawnGenerator.h"
#include "NCPlusVersionGate.h"
#include "NCConcedeVote.h"
#include "UnrealTournament.h"
#include "UTTeamGameMode.h"
#include "UTGameState.h"
#include "UTHUD_InstantReplay.h"
#include "UTCharacter.h"
#include "UTPlayerState.h"
#include "Sound/SoundBase.h"
#include "UTPlayerController.h"
#include "NPPlayerController.h"
#include "UTTeamInfo.h"
#include "UTTeamPlayerStart.h"
#include "UTDroppedPickup.h"
#include "UTPickup.h"
#include "UTPickupWeapon.h"
#include "UTPickupHealth.h"
#include "UTPickupAmmo.h"
#include "UTPickupInventory.h"
#include "Engine/DemoNetDriver.h"
#include "WipeoutDamageReplicator.h"
#include "NCAccuracyStatsReplicator.h"
#include "TeamArenaCharacter.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "UTCountDownMessage.h"
#include "UTGameMessage.h"
#include "UTShowdownStatusMessage.h"
#include "WipeoutHUD.h"
#include "SiphonPowerup.h"
#include "WipeoutRatingSystem.h"
#include "NCEloUploader.h"
#include "GameFramework/WorldSettings.h"            // KillZ for bad-spawn detection
#include "Components/CapsuleComponent.h"            // capsule half-height for snap-back
#include "GameFramework/CharacterMovementComponent.h" // IsFalling / StopMovementImmediately


// ============================================================================
// CONSTRUCTOR
// ============================================================================

AUWipeoutGame::AUWipeoutGame(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DisplayName = NSLOCTEXT("UTGameMode", "Wipeout", "Wipeout");
	bTeamGame = true;
	HUDClass = AWipeoutHUD::StaticClass();
	//PlayerControllerClass = ANPPlayerController::StaticClass(); // Enable when ready to test debounce fix

	// Team defaults
	NumTeams = 2;
	bBalanceTeams = true;
	bUseTeamStarts = false;
	bAnnounceTeam = true;
	bRecordReplays = true;

	// Sound defaults
	RedTeamVictorySound = nullptr;
	BlueTeamVictorySound = nullptr;
	RoundDrawSound = nullptr;
	MatchVictorySound = nullptr;
	RedTeamDominatingSound = nullptr;
	BlueTeamDominatingSound = nullptr;
	RedTeamTakesLeadSound = nullptr;
	BlueTeamTakesLeadSound = nullptr;
	OvertimeAnnouncementSound = nullptr;

	// Round defaults
	bForceRespawn = false;
	bHasRespawnChoices = false;
	ScoreLimit = 7;
	RoundTimeSeconds = 120; // Wipeout rounds run longer than elim (respawns = more time needed)
	bRoundInProgress = false;
	bAllowPlayerRespawns = false;
	LastRoundWinningTeamIndex = INDEX_NONE;
	AwardDisplayTime = 5.f;
	PreRoundCountdown = 3.f;
	SpectateDelay = 1.f;  // Shorter spectate delay since you're coming back
	TotalRoundsPlayed = 0;
	bWinByTwo = false;
	bWarmupMode = false;
	bCompetitiveAutoPause = false;
	useBPSpecFunction = false;

	// Wipeout-specific defaults: escalating respawn delays.
	// Indexed by death count — PER-PLAYER in the live WipeoutPlus_C BP, which sets
	// bTeamSharedDeathCounter=false (the C++ default below is team-shared=true).
	// NOTE: if the BP CDO ALSO overrides RespawnDelays, this array is shadowed —
	// change it in the BP (or clear that override) for these values to take effect.
	RespawnDelays.Add(5.0f);   // 1st death
	RespawnDelays.Add(9.0f);   // 2nd death
	RespawnDelays.Add(13.0f);  // 3rd death
	RespawnDelays.Add(20.0f);  // 4th death
	RespawnDelays.Add(30.0f);  // 5th death
	RespawnDelays.Add(40.0f);  // 6th+ deaths (cap)

	RespawnProtectionTime = 1.5f;
	WipeoutGracePeriod = 0.15f;
	bTeamSharedDeathCounter = true;

	// Link Gun beam teammate healing (5 HP/sec, capped at 100)
	LinkHealPerSecond = 5.0f;
	LinkHealMaxHP = 100;

	// Wipeout runtime
	Team0DeathCount = 0;
	Team1DeathCount = 0;
	Team0RoundDamage = 0.f;
	Team1RoundDamage = 0.f;
	bInSuddenDeath = false;
	bSuddenDeathPending = false;
	HighDamageCarryThreshold = 60.0f;

	// Replay
	RoundWinningKiller = nullptr;
	WinningKillerPawn = nullptr;
	RoundWinningKillTime = 0.0f;

	// Scoring — default GoalScore, can be overridden via URL (?GoalScore=X) or BP subclass
	GoalScore = 5;

	// Overtime
	bOvertimeEnabled = true;
	OvertimeStartDelay = 5.0f;
	OvertimeBaseDamage = 5.0f;
	OvertimeDamageMultiplier = 1.5f;
	OvertimeMaxDamage = 0.0f;
	bOvertimeNonLethal = false;
	OvertimeWaveInterval = 5.0f;
	OvertimeDamageType = UUTDamageType::StaticClass();
	CurrentOvertimeWave = 0;
	CurrentWaveDamage = 0.0f;

	// Spawn defaults
	MinimumEnemySpawnDistance = 4000.0f;
	MinimumEnemyHorizontalDistance = 3000.0f;
	MidRoundMinEnemyDistance = 2000.0f;

	// Score tracking
	PreviousRedScore = 0;
	PreviousBlueScore = 0;
	bHasBroadcastTeamDominating = false;

	// Last alive tracking
	bTeam0LastAliveAnnounced = false;
	bTeam1LastAliveAnnounced = false;
	Team0StartingSize = 0;
	Team1StartingSize = 0;
}


// ============================================================================
// REPLAYS
// ============================================================================

bool AUWipeoutGame::SupportsInstantReplay() const
{
	// Re-enabled — the "InWorld == World" crash was caused by RestartPlayer
	// firing before team assignment, not by instant replay itself.
	// The team guard in RestartPlayer prevents the race condition.
	return true;
}


// ============================================================================
// MATCH STATE SHORTCUTS (BP-callable)
// ============================================================================

void AUWipeoutGame::BP_SetMatchState_RoundCooldown()
{
	if (HasAuthority())
	{
		SetMatchState(FName(TEXT("RoundCooldown")));
	}
}

void AUWipeoutGame::BP_SetMatchState_Intermission()
{
	if (HasAuthority())
	{
		SetMatchState(FName(TEXT("Intermission")));
	}
}

void AUWipeoutGame::BP_SetMatchState_InProgress()
{
	if (HasAuthority())
	{
		SetMatchState(MatchState::InProgress);
	}
}





// ============================================================================
// INIT
// ============================================================================

void AUWipeoutGame::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	Super::InitGame(MapName, Options, ErrorMessage);
	// Super::InitGame already parses ?GoalScore=X from the URL.
	// Only override if the URL explicitly provided a GoalScore option;
	// otherwise keep the BP default (GoalScore set in constructor).
	int32 URLGoalScore = UGameplayStatics::GetIntOption(Options, TEXT("GoalScore"), -1);
	if (URLGoalScore >= 0)
	{
		GoalScore = URLGoalScore;
	}
	TimeLimit = 0; // We manage time per-round

	// Match-scoped: reset the rating flush guard on each map load.
	bRatingFlushedThisMatch = false;

	// A bot-hosted PUG passes ?PugId=N — gate team pinning to real PUGs.
	bIsPugMatch = UGameplayStatics::HasOption(Options, TEXT("PugId"));

	// Default-on opening-round hybrid queue. Mid-round Wipeout respawns never
	// enter this path. This Mod.ini switch is the immediate rollback lever.
	const FString ModIni = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
	if (GConfig)
	{
		GConfig->GetBool(TEXT("NetcodePlus"), TEXT("WipeoutHybridRoundSpawns"), bEnableHybridRoundSpawns, ModIni);
	}

	// Bot-assigned teams: ?PugTeams=<ut4id>:0,<ut4id>:1,...  The bot balanced the
	// teams off the ut4stats ladder; pin each listed player to their side in
	// ChangeTeam so the engine's warmup auto-balance can't reshuffle them. Keys are
	// lowercased EOS ids (== MutBotEvents' Ut4Id == bot players.ut4_id). Players not
	// listed (unlinked, subs) aren't pinned and use the stock balancer. PUG-only.
	// Mirrors ANCPlusCTFGameMode.
	PugRosterTeam.Reset();
	const FString PugTeamsOpt = UGameplayStatics::ParseOption(Options, TEXT("PugTeams"));
	if (bIsPugMatch && !PugTeamsOpt.IsEmpty())
	{
		TArray<FString> Entries;
		PugTeamsOpt.ParseIntoArray(Entries, TEXT(","), true);
		for (const FString& Entry : Entries)
		{
			FString IdPart, TeamPart;
			if (Entry.Split(TEXT(":"), &IdPart, &TeamPart))
			{
				const FString Key = IdPart.ToLower();
				const uint8 TeamNum = (uint8)FMath::Clamp(FCString::Atoi(*TeamPart), 0, 1);
				if (!Key.IsEmpty())
				{
					PugRosterTeam.Add(Key, TeamNum);
				}
			}
		}
		UE_LOG(LogGameMode, Log, TEXT("Wipeout: PUG roster parsed — %d players pinned to teams"), PugRosterTeam.Num());
	}
}

bool AUWipeoutGame::ChangeTeam(AController* Player, uint8 NewTeam, bool bBroadcast)
{
	// Bot PUG: keep each rostered player on the side the bot balanced. Login picks,
	// player-initiated switches, and the engine's warmup auto-balance all funnel
	// through ChangeTeam, so this is the one place that pins them. Non-roster joiners
	// (subs, late fills, unlinked) get the stock balancer via Super. Mirrors
	// ANCPlusCTFGameMode::ChangeTeam.
	if (bIsPugMatch && PugRosterTeam.Num() > 0 && Player && HasAuthority())
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(Player->PlayerState);
		if (PS && !PS->bOnlySpectator && PS->UniqueId.IsValid())
		{
			// Match on UniqueId.ToString() — the same id MutBotEvents posts as Ut4Id
			// and the bot stores in players.ut4_id.
			if (const uint8* Assigned = PugRosterTeam.Find(PS->UniqueId.ToString().ToLower()))
			{
				const uint8 Want = *Assigned;
				// Already on the right side — accept without re-suiciding them
				// (MovePlayerToTeam kills the pawn on an actual move).
				if (PS->Team && PS->Team->TeamIndex == Want)
				{
					return true;
				}
				return MovePlayerToTeam(Player, PS, Want);
			}
		}
	}

	return Super::ChangeTeam(Player, NewTeam, bBroadcast);
}

void AUWipeoutGame::BeginPlay()
{
	Super::BeginPlay();

	if (!bSpawnPointsInitialized)
	{
		InitializeSpawnPointSystem();
		bSpawnPointsInitialized = true;
	}

	PrecomputeSpawnLayouts();

	// All actors have passed through CheckRelevance by now — resolve
	// whether the stashed vest pickup should become a ShieldBelt.
	ResolveShieldBeltSubstitution();

	// Siphon powerup spawned in HandleMatchHasStarted — BP actors may not
	// be fully loaded during BeginPlay (same issue as DamageReplicator).

	// Damage replicator is spawned in HandleMatchHasStarted instead of here —
	// spawning bAlwaysRelevant actors during BeginPlay can trigger package
	// loading on connecting clients before their world is fully set up.

	// Server-only rating system. Initialize Mods.db schema once per map load.
	if (HasAuthority() && !RatingSystem.IsValid())
	{
		RatingSystem = MakeUnique<FWipeoutRatingSystem>();
		FWipeoutRatingSystem::InitDatabase(GetWorld());
	}
}

void AUWipeoutGame::InitGameState()
{
	Super::InitGameState();
	// GameModeClass is left as-is — clients have NetcodePlus installed,
	// so the base class sets it correctly and the scoreboard reads
	// DisplayName from the actual game mode class (or BP subclass).

	// Disable automatic boost recharge — charges come from spawning only
	AUTGameState* GS = GetGameState<AUTGameState>();
	if (GS)
	{
		GS->BoostRechargeTime = 0.0f;
		GS->BoostRechargeMaxCharges = 0;
	}
}

bool AUWipeoutGame::ValidateHat(AUTPlayerState* HatOwner, const FString& HatClass)
{
	// Same as NCPlusCTF: force the chosen hat via OverrideHatClass (not entitlement-checked) next tick so
	// the community master's missing cosmetic entitlements can't strip it. Server-side; never kicks.
	if (HatOwner && !HatClass.IsEmpty())
	{
		TWeakObjectPtr<AUTPlayerState> WeakPS(HatOwner);
		const FString Path = HatClass;
		GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakPS, Path]()
		{
			if (AUTPlayerState* PS = WeakPS.Get())
			{
				PS->SetOverrideHatClass(Path);
				UE_LOG(LogTemp, Warning, TEXT("[Cosmetics] ForceOverrideHat '%s' -> %s"), *Path,
					PS->OverrideHatClass ? *PS->OverrideHatClass->GetName() : TEXT("NULL (class did not load)"));
			}
		}));
	}
	return Super::ValidateHat(HatOwner, HatClass);
}

void AUWipeoutGame::HandleMatchHasStarted()
{
	UE_LOG(LogGameMode, Warning, TEXT("=== Wipeout::HandleMatchHasStarted ==="));
	Super::HandleMatchHasStarted();
	bWarmupMode = false;

	// Defense-in-depth reset: InitGame already clears this on map load, but a
	// single server session can host multiple matches. Reset here so a subsequent
	// match can flush its own ratings.
	bRatingFlushedThisMatch = false;
	HealingDoneThisMatch.Empty();   // match-cumulative healing stat resets per match

	// Spawn the damage replicator now — all clients are fully loaded at this point.
	// Spawning in BeginPlay was too early and could cause client crashes.
	if (HasAuthority() && !DamageReplicator)
	{
		FActorSpawnParameters SpawnParams;
		SpawnParams.Owner = this;
		DamageReplicator = GetWorld()->SpawnActor<AWipeoutDamageReplicator>(SpawnParams);
	}

	// Per-weapon hits/shots replicator — Wipeout doesn't show accuracy by
	// default but the widget is opt-in via nchud, so spawn defensively.
	ANCAccuracyStatsReplicator::EnsureSpawned(this);

	// Spawn Siphon pickup here — BP actors fail to spawn during BeginPlay
	// because their packages aren't fully loaded yet.
	if (HasAuthority() && !SiphonPickup)
	{
		SpawnSiphonPickup();
	}

	// Snapshot every loaded player's current rating as their "match-start" value
	// for the per-match delta reported in BuildResultPayload.
	if (HasAuthority() && RatingSystem.IsValid())
	{
		RatingSystem->SnapshotMatchStart();
	}
}


void AUWipeoutGame::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);

	// Early plugin-version check — kicks mismatched clients within 10s of join.
	NCPlusVersionGate::SpawnFor(NewPlayer);
	// Concede-vote RPC channel (gg / F1 / F4) — skips bots + the listen host.
	NCConcede::SpawnFor(NewPlayer);

	// Server-only: pull this player's rating from Mods.db into the cache so it's
	// ready before the first round ends. Late joiners arriving mid-match also
	// get their rating loaded; SnapshotMatchStart for them is best-effort.
	if (!HasAuthority() || !RatingSystem.IsValid()) return;
	if (!NewPlayer) return;

	AUTPlayerState* UTPS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (UTPS && UTPS->UniqueId.IsValid())
	{
		RatingSystem->LoadPlayerFromDB(GetWorld(), UTPS->UniqueId.ToString());
	}
}


void AUWipeoutGame::HandleMatchHasEnded()
{
	Super::HandleMatchHasEnded();

	if (!HasAuthority() || !RatingSystem.IsValid() || bRatingFlushedThisMatch)
	{
		return;
	}

	// Persist updated ratings to Mods.db.
	RatingSystem->FlushAtMatchEnd(GetWorld());
	bRatingFlushedThisMatch = true;

	// Build + push the global-ELO payload to ut4stats.com.
	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (!GS) return;

	FNCWipeoutMatchInput UploadIn;

	int32 RedScore  = 0;
	int32 BlueScore = 0;
	if (GS->Teams.Num() >= 2 && GS->Teams[0] && GS->Teams[1])
	{
		RedScore  = static_cast<int32>(GS->Teams[0]->Score);
		BlueScore = static_cast<int32>(GS->Teams[1]->Score);
	}
	UploadIn.RedScore  = RedScore;
	UploadIn.BlueScore = BlueScore;
	if (RedScore > BlueScore)      UploadIn.WinnerTeamIndex = 0;
	else if (BlueScore > RedScore) UploadIn.WinnerTeamIndex = 1;
	else                            UploadIn.WinnerTeamIndex = -1;

	for (APlayerState* APS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS);
		if (!UTPS || UTPS->bOnlySpectator) continue;
		if (!UTPS->UniqueId.IsValid()) continue;  // bot

		FNCWipeoutPlayerInput P;
		P.UniqueId   = UTPS->UniqueId.ToString();
		P.PlayerName = UTPS->PlayerName;
		P.TeamIndex  = UTPS->GetTeamNum();
		P.Kills      = UTPS->Kills;
		P.Deaths     = UTPS->Deaths;
		P.Damage     = static_cast<int32>(UTPS->DamageDone);
		UploadIn.Players.Add(MoveTemp(P));
	}

	const FString Json = RatingSystem->BuildResultPayload(GetWorld(), UploadIn);
	if (!Json.IsEmpty())
	{
		FNCEloUploader::PostMatchResult(GetWorld(), Json);
	}
}


// ============================================================================
// STATE MACHINE — CallMatchStateChangeNotify
// (Same pattern as TeamArena: intercepts state transitions)
// ============================================================================

void AUWipeoutGame::CallMatchStateChangeNotify()
{
	UE_LOG(LogGameMode, Log, TEXT("Wipeout matchstate: %s"), *GetMatchState().ToString());

	if (GetMatchState() == MatchState::WaitingToStart)
	{
		bWarmupMode = true;
	}
	else if (GetMatchState() == MatchState::PlayerIntro)
	{
		bWarmupMode = false;
		ResetSpawnSelectionForNewRound();
	}

	if (GetMatchState() == MatchState::MatchIntermission || GetMatchState() == FName(TEXT("RoundCooldown")))
	{
		HandleMatchIntermission();
	}
	else if (GetMatchState() == MatchState::InProgress)
	{
		if (TotalRoundsPlayed == 0)
		{
			Super::CallMatchStateChangeNotify(); // Triggers HandleMatchHasStarted
		}
		bWarmupMode = false;
		StartNextRound();
	}
	else
	{
		Super::CallMatchStateChangeNotify();
	}
}


// ============================================================================
// DEFAULT TIMER — Drives intermission countdown + round clock
// ============================================================================

void AUWipeoutGame::DefaultTimer()
{
	if (GetWorld()->WorldType == EWorldType::EditorPreview) return;
	if (IsPendingKill() || HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed)) return;

	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (GS == nullptr || GS->IsPendingKill() || GetWorld()->bIsTearingDown) return;

	HandleServerManagement();

	// Re-assert the round camera lock once a second: the immediate lock at the
	// RestartPlayer refusal can be raced by the joining client's own
	// BeginSpectatingState, which spawns its spectator pawn locally and points the
	// camera at it without telling the server.
	EnforceRoundSpectatorLock();

	// --- Intermission Logic ---
	if (GS->GetMatchState() == MatchState::MatchIntermission || GetMatchState() == FName(TEXT("RoundCooldown")))
	{
		if (IntermissionSecondsRemaining > 0)
		{
			--IntermissionSecondsRemaining;
			BP_OnSetIntermission(true, IntermissionSecondsRemaining);

			if (IntermissionSecondsRemaining > 0 && IntermissionSecondsRemaining <= 3)
			{
				BroadcastLocalized(this, UUTCountDownMessage::StaticClass(), IntermissionSecondsRemaining, nullptr, nullptr, nullptr);
			}
		}
		else
		{
			BP_OnSetIntermission(false, IntermissionSecondsRemaining);
			CleanupWorldForNewRound();
			SetMatchState(MatchState::InProgress);
		}
		return;
	}

	// --- Round Active Logic ---
	if (bRoundInProgress)
	{
		int32 Alive0, Alive1;
		GetAliveCounts(Alive0, Alive1);

		// Solo/practice round bookkeeping (round started with one team empty).
		const bool bSoloRound = (Team0StartingSize == 0) ^ (Team1StartingSize == 0);
		if (bSoloRound && !GetWorldTimerManager().IsTimerActive(TH_RoundEndDelay))
		{
			// Someone joined the previously-empty team mid-practice-round: end it
			// now with no score so intermission starts a REAL round, both sides
			// fresh — wipeout rules should never judge a player who hasn't had a
			// spawn wave yet.
			int32 Members0, Members1;
			GetTeamMemberCounts(Members0, Members1);
			if (Members0 > 0 && Members1 > 0)
			{
				UE_LOG(LogGameMode, Warning, TEXT("Wipeout: practice round reset — %dv%d players now present, restarting as a real round."), Members0, Members1);
				FTimerDelegate TimerDelegate;
				TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), (int32)INDEX_NONE, FName(TEXT("PracticeReset")));
				GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
				return;
			}
		}

		int32 RoundRemain = 0;
		if (RoundEndTimeSeconds > 0.f)
		{
			RoundRemain = FMath::Max(0, (int32)FMath::CeilToInt(RoundEndTimeSeconds - GetWorld()->GetTimeSeconds()));

			BP_OnSetRound(true, RoundRemain, LastRoundWinningTeamIndex,
				Alive0, Alive1, Team0DeathCount, Team1DeathCount);

			CheckFinalLifeAnnouncements();

			if (RoundRemain == 0 && !bInSuddenDeath && !bSuddenDeathPending)
			{
				// Solo/practice: time's up vs an empty team — no sudden death against
				// nobody. Alive → the populated side takes the round ("TimeExpired");
				// dead between respawn waves at this exact second → draw. Real games
				// are unaffected (bSoloRound needs an empty team at round start).
				if (bSoloRound)
				{
					if (!GetWorldTimerManager().IsTimerActive(TH_RoundEndDelay))
					{
						const int32 Winner = (Team0StartingSize == 0)
							? ((Alive1 > 0) ? 1 : INDEX_NONE)
							: ((Alive0 > 0) ? 0 : INDEX_NONE);
						FTimerDelegate TimerDelegate;
						TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), Winner, FName(TEXT("TimeExpired")));
						GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
					}
					return;
				}

				// Time is up — start the grace period before sudden death.
				// Players with pending respawns within the grace window can still spawn.
				// (SuddenDeathGraceSeconds is shared with the HUD, which uses it to X
				// out respawn countdowns that can no longer land — keep them in sync.)
				bSuddenDeathPending = true;

				FTimerDelegate SuddenDeathDelegate;
				SuddenDeathDelegate.BindLambda([this]()
				{
					if (!bRoundInProgress || bInSuddenDeath) return;

					bInSuddenDeath = true;
					PendingRespawns.Empty();

					// Restart the round clock so players can time items during OT.
					RoundEndTimeSeconds = GetWorld()->GetTimeSeconds() + 300.f;

					// Force all dead players to spectate
					for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
					{
						AUTPlayerState* DeadPS = It->Get() ? Cast<AUTPlayerState>(It->Get()->PlayerState) : nullptr;
						if (DeadPS && DeadPS->bOutOfLives && !DeadPS->bOnlySpectator)
						{
							DeadPS->RespawnTime = 0.f;
							DeadPS->ForceNetUpdate();
							ForceTeamSpectate(DeadPS);
						}
					}

					int32 A0, A1;
					GetAliveCounts(A0, A1);
					if (A0 == 0 || A1 == 0)
					{
						CheckWipeoutCondition();
					}
				});

				FTimerHandle SuddenDeathGraceHandle;
				GetWorldTimerManager().SetTimer(SuddenDeathGraceHandle, SuddenDeathDelegate,
					SuddenDeathGraceSeconds, false);
				return;
			}
		}

		// Normal tick: check for wipeout
		CheckWipeoutCondition();

		// Reset LMS announce flag when team goes back above 1 alive (teammate respawned)
		if (Alive0 > 1) bTeam0LastAliveAnnounced = false;
		if (Alive1 > 1) bTeam1LastAliveAnnounced = false;

		// Check for "last player alive" situations (clutch)
		if (Alive0 == 1 && Team0StartingSize > 1 && !bTeam0LastAliveAnnounced)
		{
			AUTPlayerState* LastPS = FindAliveOnTeamPS(0);
			if (LastPS)
			{
				bTeam0LastAliveAnnounced = true;
				// Suppress LMS sound if a teammate respawns within 1 second
				if (!HasImminentRespawnOnTeam(0, 1.0f))
				{
					BP_OnLastPlayerAlive(0, LastPS, Alive1);
					OnClutchSituationStarted.Broadcast(LastPS, Alive1);
				}
			}
		}
		if (Alive1 == 1 && Team1StartingSize > 1 && !bTeam1LastAliveAnnounced)
		{
			AUTPlayerState* LastPS = FindAliveOnTeamPS(1);
			if (LastPS)
			{
				bTeam1LastAliveAnnounced = true;
				if (!HasImminentRespawnOnTeam(1, 1.0f))
				{
					BP_OnLastPlayerAlive(1, LastPS, Alive0);
					OnClutchSituationStarted.Broadcast(LastPS, Alive0);
				}
			}
		}

		Super::DefaultTimer();
	}
}


// ============================================================================
// WIPEOUT RESPAWN SYSTEM — The core new mechanic
// ============================================================================

float AUWipeoutGame::ComputeRespawnDelay(int32 TeamIndex, AUTPlayerState* PS) const
{
	int32 DeathIndex = 0;

	if (bTeamSharedDeathCounter)
	{
		// Team-wide escalation (Diabotical default)
		DeathIndex = (TeamIndex == 0) ? Team0DeathCount : Team1DeathCount;
	}
	else
	{
		// Per-player escalation (alternative mode)
		const int32* PlayerDeaths = PlayerDeathCounts.Find(PS);
		DeathIndex = PlayerDeaths ? *PlayerDeaths : 0;
	}

	return GetRespawnDelayForDeathIndex(DeathIndex);
}


float AUWipeoutGame::GetRespawnDelayForDeathIndex(int32 DeathIndex) const
{
	if (RespawnDelays.Num() == 0)
	{
		return 5.0f; // Fallback
	}

	// Clamp to last element (the cap)
	int32 Index = FMath::Clamp(DeathIndex, 0, RespawnDelays.Num() - 1);
	return RespawnDelays[Index];
}


float AUWipeoutGame::GetCurrentRespawnDelay(int32 TeamIndex) const
{
	int32 DeathCount = (TeamIndex == 0) ? Team0DeathCount : Team1DeathCount;
	return GetRespawnDelayForDeathIndex(DeathCount);
}


float AUWipeoutGame::GetPlayerRespawnTimeRemaining(AUTPlayerState* PS) const
{
	if (!PS) return 0.f;

	const FPendingRespawn* Pending = PendingRespawns.Find(PS);
	if (!Pending) return 0.f;

	float Elapsed = GetWorld()->GetTimeSeconds() - Pending->RespawnStartTime;
	return FMath::Max(0.f, Pending->TotalRespawnTime - Elapsed);
}


void AUWipeoutGame::CheckFinalLifeAnnouncements(int32 TeamFilter)
{
	if (!HasAuthority() || !bRoundInProgress || bInSuddenDeath || bSuddenDeathPending
		|| RoundEndTimeSeconds <= 0.f)
	{
		return;
	}

	// Solo/practice rounds end at regulation expiry instead of entering sudden death.
	if ((Team0StartingSize == 0) ^ (Team1StartingSize == 0))
	{
		return;
	}

	const int32 RoundSecondsRemaining = FMath::Max(0,
		FMath::CeilToInt(RoundEndTimeSeconds - GetWorld()->GetTimeSeconds()));
	const float RespawnWindow = float(RoundSecondsRemaining) + SuddenDeathGraceSeconds;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(*It);
		AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
		if (!PS || PS->bOnlySpectator || PS->bOutOfLives || !PS->Team || !PC->GetPawn()
			|| FinalLifeAnnouncedPlayers.Contains(PS))
		{
			continue;
		}

		const int32 TeamIndex = PS->Team->TeamIndex;
		if (TeamIndex > 1 || (TeamFilter != INDEX_NONE && TeamIndex != TeamFilter))
		{
			continue;
		}

		const float NextRespawnDelay = ComputeRespawnDelay(TeamIndex, PS);
		if (NextRespawnDelay <= RespawnWindow)
		{
			continue;
		}

		FinalLifeAnnouncedPlayers.Add(PS);
		PC->ClientReceiveLocalizedMessage(
			UUTShowdownStatusMessage::StaticClass(), 5, PS, nullptr, nullptr);

		UE_LOG(LogGameMode, Log,
			TEXT("Wipeout: final life announced for %s (next respawn %.1fs, window %.1fs)"),
			*PS->PlayerName, NextRespawnDelay, RespawnWindow);
	}
}


bool AUWipeoutGame::IsPlayerWaitingToRespawn(AUTPlayerState* PS) const
{
	if (!PS) return false;
	return PendingRespawns.Contains(PS);
}


int32 AUWipeoutGame::CountPendingRespawnsOnTeam(int32 TeamIndex) const
{
	int32 Count = 0;
	for (auto& Pair : PendingRespawns)
	{
		AUTPlayerState* PS = Pair.Key.Get();
		if (PS && PS->Team && PS->Team->TeamIndex == TeamIndex)
		{
			Count++;
		}
	}
	return Count;
}

bool AUWipeoutGame::HasImminentRespawnOnTeam(int32 TeamIndex, float WithinSeconds) const
{
	float Now = GetWorld()->GetTimeSeconds();
	for (auto& Pair : PendingRespawns)
	{
		AUTPlayerState* PS = Pair.Key.Get();
		if (PS && PS->Team && PS->Team->TeamIndex == TeamIndex)
		{
			float TimeRemaining = (Pair.Value.RespawnStartTime + Pair.Value.TotalRespawnTime) - Now;
			if (TimeRemaining <= WithinSeconds)
			{
				return true;
			}
		}
	}
	return false;
}


void AUWipeoutGame::StartRespawnTimer(AUTPlayerState* DeadPS)
{
	if (!DeadPS || !DeadPS->Team) return;

	const int32 TeamIndex = DeadPS->Team->TeamIndex;

	// Compute delay BEFORE incrementing so first death uses index 0 (4s),
	// second death uses index 1 (7s), etc.
	float RespawnDelay = ComputeRespawnDelay(TeamIndex, DeadPS);

	// Now increment counters
	if (TeamIndex == 0) { Team0DeathCount++; }
	else                { Team1DeathCount++; }

	// Track per-player deaths too
	int32& PlayerDeaths = PlayerDeathCounts.FindOrAdd(DeadPS);
	PlayerDeaths++;

	// A shared counter can make every surviving teammate's next death final at
	// this instant, so do not wait for the next one-second round-clock tick.
	CheckFinalLifeAnnouncements(TeamIndex);

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: %s died (Team %d, death #%d). Respawn in %.1fs"),
		*DeadPS->PlayerName, TeamIndex,
		(TeamIndex == 0) ? Team0DeathCount : Team1DeathCount,
		RespawnDelay);

	// Create the pending respawn entry
	FPendingRespawn& Pending = PendingRespawns.FindOrAdd(DeadPS);
	Pending.TotalRespawnTime = RespawnDelay;
	Pending.RespawnStartTime = GetWorld()->GetTimeSeconds();
	Pending.DeathIndex = (TeamIndex == 0) ? Team0DeathCount : Team1DeathCount;

	// Clear any existing timer for this player (safety)
	if (Pending.RespawnTimerHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(Pending.RespawnTimerHandle);
	}

	// Set the respawn timer
	FTimerDelegate TimerDelegate;
	TimerDelegate.BindUFunction(this, FName("OnRespawnTimerFired"), DeadPS);
	GetWorldTimerManager().SetTimer(Pending.RespawnTimerHandle, TimerDelegate, RespawnDelay, false);

	// Update replicated PlayerState fields so HUD/Scoreboard can display
	DeadPS->RespawnWaitTime = RespawnDelay;
	DeadPS->RespawnTime = RespawnDelay;
	DeadPS->ForceNetUpdate();

	// Notify Blueprint
	BP_OnPlayerWaitingRespawn(DeadPS, RespawnDelay,
		(TeamIndex == 0) ? Team0DeathCount : Team1DeathCount);

	// Force spectate during respawn wait
	// Small delay to let death cam play
	FTimerHandle SpectateDelayHandle;
	FTimerDelegate SpectateDelegate;
	SpectateDelegate.BindLambda([this, DeadPS]()
	{
		if (DeadPS && !DeadPS->IsPendingKill() && bRoundInProgress && IsPlayerWaitingToRespawn(DeadPS))
		{
			ForceTeamSpectate(DeadPS);
		}
	});
	GetWorldTimerManager().SetTimer(SpectateDelayHandle, SpectateDelegate, SpectateDelay, false);
}


void AUWipeoutGame::OnRespawnTimerFired(AUTPlayerState* PS)
{
	if (!PS || PS->IsPendingKill()) return;

	// Safety: round might have ended during our wait
	if (!bRoundInProgress)
	{
		PendingRespawns.Remove(PS);
		return;
	}

	// Sudden death — round timer expired, no more respawns allowed
	if (bInSuddenDeath)
	{
		UE_LOG(LogGameMode, Log, TEXT("Wipeout: Respawn blocked for %s — sudden death active"), *PS->PlayerName);
		PendingRespawns.Remove(PS);
		PS->RespawnTime = 0.f;
		PS->ForceNetUpdate();
		ForceTeamSpectate(PS);
		return;
	}

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: Respawn timer fired for %s"), *PS->PlayerName);

	// Remove from pending list
	PendingRespawns.Remove(PS);

	// Clear respawn state so HUD shows alive
	PS->bOutOfLives = false;
	PS->RespawnTime = 0.f;
	PS->RespawnWaitTime = 0.f;
	PS->ForceNetUpdate();

	// Get the controller
	AController* C = Cast<AController>(PS->GetOwner());
	if (!C)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: No controller for %s, cannot respawn"), *PS->PlayerName);
		return;
	}

	// Transition from spectating to playing
	if (AUTPlayerController* PC = Cast<AUTPlayerController>(C))
	{
		PC->ChangeState(NAME_Playing);
		PC->ClientGotoState(NAME_Playing);
	}

	// Actually respawn them (mid-round)
	bAllowPlayerRespawns = true;
	RestartPlayer(C);
	bAllowPlayerRespawns = false;

	if (C->GetPawn())
	{
		// Apply spawn protection
		if (RespawnProtectionTime > 0.f)
		{
			SpawnProtectedUntil.FindOrAdd(PS) = GetWorld()->GetTimeSeconds() + RespawnProtectionTime;
		}

		// Notify Blueprint
		BP_OnPlayerRespawnedMidRound(PS);
		CheckFinalLifeAnnouncements();

		UE_LOG(LogGameMode, Log, TEXT("Wipeout: %s respawned successfully"), *PS->PlayerName);
	}
	else
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Failed to spawn pawn for %s"), *PS->PlayerName);
	}
}


void AUWipeoutGame::CancelAllPendingRespawns()
{
	for (auto& Pair : PendingRespawns)
	{
		if (Pair.Value.RespawnTimerHandle.IsValid())
		{
			GetWorldTimerManager().ClearTimer(Pair.Value.RespawnTimerHandle);
		}
	}
	PendingRespawns.Empty();

	if (RespawnCountdownTickHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(RespawnCountdownTickHandle);
	}

	// Clear any in-flight bad-spawn watchdogs too (round end / cleanup).
	CancelAllSpawnRemediations();
}


void AUWipeoutGame::CancelPendingRespawn(AUTPlayerState* PS)
{
	if (!PS) return;

	FPendingRespawn* Pending = PendingRespawns.Find(PS);
	if (Pending)
	{
		if (Pending->RespawnTimerHandle.IsValid())
		{
			GetWorldTimerManager().ClearTimer(Pending->RespawnTimerHandle);
		}
		PendingRespawns.Remove(PS);
	}
}


void AUWipeoutGame::TickRespawnCountdowns()
{
	if (!bRoundInProgress) return;

	for (auto& Pair : PendingRespawns)
	{
		AUTPlayerState* PS = Pair.Key.Get();
		if (!PS) continue;

		float Remaining = GetPlayerRespawnTimeRemaining(PS);
		int32 SecondsRemaining = FMath::CeilToInt(Remaining);

		// Update replicated PlayerState so HUD shows countdown
		PS->RespawnTime = Remaining;

		if (SecondsRemaining > 0)
		{
			BP_OnPlayerRespawnCountdown(PS, SecondsRemaining);
		}
	}
}


// ============================================================================
// SCORE KILL — The key Wipeout change
// In elimination: death = permanent. In Wipeout: death = respawn timer.
// ============================================================================

void AUWipeoutGame::ScoreKill_Implementation(AController* Killer, AController* Other, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType)
{
	// Call parent for individual player scoring (kills/deaths stats)
	Super::ScoreKill_Implementation(Killer, Other, KilledPawn, DamageType);

	if (GetMatchState() != MatchState::InProgress || !bRoundInProgress)
		return;

	AUTPlayerState* OtherPS = Other ? Cast<AUTPlayerState>(Other->PlayerState) : nullptr;
	if (!OtherPS) return;

	// Track the killing blow for potential replay
	if (Killer && Killer->PlayerState)
	{
		RoundWinningKillTime = GetWorld()->GetTimeSeconds();
		RoundWinningKiller = Cast<AUTPlayerState>(Killer->PlayerState);
		if (!RoundWinningKiller)
		{
			RoundWinningKiller = OtherPS;
		}
		WinningKillerPawn = Killer->GetPawn();   // focus actor for the instant replay (BroadcastKillReplay)
	}

	// Death recap (the per-life "You dealt N to X | They dealt M to you" system-chat
	// line) intentionally DISABLED — it spammed chat in respawn-wave play. The
	// SendDeathRecap() function + LifeDamageMap tracking are left in place (dormant)
	// so it can be re-enabled later if wanted.

	// In Wipeout, death doesn't mean permanently out.
	// Mark as bOutOfLives temporarily (so GetAliveCounts returns correctly)
	// but start a respawn timer to bring them back.
	if (!OtherPS->bOutOfLives)
	{
		OtherPS->bOutOfLives = true;
		OtherPS->ForceNetUpdate();
	}

	// Clear life-damage tracking for the dead player
	ClearLifeDamageFor(OtherPS);

	if (bInSuddenDeath)
	{
		// Sudden death / overtime: no respawn. Force spectate after death cam.
		FTimerHandle SuddenDeathSpecHandle;
		FTimerDelegate SpecDelegate;
		SpecDelegate.BindLambda([this, OtherPS]()
		{
			if (OtherPS && !OtherPS->IsPendingKill() && bRoundInProgress)
			{
				ForceTeamSpectate(OtherPS);
			}
		});
		GetWorldTimerManager().SetTimer(SuddenDeathSpecHandle, SpecDelegate, SpectateDelay, false);
	}
	else if (WasBadSpawnDeath(OtherPS, Killer, Other))
	{
		// Bad-spawn artifact (engine ejected them off/under the map shortly after
		// spawning): refund it — undo the death, free instant respawn, and do NOT
		// escalate the team wave timer.
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: refunding bad-spawn death for %s (no wave penalty)"),
			*OtherPS->PlayerName);
		RefundBadSpawnDeath(OtherPS);
	}
	else
	{
		// Normal: start the escalating respawn timer
		StartRespawnTimer(OtherPS);
	}

	// Check for wipeout with a brief grace period
	// (prevents false triggers from simultaneous kills)
	if (!GetWorldTimerManager().IsTimerActive(TH_RoundEndDelay))
	{
		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DeferredCheckWipeout"));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, WipeoutGracePeriod, false);
	}
}


// ============================================================================
// WIPEOUT WIN CONDITION CHECK
// A team loses when ALL players are dead simultaneously.
// Players with pending respawn timers count as dead.
// ============================================================================

void AUWipeoutGame::CheckWipeoutCondition()
{
	if (bWarmupMode) return;
	if (!bRoundInProgress || GetWorld()->GetTimeSeconds() < WinCheckHoldUntilSeconds) return;

	int32 Alive0, Alive1;
	GetAliveCounts(Alive0, Alive1);

	// Solo/practice rounds (one team empty at round start): nobody to wipe or
	// be wiped by — respawn waves keep the solo player going and the round
	// clock decides (DefaultTimer awards "TimeExpired" at expiry, or resets to
	// a real round if someone joins). Real games never have a 0 starting size.
	const bool bSoloRound = (Team0StartingSize == 0) ^ (Team1StartingSize == 0);
	if (bSoloRound && RoundEndTimeSeconds > 0.f
		&& GetWorld()->GetTimeSeconds() < RoundEndTimeSeconds)
	{
		return;
	}

	const bool Team0Wiped = (Alive0 == 0);
	const bool Team1Wiped = (Alive1 == 0);

	if (!Team0Wiped && !Team1Wiped)
	{
		return; // No wipeout
	}

	// A team is wiped if ALL players are dead AND none have pending respawn timers.
	// Wait... In Diabotical, the wipeout happens when all are dead simultaneously,
	// even if someone has a pending respawn. The respawn timer is the risk —
	// if your whole team dies before anyone respawns, you lose.
	//
	// So the check is simply: are all players on a team lacking a living pawn?
	// This is exactly what GetAliveCounts gives us.

	if (Team0Wiped && !Team1Wiped)
	{
		// Team 0 wiped out — Team 1 wins
		BP_OnTeamWipeout(0);
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Team 0 WIPED OUT! Team 1 wins the round."));

		// Cancel Team 0's pending respawns (round is over)
		CancelAllPendingRespawns();

		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), 1, FName(TEXT("Wipeout")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
	else if (Team1Wiped && !Team0Wiped)
	{
		BP_OnTeamWipeout(1);
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Team 1 WIPED OUT! Team 0 wins the round."));

		CancelAllPendingRespawns();

		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), 0, FName(TEXT("Wipeout")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
	else if (Team0Wiped && Team1Wiped)
	{
		// Both teams wiped simultaneously — draw
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: DOUBLE WIPEOUT — Round is a draw."));

		CancelAllPendingRespawns();

		FTimerDelegate TimerDelegate;
		TimerDelegate.BindUFunction(this, FName("DelayedEndRound"), (int32)INDEX_NONE, FName(TEXT("DoubleWipeout")));
		GetWorldTimerManager().SetTimer(TH_RoundEndDelay, TimerDelegate, 0.2f, false);
	}
}


void AUWipeoutGame::DeferredCheckWipeout()
{
	if (bRoundInProgress)
	{
		CheckWipeoutCondition();
	}
}


// ============================================================================
// INTERMISSION & ROUND FLOW
// ============================================================================

void AUWipeoutGame::HandleMatchIntermission()
{
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout::HandleMatchIntermission"));
	ForceLosersToViewWinners(LastRoundWinningTeamIndex);
}


void AUWipeoutGame::StartIntermission(int32 Seconds)
{
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout::StartIntermission: %d seconds"), Seconds);

	bRoundInProgress = false;
	IntermissionSecondsRemaining = FMath::Max(1, Seconds);
	RoundEndTimeSeconds = 0.f;

	// Stop overtime and cancel pending respawns
	StopOvertime();
	CancelAllPendingRespawns();

	if (AUTGameState* GS = GetGameState<AUTGameState>())
	{
		BP_OnSetIntermission(true, IntermissionSecondsRemaining);
		GS->ForceNetUpdate();
	}

	SetMatchState(FName(TEXT("RoundCooldown")));
}


void AUWipeoutGame::StartNextRound()
{
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout::StartNextRound"));

	if (bWarmupMode)
	{
		bRoundInProgress = false;
		return;
	}

	if (bAnnounceTeam)
	{
		bAnnounceTeam = false;
	}

	// Reset all Wipeout state for the new round
	RoundWinningKiller = nullptr;
	WinningKillerPawn = nullptr;
	RoundWinningKillTime = 0.0f;
	LastRoundWinningTeamIndex = INDEX_NONE;
	Team0DeathCount = 0;
	Team1DeathCount = 0;
	PlayerDeathCounts.Empty();
	FinalLifeAnnouncedPlayers.Empty();
	CancelAllPendingRespawns();
	SpawnProtectedUntil.Empty();
	LinkHealAccumulator.Empty();
	bInSuddenDeath = false;
	bSuddenDeathPending = false;
	Team0RoundDamage = 0.0f;
	Team1RoundDamage = 0.0f;
	PlayerRoundDamage.Empty();
	bTeam0LastAliveAnnounced = false;
	bTeam1LastAliveAnnounced = false;
	Team0StartingSize = 0;
	Team1StartingSize = 0;

	ResetPlayersForNewRound();
	ResetSpawnSelectionForNewRound();
	SelectSpawnLayoutForRound();

	int32 HybridTeam0Players = 0;
	int32 HybridTeam1Players = 0;
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* C = It->Get();
		AUTPlayerState* PS = C ? Cast<AUTPlayerState>(C->PlayerState) : nullptr;
		if (!PS || PS->bOnlySpectator || !PS->Team) continue;
		if (PS->Team->TeamIndex == 0) ++HybridTeam0Players;
		else if (PS->Team->TeamIndex == 1) ++HybridTeam1Players;
	}
	PrepareHybridRoundSpawnQueues(HybridTeam0Players, HybridTeam1Players);

	// Fallback if layout selection failed
	if (Team0SelectedSpawns.Num() == 0 || Team1SelectedSpawns.Num() == 0)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Layout selection failed — insufficient spawn layouts on this map"));
	}

	// Spawn all players in two passes — alternate which team spawns first
	// each round so neither side gets a permanent "spawns second sees enemies" advantage.
	bAllowPlayerRespawns = true;
	int32 PlayersSpawned = 0;

	const int32 FirstTeam  = (TotalRoundsPlayed % 2 == 0) ? 0 : 1;
	const int32 SecondTeam = 1 - FirstTeam;

	auto SpawnPassForTeam = [&](int32 TargetTeam)
	{
		for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
		{
			AController* C = It->Get();
			if (!C) continue;

			AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
			if (!PS || PS->bOnlySpectator || !PS->Team) continue;
			if (PS->Team->TeamIndex != TargetTeam) continue;

			PS->bOutOfLives = false;
			PS->ForceNetUpdate();

			if (AUTPlayerController* PC = Cast<AUTPlayerController>(C))
			{
				PC->ChangeState(NAME_Playing);
				PC->ClientGotoState(NAME_Playing);
			}

			RestartPlayer(C);
			PlayersSpawned++;

			if (PS->Team->TeamIndex == 0) { Team0StartingSize++; }
			else if (PS->Team->TeamIndex == 1) { Team1StartingSize++; }
		}
	};

	SpawnPassForTeam(FirstTeam);
	SpawnPassForTeam(SecondTeam);

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: Spawn order this round — team %d first, team %d second"),
		FirstTeam, SecondTeam);

	bAllowPlayerRespawns = false;
	ClearHybridRoundSpawnState();

	UE_LOG(LogGameMode, Warning, TEXT("Wipeout round starting: Team0=%d, Team1=%d"), Team0StartingSize, Team1StartingSize);

	// Set round timer
	if (RoundTimeSeconds > 0)
	{
		RoundEndTimeSeconds = GetWorld()->GetTimeSeconds() + RoundTimeSeconds;
	}
	else
	{
		RoundEndTimeSeconds = 0.f;
	}

	bRoundInProgress = true;

	if (AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>())
	{
		int32 Alive0, Alive1;
		GetAliveCounts(Alive0, Alive1);
		BP_OnSetRound(true, RoundTimeSeconds, LastRoundWinningTeamIndex,
			Alive0, Alive1, Team0DeathCount, Team1DeathCount);
		BP_OnSetIntermission(false, 0);
		GS->ForceNetUpdate();
	}

	// Handles custom rounds whose opening clock is already shorter than a
	// player's first possible respawn window.
	CheckFinalLifeAnnouncements();

	// Reset pickup timers at round start so Shield Belt and UDamage
	// respawn on a clean schedule each round
	ResetPickupTimers();

	BroadcastLocalized(this, UUTGameMessage::StaticClass(), 0, NULL, NULL, NULL);

	// Start the respawn countdown ticker (fires every 1s for BP_OnPlayerRespawnCountdown)
	GetWorldTimerManager().SetTimer(RespawnCountdownTickHandle, this,
		&AUWipeoutGame::TickRespawnCountdowns, 1.0f, true);

	WinCheckHoldUntilSeconds = GetWorld()->GetTimeSeconds() + 0.25f;
	GetWorldTimerManager().ClearTimer(InitialWinCheckHandle);
	GetWorldTimerManager().SetTimer(
		InitialWinCheckHandle, this,
		&AUWipeoutGame::DelayedInitialWinCheck, 0.25f, false);
}


// ============================================================================
// END ROUND
// ============================================================================

void AUWipeoutGame::EndRoundForTeam(int32 WinnerTeamIndex, FName Reason)
{
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout::EndRoundForTeam: Winner=%d, Reason=%s"),
		WinnerTeamIndex, *Reason.ToString());

	if (bWarmupMode || !bRoundInProgress) return;

	bRoundInProgress = false;
	RoundEndTimeSeconds = 0.f;
	LastRoundWinningTeamIndex = WinnerTeamIndex;
	TotalRoundsPlayed++;

	StopOvertime();
	CancelAllPendingRespawns();

	// Rating system: per-round ProcessMatch (in-memory only — flushed at match end).
	if (RatingSystem.IsValid())
	{
		AUTGameState* RGS = GetGameState<AUTGameState>();
		if (RGS)
		{
			FWipeoutRoundResult RoundResult;
			RoundResult.bIsDraw = (WinnerTeamIndex == INDEX_NONE);

			auto BuildPerf = [this, RGS](int32 TeamIdx) -> TArray<FWipeoutPlayerRoundPerf>
			{
				TArray<FWipeoutPlayerRoundPerf> Out;
				for (APlayerState* PS : RGS->PlayerArray)
				{
					AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
					if (!UTPS || UTPS->bOnlySpectator) continue;
					if (UTPS->GetTeamNum() != TeamIdx) continue;

					FWipeoutPlayerRoundPerf P;
					// Bots get synthetic key; filtered at write-back time so their
					// transient PlayerRating placeholders never persist.
					P.UniqueId = UTPS->UniqueId.IsValid()
						? UTPS->UniqueId.ToString()
						: FString::Printf(TEXT("BOT:%s"), *UTPS->PlayerName);
					P.PlayerName = UTPS->PlayerName;
					P.Kills    = UTPS->RoundKills;
					// Wipeout has mid-round respawns, so a single bOutOfLives flag
					// at round-end can under-count actual deaths. PlayerDeathCounts
					// tracks per-individual death counts when the team-shared
					// counter is off; fall back to bOutOfLives otherwise.
					const int32* DeathPtr = PlayerDeathCounts.Find(UTPS);
					P.Deaths   = DeathPtr ? *DeathPtr : (UTPS->bOutOfLives ? 1 : 0);
					P.Damage   = PlayerRoundDamage.Contains(UTPS) ? PlayerRoundDamage[UTPS] : 0.f;
					Out.Add(MoveTemp(P));
				}
				return Out;
			};

			if (RoundResult.bIsDraw)
			{
				RoundResult.WinnerTeam = BuildPerf(0);
				RoundResult.LoserTeam  = BuildPerf(1);
			}
			else
			{
				const int32 LoserIdx = (WinnerTeamIndex == 0) ? 1 : 0;
				RoundResult.WinnerTeam = BuildPerf(WinnerTeamIndex);
				RoundResult.LoserTeam  = BuildPerf(LoserIdx);
				RoundResult.WinnerTeamIndex = WinnerTeamIndex;
			}

			RatingSystem->ProcessRound(RoundResult);
		}
	}

	// Score the round
	bool bIsDraw = (WinnerTeamIndex == INDEX_NONE);
	if (!bIsDraw)
	{
		if (Teams.IsValidIndex(WinnerTeamIndex))
		{
			Teams[WinnerTeamIndex]->Score += 1;
			Teams[WinnerTeamIndex]->ForceNetUpdate();
			UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Team %d wins round! Score: %d"), WinnerTeamIndex, Teams[WinnerTeamIndex]->Score);
		}
	}

	// Check for high damage carry
	CheckForHighDamageCarry(WinnerTeamIndex);

	// Check for match end
	if (!bIsDraw && Teams[WinnerTeamIndex]->Score >= GoalScore)
	{
		bool bCanEndMatch = true;
		if (bWinByTwo)
		{
			int32 OtherTeamIndex = (WinnerTeamIndex == 0) ? 1 : 0;
			if (Teams.IsValidIndex(OtherTeamIndex))
			{
				int32 ScoreDifference = Teams[WinnerTeamIndex]->Score - Teams[OtherTeamIndex]->Score;
				bCanEndMatch = (ScoreDifference >= 2);
			}
		}

		if (bCanEndMatch)
		{
			BroadcastKillReplay();
			UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Team %d wins the match!"), WinnerTeamIndex);

			FTimerHandle UnusedHandle;
			FTimerDelegate TimerDel;
			TimerDel.BindUFunction(this, FName("DelayedEndGame"), WinnerTeamIndex, FName(TEXT("ScoreLimit")));
			GetWorldTimerManager().SetTimer(UnusedHandle, TimerDel, 1.0f, false);
			return;
		}
	}

	// Game not over — proceed to intermission
	if (AUTGameState* GS = GetGameState<AUTGameState>())
	{
		int32 Alive0, Alive1;
		GetAliveCounts(Alive0, Alive1);
		BP_OnSetRound(false, 0, LastRoundWinningTeamIndex,
			Alive0, Alive1, Team0DeathCount, Team1DeathCount);
		GS->ForceNetUpdate();
	}

	BroadcastRoundResults(WinnerTeamIndex, bIsDraw);
	CheckForDominationAndLead(WinnerTeamIndex);
	ForceLosersToViewWinners(WinnerTeamIndex);
	StartIntermission(AwardDisplayTime);
}


void AUWipeoutGame::DelayedEndRound(int32 WinnerTeamIndex, FName Reason)
{
	EndRoundForTeam(WinnerTeamIndex, Reason);
}


void AUWipeoutGame::DelayedEndGame(int32 WinnerTeamIndex, FName Reason)
{
	AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIndex);
	EndGame(BestPlayer, Reason);
}


void AUWipeoutGame::EndGame(AUTPlayerState* Winner, FName Reason)
{
	// Safety: only access replay data if demo recording is actually running.
	// Standalone PIE and servers without demo recording will crash in
	// PickMostCoolMoments if DemoNetDriver is null.
	if (GetWorld()->DemoNetDriver != nullptr)
	{
		PickMostCoolMoments();
	}

	Super::EndGame(Winner, Reason);
}


void AUWipeoutGame::DeferredHandleMatchStart()
{
	if (GetGameState<AUTGameState>())
	{
		HandleMatchHasStarted();
	}
}


// ============================================================================
// CHECK SCORE (win-by-two support)
// ============================================================================

bool AUWipeoutGame::CheckScore_Implementation(AUTPlayerState* Scorer)
{
	if (!Teams.IsValidIndex(0) || !Teams.IsValidIndex(1)) return false;

	int32 ScoreA = Teams[0]->Score;
	int32 ScoreB = Teams[1]->Score;

	if (!bWinByTwo)
	{
		return Super::CheckScore_Implementation(Scorer);
	}

	int32 LeadingScore = FMath::Max(ScoreA, ScoreB);
	int32 TrailingScore = FMath::Min(ScoreA, ScoreB);

	if (LeadingScore >= GoalScore && (LeadingScore - TrailingScore) >= 2)
	{
		int32 WinnerTeamIndex = (ScoreA > ScoreB) ? 0 : 1;
		AUTPlayerState* BestPlayer = FindBestPlayerOnTeam(WinnerTeamIndex);
		EndGame(BestPlayer, TEXT("fraglimit"));
		return true;
	}

	return false;
}


// ============================================================================
// RESTART PLAYER — Allows mid-round spawns when timer fires
// ============================================================================

void AUWipeoutGame::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer) return;

	if (APlayerController* PC = Cast<APlayerController>(NewPlayer))
	{
		if (MustSpectate(PC)) return;
	}

	// Guard: don't spawn until team is assigned. Without a team, the spawn
	// falls through to the engine's default FindPlayerStart which can trigger
	// "InWorld == NULL || InWorld == World" assertion when the client's world
	// context isn't fully set up. The engine retries RestartPlayer on
	// subsequent frames once team assignment completes.
	AUTPlayerState* PS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
	if (!PS || !PS->Team)
	{
		return;
	}

	// Idempotency guard (hoisted ABOVE the warmup branch) — same defect as ElimPlus: a
	// second RestartPlayer on a still-living warmup pawn re-runs GiveDefaultInventory, and
	// stock AddInventory dedupes only by instance (not class), so the whole arsenal is
	// granted twice = the doubled weapon bar. First spawn (no pawn) is unaffected; the
	// live/lineup paths below already relied on this guard.
	if (NewPlayer->GetPawn()) return;

	if (GetMatchState() == MatchState::WaitingToStart)
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	AUTGameState* GS = GetGameState<AUTGameState>();
	bool bLineupIsActive = (GS && GS->ActiveLineUpHelper && GS->ActiveLineUpHelper->bIsPlacingPlayers);
	if (bLineupIsActive)
	{
		Super::RestartPlayer(NewPlayer);
		return;
	}

	// Check if this player has ever spawned. If not, this is a late joiner
	// who needs to be let into the game (or forced to spectate until next round).
	// (PS already obtained above for the team guard)
	const bool bIsLateJoiner = (PS && PS->Deaths == 0 && PS->Kills == 0 && !PS->bOutOfLives);

	// Detect reconnecting players: they have stats (played before) but no pawn
	// (crashed/disconnected). Allow them to respawn mid-round unless sudden death.
	const bool bIsReconnect = (PS && !NewPlayer->GetPawn() && bRoundInProgress
		&& !bInSuddenDeath && (PS->Deaths > 0 || PS->Kills > 0));

	// Allow spawn during: warmup, waiting to start, explicit respawn window
	// (wave respawn), late joiners, or reconnecting players.
	const bool bShouldAllowSpawn = (bAllowPlayerRespawns || bWarmupMode
		|| GetMatchState() == MatchState::WaitingToStart
		|| bIsLateJoiner || bIsReconnect);

	if (!bShouldAllowSpawn)
	{
		// Refused mid-round (chiefly a reconnect during sudden death, which
		// bIsReconnect deliberately excludes): the controller is left pawn-less in
		// NAME_Spectating with no view target, which IS the free-fly camera.
		if (AUTPlayerController* PC = Cast<AUTPlayerController>(NewPlayer))
		{
			if (bRoundInProgress && NCPlusRoundSpectate::ShouldLock(PC, PS))
			{
				NCPlusRoundSpectate::Lock(PC, FindAliveTeammate(PS));
			}
		}
	}

	if (bShouldAllowSpawn)
	{
		AActor* ChosenStart = nullptr;
		const int32 TeamIndex = (PS && PS->Team) ? PS->Team->TeamIndex : INDEX_NONE;
		FTransform HybridTransform;
		bool bUsedHybridTransform = false;

		if (bHybridRoundSpawnWindow && (TeamIndex == 0 || TeamIndex == 1))
		{
			bUsedHybridTransform = TryConsumeHybridSpawnTransform(TeamIndex, HybridTransform);
			if (bUsedHybridTransform)
			{
				const TArray<APlayerStart*>& PreferredStarts =
					(TeamIndex == 0) ? Team0SelectedSpawns : Team1SelectedSpawns;
				ChosenStart = FNCHybridSpawnGenerator::FindNearestPlayerStart(
					PreferredStarts, HybridTransform.GetLocation());
				if (!ChosenStart)
				{
					ChosenStart = FNCHybridSpawnGenerator::FindNearestPlayerStart(
						AllSpawnPointsList, HybridTransform.GetLocation());
				}
			}
			else
			{
				UE_LOG(LogGameMode, Warning,
					TEXT("Wipeout hybrid opening queue exhausted for %s (team %d); using PlayerStart fallback"),
					PS ? *PS->PlayerName : TEXT("Unknown"), TeamIndex);
			}
		}

		// Determine if this is a round-start spawn or mid-round respawn
		if (!ChosenStart && bRoundInProgress && Team0StartingSize > 0)
		{
			// Mid-round respawn — use dynamic spawn selection away from enemies
			ChosenStart = ChooseMidRoundSpawn(NewPlayer);
		}

		if (!ChosenStart)
		{
			// Round-start or fallback — use precomputed layout
			ChosenStart = ChoosePlayerStart_Implementation(NewPlayer);
		}

		if (!ChosenStart)
		{
			bUsedHybridTransform = false;
		}
		bHasPendingHybridSpawnTransform = bUsedHybridTransform && (ChosenStart != nullptr);
		if (bHasPendingHybridSpawnTransform)
		{
			PendingHybridSpawnTransform = HybridTransform;
		}
		// The watchdog anchor must be an independently safe authored start. Using
		// the hybrid transform here made a transform on an under-map surface its
		// own recovery destination, so the watchdog could never rescue it.
		const FVector IntendedSpawnLocation = ChosenStart
			? ChosenStart->GetActorLocation()
			: FVector::ZeroVector;

		OverriddenPlayerStart = ChosenStart;
		Super::RestartPlayer(NewPlayer);
		OverriddenPlayerStart = nullptr;
		bHasPendingHybridSpawnTransform = false;

		if (bUsedHybridTransform && NewPlayer->GetPawn())
		{
			FRotator SpawnRotation = HybridTransform.Rotator();
			SpawnRotation.Pitch = 0.f;
			SpawnRotation.Roll = 0.f;
			NewPlayer->SetControlRotation(SpawnRotation);
			NewPlayer->ClientSetRotation(SpawnRotation, true);
		}

		// Bad-spawn remediation: the start is fine, but engine collision handling
		// can eject the fresh pawn off/under the map. Watch it briefly and snap
		// it back to the intended start if that happens.
		if (NewPlayer->GetPawn() && (bUsedHybridTransform || ChosenStart))
		{
			ArmSpawnRemediation(NewPlayer->GetPawn(), IntendedSpawnLocation);
		}

		// Ping-compensated spawn: hide pawn until client confirms control.
		// Skip bots (no remote client to confirm) — they'd timeout after 500ms.
		ATeamArenaCharacter* SpawnedChar = NewPlayer->GetPawn() ? Cast<ATeamArenaCharacter>(NewPlayer->GetPawn()) : nullptr;
		if (bEnablePingCompensatedSpawn && SpawnedChar && NewPlayer->GetPawn()->GetRemoteRole() == ROLE_AutonomousProxy)
		{
			SpawnedChar->BeginPingCompensatedSpawnHide();   // ping-floored: skips low-ping spawners
		}

		// Force switch to best weapon on spawn — bypasses the player's
		// bAutoWeaponSwitch preference so they don't spawn with Enforcer/Hammer.
		// Super::RestartPlayer already calls ClientSwitchToBestWeapon for remote
		// players, but it can fire before all weapons have replicated. Defer to
		// next tick to ensure full inventory is available.
		AUTPlayerController* SpawnPC = Cast<AUTPlayerController>(NewPlayer);
		if (SpawnPC && NewPlayer->GetPawn())
		{
			TWeakObjectPtr<AUTPlayerController> WeakPC = SpawnPC;
			GetWorldTimerManager().SetTimerForNextTick([WeakPC]()
			{
				if (WeakPC.IsValid())
				{
					WeakPC->ClientSwitchToBestWeapon();
				}
			});
		}

		// Grant ability charge on spawn (piggybacks on Epic's boost system)
		if (SpawnAbilityClass && NewPlayer->GetPawn())
		{
			AUTPlayerState* SpawnPS = Cast<AUTPlayerState>(NewPlayer->PlayerState);
			if (SpawnPS)
			{
				SpawnPS->BoostClass = SpawnAbilityClass;
				SpawnPS->SetRemainingBoosts(1);
			}
		}

		if (!NewPlayer->GetPawn())
		{
			// Throttle: RestartPlayer retries every frame on a map with no valid spawn, so this can flood
			// the log (thousands of lines). Rate-limit to once per 5s (monotonic wall-clock so it survives
			// map changes, unlike GetWorld()->GetTimeSeconds()).
			static double LastSpawnFailWarnTime = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - LastSpawnFailWarnTime >= 5.0)
			{
				LastSpawnFailWarnTime = Now;
				UE_LOG(LogGameMode, Warning, TEXT("Wipeout::RestartPlayer: FAILED to spawn pawn for %s (throttled 5s)"),
					NewPlayer->PlayerState ? *NewPlayer->PlayerState->PlayerName : TEXT("Unknown"));
			}
		}
	}
}


// ============================================================================
// BAD-SPAWN REMEDIATION
// Spawn points are fine; the engine's collision handling (AlwaysSpawn + the
// ping-comp collision toggle) occasionally depenetrates a fresh pawn off the
// map or under it. Watch each spawn briefly and snap it back to its intended
// start; if it still dies to the world/fall within a short grace, refund the
// death (no wave escalation). Server-only.
// ============================================================================

void AUWipeoutGame::ArmSpawnRemediation(APawn* Pawn, const FVector& IntendedLoc)
{
	if (!Pawn || !HasAuthority()) return;

	const float Now = GetWorld()->GetTimeSeconds();

	if (AUTPlayerState* PS = Cast<AUTPlayerState>(Pawn->PlayerState))
	{
		LastSpawnWorldTime.Add(PS, Now);   // for the death-refund window
	}

	FSpawnRemediation& R = SpawnRemediations.FindOrAdd(Pawn);
	R.Anchor     = IntendedLoc;
	R.SpawnTime  = Now;
	R.ChecksLeft = FMath::Max(1, FMath::CeilToInt(BadSpawnWatchWindow / FMath::Max(0.03f, BadSpawnCheckInterval)));

	if (R.CheckHandle.IsValid())
	{
		GetWorldTimerManager().ClearTimer(R.CheckHandle);
	}
	const TWeakObjectPtr<APawn> WeakPawn(Pawn);
	GetWorldTimerManager().SetTimer(R.CheckHandle,
		FTimerDelegate::CreateLambda([this, WeakPawn]() { CheckSpawnRemediation(WeakPawn); }),
		FMath::Max(0.03f, BadSpawnCheckInterval), true);
}

void AUWipeoutGame::CheckSpawnRemediation(TWeakObjectPtr<APawn> WeakPawn)
{
	FSpawnRemediation* R = SpawnRemediations.Find(WeakPawn);
	if (!R) return;

	APawn* Pawn = WeakPawn.Get();   // null if the pawn was destroyed mid-watch
	AUTCharacter* Char = Pawn ? Cast<AUTCharacter>(Pawn) : nullptr;
	bool bStop = (Pawn == nullptr) || Pawn->IsPendingKill() || (Char && Char->IsDead());

	if (!bStop && IsSpawnEjected(Pawn, R->Anchor))
	{
		float HalfHeight = 0.f;
		if (Char && Char->GetCapsuleComponent())
		{
			HalfHeight = Char->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		}
		const FVector Target = R->Anchor + FVector(0.f, 0.f, HalfHeight + 12.f);
		// 4.15: ETeleportType has only None / TeleportPhysics (no ResetPhysics).
		// TeleportPhysics hard-moves the body without sweeping; velocity is zeroed below.
		Pawn->SetActorLocation(Target, false, nullptr, ETeleportType::TeleportPhysics);
		if (Char && Char->GetCharacterMovement())
		{
			Char->GetCharacterMovement()->StopMovementImmediately();
			Char->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		}
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: bad spawn remediated — snapped %s back to %s"),
			Pawn->PlayerState ? *Pawn->PlayerState->PlayerName : TEXT("?"), *R->Anchor.ToString());
		bStop = true;   // one snap-back is enough
	}

	if (bStop || --R->ChecksLeft <= 0)
	{
		GetWorldTimerManager().ClearTimer(R->CheckHandle);
		SpawnRemediations.Remove(WeakPawn);
	}
}

bool AUWipeoutGame::IsSpawnEjected(APawn* Pawn, const FVector& Anchor) const
{
	if (!Pawn) return false;
	const FVector Loc = Pawn->GetActorLocation();

	// Under the world.
	if (AWorldSettings* WS = GetWorld()->GetWorldSettings())
	{
		if (Loc.Z < WS->KillZ + BadSpawnKillZMargin)
		{
			return true;
		}
	}

	// Dropped well below the intended start — went through the floor.
	if (Loc.Z < Anchor.Z - BadSpawnMaxDropBelowStart)
	{
		return true;
	}

	// Falling with no walkable ground within reach — ejected over the void.
	const AUTCharacter* Char = Cast<AUTCharacter>(Pawn);
	if (Char && Char->GetCharacterMovement() && Char->GetCharacterMovement()->IsFalling())
	{
		FHitResult Hit;
		const FVector End = Loc - FVector(0.f, 0.f, BadSpawnGroundTrace);
		FCollisionQueryParams Q(FName("WipeoutBadSpawnGround"), false, Pawn);
		if (!GetWorld()->LineTraceSingleByChannel(Hit, Loc, End, ECC_Visibility, Q))
		{
			return true;
		}
	}
	return false;
}

bool AUWipeoutGame::WasBadSpawnDeath(AUTPlayerState* PS, AController* Killer, AController* Other) const
{
	if (!PS) return false;
	const float* SpawnT = LastSpawnWorldTime.Find(PS);
	if (!SpawnT) return false;
	if (GetWorld()->GetTimeSeconds() - *SpawnT > BadSpawnRefundGrace) return false;
	// Only self / world deaths — never refund a real enemy kill that happened to
	// land within the grace window.
	return (Killer == nullptr || Killer == Other);
}

void AUWipeoutGame::RefundBadSpawnDeath(AUTPlayerState* PS)
{
	if (!PS) return;

	// Undo the death Super::ScoreKill just counted; clear respawn state. No
	// StartRespawnTimer -> no team death-count increment, no wave escalation.
	PS->Deaths = FMath::Max(0, PS->Deaths - 1);
	PS->bOutOfLives = false;
	PS->RespawnTime = 0.f;
	PS->RespawnWaitTime = 0.f;
	PS->ForceNetUpdate();

	// Deferred instant respawn — don't restart mid-death-processing.
	TWeakObjectPtr<AUTPlayerState> WeakPS = PS;
	GetWorldTimerManager().SetTimerForNextTick([this, WeakPS]()
	{
		if (!WeakPS.IsValid() || !bRoundInProgress || bInSuddenDeath) return;
		AController* C = Cast<AController>(WeakPS->GetOwner());
		if (C && !C->GetPawn())
		{
			RestartPlayer(C);
		}
	});
}

void AUWipeoutGame::CancelAllSpawnRemediations()
{
	for (TPair<TWeakObjectPtr<APawn>, FSpawnRemediation>& Pair : SpawnRemediations)
	{
		if (Pair.Value.CheckHandle.IsValid())
		{
			GetWorldTimerManager().ClearTimer(Pair.Value.CheckHandle);
		}
	}
	SpawnRemediations.Empty();
}


// ============================================================================
// MID-ROUND SPAWN SELECTION
// Picks the spawn point furthest from all living enemies.
// ============================================================================

AActor* AUWipeoutGame::ChooseMidRoundSpawn(AController* Player)
{
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || !PS->Team) return nullptr;

	const int32 MyTeam = PS->Team->TeamIndex;

	// Gather all living enemy positions
	TArray<FVector> EnemyPositions;
	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		APawn* Pawn = It->Get();
		if (!Pawn) continue;

		AUTCharacter* UTC = Cast<AUTCharacter>(Pawn);
		if (UTC && UTC->IsDead()) continue;

		AUTPlayerState* OtherPS = Cast<AUTPlayerState>(Pawn->PlayerState);
		if (OtherPS && OtherPS->Team && OtherPS->Team->TeamIndex != MyTeam)
		{
			EnemyPositions.Add(Pawn->GetActorLocation());
		}
	}

	// If no living enemies, just use normal spawn
	if (EnemyPositions.Num() == 0)
	{
		return ChoosePlayerStart_Implementation(Player);
	}

	// Score all spawn points by minimum distance to any enemy
	APlayerStart* BestSpawn = nullptr;
	float BestMinDist = -1.0f;

	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		APlayerStart* Spawn = *It;
		if (!Spawn) continue;

		FVector SpawnLoc = Spawn->GetActorLocation();
		float MinDist = FLT_MAX;

		for (const FVector& EnemyLoc : EnemyPositions)
		{
			float Dist = (SpawnLoc - EnemyLoc).Size2D();
			MinDist = FMath::Min(MinDist, Dist);
		}

		// Reject spawns too close to enemies
		if (MinDist < MidRoundMinEnemyDistance)
			continue;

		// Add slight randomness to prevent always picking the exact same spot
		float Score = MinDist + FMath::FRandRange(0.f, 500.f);

		if (Score > BestMinDist)
		{
			BestMinDist = Score;
			BestSpawn = Spawn;
		}
	}

	if (BestSpawn)
	{
		UE_LOG(LogGameMode, Log, TEXT("Wipeout: Mid-round spawn for %s at %s (dist from enemy: %.0f)"),
			*PS->PlayerName, *BestSpawn->GetName(), BestMinDist);
		return BestSpawn;
	}

	// Fallback to round-start spawn
	return ChoosePlayerStart_Implementation(Player);
}


// ============================================================================
// CHOOSE PLAYER START (round-start — uses precomputed layouts)
// ============================================================================

AActor* AUWipeoutGame::ChoosePlayerStart_Implementation(AController* Player)
{
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || !PS->Team) return Super::ChoosePlayerStart_Implementation(Player);

	const int32 TeamIndex = PS->Team->TeamIndex;

	// Three-tier algorithm with HARD enemy-distance floor:
	//   Tier 1a: curated 4 spawns, must be >= MinimumEnemySpawnDistance from enemies
	//   Tier 1b: any spawn on the map, same hard floor (if 1a fails)
	//   Tier 2:  teammate-stack — pick the spawn closest to a teammate (last resort,
	//            ignores enemy distance; safety over solitude)
	//
	// Per-candidate scoring (Tier 1a/1b): cluster teammates, reward enemy distance.
	// Bonus terms guard against FLT_MAX dominance when no teammates/enemies exist.

	const float EnemyBonusCap = 5000.f;
	auto ScoreCandidate = [&](float MinTeammateDist, int32 NearbyCount, float MinEnemyDist) -> float
	{
		const bool bAnyTeammate = (MinTeammateDist < FLT_MAX);
		const bool bAnyEnemy    = (MinEnemyDist    < FLT_MAX);
		const float TeammateTerm = bAnyTeammate ? -MinTeammateDist : 0.f;
		const float EnemyBonus   = bAnyEnemy ? FMath::Min(MinEnemyDist, EnemyBonusCap) * 0.5f : 0.f;
		return NearbyCount * 5000.f + TeammateTerm + EnemyBonus + FMath::FRandRange(0.f, 300.f);
	};

	// Helper: scan all pawns relative to a candidate spawn.
	auto ScanCandidate = [&](APlayerStart* Spawn, float& OutMinEnemy, float& OutMinTeammate, int32& OutNearby, bool& OutOccupied)
	{
		OutMinEnemy = FLT_MAX;
		OutMinTeammate = FLT_MAX;
		OutNearby = 0;
		OutOccupied = false;
		if (!Spawn) return;

		const FVector SpawnLoc = Spawn->GetActorLocation();
		for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
		{
			APawn* Pawn = It->Get();
			if (!Pawn || !Pawn->PlayerState) continue;
			AUTPlayerState* OtherPS = Cast<AUTPlayerState>(Pawn->PlayerState);
			if (!OtherPS || OtherPS == PS || !OtherPS->Team) continue;

			const float Dist = (Pawn->GetActorLocation() - SpawnLoc).Size2D();
			if (OtherPS->Team->TeamIndex == TeamIndex)
			{
				if (Dist < 100.f) { OutOccupied = true; return; }
				OutMinTeammate = FMath::Min(OutMinTeammate, Dist);
				if (Dist < 800.f) OutNearby++;
			}
			else
			{
				OutMinEnemy = FMath::Min(OutMinEnemy, Dist);
			}
		}
	};

	APlayerStart* BestSpawn = nullptr;
	float BestScore = -FLT_MAX;

	// Tier 1a: curated spawns for this team, with hard enemy floor.
	TArray<APlayerStart*> MySpawns;
	if (TeamIndex == 0 && Team0SelectedSpawns.Num() > 0)      MySpawns = Team0SelectedSpawns;
	else if (TeamIndex == 1 && Team1SelectedSpawns.Num() > 0) MySpawns = Team1SelectedSpawns;

	for (APlayerStart* Spawn : MySpawns)
	{
		float MinEnemy, MinTeammate; int32 Nearby; bool bOccupied;
		ScanCandidate(Spawn, MinEnemy, MinTeammate, Nearby, bOccupied);
		if (bOccupied) continue;
		if (MinEnemy < MinimumEnemySpawnDistance) continue;   // hard floor

		const float Score = ScoreCandidate(MinTeammate, Nearby, MinEnemy);
		if (Score > BestScore) { BestScore = Score; BestSpawn = Spawn; }
	}

	// Tier 1b: full map with hard enemy floor (curated couldn't satisfy 4500u).
	if (!BestSpawn && AllSpawnPointsList.Num() > 0)
	{
		for (APlayerStart* Spawn : AllSpawnPointsList)
		{
			float MinEnemy, MinTeammate; int32 Nearby; bool bOccupied;
			ScanCandidate(Spawn, MinEnemy, MinTeammate, Nearby, bOccupied);
			if (bOccupied) continue;
			if (MinEnemy < MinimumEnemySpawnDistance) continue;

			const float Score = ScoreCandidate(MinTeammate, Nearby, MinEnemy);
			if (Score > BestScore) { BestScore = Score; BestSpawn = Spawn; }
		}
		if (BestSpawn)
		{
			// Throttle: fires per spawn attempt and can flood the log on a bad map. Rate-limit to once per 5s
			// (monotonic wall-clock so it survives map changes). The fallback spawn itself still happens.
			static double LastSpawnFallbackWarnTime = 0.0;
			const double Now = FPlatformTime::Seconds();
			if (Now - LastSpawnFallbackWarnTime >= 5.0)
			{
				LastSpawnFallbackWarnTime = Now;
				UE_LOG(LogGameMode, Warning,
					TEXT("Wipeout: %s curated spawns failed %.0fu floor — using full-map spawn (passed floor) (throttled 5s)"),
					*PS->PlayerName, MinimumEnemySpawnDistance);
			}
		}
	}

	// Tier 2: TEAMMATE-STACK fallback. No spawn on the entire map passed the
	// hard enemy floor. Spawn ON a teammate (closest spawn to any living
	// teammate). Better to die alongside a friend than alone in enemy territory.
	// This intentionally bypasses the 100u "occupied" check.
	if (!BestSpawn && AllSpawnPointsList.Num() > 0)
	{
		// Gather living teammates
		TArray<FVector> TeammateLocs;
		for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
		{
			APawn* Pawn = It->Get();
			if (!Pawn || !Pawn->PlayerState) continue;
			AUTPlayerState* OtherPS = Cast<AUTPlayerState>(Pawn->PlayerState);
			if (!OtherPS || OtherPS == PS || !OtherPS->Team) continue;
			if (OtherPS->Team->TeamIndex == TeamIndex)
			{
				TeammateLocs.Add(Pawn->GetActorLocation());
			}
		}

		if (TeammateLocs.Num() > 0)
		{
			float BestTeammateDist = FLT_MAX;
			for (APlayerStart* Spawn : AllSpawnPointsList)
			{
				if (!Spawn) continue;
				const FVector SpawnLoc = Spawn->GetActorLocation();
				float MinDist = FLT_MAX;
				for (const FVector& Loc : TeammateLocs)
				{
					MinDist = FMath::Min(MinDist, (Loc - SpawnLoc).Size2D());
				}
				if (MinDist < BestTeammateDist)
				{
					BestTeammateDist = MinDist;
					BestSpawn = Spawn;
				}
			}
			if (BestSpawn)
			{
				// Throttle (same reason as the other spawn warnings): per-attempt, floods on a bad map.
				static double LastTeammateStackWarnTime = 0.0;
				const double Now = FPlatformTime::Seconds();
				if (Now - LastTeammateStackWarnTime >= 5.0)
				{
					LastTeammateStackWarnTime = Now;
					UE_LOG(LogGameMode, Warning,
						TEXT("Wipeout: %s — no spawn passes %.0fu floor; teammate-stacking at %.0fu from teammate (throttled 5s)"),
						*PS->PlayerName, MinimumEnemySpawnDistance, BestTeammateDist);
				}
			}
		}
	}

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: %s (team %d) assigned spawn at %s (curated pool %d)"),
		*PS->PlayerName, TeamIndex,
		BestSpawn ? *BestSpawn->GetActorLocation().ToString() : TEXT("NONE"),
		MySpawns.Num());

	return BestSpawn ? BestSpawn : Super::ChoosePlayerStart_Implementation(Player);
}


AActor* AUWipeoutGame::FindPlayerStart_Implementation(AController* Player, const FString& IncomingName)
{
	if (OverriddenPlayerStart)
	{
		return OverriddenPlayerStart;
	}
	return Super::FindPlayerStart_Implementation(Player, IncomingName);
}


// ============================================================================
// SPAWN PAWN — tiered: adjust-or-fail → shoulder offsets → AlwaysSpawn fallback
// ============================================================================

APawn* AUWipeoutGame::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
	if (!StartSpot && !bHasPendingHybridSpawnTransform)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout::SpawnDefaultPawnFor: no PlayerStart or hybrid transform"));
		return nullptr;
	}

	const bool bUsingHybridTransform = bHasPendingHybridSpawnTransform;
	FRotator StartRotation(ForceInit);
	StartRotation.Yaw = bUsingHybridTransform
		? PendingHybridSpawnTransform.Rotator().Yaw
		: StartSpot->GetActorRotation().Yaw;
	FVector StartLocation = bUsingHybridTransform
		? PendingHybridSpawnTransform.GetLocation()
		: StartSpot->GetActorLocation();

	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	if (!PawnClass)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout::SpawnDefaultPawnFor: No PawnClass for %s"), *GetNameSafe(NewPlayer));
		return nullptr;
	}

	// Tiered spawn, mirroring ElimPlus and the engine adjust-first default the
	// original Absolute mode relied on. The old single raw AlwaysSpawn at the
	// transform was why residual bad hybrid candidates were EXPOSED verbatim in
	// Wipeout — pawns planted inside geometry their own capsule collides with —
	// while ElimPlus's pattern masked the same class. Every path still ends in an
	// AlwaysSpawn, so round-start stack spawns can never fail outright.
	// --- ATTEMPT 1: adjust-or-fail at the exact transform ---
	FActorSpawnParameters SpawnInfo;
	SpawnInfo.Instigator = Instigator;
	SpawnInfo.ObjectFlags |= RF_Transient;
	SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButDontSpawnIfColliding;
	APawn* ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, FTransform(StartRotation, StartLocation), SpawnInfo);

	// --- ATTEMPT 2: cardinal shoulder-width offsets (just outside a 40uu capsule) ---
	if (!ResultPawn)
	{
		const FVector Offsets[] = {
			FVector(45.f,   0.f, 0.f),
			FVector(-45.f,  0.f, 0.f),
			FVector(0.f,   45.f, 0.f),
			FVector(0.f,  -45.f, 0.f),
		};
		for (const FVector& Offset : Offsets)
		{
			ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, FTransform(StartRotation, StartLocation + Offset), SpawnInfo);
			if (ResultPawn)
			{
				UE_LOG(LogGameMode, Log, TEXT("Wipeout::SpawnDefaultPawnFor: Used offset spawn at %s"), *GetNameSafe(StartSpot));
				break;
			}
		}
	}

	// --- ATTEMPT 3: force spawn. A hybrid transform that failed every
	// collision-aware attempt must not be its own recovery destination — force
	// at the AUTHORED start instead; micro-jitter prevents two force-spawned
	// pawns sharing an exact origin (physics explosion). ---
	if (!ResultPawn)
	{
		if (bUsingHybridTransform && StartSpot)
		{
			StartLocation = StartSpot->GetActorLocation();
			StartRotation.Yaw = StartSpot->GetActorRotation().Yaw;
			UE_LOG(LogGameMode, Warning,
				TEXT("Wipeout::SpawnDefaultPawnFor: Hybrid transform blocked; falling back to authored PlayerStart %s"),
				*GetNameSafe(StartSpot));
		}
		SpawnInfo.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		const FVector JitteredLocation = StartLocation + FVector(FMath::RandRange(-10.f, 10.f), FMath::RandRange(-10.f, 10.f), 0.f);
		ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, FTransform(StartRotation, JitteredLocation), SpawnInfo);
	}

	if (!ResultPawn)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout::SpawnDefaultPawnFor: Failed at %s"), *StartLocation.ToString());
	}

	return ResultPawn;
}


// ============================================================================
// MODIFY DAMAGE — Spawn protection for mid-round respawns + intermission invuln
// ============================================================================

// ---------------------------------------------------------------------------
// CheckRelevance — Strip pickups not appropriate for Wipeout:
//   - Remove Redeemer weapon base (and its weapon)
//   - Remove ALL health pickups and vials
//   - Remove armor EXCEPT ShieldBelt
//   - Remove powerups EXCEPT UDamage/Amp
//   - Keep all other weapon bases (ammo refill encourages movement)
// ---------------------------------------------------------------------------
bool AUWipeoutGame::CheckRelevance_Implementation(AActor* Other)
{
	if (!Other)
	{
		return Super::CheckRelevance_Implementation(Other);
	}

	// --- Weapon bases: only remove Redeemer ---
	AUTPickupWeapon* WeaponPickup = Cast<AUTPickupWeapon>(Other);
	if (WeaponPickup)
	{
		if (WeaponPickup->WeaponType)
		{
			FString WeaponName = WeaponPickup->WeaponType->GetName();
			if (WeaponName.Contains(TEXT("Redeemer")))
			{
				return false;
			}
		}
		return Super::CheckRelevance_Implementation(Other);
	}

	// --- Health pickups: remove all EXCEPT CandyPlaceholder (custom pickup) ---
	if (Other->IsA(AUTPickupHealth::StaticClass()))
	{
		// Whitelist CandyPlaceholder — BP subclass of UTPickupHealth used for custom mechanics
		FString ClassName = Other->GetClass()->GetName();
		if (!ClassName.Contains(TEXT("Candy")))
		{
			return false;
		}
	}

	// --- Standalone ammo pickups: remove (weapon bases handle ammo refill) ---
	if (Other->IsA(AUTPickupAmmo::StaticClass()))
	{
		return false;
	}

	// --- Inventory pickups (armor + powerups): selective removal ---
	AUTPickupInventory* InvPickup = Cast<AUTPickupInventory>(Other);
	if (InvPickup && InvPickup->GetInventoryType())
	{
		FString InvName = InvPickup->GetInventoryType()->GetName();

		// Keep: ShieldBelt
		if (InvName.Contains(TEXT("ShieldBelt")))
		{
			bMapHasShieldBelt = true;
			return Super::CheckRelevance_Implementation(Other);
		}

		// Keep: UDamage / Amp / Siphon
		if (InvName.Contains(TEXT("UDamage")) || InvName.Contains(TEXT("Amp")) || InvName.Contains(TEXT("Berserk")) || InvName.Contains(TEXT("Siphon")))
		{
			return Super::CheckRelevance_Implementation(Other);
		}

		// Stash the first Chest/Vest pickup — might become a ShieldBelt later
		if (!PendingVestPickup && InvName.Contains(TEXT("Armor_Chest")))
		{
			PendingVestPickup = InvPickup;
			return Super::CheckRelevance_Implementation(Other); // keep alive for now
		}

		// Remove everything else (Thighpads, extra Chests, Helmet, Jumpboots, Invisibility, etc.)
		return false;
	}

	return Super::CheckRelevance_Implementation(Other);
}

bool AUWipeoutGame::ModifyDamage_Implementation(int32& Damage, FVector& Momentum, APawn* Injured,
	AController* InstigatedBy, const FHitResult& HitInfo, AActor* DamageCauser, TSubclassOf<UDamageType> DamageType)
{
	// Intermission invulnerability for winners
	if (GetMatchState() == FName(TEXT("RoundCooldown")) && Injured && Injured->PlayerState)
	{
		AUTPlayerState* InjuredPS = Cast<AUTPlayerState>(Injured->PlayerState);
		if (InjuredPS && InjuredPS->Team && InjuredPS->Team->TeamIndex == LastRoundWinningTeamIndex)
		{
			Damage = 0;
			return true;
		}
	}

	// Mid-round spawn protection
	if (bRoundInProgress && Injured && Injured->PlayerState && RespawnProtectionTime > 0.f)
	{
		AUTPlayerState* InjuredPS = Cast<AUTPlayerState>(Injured->PlayerState);
		if (InjuredPS)
		{
			float* ProtectedUntil = SpawnProtectedUntil.Find(InjuredPS);
			if (ProtectedUntil && GetWorld()->GetTimeSeconds() < *ProtectedUntil)
			{
				// Player is still under spawn protection
				// Allow self-damage to break protection (prevents abuse)
				if (InstigatedBy && InstigatedBy->PlayerState == Injured->PlayerState)
				{
					SpawnProtectedUntil.Remove(InjuredPS);
				}
				else
				{
					Damage = 0;
					return true;
				}
			}
			else if (ProtectedUntil)
			{
				// Protection expired, clean up
				SpawnProtectedUntil.Remove(InjuredPS);
			}
		}
	}

	// -----------------------------------------------------------------------
	// Link Gun beam teammate healing
	// When the beam (UTDMG_Link_Alt) hits a same-team player, heal them
	// instead of dealing damage. Rate: ~5 HP/sec (beam ticks ~8x/sec at
	// 0.12s interval, so we accumulate fractional healing per tick).
	// Capped at 100 HP (normal max). Works with stock Link Gun.
	// -----------------------------------------------------------------------
	if (LinkHealPerSecond > 0.f && DamageType && Injured && InstigatedBy)
	{
		FString DTName = DamageType->GetName();
		if (DTName.Contains(TEXT("Link_Alt")) || DTName.Contains(TEXT("LinkBeam")))
		{
			AUTCharacter* InjuredChar = Cast<AUTCharacter>(Injured);
			AUTPlayerState* InjuredPS = InjuredChar ? Cast<AUTPlayerState>(InjuredChar->PlayerState) : nullptr;
			AUTPlayerState* InstigatorPS = InstigatedBy ? Cast<AUTPlayerState>(InstigatedBy->PlayerState) : nullptr;

			if (InjuredPS && InstigatorPS && InjuredPS != InstigatorPS
				&& InjuredPS->Team && InstigatorPS->Team
				&& InjuredPS->Team->TeamIndex == InstigatorPS->Team->TeamIndex)
			{
				// Same team — heal instead of damage
				if (InjuredChar->Health < LinkHealMaxHP)
				{
					// Accumulate fractional healing per beam tick
					// Beam fires every ~0.12s, so per-tick heal = HealPerSecond * 0.12
					float HealPerTick = LinkHealPerSecond * 0.12f;
					float& Accumulator = LinkHealAccumulator.FindOrAdd(InjuredPS);
					Accumulator += HealPerTick;

					int32 WholeHeal = FMath::FloorToInt(Accumulator);
					if (WholeHeal > 0)
					{
						Accumulator -= (float)WholeHeal;
						int32 ActualHeal = FMath::Min(WholeHeal, LinkHealMaxHP - InjuredChar->Health);
						if (ActualHeal > 0)
						{
							InjuredChar->Health += ActualHeal;
							// Credit the beam owner with the HP actually restored.
							CreditHealing(InstigatorPS, ActualHeal);
						}
					}
				}

				// Block damage to teammate
				Damage = 0;
				return true;
			}
		}
	}

	return Super::ModifyDamage_Implementation(Damage, Momentum, Injured, InstigatedBy, HitInfo, DamageCauser, DamageType);
}


// ============================================================================
// HEALING CREDIT (link beam + BP heal ability) — match-cumulative per healer
// ============================================================================

void AUWipeoutGame::CreditHealing(AUTPlayerState* HealerPS, int32 Amount)
{
	if (!HasAuthority() || HealerPS == nullptr || Amount <= 0)
	{
		return;
	}
	int32& Total = HealingDoneThisMatch.FindOrAdd(HealerPS);
	Total += Amount;
}

int32 AUWipeoutGame::GetHealingDoneForPlayer(AUTPlayerState* PS) const
{
	if (PS == nullptr)
	{
		return 0;
	}
	const int32* Found = HealingDoneThisMatch.Find(PS);
	return (Found != nullptr) ? *Found : 0;
}

// ============================================================================
// SCORE DAMAGE (damage tracking for achievements)
// ============================================================================

void AUWipeoutGame::ScoreDamage_Implementation(int32 DamageAmount, AUTPlayerState* Victim, AUTPlayerState* Attacker)
{
	Super::ScoreDamage_Implementation(DamageAmount, Victim, Attacker);

	if (!Victim || !Attacker || !UTGameState) return;
	if (UTGameState->OnSameTeam(Victim, Attacker)) return;
	if (!bRoundInProgress || !Attacker->Team || DamageAmount <= 0) return;

	// Calculate actual damage dealt (no overkill)
	int32 ActualDamage = DamageAmount;
	if (Victim && Victim->GetUTCharacter())
	{
		AUTCharacter* VictimChar = Victim->GetUTCharacter();
		int32 TotalHP = VictimChar->Health + FMath::FloorToInt(VictimChar->GetArmorAmount());
		ActualDamage = FMath::Min(DamageAmount, TotalHP);
	}

	if (!PlayerRoundDamage.Contains(Attacker))
	{
		PlayerRoundDamage.Add(Attacker, 0.0f);
	}
	PlayerRoundDamage[Attacker] += ActualDamage;

	if (Attacker->Team->TeamIndex == 0)      Team0RoundDamage += ActualDamage;
	else if (Attacker->Team->TeamIndex == 1)  Team1RoundDamage += ActualDamage;

	// Track per-life mutual damage for death recap
	uint64 Key = MakeDamagePairKey(Attacker, Victim);
	int32& PairDmg = LifeDamageMap.FindOrAdd(Key);
	PairDmg += ActualDamage;

	// Siphon: heal attacker for a percentage of raw damage dealt
	AUTCharacter* AttackerChar = Attacker->GetUTCharacter();
	if (AttackerChar && !AttackerChar->IsDead() && !AttackerChar->IsPendingKillPending())
	{
		AUTSiphonPowerup* Siphon = AttackerChar->FindInventoryType<AUTSiphonPowerup>(
			AUTSiphonPowerup::StaticClass(), false);
		if (Siphon)
		{
			int32 HealAmount = FMath::CeilToInt(DamageAmount * Siphon->SiphonPercent);
			int32 NewHealth = FMath::Min<int32>(AttackerChar->Health + HealAmount, Siphon->HealCap);
			if (NewHealth > AttackerChar->Health)
			{
				AttackerChar->Health = NewHealth;
				AttackerChar->OnHealthUpdated();
			}
		}
	}
}


// ============================================================================
// LIFE DAMAGE TRACKING (for death recap messages)
// ============================================================================

uint64 AUWipeoutGame::MakeDamagePairKey(const AUTPlayerState* From, const AUTPlayerState* To)
{
	// Pack two 32-bit IDs into one 64-bit key
	uint32 FromID = From ? From->GetUniqueID() : 0;
	uint32 ToID = To ? To->GetUniqueID() : 0;
	return (uint64(FromID) << 32) | uint64(ToID);
}

int32 AUWipeoutGame::GetLifeDamage(const AUTPlayerState* From, const AUTPlayerState* To) const
{
	uint64 Key = MakeDamagePairKey(From, To);
	const int32* Val = LifeDamageMap.Find(Key);
	return Val ? *Val : 0;
}

void AUWipeoutGame::ClearLifeDamageFor(AUTPlayerState* PS)
{
	if (!PS) return;
	uint32 PSID = PS->GetUniqueID();

	TArray<uint64> KeysToRemove;
	for (auto& Pair : LifeDamageMap)
	{
		uint32 FromID = uint32(Pair.Key >> 32);
		uint32 ToID = uint32(Pair.Key & 0xFFFFFFFF);
		if (FromID == PSID || ToID == PSID)
		{
			KeysToRemove.Add(Pair.Key);
		}
	}
	for (uint64 Key : KeysToRemove)
	{
		LifeDamageMap.Remove(Key);
	}
}

void AUWipeoutGame::SendDeathRecap(AUTPlayerState* Victim, AUTPlayerState* Killer)
{
	if (!Victim || !Killer || Victim == Killer) return;

	AUTPlayerController* VictimPC = Cast<AUTPlayerController>(Cast<AController>(Victim->GetOwner()));
	if (!VictimPC) return;

	int32 DmgToKiller = GetLifeDamage(Victim, Killer);
	int32 DmgFromKiller = GetLifeDamage(Killer, Victim);

	FString Msg = FString::Printf(TEXT("You dealt %d to %s | They dealt %d to you"),
		DmgToKiller, *Killer->PlayerName, DmgFromKiller);

	VictimPC->ClientSay(nullptr, Msg, ChatDestinations::System);
}


// ============================================================================
// RESET / CLEANUP
// ============================================================================

void AUWipeoutGame::ResetPlayersForNewRound()
{
	Team0RoundDamage = 0.0f;
	Team1RoundDamage = 0.0f;
	PlayerRoundDamage.Empty();
	LifeDamageMap.Empty();

	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AController* C = It->Get();
		if (!C) continue;

		AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
		if (PS)
		{
			PS->SetOutOfLives(false);
			PS->RoundDamageDone = 0;
			PS->RoundKills = 0;
		}

		APawn* Pawn = C->GetPawn();
		if (Pawn)
		{
			DiscardInventory(Pawn, C);
			C->UnPossess();
			Pawn->Destroy();
		}
	}

	// The controller pass cannot see a live pawn that was unpossessed earlier in
	// the round. Such an orphan survives CleanupWorldForNewRound() (which only
	// removes dead characters), and stock true-spectator TacCom outlines every
	// AUTCharacter it finds, exposing the orphan as a frozen X-ray ghost. This
	// reset runs before the next round's pawns are spawned, so every character
	// still present here is stale.
	TArray<AUTCharacter*> LeftoverCharacters;
	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		AUTCharacter* UTC = Cast<AUTCharacter>(It->Get());
		if (UTC && !UTC->IsPendingKill())
		{
			LeftoverCharacters.Add(UTC);
		}
	}

	for (AUTCharacter* UTC : LeftoverCharacters)
	{
		UTC->Destroy();
	}
}


void AUWipeoutGame::CleanupWorldForNewRound()
{
	for (TActorIterator<AUTDroppedPickup> It(GetWorld()); It; ++It)
	{
		It->Destroy();
	}

	for (FActorIterator It(GetWorld()); It; ++It)
	{
		if (It->GetClass()->ImplementsInterface(UUTResetInterface::StaticClass()))
		{
			IUTResetInterface::Execute_Reset(*It);
		}
	}

	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		if (AUTCharacter* UTC = Cast<AUTCharacter>(It->Get()))
		{
			if (UTC->IsDead() && !UTC->IsPendingKill())
			{
				UTC->Destroy();
			}
		}
	}
}


// ============================================================================
// ALIVE COUNTS & HELPERS
// ============================================================================

bool AUWipeoutGame::GetAliveCounts(int32& OutAliveTeam0, int32& OutAliveTeam1) const
{
	OutAliveTeam0 = 0;
	OutAliveTeam1 = 0;

	AUTGameState* GS = GetGameState<AUTGameState>();
	if (!GS || GS->IsPendingKill()) return false;

	for (APlayerState* PSBase : GS->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(PSBase);
		if (!PS || PS->bOnlySpectator || PS->bIsInactive || !PS->Team) continue;

		AUTCharacter* Pawn = Cast<AUTCharacter>(PS->GetUTCharacter());
		if (!Pawn || Pawn->IsDead() || Pawn->Health <= 0) continue;

		if (PS->Team->TeamIndex == 0) OutAliveTeam0++;
		else if (PS->Team->TeamIndex == 1) OutAliveTeam1++;
	}

	return true;
}

bool AUWipeoutGame::GetTeamMemberCounts(int32& OutTeam0, int32& OutTeam1) const
{
	OutTeam0 = 0;
	OutTeam1 = 0;

	AUTGameState* GS = GetGameState<AUTGameState>();
	if (!GS || GS->IsPendingKill()) return false;

	// Same filters as GetAliveCounts, minus the living-pawn requirement: counts
	// everyone ON a team — alive, dead, or waiting on a respawn wave.
	for (APlayerState* PSBase : GS->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(PSBase);
		if (!PS || PS->bOnlySpectator || PS->bIsInactive || !PS->Team) continue;

		if (PS->Team->TeamIndex == 0) OutTeam0++;
		else if (PS->Team->TeamIndex == 1) OutTeam1++;
	}

	return true;
}


int32 AUWipeoutGame::CountAliveOnTeam(int32 TeamIndex) const
{
	int32 Alive = 0;
	if (Teams.IsValidIndex(TeamIndex) && Teams[TeamIndex])
	{
		TArray<AController*> Members = Teams[TeamIndex]->GetTeamMembers();
		for (AController* C : Members)
		{
			if (!C) continue;
			AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
			if (!PS || PS->bOnlySpectator) continue;
			const APawn* P = C->GetPawn();
			const AUTCharacter* UTC = Cast<AUTCharacter>(P);
			if (P && (!UTC || !UTC->IsDead())) ++Alive;
		}
	}
	return Alive;
}


int32 AUWipeoutGame::GetTiebreakWinnerByTeamHealth() const
{
	float Sum0 = 0.f, Sum1 = 0.f;
	for (TActorIterator<AUTCharacter> It(GetWorld()); It; ++It)
	{
		AUTCharacter* C = *It;
		if (!C || C->IsDead()) continue;
		if (AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState))
		{
			if (PS->Team)
			{
				const float HP = FMath::Max(0.f, (float)C->Health) + C->GetArmorAmount();
				(PS->Team->TeamIndex == 0 ? Sum0 : Sum1) += HP;
			}
		}
	}
	if (Sum0 > Sum1) return 0;
	if (Sum1 > Sum0) return 1;
	return INDEX_NONE;
}


void AUWipeoutGame::DelayedInitialWinCheck()
{
	if (bRoundInProgress)
	{
		WinCheckHoldUntilSeconds = 0.f;
		CheckWipeoutCondition();
	}
}


// ============================================================================
// SPECTATING
// ============================================================================

void AUWipeoutGame::EnforceRoundSpectatorLock()
{
	if (!bRoundInProgress || bWarmupMode || GetNetMode() == NM_Client)
	{
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
		if (!NCPlusRoundSpectate::ShouldLock(PC, PS))
		{
			continue;
		}
		NCPlusRoundSpectate::Lock(PC, FindAliveTeammate(PS));
	}
}

void AUWipeoutGame::ForceTeamSpectate(AUTPlayerState* DeadPS)
{
	if (!DeadPS) return;

	if (useBPSpecFunction)
	{
		BP_SpectatePSImplementation(DeadPS);
		return;
	}

	AUTPlayerController* PC = Cast<AUTPlayerController>(DeadPS->GetOwner());
	if (!PC) return;

	PC->ChangeState(NAME_Spectating);
	PC->ClientGotoState(NAME_Spectating);

	if (AUTPlayerState* TeamTarget = FindAliveTeammate(DeadPS))
	{
		PC->SetViewTarget(TeamTarget->GetUTCharacter());
		PC->bSpectateBehindView = false;
		PC->BehindView(false);
		return;
	}
	if (AUTPlayerState* EnemyTarget = FindAliveEnemy(DeadPS))
	{
		PC->SetViewTarget(EnemyTarget->GetUTCharacter());
		PC->bSpectateBehindView = false;
		PC->BehindView(false);
		return;
	}
	PC->ServerViewSelf();
}


AUTPlayerState* AUWipeoutGame::FindAliveTeammate(AUTPlayerState* PS) const
{
	if (!PS || !PS->Team) return nullptr;
	const int32 TeamIdx = PS->Team->TeamIndex;
	if (!Teams.IsValidIndex(TeamIdx) || !Teams[TeamIdx]) return nullptr;

	TArray<AController*> Members = Teams[TeamIdx]->GetTeamMembers();
	for (AController* C : Members)
	{
		if (!C) continue;
		AUTPlayerState* OtherPS = Cast<AUTPlayerState>(C->PlayerState);
		if (!OtherPS || OtherPS == PS || OtherPS->bOnlySpectator) continue;
		APawn* P = C->GetPawn();
		AUTCharacter* UTC = Cast<AUTCharacter>(P);
		if (P && (!UTC || !UTC->IsDead())) return OtherPS;
	}
	return nullptr;
}


AUTPlayerState* AUWipeoutGame::FindAliveEnemy(AUTPlayerState* PS) const
{
	if (!PS || !PS->Team) return nullptr;
	const int32 MyTeam = PS->Team->TeamIndex;

	for (int32 TeamIdx = 0; TeamIdx < Teams.Num(); TeamIdx++)
	{
		if (TeamIdx == MyTeam || !Teams.IsValidIndex(TeamIdx)) continue;
		TArray<AController*> Members = Teams[TeamIdx]->GetTeamMembers();
		for (AController* C : Members)
		{
			if (!C) continue;
			AUTPlayerState* OtherPS = Cast<AUTPlayerState>(C->PlayerState);
			if (!OtherPS || OtherPS->bOnlySpectator) continue;
			APawn* P = C->GetPawn();
			AUTCharacter* UTC = Cast<AUTCharacter>(P);
			if (P && (!UTC || !UTC->IsDead())) return OtherPS;
		}
	}
	return nullptr;
}


AUTPlayerState* AUWipeoutGame::FindAliveOnTeamPS(int32 TeamIndex) const
{
	if (!Teams.IsValidIndex(TeamIndex)) return nullptr;
	TArray<AController*> Members = Teams[TeamIndex]->GetTeamMembers();
	for (AController* C : Members)
	{
		if (!C) continue;
		AUTPlayerState* PS = Cast<AUTPlayerState>(C->PlayerState);
		if (!PS || PS->bOnlySpectator) continue;
		APawn* P = C->GetPawn();
		const AUTCharacter* UTC = Cast<AUTCharacter>(P);
		if (P && (!UTC || !UTC->IsDead())) return PS;
	}
	return nullptr;
}


AUTPlayerState* AUWipeoutGame::FindAnyOnTeamPS(int32 TeamIndex) const
{
	if (const AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>())
	{
		for (APlayerState* APS : GS->PlayerArray)
		{
			if (AUTPlayerState* UTPS = Cast<AUTPlayerState>(APS))
			{
				if (UTPS->Team && UTPS->Team->TeamIndex == TeamIndex) return UTPS;
			}
		}
	}
	return nullptr;
}


void AUWipeoutGame::ForceLosersToViewWinners(int32 WinnerTeamIndex)
{
	if (WinnerTeamIndex < 0) return;

	const int32 LoserTeamIndex = (WinnerTeamIndex == 0) ? 1 : 0;
	AUTPlayerState* TargetPS = FindAliveOnTeamPS(WinnerTeamIndex);
	if (!TargetPS) TargetPS = FindAnyOnTeamPS(WinnerTeamIndex);
	if (!TargetPS) return;

	AUTCharacter* TargetCharacter = TargetPS->GetUTCharacter();
	if (!TargetCharacter) return;

	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
		if (!PC || !PS || !PS->Team) continue;

		if (PS->Team->TeamIndex == LoserTeamIndex)
		{
			if (!PC->IsInState(NAME_Spectating))
			{
				PC->ChangeState(NAME_Spectating);
				PC->ClientGotoState(NAME_Spectating);
			}
			if (!PS->bOutOfLives)
			{
				PS->bOutOfLives = true;
				PS->ForceNetUpdate();
			}
			PC->SetViewTarget(TargetCharacter);
			PC->bSpectateBehindView = false;
			PC->BehindView(false);
		}
	}
}


bool AUWipeoutGame::CanSpectate_Implementation(APlayerController* Viewer, APlayerState* ViewTarget)
{
	if (!Viewer || !ViewTarget) return false;

	const AUTPlayerState* ViewerPS = Cast<AUTPlayerState>(Viewer->PlayerState);
	const AUTPlayerState* TargetPS = Cast<AUTPlayerState>(ViewTarget);
	if (!ViewerPS || !TargetPS || !ViewerPS->Team || !TargetPS->Team)
	{
		return Super::CanSpectate_Implementation(Viewer, ViewTarget);
	}

	if (!bRoundInProgress)
	{
		// Round over (custom 'RoundCooldown' state): losers are forced to spectate
		// the winners and Mouse1 (ServerViewNextPlayer) should cycle through them.
		// Deferring to Super rejected the cycle because RoundCooldown isn't "match
		// in progress" — so explicitly allow spectating any alive non-spectator.
		if (!TargetPS->bOnlySpectator)
		{
			const AController* TPC = Cast<AController>(TargetPS->GetOwner());
			const APawn* TP = TPC ? TPC->GetPawn() : nullptr;
			const AUTCharacter* TC = Cast<AUTCharacter>(TP);
			if (TP && (!TC || !TC->IsDead())) return true;
		}
		return Super::CanSpectate_Implementation(Viewer, ViewTarget);
	}

	// During round: dead players can only spectate teammates if teammates are alive
	if (bRoundInProgress)
	{
		const int32 MyTeamAlive = CountAliveOnTeam(ViewerPS->Team->TeamIndex);
		if (MyTeamAlive > 0 && ViewerPS->Team != TargetPS->Team)
		{
			return false; // Can't spectate enemies while teammates are alive
		}
	}

	const AController* TargetPC = Cast<AController>(TargetPS->GetOwner());
	const APawn* P = TargetPC ? TargetPC->GetPawn() : nullptr;
	const AUTCharacter* C = Cast<AUTCharacter>(P);
	const bool bTargetAlive = P && (!C || !C->IsDead());
	if (!bTargetAlive && bRoundInProgress) return false;

	return Super::CanSpectate_Implementation(Viewer, ViewTarget);
}


// ============================================================================
// SPAWN LAYOUT PRECOMPUTATION (from TeamArena, for round-start spawns)
// ============================================================================

void AUWipeoutGame::InitializeSpawnPointSystem()
{
	AllSpawnPointsList.Empty();
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		if (APlayerStart* Spawn = *It)
		{
			AllSpawnPointsList.Add(Spawn);
		}
	}

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: Found %d spawn points"), AllSpawnPointsList.Num());
}


void AUWipeoutGame::PrecomputeSpawnLayouts()
{
	ValidLayouts_2v2.Empty();
	ValidLayouts_1v1.Empty();

	const int32 N = AllSpawnPointsList.Num();
	if (N < 2)
	{
		UE_LOG(LogGameMode, Error, TEXT("Wipeout::PrecomputeSpawnLayouts: Only %d spawns!"), N);
		return;
	}

	// ── Multi-axis clustering ──
	// Try several axis candidates (E-W, N-S, diagonals, principal axis).
	// For each, project all spawns onto the axis, split at median, take 4 deepest
	// per side. Pick whichever axis produces the biggest MinCrossDistance.

	const int32 SpawnsPerTeam = 4;

	// Compute overall centroid (for axis projection origin)
	FVector Centroid = FVector::ZeroVector;
	for (APlayerStart* S : AllSpawnPointsList) Centroid += S->GetActorLocation();
	Centroid /= N;

	// Principal axis via covariance (2D): find the direction of max spread
	float Sxx = 0.f, Sxy = 0.f, Syy = 0.f;
	for (APlayerStart* S : AllSpawnPointsList)
	{
		FVector D = S->GetActorLocation() - Centroid;
		Sxx += D.X * D.X;
		Sxy += D.X * D.Y;
		Syy += D.Y * D.Y;
	}
	// Largest eigenvector of [[Sxx Sxy][Sxy Syy]] — angle = 0.5 * atan2(2*Sxy, Sxx - Syy)
	float PrincipalAngle = 0.5f * FMath::Atan2(2.f * Sxy, Sxx - Syy);
	FVector2D PrincipalAxis(FMath::Cos(PrincipalAngle), FMath::Sin(PrincipalAngle));

	struct FAxisCandidate
	{
		FString Name;
		FVector2D Dir;
	};

	TArray<FAxisCandidate> Candidates = {
		{ TEXT("East-West"),  FVector2D(1.f, 0.f) },
		{ TEXT("North-South"), FVector2D(0.f, 1.f) },
		{ TEXT("NE-SW"),      FVector2D(0.707f, 0.707f) },
		{ TEXT("NW-SE"),      FVector2D(0.707f, -0.707f) },
		{ TEXT("Principal"),  PrincipalAxis }
	};

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: Evaluating %d axis candidates for %d spawns (centroid %.0f,%.0f, principal angle %.1f deg)"),
		Candidates.Num(), N, Centroid.X, Centroid.Y, FMath::RadiansToDegrees(PrincipalAngle));

	// Helper: max pairwise 2D distance ("diameter") of a spawn set.
	// Used as an intra-team cohesion metric — smaller = tighter cluster.
	auto IntraDiameter2D = [](const TArray<APlayerStart*>& Side) -> float
	{
		float D = 0.f;
		for (int32 i = 0; i < Side.Num(); ++i)
		{
			for (int32 j = i + 1; j < Side.Num(); ++j)
			{
				if (Side[i] && Side[j])
				{
					D = FMath::Max(D, (Side[i]->GetActorLocation() - Side[j]->GetActorLocation()).Size2D());
				}
			}
		}
		return D;
	};

	// Penalty weight for intra-team spread in layout scoring.
	// QualityScore = MinCrossDistance2D - IntraSpreadPenalty * MaxIntraDiameter.
	// 0.5 means a 2000u spread reduction is worth the same as a 1000u cross gain.
	const float IntraSpreadPenalty = 0.5f;

	TArray<APlayerStart*> SideA, SideB;
	FString BestAxisName;
	float BestMinCross = -1.f;
	float BestAxisScore = -FLT_MAX;

	for (const FAxisCandidate& Axis : Candidates)
	{
		// Project each spawn onto this axis (scalar = dot product with axis direction)
		struct FSpawnProj
		{
			float Proj;
			APlayerStart* Spawn;
		};
		TArray<FSpawnProj> Projections;
		for (APlayerStart* S : AllSpawnPointsList)
		{
			FVector D = S->GetActorLocation() - Centroid;
			FSpawnProj Item;
			Item.Proj = D.X * Axis.Dir.X + D.Y * Axis.Dir.Y;
			Item.Spawn = S;
			Projections.Add(Item);
		}

		// Sort by projection, split at median
		Projections.Sort([](const FSpawnProj& A, const FSpawnProj& B)
		{
			return A.Proj < B.Proj;
		});

		int32 SplitIdx = N / 2;
		TArray<APlayerStart*> CandidateA, CandidateB;
		for (int32 i = 0; i < SplitIdx; ++i) CandidateA.Add(Projections[i].Spawn);
		for (int32 i = SplitIdx; i < N; ++i) CandidateB.Add(Projections[i].Spawn);

		// Pick the 4 "deepest" from each side — those with extreme projections
		// (SideA deepest = most negative projection, SideB deepest = most positive)
		// Projections are already sorted ascending, so:
		// - CandidateA is [most-negative...median], deepest = index 0
		// - CandidateB is [median...most-positive], deepest = last index
		TArray<APlayerStart*> PickA, PickB;
		for (int32 i = 0; i < FMath::Min(SpawnsPerTeam, CandidateA.Num()); ++i)
			PickA.Add(CandidateA[i]);
		for (int32 i = 0; i < FMath::Min(SpawnsPerTeam, CandidateB.Num()); ++i)
			PickB.Add(CandidateB[CandidateB.Num() - 1 - i]);

		// Compute MinCrossDistance for this axis' pick
		float MinCross = FLT_MAX;
		for (APlayerStart* S0 : PickA)
			for (APlayerStart* S1 : PickB)
				MinCross = FMath::Min(MinCross, (S0->GetActorLocation() - S1->GetActorLocation()).Size2D());

		// Intra-team cohesion: max diameter among the picked 4 per side.
		// We want this LOW so teammates spawn close together (no outliers).
		const float IntraSpread = FMath::Max(IntraDiameter2D(PickA), IntraDiameter2D(PickB));
		const float AxisScore = MinCross - IntraSpreadPenalty * IntraSpread;

		UE_LOG(LogGameMode, Log, TEXT("  Axis %-12s: split %d/%d, picked %d/%d, MinCross %.0f, IntraSpread %.0f, Score %.0f"),
			*Axis.Name, CandidateA.Num(), CandidateB.Num(), PickA.Num(), PickB.Num(),
			MinCross, IntraSpread, AxisScore);

		if (AxisScore > BestAxisScore)
		{
			BestAxisScore = AxisScore;
			BestMinCross = MinCross;
			BestAxisName = Axis.Name;
			SideA = CandidateA;
			SideB = CandidateB;
		}
	}

	UE_LOG(LogGameMode, Log, TEXT("Wipeout: Selected axis '%s' with Score %.0f, MinCross %.0f (%d on side A, %d on side B)"),
		*BestAxisName, BestAxisScore, BestMinCross, SideA.Num(), SideB.Num());

	// Persist full side arrays for fallback (should not be used anymore but keep for safety)
	PrecomputedSideA = SideA;
	PrecomputedSideB = SideB;

	// ── Generate layouts: pick SpawnsPerTeam from each side ──
	const int32 SpawnsA = FMath::Min(SideA.Num(), SpawnsPerTeam);
	const int32 SpawnsB = FMath::Min(SideB.Num(), SpawnsPerTeam);

	if (SpawnsA == 0 || SpawnsB == 0)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: One side has no spawns (A=%d, B=%d) — falling back to 1v1 only"), SpawnsA, SpawnsB);
	}

	// Sort each side by distance from the other side's centroid (prefer "deeper" spawns)
	auto SortByDistFromCentroid = [](TArray<APlayerStart*>& Side, const FVector& OtherCentroid)
	{
		Side.Sort([&OtherCentroid](const APlayerStart& A, const APlayerStart& B)
		{
			return (A.GetActorLocation() - OtherCentroid).Size2D() > (B.GetActorLocation() - OtherCentroid).Size2D();
		});
	};

	// Compute centroids
	FVector CentroidA = FVector::ZeroVector, CentroidB = FVector::ZeroVector;
	for (APlayerStart* S : SideA) CentroidA += S->GetActorLocation();
	for (APlayerStart* S : SideB) CentroidB += S->GetActorLocation();
	if (SideA.Num() > 0) CentroidA /= SideA.Num();
	if (SideB.Num() > 0) CentroidB /= SideB.Num();

	SortByDistFromCentroid(SideA, CentroidB);
	SortByDistFromCentroid(SideB, CentroidA);

	// Build the main layout (takes top N spawns from each sorted side)
	if (SpawnsA >= 1 && SpawnsB >= 1)
	{
		FWipeoutSpawnLayout Layout;
		for (int32 i = 0; i < SpawnsA; ++i) Layout.T0_Spawns.Add(SideA[i]);
		for (int32 i = 0; i < SpawnsB; ++i) Layout.T1_Spawns.Add(SideB[i]);

		// MinCross = minimum distance between any T0 spawn and any T1 spawn
		float MinCross = FLT_MAX;
		for (APlayerStart* S0 : Layout.T0_Spawns)
		{
			for (APlayerStart* S1 : Layout.T1_Spawns)
			{
				MinCross = FMath::Min(MinCross, (S0->GetActorLocation() - S1->GetActorLocation()).Size2D());
			}
		}
		// Intra-team cohesion penalty: ensure the curated 4 cluster together.
		// Without this, a "deep" but isolated spawn (e.g. top link on DM-Rankin)
		// can end up in the curated set and isolate one player with the enemy.
		float IntraSpread = FMath::Max(IntraDiameter2D(Layout.T0_Spawns), IntraDiameter2D(Layout.T1_Spawns));
		Layout.MinCrossDistance2D = MinCross;
		Layout.QualityScore = MinCross - IntraSpreadPenalty * IntraSpread;
		Layout.UsageCount = 0;
		ValidLayouts_2v2.Add(Layout);

		// Generate a few more layouts by shuffling which spawns from each side are picked
		// (if sides have more spawns than SpawnsPerTeam, try alternate selections)
		for (int32 Variant = 1; Variant < FMath::Min(SideA.Num(), 6); ++Variant)
		{
			FWipeoutSpawnLayout VLayout;
			// Rotate: shift the starting index for side A
			for (int32 i = 0; i < SpawnsA; ++i) VLayout.T0_Spawns.Add(SideA[(i + Variant) % SideA.Num()]);
			for (int32 i = 0; i < SpawnsB; ++i) VLayout.T1_Spawns.Add(SideB[i]);

			MinCross = FLT_MAX;
			for (APlayerStart* S0 : VLayout.T0_Spawns)
			{
				for (APlayerStart* S1 : VLayout.T1_Spawns)
				{
					MinCross = FMath::Min(MinCross, (S0->GetActorLocation() - S1->GetActorLocation()).Size2D());
				}
			}
			float VIntraSpread = FMath::Max(IntraDiameter2D(VLayout.T0_Spawns), IntraDiameter2D(VLayout.T1_Spawns));
			VLayout.MinCrossDistance2D = MinCross;
			VLayout.QualityScore = MinCross - IntraSpreadPenalty * VIntraSpread;
			VLayout.UsageCount = 0;
			ValidLayouts_2v2.Add(VLayout);
		}
	}

	// 1v1 layouts (single spawn per team, maximum cross distance)
	for (int32 a = 0; a < SideA.Num(); ++a)
	{
		for (int32 b = 0; b < SideB.Num(); ++b)
		{
			float Dist = (SideA[a]->GetActorLocation() - SideB[b]->GetActorLocation()).Size2D();

			FWipeoutSpawnLayout Layout;
			Layout.T0_Spawns.Add(SideA[a]);
			Layout.T1_Spawns.Add(SideB[b]);
			Layout.MinCrossDistance2D = Dist;
			Layout.QualityScore = Dist;
			Layout.UsageCount = 0;
			ValidLayouts_1v1.Add(Layout);
		}
	}

	ValidLayouts_2v2.Sort([](const FWipeoutSpawnLayout& A, const FWipeoutSpawnLayout& B) { return A.QualityScore > B.QualityScore; });
	ValidLayouts_1v1.Sort([](const FWipeoutSpawnLayout& A, const FWipeoutSpawnLayout& B) { return A.QualityScore > B.QualityScore; });

	UE_LOG(LogGameMode, Log, TEXT("Wipeout spawn precompute: %d team, %d 1v1 layouts"), ValidLayouts_2v2.Num(), ValidLayouts_1v1.Num());
}


void AUWipeoutGame::SelectSpawnLayoutForRound()
{
	Team0SelectedSpawns.Empty();
	Team1SelectedSpawns.Empty();

	// Always prefer the team-size (4v4) layouts. 1v1 layouts are only a
	// fallback when no 4v4 layouts exist (degenerate maps). The previous
	// "force 1v1 every 3rd round" path broke 4v4 spawning catastrophically
	// — a 1v1 layout has 1 spawn per team, so 3 of 4 teammates ended up in
	// Tier 3 fallback (full map, no threshold) and spawned in enemy territory.
	TArray<FWipeoutSpawnLayout>& Pool = (ValidLayouts_2v2.Num() > 0) ? ValidLayouts_2v2 : ValidLayouts_1v1;

	if (Pool.Num() == 0)
	{
		UE_LOG(LogGameMode, Error, TEXT("Wipeout: No valid spawn layouts!"));
		return;
	}

	const int32 PoolSize = FMath::Min(Pool.Num(), 30);
	int32 BestIndex = 0;
	float BestScore = -FLT_MAX;

	for (int32 i = 0; i < PoolSize; ++i)
	{
		float UsagePenalty = Pool[i].UsageCount * 3000.0f;
		float Jitter = FMath::FRandRange(0.0f, 500.0f);
		float Score = Pool[i].QualityScore - UsagePenalty + Jitter;
		if (Score > BestScore)
		{
			BestScore = Score;
			BestIndex = i;
		}
	}

	FWipeoutSpawnLayout& Chosen = Pool[BestIndex];
	Chosen.UsageCount++;

	// Swap team sides on odd rounds for fairness
	bool bSwapTeams = (TotalRoundsPlayed % 2 == 1);
	const TArray<APlayerStart*>& T0Source = bSwapTeams ? Chosen.T1_Spawns : Chosen.T0_Spawns;
	const TArray<APlayerStart*>& T1Source = bSwapTeams ? Chosen.T0_Spawns : Chosen.T1_Spawns;

	Team0SelectedSpawns = T0Source;
	Team1SelectedSpawns = T1Source;

	UE_LOG(LogGameMode, Log, TEXT("Wipeout round %d: Layout idx %d, CrossDist %.0f, T0 spawns=%d, T1 spawns=%d"),
		TotalRoundsPlayed, BestIndex, Chosen.MinCrossDistance2D,
		Team0SelectedSpawns.Num(), Team1SelectedSpawns.Num());
	for (int32 i = 0; i < Team0SelectedSpawns.Num(); i++)
	{
		if (Team0SelectedSpawns[i])
			UE_LOG(LogGameMode, Log, TEXT("  T0[%d] = %s"), i, *Team0SelectedSpawns[i]->GetActorLocation().ToString());
	}
	for (int32 i = 0; i < Team1SelectedSpawns.Num(); i++)
	{
		if (Team1SelectedSpawns[i])
			UE_LOG(LogGameMode, Log, TEXT("  T1[%d] = %s"), i, *Team1SelectedSpawns[i]->GetActorLocation().ToString());
	}
}

void AUWipeoutGame::PrepareHybridRoundSpawnQueues(int32 Team0PlayerCount, int32 Team1PlayerCount)
{
	ClearHybridRoundSpawnState();
	if (Team0PlayerCount <= 0 && Team1PlayerCount <= 0)
	{
		return;
	}
	if (!bEnableHybridRoundSpawns)
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("[HybridSpawn] Wipeout disabled by config; using PlayerStart-only opening spawns (players %d/%d)"),
			Team0PlayerCount, Team1PlayerCount);
		return;
	}

	FNCHybridSpawnSettings Settings;
	Settings.MinimumCrossTeamDistance = MinimumEnemySpawnDistance;

	FNCHybridSpawnResult Result;
	if (!FNCHybridSpawnGenerator::Generate(
		GetWorld(), AllSpawnPointsList, Team0SelectedSpawns, Team1SelectedSpawns,
		Team0PlayerCount, Team1PlayerCount, Settings, Result))
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("Wipeout hybrid opening spawn generation had no safe anchor pair; using PlayerStart fallback"));
		return;
	}
	Team0HybridSpawnQueue = MoveTemp(Result.Team0Queue);
	Team1HybridSpawnQueue = MoveTemp(Result.Team1Queue);
	bHybridRoundSpawnWindow = Team0HybridSpawnQueue.Num() > 0 || Team1HybridSpawnQueue.Num() > 0;

	UE_LOG(LogGameMode, Warning,
		TEXT("[HybridSpawn] Wipeout active: anchors %s/%s, separation %.0f, radius %.0f, players %d/%d, queues %d/%d"),
		*Result.Team0AnchorName.ToString(), *Result.Team1AnchorName.ToString(),
		Result.AnchorDistance2D, Result.TeamRadius, Team0PlayerCount, Team1PlayerCount,
		Team0HybridSpawnQueue.Num(), Team1HybridSpawnQueue.Num());

	if (!Result.bTeam0Complete || !Result.bTeam1Complete)
	{
		UE_LOG(LogGameMode, Warning,
			TEXT("Wipeout hybrid opening queues are partial (needed %d/%d, built %d/%d); missing players use the existing fallback"),
			Team0PlayerCount, Team1PlayerCount,
			Team0HybridSpawnQueue.Num(), Team1HybridSpawnQueue.Num());
	}

	UE_LOG(LogGameMode, Verbose,
		TEXT("Wipeout hybrid rejects T0[R=%d L=%d F=%d S=%d Z=%d C=%d D=%d K=%d P=%d V=%d] T1[R=%d L=%d F=%d S=%d Z=%d C=%d D=%d K=%d P=%d V=%d]"),
		Result.Team0Stats.RejectedRadius, Result.Team0Stats.RejectedPath,
		Result.Team0Stats.RejectedFloor,
		Result.Team0Stats.RejectedSlope, Result.Team0Stats.RejectedDrop,
		Result.Team0Stats.RejectedClearance,
		Result.Team0Stats.RejectedSpacing, Result.Team0Stats.RejectedKillZ,
		Result.Team0Stats.RejectedPit, Result.Team0Stats.RejectedPainVolume,
		Result.Team1Stats.RejectedRadius, Result.Team1Stats.RejectedPath,
		Result.Team1Stats.RejectedFloor,
		Result.Team1Stats.RejectedSlope, Result.Team1Stats.RejectedDrop,
		Result.Team1Stats.RejectedClearance,
		Result.Team1Stats.RejectedSpacing, Result.Team1Stats.RejectedKillZ,
		Result.Team1Stats.RejectedPit, Result.Team1Stats.RejectedPainVolume);
}

bool AUWipeoutGame::TryConsumeHybridSpawnTransform(int32 TeamIndex, FTransform& OutTransform)
{
	TArray<FTransform>* Queue = nullptr;
	if (TeamIndex == 0) Queue = &Team0HybridSpawnQueue;
	else if (TeamIndex == 1) Queue = &Team1HybridSpawnQueue;

	if (!Queue || Queue->Num() == 0)
	{
		return false;
	}

	OutTransform = (*Queue)[0];
	Queue->RemoveAt(0, 1, false);
	return true;
}

void AUWipeoutGame::ClearHybridRoundSpawnState()
{
	bHybridRoundSpawnWindow = false;
	bHasPendingHybridSpawnTransform = false;
	Team0HybridSpawnQueue.Empty();
	Team1HybridSpawnQueue.Empty();
}


void AUWipeoutGame::ResetSpawnSelectionForNewRound()
{
	ClearHybridRoundSpawnState();
	Team0SelectedSpawns.Empty();
	Team1SelectedSpawns.Empty();
	++CurrentRoundNumber;
	FMath::SRandInit(static_cast<int32>(FPlatformTime::Cycles()));
}


// ============================================================================
// OVERTIME (wave-based damage — same system as TeamArena)
// ============================================================================

void AUWipeoutGame::StartOvertime()
{
	if (!bOvertimeEnabled || !bRoundInProgress) return;

	CurrentOvertimeWave = 0;
	CurrentWaveDamage = OvertimeBaseDamage;

	if (OvertimeStartDelay >= 3.0f)
	{
		float CountdownStart = OvertimeStartDelay - 3.0f;
		GetWorldTimerManager().SetTimer(
			OvertimeCountdownTimerHandle,
			[this]() { BroadcastOvertimeCountdown(3); },
			CountdownStart, false);
	}

	GetWorldTimerManager().SetTimer(
		OvertimeWaveTimerHandle, this,
		&AUWipeoutGame::ExecuteOvertimeWave,
		FMath::Max(0.1f, OvertimeStartDelay), false);

	BP_OnOvertimeStarted();
	UE_LOG(LogGameMode, Warning, TEXT("Wipeout Overtime started! First wave in %.1fs with %.1f dmg"),
		OvertimeStartDelay, OvertimeBaseDamage);
}


void AUWipeoutGame::StopOvertime()
{
	if (OvertimeTimerHandle.IsValid())      GetWorldTimerManager().ClearTimer(OvertimeTimerHandle);
	if (OvertimeWaveTimerHandle.IsValid())   GetWorldTimerManager().ClearTimer(OvertimeWaveTimerHandle);
	CurrentOvertimeWave = 0;
	CurrentWaveDamage = 0.0f;
}


void AUWipeoutGame::ExecuteOvertimeWave()
{
	if (GetNetMode() == NM_Client || !bRoundInProgress)
	{
		StopOvertime();
		return;
	}

	CurrentOvertimeWave++;
	CurrentWaveDamage = (CurrentOvertimeWave == 1) ? OvertimeBaseDamage : CurrentWaveDamage * OvertimeDamageMultiplier;
	if (OvertimeMaxDamage > 0.0f) CurrentWaveDamage = FMath::Min(CurrentWaveDamage, OvertimeMaxDamage);

	BP_OnOvertimeWave(CurrentWaveDamage, CurrentOvertimeWave);

	for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
	{
		if (!bRoundInProgress) break;

		AUTCharacter* C = Cast<AUTCharacter>(*It);
		if (!C || C->IsDead()) continue;

		float DamageToApply = CurrentWaveDamage;
		if (bOvertimeNonLethal && C->Health <= 1.f) continue;
		if (bOvertimeNonLethal && (C->Health - DamageToApply) < 1.f)
		{
			DamageToApply = FMath::Max(0.f, C->Health - 1.f);
			if (DamageToApply <= 0.f) continue;
		}

		FHitResult Hit;
		Hit.bBlockingHit = true;
		Hit.Location = C->GetActorLocation();
		Hit.ImpactPoint = C->GetActorLocation();
		Hit.Normal = FVector(0, 0, 1);
		Hit.ImpactNormal = FVector(0, 0, 1);
		Hit.Actor = C;
		Hit.Component = Cast<UPrimitiveComponent>(C->GetRootComponent());

		FUTPointDamageEvent DamageEvent(
			DamageToApply, Hit, FVector(0, 0, -1),
			OvertimeDamageType ? *OvertimeDamageType : UUTDamageType::StaticClass(),
			FVector::ZeroVector);

		// Pass the victim's own controller as instigator for overtime environment damage.
		// Passing nullptr crashes Epic's damage pipeline which dereferences InstigatedBy
		// without null checks in kill credit / death message code paths.
		AController* VictimController = C->GetController();
		C->TakeDamage(DamageToApply, DamageEvent, VictimController, this);
	}

	CheckWipeoutCondition();

	if (bRoundInProgress)
	{
		GetWorldTimerManager().SetTimer(
			OvertimeWaveTimerHandle, this,
			&AUWipeoutGame::ExecuteOvertimeWave,
			OvertimeWaveInterval, false);
	}
}


void AUWipeoutGame::BroadcastOvertimeCountdown(int32 CountdownValue)
{
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get()))
		{
			PC->ClientReceiveLocalizedMessage(UUTCountDownMessage::StaticClass(), CountdownValue);
		}
	}

	if (CountdownValue > 1)
	{
		GetWorldTimerManager().SetTimer(
			OvertimeCountdownTimerHandle,
			[this, CountdownValue]() { BroadcastOvertimeCountdown(CountdownValue - 1); },
			1.0f, false);
	}
	else if (CountdownValue == 1)
	{
		GetWorldTimerManager().SetTimer(
			OvertimeCountdownTimerHandle,
			[this]() { BroadcastOvertimeAnnouncement(); },
			1.0f, false);
	}
}


void AUWipeoutGame::BroadcastOvertimeAnnouncement()
{
	if (!OvertimeAnnouncementSound) return;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		if (AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get()))
		{
			PC->UTClientPlaySound(OvertimeAnnouncementSound);
		}
	}
}


// ============================================================================
// BROADCAST RESULTS
// ============================================================================

void AUWipeoutGame::BroadcastRoundResults(int32 WinnerTeamIndex, bool bIsDraw)
{
	BP_OnRoundResults(WinnerTeamIndex, bIsDraw);
}


void AUWipeoutGame::BroadcastKillReplay()
{
	// Instant replay path (ClientPlayInstantReplay), NOT ClientQueueCoolMoment.
	// CoolMoment routes through UUTKillcamPlayback::CoolMomentCamStart, which crashes
	// after MatchEnded (TaskGraphThreadNP access-violation in CoreUObject) because new
	// actors spawn before the playback finishes cleanup. Mirrors
	// AElimPlusGame::BroadcastKillReplay — focus the killer pawn, hard stop timer.
	if (RoundWinningKiller && RoundWinningKillTime > 0.f && WinningKillerPawn)
	{
		const float TimeToRewind = (GetWorld()->GetTimeSeconds() - RoundWinningKillTime) + 5.0f;
		const float StartDelay   = 0.5f;
		for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			if (AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get()))
			{
				PC->ClientPlayInstantReplay(WinningKillerPawn, TimeToRewind, StartDelay);
			}
		}
	}
}


// ============================================================================
// DOMINATION / LEAD TRACKING
// ============================================================================

void AUWipeoutGame::CheckForDominationAndLead(int32 WinnerTeamIndex)
{
	if (WinnerTeamIndex == INDEX_NONE || !Teams.IsValidIndex(0) || !Teams.IsValidIndex(1)) return;

	const int32 RedScore = Teams[0]->Score;
	const int32 BlueScore = Teams[1]->Score;
	const int32 ScoreDiff = FMath::Abs(RedScore - BlueScore);
	const bool bWasTied = (PreviousRedScore == PreviousBlueScore);
	const bool bNowHasLead = (RedScore != BlueScore);

	if (bWasTied && bNowHasLead)
	{
		BroadcastTakesLead(WinnerTeamIndex);
	}
	else if (!bWasTied && bNowHasLead)
	{
		const bool bRedWasWinning = (PreviousRedScore > PreviousBlueScore);
		const bool bRedNowWinning = (RedScore > BlueScore);
		if (bRedWasWinning != bRedNowWinning)
		{
			BroadcastTakesLead(WinnerTeamIndex);
		}
	}

	if (ScoreDiff >= 5)
	{
		const int32 DominatingTeam = (RedScore > BlueScore) ? 0 : 1;
		if (!bHasBroadcastTeamDominating)
		{
			BroadcastDomination(DominatingTeam);
			bHasBroadcastTeamDominating = true;
		}
	}
	else
	{
		bHasBroadcastTeamDominating = false;
	}

	PreviousRedScore = RedScore;
	PreviousBlueScore = BlueScore;
}


void AUWipeoutGame::BroadcastDomination(int32 DominatingTeamIndex) { BP_OnDomination(DominatingTeamIndex); }
void AUWipeoutGame::BroadcastTakesLead(int32 LeadingTeamIndex)     { BP_OnTakesLead(LeadingTeamIndex); }


// ============================================================================
// ACHIEVEMENTS
// ============================================================================

void AUWipeoutGame::CheckForHighDamageCarry(int32 WinnerTeamIndex)
{
	if (WinnerTeamIndex == INDEX_NONE) return;

	float TeamTotalDamage = (WinnerTeamIndex == 0) ? Team0RoundDamage : Team1RoundDamage;
	if (TeamTotalDamage <= 0) return;

	for (auto& DamagePair : PlayerRoundDamage)
	{
		AUTPlayerState* PS = DamagePair.Key.Get();
		float PlayerDamage = DamagePair.Value;
		if (PS && PS->Team && PS->Team->TeamIndex == WinnerTeamIndex && PlayerDamage > 440.f)
		{
			float DamagePct = (PlayerDamage / TeamTotalDamage) * 100.0f;
			if (DamagePct >= HighDamageCarryThreshold)
			{
				RecordHighDamageCarry(PS, DamagePct);
			}
		}
	}
}


void AUWipeoutGame::RecordHighDamageCarry(AUTPlayerState* PlayerState, float DamagePercentage)
{
	if (!PlayerState) return;
	UE_LOG(LogGameMode, Log, TEXT("Wipeout High Damage Carry: %s (%.1f%%)"), *PlayerState->PlayerName, DamagePercentage);
	OnPlayerHighDamageCarry.Broadcast(PlayerState, DamagePercentage);
}


// ============================================================================
// SERVER MANAGEMENT (from TeamArena)
// ============================================================================

void AUWipeoutGame::HandleServerManagement()
{
	HandleInstanceCleanup();
	HandleMapVoting();
	CheckBotCount();

	if (UTIsHandlingReplays())
	{
		UDemoNetDriver* DemoNetDriver = GetWorld()->DemoNetDriver;
		if (DemoNetDriver != nullptr && DemoNetDriver->ReplayStreamer.IsValid())
		{
			UTGameState->ReplayID = DemoNetDriver->ReplayStreamer->GetReplayID();
		}
	}
}


void AUWipeoutGame::HandleMapVoting()
{
	if (MatchState == MatchState::MapVoteHappening)
	{
		if (GetWorld()->GetNetMode() != NM_Standalone)
		{
			UTGameState->VoteTimer--;
			if (UTGameState->VoteTimer <= 0) UTGameState->VoteTimer = 0;
		}

		TArray<AUTReplicatedMapInfo*> Best;
		for (int32 i = 0; i < UTGameState->MapVoteList.Num(); i++)
		{
			if (UTGameState->MapVoteList[i]->VoteCount > 0)
			{
				if (Best.Num() == 0 || Best[0]->VoteCount < UTGameState->MapVoteList[i]->VoteCount)
				{
					Best.Empty();
					Best.Add(UTGameState->MapVoteList[i]);
				}
			}
		}

		if (Best.Num() > 0)
		{
			int32 Target = int32(float(GetNumPlayers()) * 0.5);
			if (Best[0]->VoteCount > Target) TallyMapVotes();
		}
	}
}


void AUWipeoutGame::HandleInstanceCleanup()
{
	if (GetWorld() == nullptr || GetWorld()->WorldType != EWorldType::Game) return;
	if (!HasAuthority() && GetNetMode() != NM_Standalone) return;

	if (IsGameInstanceServer() && LobbyBeacon)
	{
		if (GetWorld()->GetTimeSeconds() - LastLobbyUpdateTime >= 10.0f)
		{
			UpdateLobbyMatchStats();
		}

		if (!bDedicatedInstance)
		{
			if (!HasMatchStarted())
			{
				if (GetWorld()->GetRealTimeSeconds() > LobbyInitialTimeoutTime && NumPlayers <= 0 &&
					(GetNetDriver() == NULL || GetNetDriver()->ClientConnections.Num() == 0))
				{
					ShutdownGameInstance();
					return;
				}
			}
			else if (NumPlayers <= 0)
			{
				ShutdownGameInstance();
				return;
			}
		}
	}
	else
	{
		if (NumPlayers <= 0 && NumSpectators <= 0 && HasMatchStarted())
		{
			EmptyServerTime++;
			if (EmptyServerTime >= AutoRestartTime)
			{
				TravelToNextMap();
				return;
			}
		}
		else
		{
			EmptyServerTime = 0;
		}
	}

	if (LobbyBeacon && LobbyBeacon->GetNetConnection()->State == EConnectionState::USOCK_Closed)
	{
		if (!bDedicatedInstance && NumPlayers <= 0 && MatchState != MatchState::WaitingToStart)
		{
			FPlatformMisc::RequestExit(false);
			return;
		}
		RecreateLobbyBeacon();
	}
}


// ============================================================================
// LOGOUT
// ============================================================================

void AUWipeoutGame::Logout(AController* Exiting)
{
	if (bCompetitiveAutoPause && IsMatchInProgress() && !HasMatchEnded() && !GetWorld()->IsPaused())
	{
		if (Exiting)
		{
			AUTPlayerState* ExitingPS = Cast<AUTPlayerState>(Exiting->PlayerState);
			if (ExitingPS && !ExitingPS->bIsABot && !ExitingPS->bOnlySpectator)
			{
				UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Player %s disconnected. Pausing match."), *ExitingPS->PlayerName);
				SetPause(nullptr);
			}
		}
	}

	if (Exiting)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(Exiting->PlayerState);
		if (PS)
		{
			// Clean up all tracking maps to prevent stale pointer access
			PlayerRoundDamage.Remove(PS);
			PlayerDeathCounts.Remove(PS);
			FinalLifeAnnouncedPlayers.Remove(PS);
			CancelPendingRespawn(PS);
			SpawnProtectedUntil.Remove(PS);
		}
	}

	Super::Logout(Exiting);
}


// ============================================================================
// BP_RESTART ROUND (admin tool)
// ============================================================================

void AUWipeoutGame::BP_RestartCurrentRound()
{
	if (!HasAuthority()) return;
	if (bWarmupMode || !HasMatchStarted()) return;

	UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Admin restarting current round"));

	StopOvertime();
	CancelAllPendingRespawns();
	GetWorldTimerManager().ClearTimer(TH_RoundEndDelay);
	GetWorldTimerManager().ClearTimer(InitialWinCheckHandle);

	bRoundInProgress = false;
	bInSuddenDeath = false;
	bSuddenDeathPending = false;
	RoundEndTimeSeconds = 0.f;
	LastRoundWinningTeamIndex = INDEX_NONE;
	Team0DeathCount = 0;
	Team1DeathCount = 0;
	PlayerDeathCounts.Empty();
	SpawnProtectedUntil.Empty();
	LinkHealAccumulator.Empty();

	ResetPlayersForNewRound();
	CleanupWorldForNewRound();
	ResetSpawnSelectionForNewRound();
	SelectSpawnLayoutForRound();

	StartIntermission(4);
}


void AUWipeoutGame::BP_SetTeamScores(int32 RedScore, int32 BlueScore)
{
	if (!HasAuthority()) return;
	if (!Teams.IsValidIndex(0) || !Teams.IsValidIndex(1)) return;

	Teams[0]->Score = FMath::Max(0, RedScore);
	Teams[1]->Score = FMath::Max(0, BlueScore);
	Teams[0]->ForceNetUpdate();
	Teams[1]->ForceNetUpdate();

	PreviousRedScore = Teams[0]->Score;
	PreviousBlueScore = Teams[1]->Score;
	bHasBroadcastTeamDominating = false;
}

// ─── ResetPickupTimers ────────────────────────────────────────────────
// At round start, hide Shield Belt and UDamage pickups and set them to
// appear at specific times into the round (Belt=60s, Amp=90s).
// Weapon bases are left alone so players can pick up ammo immediately.
void AUWipeoutGame::ResetPickupTimers()
{
	for (TActorIterator<AUTPickupInventory> It(GetWorld()); It; ++It)
	{
		AUTPickupInventory* Pickup = *It;
		if (!Pickup || Pickup->IsPendingKillPending()) continue;

		UClass* InvType = Pickup->GetInventoryType();
		if (!InvType) continue;

		FString ClassName = InvType->GetName();

		float DelaySeconds = 0.f;

		// Shield Belt — spawn 60s into the round
		if (ClassName.Contains(TEXT("ShieldBelt")))
		{
			DelaySeconds = 60.f;
		}
		// UDamage/Amp — spawn 90s into the round
		else if (ClassName.Contains(TEXT("UDamage")) || ClassName.Contains(TEXT("Amp")))
		{
			DelaySeconds = 90.f;
		}
		else
		{
			continue; // Don't touch other pickups
		}

		// Hide the pickup and set it to respawn after the delay
		Pickup->StartSleeping();
		Pickup->RespawnTime = DelaySeconds;
		// Force the respawn timer — WakeUp will be called when timer expires
		GetWorldTimerManager().SetTimer(
			Pickup->WakeUpTimerHandle, Pickup,
			&AUTPickup::WakeUp, DelaySeconds, false);

		UE_LOG(LogGameMode, Log, TEXT("WipeoutGame: %s will spawn in %.0fs"), *ClassName, DelaySeconds);
	}

	// Siphon pickup — same 90s timer as Amp
	if (SiphonPickup && !SiphonPickup->IsPendingKillPending())
	{
		SiphonPickup->StartSleeping();
		SiphonPickup->RespawnTime = 90.f;
		GetWorldTimerManager().SetTimer(
			SiphonPickup->WakeUpTimerHandle, SiphonPickup,
			&AUTPickup::WakeUp, 90.f, false);
		UE_LOG(LogGameMode, Log, TEXT("WipeoutGame: Siphon will spawn in 90s"));
	}
}


// ─── ResolveShieldBeltSubstitution ──────────────────────────────────────
// Called from BeginPlay after all actors have been CheckRelevance'd.
// If the map had no ShieldBelt pickup, spawn a fresh ArmorBase pickup at the
// vest's location with ShieldBelt as its inventory type (Showdown pattern).
void AUWipeoutGame::ResolveShieldBeltSubstitution()
{
	if (bMapHasShieldBelt)
	{
		// Map already has a belt — destroy the stashed vest
		if (PendingVestPickup && !PendingVestPickup->IsPendingKillPending())
		{
			UE_LOG(LogGameMode, Log, TEXT("WipeoutGame: Map has ShieldBelt — removing stashed vest pickup"));
			PendingVestPickup->Destroy();
		}
		PendingVestPickup = nullptr;
		return;
	}

	if (!PendingVestPickup)
	{
		UE_LOG(LogGameMode, Log, TEXT("WipeoutGame: Map has no ShieldBelt and no vest — nothing to substitute"));
		return;
	}

	// No ShieldBelt on this map — swap the vest's inventory type to ShieldBelt.
	// The vest is already a valid AUTPickupInventory in the world; SetInventoryType
	// updates the replicated InventoryType, rebuilds the mesh, and handles respawn.
	TSubclassOf<AUTInventory> ShieldBeltClass = LoadClass<AUTInventory>(
		nullptr, TEXT("/Game/RestrictedAssets/Pickups/Armor/Armor_ShieldBelt.Armor_ShieldBelt_C"));
	if (ShieldBeltClass)
	{
		PendingVestPickup->SetInventoryType(ShieldBeltClass);
		bMapHasShieldBelt = true;
		UE_LOG(LogGameMode, Log, TEXT("WipeoutGame: Converted vest to ShieldBelt at %s"),
			*PendingVestPickup->GetActorLocation().ToString());
	}
	else
	{
		UE_LOG(LogGameMode, Warning, TEXT("WipeoutGame: Failed to load Armor_ShieldBelt class"));
	}

	PendingVestPickup = nullptr;
}


// ─── SpawnSiphonPickup ─────────────────────────────────────────────────
// Finds the highest-Z sniper weapon base on the map and spawns a
// PowerupBase pickup there with Siphon as the inventory type.
void AUWipeoutGame::SpawnSiphonPickup()
{
	SiphonPickup = nullptr;

	// Find the highest sniper weapon base
	FVector BestLoc = FVector::ZeroVector;
	FRotator BestRot = FRotator::ZeroRotator;
	float HighestZ = -FLT_MAX;
	AUTPickupWeapon* BestSniperPickup = nullptr;

	for (TActorIterator<AUTPickupWeapon> It(GetWorld()); It; ++It)
	{
		AUTPickupWeapon* WP = *It;
		if (!WP || !WP->WeaponType) continue;

		FString WeaponName = WP->WeaponType->GetName();
		if (WeaponName.Contains(TEXT("Sniper")))
		{
			float Z = WP->GetActorLocation().Z;
			if (Z > HighestZ)
			{
				HighestZ = Z;
				BestLoc = WP->GetActorLocation();
				BestRot = WP->GetActorRotation();
				BestSniperPickup = WP;
			}
		}
	}

	if (!BestSniperPickup)
	{
		UE_LOG(LogGameMode, Log, TEXT("Wipeout: No sniper weapon base found — skipping Siphon pickup"));
		return;
	}

	// Remove the sniper weapon pickup we're replacing
	BestSniperPickup->Destroy();

	// Try BP class first, fall back to hardcoded PowerupBase_C
	TSubclassOf<AUTPickupInventory> SpawnClass = SiphonPickupClass;
	if (!SpawnClass)
	{
		SpawnClass = LoadClass<AUTPickupInventory>(
			nullptr, TEXT("/Game/RestrictedAssets/Pickups/Powerups/PowerupBase.PowerupBase_C"));
	}
	if (!SpawnClass)
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: No pickup class available for Siphon"));
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	UE_LOG(LogGameMode, Warning, TEXT("Wipeout: Spawning Siphon class=%s at %s"), *SpawnClass->GetPathName(), *BestLoc.ToString());
	SiphonPickup = GetWorld()->SpawnActor<AUTPickupInventory>(SpawnClass, BestLoc, BestRot, Params);

	if (SiphonPickup)
	{
		// If using fallback PowerupBase (not custom BP), set inventory type
		if (!SiphonPickupClass)
		{
			SiphonPickup->SetInventoryType(AUTSiphonPowerup::StaticClass());
		}

		// Register the Siphon's overlay effect with the GameState
		AUTGameState* GS = GetGameState<AUTGameState>();
		if (GS)
		{
			const AUTSiphonPowerup* SiphonCDO = AUTSiphonPowerup::StaticClass()->GetDefaultObject<AUTSiphonPowerup>();
			if (SiphonCDO)
			{
				SiphonCDO->AddOverlayMaterials(GS);
			}
		}

		UE_LOG(LogGameMode, Log, TEXT("Wipeout: Spawned Siphon pickup at sniper location %s (Z=%.0f)"),
			*BestLoc.ToString(), HighestZ);
	}
	else
	{
		UE_LOG(LogGameMode, Warning, TEXT("Wipeout: SpawnActor failed for Siphon pickup at %s (class=%s)"),
			*BestLoc.ToString(), *SpawnClass->GetPathName());
	}
}

// --- Mod.ini-gated match-host pause (see NCPlusHostPause.h) ---
#include "NCPlusHostPause.h"

bool AUWipeoutGame::AllowPausing(APlayerController* PC)
{
	// Stock permissions (rcon admin / listen with no remotes) are preserved; this ADDS
	// the ?HostId= match host ([NetcodePlus] bAllowHostPause) AND the two bot-designated
	// team captains ([NetcodePlus] bAllowCaptainPause, ?Captains=) — see NCPlusHostPause.
	return Super::AllowPausing(PC) || NCPlusHostPause::MayPause(PC, this);
}

bool AUWipeoutGame::ClearPause()
{
	// Host/rcon unpause: hold behind a short server-only resume countdown
	// (Mod.ini [NetcodePlus] UnpauseCountdownSec). Only engages while actually paused.
	if (NCPlusHostPause::DeferUnpauseForCountdown(this))
	{
		return false;
	}
	return Super::ClearPause();
}
