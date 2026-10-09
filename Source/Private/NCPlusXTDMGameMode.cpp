#include "NCPlusXTDMGameMode.h"
#include "NCPlusXTDMRules.h"
#include "UnrealTournament.h"
#include "NCPlusXTDMReplicator.h"
#include "NCPlusXTDMHUD.h"
#include "NCPlusXTDMMessage.h"
#include "NCReadyUp.h"
#include "NCPlusVersionGate.h"
#include "NCAccuracyStatsReplicator.h"
#include "NPPlayerController.h"
#include "TeamArenaCharacter.h"
#include "UTCharacter.h"
#include "UTGameState.h"
#include "UTGameSession.h"
#include "UTPlayerState.h"
#include "UTPlayerController.h"
#include "UTBotPlayer.h"
#include "UTTeamInfo.h"
#include "UTTeamPlayerStart.h"
#include "UTPickup.h"
#include "UTDroppedPickup.h"
#include "UTInventory.h"
#include "UTWeapon.h"
#include "EngineUtils.h"
#include "Components/CapsuleComponent.h"
#include "Kismet/GameplayStatics.h"

ANCPlusXTDMGameMode::ANCPlusXTDMGameMode(const FObjectInitializer& OI)
	: Super(OI), TeamSize(2), bAllowIncompleteTeams(true), XTDMSpawnProtectionTime(0.f),
	  XTDMState(nullptr), FinalWinningTeam(255), bRosterLocked(false),
	  bConfigurationValid(false), bStartsCached(false)
{
	DisplayName = NSLOCTEXT("NCPlus", "XTDMName", "NetcodePlus xTDM Instagib");
	NumTeams = 4;
	bAllowURLTeamCountOverride = false;
	bBalanceTeams = false;
	bAnnounceTeam = false;
	bUseTeamStarts = false;
	bHighScorerPerTeamBasis = true;
	TeamColors = { FLinearColor(1, 0, 0), FLinearColor(0, 0, 1), FLinearColor(0, 1, 0), FLinearColor(1, 1, 0) };
	TeamNames[3] = NSLOCTEXT("NCPlus", "XTDMYellow", "Yellow");
	DefaultMaxPlayers = 8;
	TimeLimit = 15; // Stock InitGame converts minutes to seconds once.
	GoalScore = 0;
	MercyScore = 0;
	bAllowOvertime = true;
	bIsInstagib = true;
	TeamDamagePct = 0.f;
	bScoreSuicides = true;
	bScoreTeamKills = true;
	bForceRespawn = true;
	RespawnWaitTime = 1.f;
	ForceRespawnTime = 0.f; // This is additional to RespawnWaitTime in stock UT.
	bHasRespawnChoices = false;
	bPlayersStartWithArmor = false;
	bTrackKillAssists = false;
	bUseProtoTeams = false;
	bAllowPickupAnnouncements = false;
	bDelayedStart = true;
	bRequireFull = false;
	bRequireReady = true;
	bForceNoBots = false;
	bIsVSAI = false;
	BotFillCount = 0;
	bRankedSession = false;
	bUseMatchmakingSession = false;
	bSkipReportingMatchResults = true;
	bDisableCloudStats = true;
	bRecordReplays = false;
	HUDClass = ANCPlusXTDMHUD::StaticClass();
	GameMessageClass = UNCPlusXTDMGameMessage::StaticClass();
	VictoryMessageClass = UNCPlusXTDMVictoryMessage::StaticClass();
	PlayerControllerClass = ANPPlayerController::StaticClass();
	PlayerStateClass = AUTPlayerState::StaticClass();
	TeamClass = AUTTeamInfo::StaticClass();
}

void ANCPlusXTDMGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
	// Reject unsupported session modes before stock InitGame selects its GameSession class.
	if (UGameplayStatics::HasOption(Options, TEXT("Ranked")) ||
		UGameplayStatics::GetIntOption(Options, TEXT("MatchmakingSession"), 0) != 0)
	{
		ErrorMessage = TEXT("xTDM does not support stock two-team ranked or matchmaking sessions.");
		return;
	}
	if (UGameplayStatics::GetIntOption(Options, TEXT("VSAI"), bIsVSAI ? 1 : 0) != 0)
	{
		ErrorMessage = TEXT("xTDM does not support stock two-team VSAI; use BotFill or Bots for four-team bots.");
		return;
	}
	TeamSize = FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("XTDMTeamSize"), TeamSize), 2, 4);
	bAllowIncompleteTeams = UGameplayStatics::GetIntOption(Options, TEXT("XTDMAllowIncompleteTeams"), bAllowIncompleteTeams ? 1 : 0) != 0;
	if (!ParseDraft(Options, ErrorMessage))
	{
		return;
	}
	// No constructor-time content loads: ordinary UT/NCP modes do not acquire these package dependencies.
	UClass* SafeGameState = LoadClass<AUTGameState>(nullptr,
		TEXT("/Game/Blueprints/XTDM/BP_NCP_XTDMGameState.BP_NCP_XTDMGameState_C"));
	UFunction* Highlights = SafeGameState ? SafeGameState->FindFunctionByName(TEXT("UpdateHighlights")) : nullptr;
	if (!SafeGameState || SafeGameState->GetSuperClass() != AUTGameState::StaticClass() || !Highlights || Highlights->Script.Num() == 0)
	{
		ErrorMessage = TEXT("xTDM requires BP_NCP_XTDMGameState directly derived from stock UTGameState with its UpdateHighlights override (no parent call).");
		return;
	}
	GameStateClass = SafeGameState;
	if (!InstagibCharacterClass)
	{
		InstagibCharacterClass = LoadClass<APawn>(nullptr, TEXT("/Game/Blueprints/Netcode/IGCharacterFootsteps.IGCharacterFootsteps_C"));
	}
	if (!InstagibRifleClass)
	{
		InstagibRifleClass = LoadClass<AUTInventory>(nullptr, TEXT("/Game/Blueprints/Netcode/N+InstagibRifle.N+InstagibRifle_C"));
	}
	if (!InstagibCharacterClass || !InstagibCharacterClass->IsChildOf(ATeamArenaCharacter::StaticClass()) ||
		!InstagibRifleClass || !InstagibRifleClass->IsChildOf(AUTWeapon::StaticClass()))
	{
		ErrorMessage = TEXT("xTDM cannot load its NetcodePlus instagib character and rifle classes.");
		return;
	}
	DefaultPawnClass = InstagibCharacterClass;
	PlayerControllerClass = ANPPlayerController::StaticClass();
	PlayerStateClass = AUTPlayerState::StaticClass();
	TeamClass = AUTTeamInfo::StaticClass();
	NumTeams = 4;
	bAllowURLTeamCountOverride = false;
	DefaultMaxPlayers = 4 * TeamSize;
	Super::InitGame(MapName, Options, ErrorMessage);
	if (!ErrorMessage.IsEmpty())
	{
		return;
	}
	if (Teams.Num() != 4 || !Teams[0] || !Teams[1] || !Teams[2] || !Teams[3])
	{
		ErrorMessage = TEXT("xTDM failed to create its four stock UTTeamInfo actors.");
		return;
	}
	if (GameSession)
	{
		GameSession->MaxPlayers = DefaultMaxPlayers;
	}
	// These are mode invariants, not client-selectable URL alternatives.
	bBalanceTeams = false;
	bUseTeamStarts = false;
	bAnnounceTeam = false;
	bHasRespawnChoices = false;
	// Stock standalone auto-fill may replace an explicit zero. Keep the requested
	// target, then cap both warmup and match populations to this mode's seats.
	if (UGameplayStatics::HasOption(Options, TEXT("Bots")))
	{
		BotFillCount = FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("Bots"), 0), -1, DefaultMaxPlayers - 1) + 1;
	}
	else if (UGameplayStatics::HasOption(Options, TEXT("BotFill")))
	{
		BotFillCount = UGameplayStatics::GetIntOption(Options, TEXT("BotFill"), BotFillCount);
	}
	BotFillCount = NCPlusXTDMRules::ClampBotFillTarget(BotFillCount, TeamSize, bForceNoBots);
	WarmupFillCount = BotFillCount;
	bRankedSession = false;
	bUseMatchmakingSession = false;
	bSkipReportingMatchResults = true;
	bRecordReplays = false;
	bIsInstagib = true;
	bForceRespawn = true;
	ForceRespawnTime = 0.f;
	RespawnWaitTime = FMath::Max(0.f, RespawnWaitTime);
	XTDMSpawnProtectionTime = FMath::Max(0.f, XTDMSpawnProtectionTime);
	bPlayersStartWithArmor = false;
	bUseProtoTeams = false;
	StartingArmorClass = nullptr;
	DefaultInventory.Reset();
	DefaultInventory.Add(InstagibRifleClass);
	// Suppress the stock two-team domination message; scoring itself remains stock TDM.
	bHasBroadcastDominating = true;
	NCReadyUp::Initialize(this);
	AddMutatorClass(ANCReadyUpMutator::StaticClass());
	bConfigurationValid = true;
}

void ANCPlusXTDMGameMode::InitGameState()
{
	Super::InitGameState();
	if (UTGameState)
	{
		UTGameState->SpawnProtectionTime = XTDMSpawnProtectionTime;
		UTGameState->SetRespawnWaitTime(RespawnWaitTime);
		UTGameState->ForceRespawnTime = ForceRespawnTime;
		UTGameState->bRankedSession = false;
	}
	EnsureStatusActors();
}

void ANCPlusXTDMGameMode::BeginPlay()
{
	Super::BeginPlay();
	CachePlayerStarts();
	EnsureStatusActors();
}

void ANCPlusXTDMGameMode::EnsureStatusActors()
{
	if (!HasAuthority() || !bConfigurationValid || !GetWorld())
	{
		return;
	}
	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	if (!IsValid(XTDMState))
	{
		XTDMState = GetWorld()->SpawnActor<ANCPlusXTDMReplicator>(ANCPlusXTDMReplicator::StaticClass(), Params);
		if (XTDMState)
		{
			XTDMState->TeamSize = TeamSize;
		}
	}
	if (!ReadyState.IsValid())
	{
		ReadyState = ANCReadyUpState::Find(GetWorld());
		if (!ReadyState.IsValid())
		{
			ReadyState = GetWorld()->SpawnActor<ANCReadyUpState>(ANCReadyUpState::StaticClass(), Params);
		}
	}
	if (!AccuracyState.IsValid()) AccuracyState = ANCAccuracyStatsReplicator::EnsureSpawned(this);
}

FString ANCPlusXTDMGameMode::PlayerIdentity(const AUTPlayerState* PS)
{
	// Names and per-session PlayerId values are not authenticated reconnect identities.
	return PS && !PS->bIsABot && PS->UniqueId.IsValid() ? PS->UniqueId.ToString().ToLower() : FString();
}

bool ANCPlusXTDMGameMode::ParseDraft(const FString& Options, FString& ErrorMessage)
{
	DraftTeams.Reset();
	Reservations.Reset();
	TArray<FString> Entries;
	UGameplayStatics::ParseOption(Options, TEXT("PugTeams")).ParseIntoArray(Entries, TEXT(","), true);
	int32 Counts[4] = { 0, 0, 0, 0 };
	for (const FString& Entry : Entries)
	{
		FString Id, TeamText;
		if (!Entry.Split(TEXT(":"), &Id, &TeamText) || Id.IsEmpty() || TeamText.Len() != 1 || TeamText[0] < TEXT('0') || TeamText[0] > TEXT('3'))
		{
			ErrorMessage = TEXT("Invalid xTDM PugTeams entry; use authenticated-id:0 through authenticated-id:3.");
			return false;
		}
		Id = Id.ToLower();
		const uint8 Team = uint8(TeamText[0] - TEXT('0'));
		if (DraftTeams.Contains(Id) || ++Counts[Team] > TeamSize)
		{
			ErrorMessage = TEXT("xTDM PugTeams contains a duplicate identity or exceeds the configured team size.");
			return false;
		}
		DraftTeams.Add(Id, Team);
	}
	return true;
}

int32 ANCPlusXTDMGameMode::OccupiedOrReservedSeats(uint8 Team, const AUTPlayerState* Requester) const
{
	int32 Count = 0;
	TSet<FString> Counted;
	const FString RequestId = PlayerIdentity(Requester);
	if (UTGameState)
	{
		for (APlayerState* BasePS : UTGameState->PlayerArray)
		{
			const AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
			if (PS && PS != Requester && !PS->bOnlySpectator && !PS->bIsInactive && PS->GetTeamNum() == Team)
			{
				++Count;
				const FString Id = PlayerIdentity(PS);
				if (!Id.IsEmpty()) Counted.Add(Id);
			}
		}
	}
	for (const auto& Entry : DraftTeams)
	{
		if (Entry.Value == Team && Entry.Key != RequestId && !Counted.Contains(Entry.Key))
		{
			++Count;
			Counted.Add(Entry.Key);
		}
	}
	for (const auto& Entry : Reservations)
	{
		if (Entry.Value.Team == Team && Entry.Key != RequestId && !Counted.Contains(Entry.Key))
		{
			++Count;
			Counted.Add(Entry.Key);
		}
	}
	return Count;
}

bool ANCPlusXTDMGameMode::IsTeamLocked() const
{
	return bRosterLocked || HasMatchStarted() || GetMatchState() == MatchState::CountdownToBegin ||
		(ReadyState.IsValid() && ReadyState->bCountdownLocked);
}

bool ANCPlusXTDMGameMode::IsOpenRoster() const
{
	return bAllowIncompleteTeams && DraftTeams.Num() == 0;
}

AUTBotPlayer* ANCPlusXTDMGameMode::FindBotOnTeam(uint8 Team) const
{
	if (!GetWorld()) return nullptr;
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AUTBotPlayer* Bot = Cast<AUTBotPlayer>(It->Get());
		const AUTPlayerState* PS = Bot ? Cast<AUTPlayerState>(Bot->PlayerState) : nullptr;
		if (IsValid(Bot) && PS && !PS->bOnlySpectator && !PS->bIsInactive && PS->GetTeamNum() == Team)
		{
			return Bot;
		}
	}
	return nullptr;
}

uint8 ANCPlusXTDMGameMode::PickHumanJoinTeam(AUTPlayerState* PS, uint8 RequestedTeam) const
{
	int32 Seats[4];
	int32 RemovableBots[4];
	for (uint8 Team = 0; Team < 4; ++Team)
	{
		Seats[Team] = OccupiedOrReservedSeats(Team, PS);
		RemovableBots[Team] = FindBotOnTeam(Team) ? 1 : 0;
	}
	return NCPlusXTDMRules::PickHumanTeam(Seats, RemovableBots, TeamSize, RequestedTeam);
}

uint8 ANCPlusXTDMGameMode::PickBalancedTeam(AUTPlayerState* PS, uint8 RequestedTeam)
{
	int32 Seats[4];
	for (uint8 Team = 0; Team < 4; ++Team)
	{
		Seats[Team] = OccupiedOrReservedSeats(Team, PS);
	}
	return NCPlusXTDMRules::PickSmallestTeam(Seats, TeamSize, RequestedTeam);
}

AUTBotPlayer* ANCPlusXTDMGameMode::AddBot(uint8 TeamNum)
{
	if (!HasAuthority() || !bConfigurationValid || !UTGameState || bForceNoBots ||
		!NCPlusXTDMRules::CanAddBot(IsTeamLocked(), bAllowIncompleteTeams, DraftTeams.Num() > 0))
	{
		return nullptr;
	}
	const uint8 Team = PickBalancedTeam(nullptr, TeamNum);
	if (Team >= 4) return nullptr;
	AUTBotPlayer* Bot = Super::AddBot(Team);
	const AUTPlayerState* PS = IsValid(Bot) ? Cast<AUTPlayerState>(Bot->PlayerState) : nullptr;
	if (!PS || PS->bOnlySpectator || !Teams.Contains(PS->Team))
	{
		if (IsValid(Bot)) Bot->Destroy();
		return nullptr;
	}
	return Bot;
}

void ANCPlusXTDMGameMode::CheckBotCount()
{
	if (!HasAuthority() || !bConfigurationValid || !UTGameState || HasMatchEnded()) return;
	BotFillCount = NCPlusXTDMRules::ClampBotFillTarget(BotFillCount, TeamSize, bForceNoBots || DraftTeams.Num() > 0);
	WarmupFillCount = BotFillCount;
	if (NumPlayers + NumBots > BotFillCount)
	{
		// Only use stock's removal branch, which removes at most one bot and
		// respects its combat/score-leader policy. Its add loop has no failure bound.
		Super::CheckBotCount();
		return;
	}
	for (int32 Attempt = 0; Attempt < 4 * TeamSize && NumPlayers + NumBots < BotFillCount; ++Attempt)
	{
		const int32 PreviousBots = NumBots;
		if (!AddBot() || NumBots <= PreviousBots) break;
	}
}

bool ANCPlusXTDMGameMode::ChangeTeam(AController* Player, uint8 NewTeam, bool bBroadcast)
{
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || PS->bOnlySpectator || !bConfigurationValid || Teams.Num() != 4)
	{
		return false;
	}
	const FString Id = PlayerIdentity(PS);
	const bool bBot = PS->bIsABot || Cast<AUTBotPlayer>(Player) != nullptr;
	const bool bOpenRoster = IsOpenRoster();
	const bool bCanReplaceBot = !bBot && DraftTeams.Num() == 0 && (bOpenRoster || !IsTeamLocked());
	const uint8* DraftTeam = DraftTeams.Find(Id);
	const FTeamReservation* Reservation = Reservations.Find(Id);
	if (Reservation && Reservation->Player.IsValid() && Reservation->Player.Get() != PS &&
		!Reservation->Player->bIsInactive && !Reservation->Player->bOnlySpectator)
	{
		return false; // A live connection still owns this authenticated seat.
	}
	if (IsTeamLocked() && PS->Team)
	{
		if (NewTeam != 255 && NewTeam != PS->GetTeamNum()) return false;
		NewTeam = PS->GetTeamNum();
		// An inactive reconnect PS can bring back an old team after its casual
		// seat was filled. Existing membership is not proof that it still fits.
		if (OccupiedOrReservedSeats(NewTeam, PS) >= TeamSize)
		{
			if (!bOpenRoster || bBot) return false;
			NewTeam = PickHumanJoinTeam(PS, NewTeam);
		}
	}
	else
	{
		if (bBot && (bForceNoBots || !NCPlusXTDMRules::CanAddBot(IsTeamLocked(), bAllowIncompleteTeams, DraftTeams.Num() > 0))) return false;
		if (IsTeamLocked() && !Reservation && !DraftTeam && !bOpenRoster) return false;
		if (DraftTeam)
		{
			NewTeam = *DraftTeam;
		}
		else if (IsTeamLocked() && Reservation && !bOpenRoster)
		{
			NewTeam = Reservation->Team;
		}
		else if (DraftTeams.Num() > 0)
		{
			return false; // An explicit draft reserves the roster; unlisted arrivals spectate.
		}
		else
		{
			NewTeam = bCanReplaceBot ? PickHumanJoinTeam(PS, NewTeam) : PickBalancedTeam(PS, NewTeam);
		}
	}
	if (NewTeam >= 4 || !Teams[NewTeam]) return false;
	if (OccupiedOrReservedSeats(NewTeam, PS) == TeamSize && bCanReplaceBot)
	{
		// Login assigns a team before stock PostLogin trims bots. Make the seat
		// available here so a human is not turned into a spectator first.
		if (AUTBotPlayer* Bot = FindBotOnTeam(NewTeam))
		{
			// AUTBot::Destroyed suicides a possessed character. Administrative
			// replacement must not deduct team score or decide sudden death.
			APawn* BotPawn = Bot->GetPawn();
			Bot->UnPossess(); // SetPawn(nullptr) also clears AUTBot::UTChar.
			if (IsValid(BotPawn)) BotPawn->Destroy();
			Bot->Destroy();
		}
	}
	if (OccupiedOrReservedSeats(NewTeam, PS) >= TeamSize)
	{
		return false;
	}
	if (PS->Team != Teams[NewTeam] && !MovePlayerToTeam(Player, PS, NewTeam))
	{
		return false;
	}
	PS->bPendingTeamSwitch = false;
	PS->ForceNetUpdate();
	if (!Id.IsEmpty())
	{
		FTeamReservation& Entry = Reservations.FindOrAdd(Id);
		Entry.Team = NewTeam;
		Entry.Player = PS;
	}
	if (ReadyState.IsValid()) ReadyState->RefreshEligibility();
	RefreshPlayerStatus(Player);
	return true;
}

APlayerController* ANCPlusXTDMGameMode::Login(UPlayer* NewPlayer, ENetRole Role, const FString& Portal,
	const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	APlayerController* PC = Super::Login(NewPlayer, Role, Portal, Options, UniqueId, ErrorMessage);
	AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
	if (PS && !PS->bOnlySpectator && !PS->Team)
	{
		// Before PostLogin counts players and starts them: no pawn may enter without a valid seat.
		PS->bOnlySpectator = true;
		PS->bIsSpectator = true;
		PC->ChangeState(NAME_Spectating);
	}
	return PC;
}

void ANCPlusXTDMGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	EnsureStatusActors();
	if (ReadyState.IsValid()) ReadyState->RefreshEligibility();
	RefreshPlayerStatus(NewPlayer);
}

void ANCPlusXTDMGameMode::GenericPlayerInitialization(AController* Player)
{
	// PostLogin may replace the newly assigned PS with its inactive reconnect PS.
	// Seamless travel also creates a new PS without going through Login.
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (AUTBotPlayer* Bot = Cast<AUTBotPlayer>(Player))
	{
		if (!PS || PS->bOnlySpectator)
		{
			Bot->Destroy();
			return;
		}
	}
	if (PS && !PS->bOnlySpectator)
	{
		if (!Teams.Contains(PS->Team)) PS->Team = nullptr;
		if (!ChangeTeam(Player, PS->GetTeamNum(), false))
		{
			if (AUTBotPlayer* Bot = Cast<AUTBotPlayer>(Player))
			{
				// Named/asset bots bypass AddBot's preflight. Stock has already
				// counted them, so Destroy must run its normal Logout bookkeeping.
				Bot->Destroy();
				return;
			}
			if (APlayerController* PC = Cast<APlayerController>(Player))
			{
				PlayerSwitchedToSpectatorOnly(PC);
				PC->StartSpectatingOnly();
			}
		}
	}
	Super::GenericPlayerInitialization(Player);
	EnsureStatusActors();
	if (APlayerController* PC = Cast<APlayerController>(Player)) NCPlusVersionGate::SpawnFor(PC);
	RefreshPlayerStatus(Player);
}

bool ANCPlusXTDMGameMode::FindInactivePlayer(APlayerController* PC)
{
	if (!PC || !PC->PlayerState || PC->PlayerState->bOnlySpectator || !PC->PlayerState->UniqueId.IsValid()) return false;
	for (int32 Index = 0; Index < InactivePlayerArray.Num(); ++Index)
	{
		APlayerState* Inactive = InactivePlayerArray[Index];
		if (!IsValid(Inactive))
		{
			InactivePlayerArray.RemoveAt(Index--);
			continue;
		}
		if (!Inactive->UniqueId.IsValid() || Inactive->UniqueId != PC->PlayerState->UniqueId) continue;
		// Match stock UT's duplication workaround for initial-only bIsInactive replication.
		APlayerState* Restored = Inactive->Duplicate();
		if (!Restored) return false;
		APlayerState* NewConnectionPS = PC->PlayerState;
		PC->PlayerState = Restored;
		Restored->SetOwner(PC);
		OverridePlayerState(PC, NewConnectionPS);
		GameState->AddPlayerState(Restored);
		if (XTDMState) XTDMState->RemovePlayer(Cast<AUTPlayerState>(NewConnectionPS));
		InactivePlayerArray.RemoveAt(Index);
		Inactive->Destroy();
		NewConnectionPS->bIsInactive = true;
		NewConnectionPS->SetUniqueId(nullptr);
		NewConnectionPS->Destroy();
		return true;
	}
	return false;
}

void ANCPlusXTDMGameMode::Logout(AController* Exiting)
{
	AUTPlayerState* PS = Exiting ? Cast<AUTPlayerState>(Exiting->PlayerState) : nullptr;
	if (PS)
	{
		const FString Id = PlayerIdentity(PS);
		if (!IsTeamLocked() || IsOpenRoster()) Reservations.Remove(Id);
		else if (FTeamReservation* Entry = Reservations.Find(Id)) Entry->Player.Reset();
		if (XTDMState) XTDMState->RemovePlayer(PS);
	}
	Super::Logout(Exiting);
	if (ReadyState.IsValid()) ReadyState->RefreshEligibility();
}

bool ANCPlusXTDMGameMode::IsRosterComplete() const
{
	int32 Counts[4] = { 0, 0, 0, 0 };
	if (!UTGameState) return false;
	for (APlayerState* BasePS : UTGameState->PlayerArray)
	{
		const AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
		if (PS && !PS->bOnlySpectator && !PS->bIsInactive && PS->GetTeamNum() < 4)
		{
			++Counts[PS->GetTeamNum()];
		}
	}
	return NCPlusXTDMRules::IsFullRoster(Counts, TeamSize);
}

bool ANCPlusXTDMGameMode::ReadyToStartMatch_Implementation()
{
	if (!bConfigurationValid || !UTGameState || GetMatchState() != MatchState::WaitingToStart)
	{
		return false;
	}
	EnsureStatusActors();
	ANCReadyUpState* State = ReadyState.Get();
	if (!State) return false;
	// xTDM always uses NCP ready-up; global host-start and global ready-up settings do not bypass its roster.
	UTGameState->bHaveMatchHost = false;
	for (APlayerState* BasePS : UTGameState->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
		if (PS && PS->bIsMatchHost)
		{
			PS->bIsMatchHost = false;
			PS->ForceNetUpdate();
		}
	}
	State->RefreshEligibility();
	int32 Humans = 0;
	int32 Occupants = 0;
	bool bEveryHumanReady = true;
	for (APlayerState* BasePS : UTGameState->PlayerArray)
	{
		AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
		if (PS && !PS->bOnlySpectator && !PS->bIsInactive)
		{
			if (PS->GetTeamNum() < 4) ++Occupants;
			if (!PS->bIsABot)
			{
				++Humans;
				bEveryHumanReady &= State->IsPlayerReady(PS);
			}
		}
	}
	UTGameState->PlayersNeeded = bAllowIncompleteTeams ? 0 : FMath::Max(0, 4 * TeamSize - Occupants);
	if (!NCPlusXTDMRules::CanStartRoster(bAllowIncompleteTeams, IsRosterComplete(), Humans, bEveryHumanReady))
	{
		if (State->bCountdownLocked) State->CancelCountdown();
		bRosterLocked = false;
		for (auto It = Reservations.CreateIterator(); It; ++It)
		{
			if (!It->Value.Player.IsValid() || It->Value.Player->bIsInactive) It.RemoveCurrent();
		}
		State->RefreshEligibility();
		UTGameState->SetRemainingTime(0.f);
		LastMatchNotReady = GetWorld()->GetTimeSeconds();
		return false;
	}
	if (!State->bCountdownLocked)
	{
		bRosterLocked = true;
		State->LockCountdown(StartDelay);
	}
	const float Remaining = State->GetRemainingCountdown(GetWorld()->GetTimeSeconds());
	UTGameState->SetRemainingTime(Remaining);
	return Remaining <= 0.f;
}

void ANCPlusXTDMGameMode::HandleMatchHasStarted()
{
	bRosterLocked = true;
	bHasBroadcastDominating = true;
	Super::HandleMatchHasStarted();
	EnsureStatusActors();
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		RefreshPlayerStatus(It->Get());
	}
}

void ANCPlusXTDMGameMode::HandlePlayerIntro()
{
	// Stock UT's lineup places only teams 0/1. Keep the ordinary final countdown without that cinematic.
	RemoveAllPawns();
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerState* PS = It->IsValid() ? Cast<AUTPlayerState>(It->Get()->PlayerState) : nullptr;
		if (PS)
		{
			PS->bIsWarmingUp = false;
			PS->RespawnChoiceA = nullptr;
			PS->RespawnChoiceB = nullptr;
		}
	}
	if (UTGameState) UTGameState->ClearLineUp();
	SetMatchState(MatchState::CountdownToBegin);
}

void ANCPlusXTDMGameMode::HandleMatchHasEnded()
{
	// Bypass AUTGameMode's two-team PostMatch lineup. AGameMode still performs session end bookkeeping.
	AGameMode::HandleMatchHasEnded();
	RestoreFinalResult();
}

void ANCPlusXTDMGameMode::CheckCountDown()
{
	if (GetMatchState() != MatchState::CountdownToBegin) return;
	bool bEveryHumanReady = true;
	int32 Humans = 0;
	if (UTGameState && ReadyState.IsValid())
	{
		for (APlayerState* BasePS : UTGameState->PlayerArray)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
			if (PS && !PS->bOnlySpectator && !PS->bIsInactive && !PS->bIsABot)
			{
				++Humans;
				bEveryHumanReady &= ReadyState->IsPlayerReady(PS);
			}
		}
	}
	if (!UTGameState || !ReadyState.IsValid() ||
		!NCPlusXTDMRules::CanStartRoster(bAllowIncompleteTeams, IsRosterComplete(), Humans, bEveryHumanReady))
	{
		if (ReadyState.IsValid()) ReadyState->CancelCountdown();
		bRosterLocked = false;
		SetMatchState(MatchState::WaitingToStart);
		return;
	}
	Super::CheckCountDown();
}

void ANCPlusXTDMGameMode::RefreshPlayerStatus(AController* Player)
{
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!XTDMState || !PS || PS->bOnlySpectator || PS->GetTeamNum() >= 4) return;
	AUTCharacter* Pawn = Cast<AUTCharacter>(Player->GetPawn());
	XTDMState->SetPlayerStatus(PS, Pawn && !Pawn->IsDead(), 0.f);
}

void ANCPlusXTDMGameMode::RestartPlayer(AController* Player)
{
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!bConfigurationValid || !PS || PS->bOnlySpectator || PS->GetTeamNum() >= 4) return;
	// Do not let AGameModeBase reuse an old cached start when every current candidate is blocked.
	Player->StartSpot.Reset();
	Super::RestartPlayer(Player);
	RefreshPlayerStatus(Player);
}

void ANCPlusXTDMGameMode::Killed(AController* Killer, AController* KilledPlayer, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType)
{
	Super::Killed(Killer, KilledPlayer, KilledPawn, DamageType);
	AUTPlayerState* PS = KilledPlayer ? Cast<AUTPlayerState>(KilledPlayer->PlayerState) : nullptr;
	if (XTDMState && PS && UTGameState)
	{
		XTDMState->SetPlayerStatus(PS, false, UTGameState->GetServerWorldTimeSeconds() + UTGameState->GetRespawnWaitTimeFor(PS));
	}
	CheckScore(nullptr); // A suicide/team kill can break a tie at an already reached optional goal.
}

void ANCPlusXTDMGameMode::GiveDefaultInventory(APawn* Pawn)
{
	AUTCharacter* Character = Cast<AUTCharacter>(Pawn);
	if (!Character || GrantedInventory.Contains(Pawn)) return;
	for (auto It = GrantedInventory.CreateIterator(); It; ++It)
	{
		if (!It->IsValid()) It.RemoveCurrent();
	}
	GrantedInventory.Add(Pawn);
	// The existing character BP also lists N+InstagibRifle. Grant the mode's list exactly once per pawn.
	const TArray<TSubclassOf<AUTInventory>> Saved = Character->DefaultCharacterInventory;
	Character->DefaultCharacterInventory.Reset();
	Super::GiveDefaultInventory(Pawn);
	Character->DefaultCharacterInventory = Saved;
	for (TInventoryIterator<AUTWeapon> It(Character); It; ++It) It->bCanThrowWeapon = false;
}

void ANCPlusXTDMGameMode::DiscardInventory(APawn* Other, AController* Killer)
{
	if (AUTCharacter* Character = Cast<AUTCharacter>(Other)) Character->DiscardAllInventory();
}

bool ANCPlusXTDMGameMode::CheckRelevance_Implementation(AActor* Other)
{
	if (Other && (Other->IsA(AUTPickup::StaticClass()) || Other->IsA(AUTDroppedPickup::StaticClass()))) return false;
	return Super::CheckRelevance_Implementation(Other);
}

bool ANCPlusXTDMGameMode::OverridePickupQuery_Implementation(APawn* Other, TSubclassOf<AUTInventory> ItemClass, AActor* Pickup, bool& bAllowPickup)
{
	bAllowPickup = false;
	return true;
}

void ANCPlusXTDMGameMode::CachePlayerStarts()
{
	if (bStartsCached) return;
	bStartsCached = true;
	for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
	{
		if (!It->IsPendingKillPending() && !It->IsA(AUTTeamPlayerStart::StaticClass())) SpawnStarts.Add(*It);
	}
	// Some converted maps have only team starts. Treat those locations as neutral rather than indexing red/blue.
	if (SpawnStarts.Num() == 0)
	{
		for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
		{
			if (!It->IsPendingKillPending()) SpawnStarts.Add(*It);
		}
	}
}

AActor* ANCPlusXTDMGameMode::FindPlayerStart_Implementation(AController* Player, const FString& IncomingName)
{
	// Stock FindPlayerStart can return the prior StartSpot or world origin without rating it.
	// Initial Login needs only a camera location; actual pawn starts always use our current danger snapshot.
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || PS->bOnlySpectator || PS->GetTeamNum() >= 4)
	{
		CachePlayerStarts();
		for (const TWeakObjectPtr<APlayerStart>& Start : SpawnStarts)
		{
			if (Start.IsValid()) return Start.Get();
		}
		return nullptr;
	}
	AActor* Start = ChoosePlayerStart(Player);
	if (Start) LastStartSpot = Start;
	return Start;
}

AActor* ANCPlusXTDMGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
	CachePlayerStarts();
	AUTPlayerState* PS = Player ? Cast<AUTPlayerState>(Player->PlayerState) : nullptr;
	if (!PS || PS->GetTeamNum() >= 4) return nullptr;
	struct FSpawnCandidate { APlayerStart* Start; FVector Location; float Score; };
	TArray<FSpawnCandidate> Candidates;
	TArray<AUTCharacter*> Enemies;
	TArray<AUTCharacter*> Friends;
	FCollisionQueryParams TraceParams(FName(TEXT("XTDMSpawn")), true);
	for (FConstControllerIterator It = GetWorld()->GetControllerIterator(); It; ++It)
	{
		AUTCharacter* Character = Cast<AUTCharacter>(It->Get()->GetPawn());
		AUTPlayerState* OtherPS = Character ? Cast<AUTPlayerState>(Character->PlayerState) : nullptr;
		if (!Character || Character->IsDead() || !OtherPS || OtherPS->bOnlySpectator || OtherPS->GetTeamNum() >= 4) continue;
		TraceParams.AddIgnoredActor(Character);
		if (OtherPS->GetTeamNum() != PS->GetTeamNum()) Enemies.Add(Character);
		else if (OtherPS != PS) Friends.Add(Character);
	}
	ACharacter* PawnCDO = DefaultPawnClass ? Cast<ACharacter>(DefaultPawnClass->GetDefaultObject()) : nullptr;
	if (!PawnCDO) return nullptr;
	const float Radius = PawnCDO->GetCapsuleComponent()->GetScaledCapsuleRadius();
	const float Now = GetWorld()->GetTimeSeconds();
	for (const TWeakObjectPtr<APlayerStart>& WeakStart : SpawnStarts)
	{
		APlayerStart* Start = WeakStart.Get();
		if (!Start) continue;
		FVector Location = Start->GetActorLocation();
		// Use the stock fit test: UT's pawn capsule is taller than the PlayerStart editor capsule.
		const FRotator FitRotation(0.f, Start->GetActorRotation().Yaw, 0.f);
		if (!GetWorld()->FindTeleportSpot(PawnCDO, Location, FitRotation)) continue;
		float Score = 0.f;
		bool bOccupied = false;
		for (AUTCharacter* Enemy : Enemies)
		{
			const float Distance = FVector::Dist(Location, Enemy->GetActorLocation());
			bOccupied |= Distance < 2.f * Radius;
			Score -= 2000.f * FMath::Max(0.f, 1.f - Distance / 2000.f);
		}
		float NearestFriend = MAX_FLT;
		for (AUTCharacter* Friend : Friends)
		{
			const float Distance = FVector::Dist(Location, Friend->GetActorLocation());
			bOccupied |= Distance < 2.f * Radius;
			NearestFriend = FMath::Min(NearestFriend, Distance);
		}
		if (bOccupied) continue;
		if (NearestFriend < MAX_FLT) Score += 100.f * FMath::Max(0.f, 1.f - NearestFriend / 2500.f);
		const float* UsedAt = LastStartUse.Find(Start);
		if (UsedAt) Score -= 1000.f * FMath::Clamp(1.f - (Now - *UsedAt) / 10.f, 0.f, 1.f);
		FSpawnCandidate Candidate = { Start, Location, Score };
		Candidates.Add(Candidate);
	}
	Candidates.Sort([](const FSpawnCandidate& A, const FSpawnCandidate& B) { return A.Score > B.Score; });
	// At most 16 candidates x 15 other players. Every tested candidate checks all three opposing teams.
	const int32 CandidateCount = FMath::Min(16, Candidates.Num());
	APlayerStart* Best = nullptr;
	float BestScore = -MAX_FLT;
	bool bBestHidden = false;
	for (int32 Index = 0; Index < CandidateCount; ++Index)
	{
		FSpawnCandidate& Candidate = Candidates[Index];
		bool bHiddenFromAll = true;
		for (AUTCharacter* Enemy : Enemies)
		{
			FHitResult Hit;
			if (!GetWorld()->LineTraceSingleByChannel(Hit, Enemy->GetPawnViewLocation(),
				Candidate.Location, ECC_Visibility, TraceParams))
			{
				bHiddenFromAll = false;
				Candidate.Score -= 2500.f;
				if (Enemy->PlayerState == PS->LastKillerPlayerState) Candidate.Score -= 500.f;
			}
		}
		if (!Best || (bHiddenFromAll && !bBestHidden) || (bHiddenFromAll == bBestHidden && Candidate.Score > BestScore))
		{
			Best = Candidate.Start;
			BestScore = Candidate.Score;
			bBestHidden = bHiddenFromAll;
		}
	}
	if (Best) LastStartUse.Add(Best, Now);
	// No blocked fallback: return null so stock RestartPlayer can fail safely and retry.
	return Best;
}

APawn* ANCPlusXTDMGameMode::SpawnDefaultPawnFor_Implementation(AController* Player, AActor* StartSpot)
{
	UClass* PawnClass = Player ? GetDefaultPawnClassForController(Player) : nullptr;
	APawn* PawnCDO = PawnClass ? PawnClass->GetDefaultObject<APawn>() : nullptr;
	if (!PawnCDO || !StartSpot) return nullptr;
	FVector Location = StartSpot->GetActorLocation();
	FRotator Rotation = StartSpot->GetActorRotation();
	Rotation.Pitch = 0.f;
	Rotation.Roll = 0.f;
	if (!GetWorld()->FindTeleportSpot(PawnCDO, Location, Rotation)) return nullptr;
	return SpawnDefaultPawnAtTransform(Player, FTransform(Rotation, Location));
}

AUTTeamInfo* ANCPlusXTDMGameMode::FindLeadingTeam(bool& bTied) const
{
	int32 Scores[4];
	for (uint8 Team = 0; Team < 4; ++Team)
	{
		if (!Teams.IsValidIndex(Team) || !Teams[Team]) { bTied = true; return nullptr; }
		Scores[Team] = Teams[Team]->Score;
	}
	return Teams[NCPlusXTDMRules::FindLeader(Scores, bTied)];
}

AUTPlayerState* ANCPlusXTDMGameMode::FindTeamRepresentative(const AUTTeamInfo* Team) const
{
	AUTPlayerState* Best = nullptr;
	if (UTGameState && Team)
	{
		for (APlayerState* BasePS : UTGameState->PlayerArray)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(BasePS);
			if (PS && PS->Team == Team && !PS->bOnlySpectator && !PS->bIsInactive && (!Best || PS->Score > Best->Score)) Best = PS;
		}
	}
	return Best;
}

AUTPlayerState* ANCPlusXTDMGameMode::IsThereAWinner_Implementation(bool& bTied)
{
	AUTTeamInfo* Team = FindLeadingTeam(bTied);
	return !bTied ? FindTeamRepresentative(Team) : nullptr;
}

bool ANCPlusXTDMGameMode::CheckScore_Implementation(AUTPlayerState* Scorer)
{
	if (!IsMatchInProgress() || HasMatchEnded() || bGameEnded || GoalScore <= 0) return false;
	bool bTied = false;
	AUTTeamInfo* Team = FindLeadingTeam(bTied);
	if (Team && !bTied && Team->Score >= GoalScore)
	{
		FinalWinningTeam = Team->TeamIndex;
		EndGame(FindTeamRepresentative(Team), TEXT("ScoreLimit"));
		return true;
	}
	return false;
}

void ANCPlusXTDMGameMode::CheckGameTime()
{
	if (!UTGameState || !IsMatchInProgress() || HasMatchEnded() || bGameEnded || TimeLimit <= 0 || UTGameState->GetRemainingTime() > 0) return;
	bool bTied = false;
	AUTTeamInfo* Team = FindLeadingTeam(bTied);
	if (bTied && bAllowOvertime)
	{
		UTGameState->SetRemainingTime(120);
		if (!UTGameState->IsMatchInOvertime()) SetMatchState(MatchState::MatchEnteringOvertime);
	}
	else
	{
		FinalWinningTeam = Team && !bTied ? Team->TeamIndex : 255;
		EndGame(FindTeamRepresentative(bTied ? nullptr : Team), TEXT("TimeLimit"));
	}
}

void ANCPlusXTDMGameMode::RestoreFinalResult()
{
	if (!UTGameState) return;
	AUTTeamInfo* Team = Teams.IsValidIndex(FinalWinningTeam) ? Teams[FinalWinningTeam] : nullptr;
	UTGameState->WinningTeam = Team;
	UTGameState->WinnerPlayerState = FindTeamRepresentative(Team);
	UTGameState->ForceNetUpdate();
	if (XTDMState) XTDMState->SetMatchResult(FinalWinningTeam);
}

void ANCPlusXTDMGameMode::EndGame(AUTPlayerState* Winner, FName Reason)
{
	if (bGameEnded || !UTGameState || GetWorld()->WorldType == EWorldType::PIE) return;
	if (FinalWinningTeam >= 4)
	{
		bool bTied = false;
		AUTTeamInfo* Team = FindLeadingTeam(bTied);
		FinalWinningTeam = Team && !bTied ? Team->TeamIndex : 255;
	}
	Super::EndGame(FindTeamRepresentative(Teams.IsValidIndex(FinalWinningTeam) ? Teams[FinalWinningTeam] : nullptr), Reason);
	RestoreFinalResult();
}

void ANCPlusXTDMGameMode::SetEndGameFocus(AUTPlayerState* Winner)
{
	// Base EndGame chooses a fallback individual if no winning player remains. Restore the actual team before presentation.
	RestoreFinalResult();
	AUTPlayerState* Representative = UTGameState ? UTGameState->WinnerPlayerState : nullptr;
	AController* WinnerController = Representative ? Cast<AController>(Representative->GetOwner()) : nullptr;
	EndGameFocus = WinnerController ? WinnerController->GetPawn() : nullptr;
	if (EndGameFocus) EndGameFocus->bAlwaysRelevant = true;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		if (!PC) continue;
		AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
		if (EndGameFocus) PC->SetViewTarget(EndGameFocus);
		PC->ClientGameEnded(EndGameFocus, PS && !PS->bOnlySpectator && FinalWinningTeam < 4 && PS->GetTeamNum() == FinalWinningTeam);
	}
}

void ANCPlusXTDMGameMode::PlayEndOfMatchMessage()
{
	RestoreFinalResult();
	if (!UTGameState) return;
	for (FConstPlayerControllerIterator It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
	{
		AUTPlayerController* PC = Cast<AUTPlayerController>(It->Get());
		if (PC)
		{
			AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
			const bool bWon = PS && !PS->bOnlySpectator && FinalWinningTeam < 4 && PS->GetTeamNum() == FinalWinningTeam;
			PC->ClientReceiveLocalizedMessage(VictoryMessageClass, bWon ? 1 : 0,
				UTGameState->WinnerPlayerState, PS, UTGameState->WinningTeam);
		}
	}
}

void ANCPlusXTDMGameMode::SendEndOfGameStats(FName Reason)
{
	RestoreFinalResult();
	Super::SendEndOfGameStats(Reason);
}
