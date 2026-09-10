// MutBotEvents.cpp - HTTP event poster for UT4IGBot Discord bot
//
// Posts match lifecycle events and flag captures to the bot's FastAPI server.
// Uses FHttpModule (same pattern as StatSQL plugin).
//
// UE4 4.15 quirks observed:
//   - No FString::TrimStartAndEnd(), use .Trim() (mutates in place)
//   - PCH mode UseExplicitOrSharedPCHs: include UnrealTournament.h before UT headers
//   - Must include own header first (UHT requirement)

#include "MutBotEvents.h"
#include "UnrealTournament.h"
#include "UTGameState.h"
#include "UTPlayerState.h"
#include "UTCarriedObject.h"
#include "UTCTFFlag.h"
#include "UTCTFFlagBase.h"
#include "UTCTFGameState.h"
#include "NCReadyUp.h"
#include "Json.h"
#include "GameFramework/GameStateBase.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Engine/NetConnection.h"
#include "UTDemoRecSpectator.h"

DEFINE_LOG_CATEGORY(LogBotEvents);

FString FBotArrivalLedger::CanonicalId(const FString& Value)
{
	if (Value.Len() != 32) return FString();
	FString Id = Value.ToLower();
	for (int32 Index = 0; Index < Id.Len(); ++Index)
	{
		const TCHAR Ch = Id[Index];
		if (!((Ch >= TEXT('0') && Ch <= TEXT('9')) || (Ch >= TEXT('a') && Ch <= TEXT('f'))))
		{
			return FString();
		}
	}
	// A nil ID is not an authenticated account identity or a launch nonce.
	return Id == TEXT("00000000000000000000000000000000") ? FString() : Id;
}

void FBotArrivalLedger::RecordJoin(const FString& Value, double FirstSeenSeconds)
{
	const FString Id = CanonicalId(Value);
	if (Id.IsEmpty() || !FMath::IsFinite(FirstSeenSeconds) || FirstSeenSeconds < 0.0)
	{
		bComplete = false;
		return;
	}
	if (Joined.Contains(Id)) return;
	if (Joined.Num() >= MaxPlayers)
	{
		bComplete = false;
		return;
	}
	Joined.Add(Id, FirstSeenSeconds);
}

bool FBotArrivalLedger::QualifyState(FName State)
{
	if (State == MatchState::WaitingToStart) bWarmupSeen = true;
	return bWarmupSeen;
}

AMutBotEvents::AMutBotEvents(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PugId = -1;
	bFlagEventsBound = false;
	ArrivalStartedAt = 0.0;
	ArrivalRequestStartedAt = 0.0;
	ArrivalSequence = 0;
	bArrivalStopped = false;
}

void AMutBotEvents::Init_Implementation(const FString& Options)
{
	Super::Init_Implementation(Options);
	if (GetNetMode() == NM_Client) return;
	// Init precedes player creation, unlike BeginPlay. Never enable this halfway
	// through a match: missed logins could otherwise look like late arrivals.
	ArrivalLaunchId = FBotArrivalLedger::CanonicalId(ParseOption(Options, TEXT("ArrivalId")));
	if (!ArrivalLaunchId.IsEmpty())
	{
		ArrivalInstanceId = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
		ArrivalStartedAt = FPlatformTime::Seconds();
	}
}

void AMutBotEvents::BeginPlay()
{
	Super::BeginPlay();

	if (GetNetMode() == NM_Client) return;

	// Read config from URL options (set by bot in server launch command)
	UWorld* World = GetWorld();
	if (World)
	{
		BotApiUrl = World->URL.GetOption(TEXT("BotApiUrl="), TEXT(""));
		BotApiToken = World->URL.GetOption(TEXT("BotApiToken="), TEXT(""));

		FString PugIdStr = World->URL.GetOption(TEXT("PugId="), TEXT("-1"));
		PugId = FCString::Atoi(*PugIdStr);
	}

	// Trim any trailing slashes from URL
	while (BotApiUrl.EndsWith(TEXT("/")))
	{
		BotApiUrl = BotApiUrl.Left(BotApiUrl.Len() - 1);
	}

	// Fallback: read from Mod.ini
	if (BotApiUrl.IsEmpty())
	{
		FString ConfigPath = FPaths::GeneratedConfigDir() + TEXT("Mod.ini");
		GConfig->GetString(TEXT("BOT_EVENTS"), TEXT("BotApiUrl"), BotApiUrl, ConfigPath);
		GConfig->GetString(TEXT("BOT_EVENTS"), TEXT("BotApiToken"), BotApiToken, ConfigPath);
	}

	if (BotApiUrl.IsEmpty())
	{
		UE_LOG(LogBotEvents, Warning, TEXT("No BotApiUrl configured - MutBotEvents will not post events"));
		return;
	}

	UE_LOG(LogBotEvents, Log, TEXT("MutBotEvents initialized: URL=%s PugId=%d"), *BotApiUrl, PugId);
	if (!ArrivalLaunchId.IsEmpty())
	{
		GetWorldTimerManager().SetTimer(ArrivalTimer, this,
			&AMutBotEvents::PollArrivals, 5.0f, true);
		PollArrivals();
	}
}

void AMutBotEvents::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopReadyPolling();
	StopArrivalPolling();
	if (ArrivalRequest.IsValid())
	{
		ArrivalRequest->OnProcessRequestComplete().Unbind();
		ArrivalRequest->CancelRequest();
		ArrivalRequest.Reset();
	}
	Super::EndPlay(EndPlayReason);
}

void AMutBotEvents::PostPlayerInit_Implementation(AController* C)
{
	Super::PostPlayerInit_Implementation(C);
	if (GetNetMode() != NM_Client && !ArrivalLaunchId.IsEmpty() && !bArrivalStopped)
	{
		ObserveArrival(Cast<APlayerController>(C), true);
	}
}

void AMutBotEvents::NotifyLogout_Implementation(AController* C)
{
	// Capture before downstream mutators can discard PlayerState/UniqueId.
	if (GetNetMode() != NM_Client && !ArrivalLaunchId.IsEmpty() && !bArrivalStopped)
	{
		APlayerController* PC = Cast<APlayerController>(C);
		ObserveArrival(PC, false);
		for (int32 Index = ArrivalConnections.Num() - 1; Index >= 0; --Index)
		{
			if (ArrivalConnections[Index].Controller.Get() == PC)
			{
				ResolveArrival(ArrivalConnections[Index]);
				if (ArrivalConnections[Index].Id.IsEmpty()) ArrivalLedger.bComplete = false;
				ArrivalConnections.RemoveAtSwap(Index);
			}
		}
	}
	Super::NotifyLogout_Implementation(C);
}

// ────────────────────────────────────────────────────────────────────
// Match State Changes
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::NotifyMatchStateChange_Implementation(FName NewState)
{
	// MutBotEvents is the chain head in bot-hosted games — forward before any
	// early return, or every downstream mutator (StatSQL submission, etc.) is starved.
	Super::NotifyMatchStateChange_Implementation(NewState);

	if (GetNetMode() == NM_Client || BotApiUrl.IsEmpty()) return;

	FString StateStr = NewState.ToString();
	UE_LOG(LogBotEvents, Log, TEXT("Match state changed: %s (PugId=%d)"), *StateStr, PugId);
	if (!ArrivalLaunchId.IsEmpty() && !bArrivalStopped)
	{
		const bool bFinal = NewState == MatchState::WaitingPostMatch ||
			NewState == MatchState::MapVoteHappening || NewState == MatchState::LeavingMap ||
			NewState == MatchState::Aborted;
		// WaitingToStart can immediately follow BeginPlay's first heartbeat.
		// Preserve that in-flight request instead of creating a sequence gap.
		PostArrivals(bFinal, NewState != MatchState::WaitingToStart);
		if (bFinal) StopArrivalPolling();
	}

	// Map UT4 match states to bot event names
	if (NewState == MatchState::WaitingToStart)
	{
		// Start polling player readiness every 5 seconds
		GetWorldTimerManager().SetTimer(ReadyCheckTimer, this,
			&AMutBotEvents::PollPlayerReadiness, 5.0f, true, 0.0f);
	}
	else if (NewState == MatchState::CountdownToBegin)
	{
		StopReadyPolling();
		PostStateChangeWithPlayers(TEXT("CountdownToBegin"));
	}
	else if (NewState == MatchState::InProgress)
	{
		StopReadyPolling();
		TryBindFlagEvents();
		PostStateChangeWithPlayers(TEXT("InProgress"));
	}
	else if (NewState == MatchState::WaitingPostMatch)
	{
		StopReadyPolling();
		PostMatchEnded();
	}
	else if (NewState == MatchState::MapVoteHappening)
	{
		// Don't post anything for map vote
	}
}

// ────────────────────────────────────────────────────────────────────
// Mutate Commands (in-game console: mutate joinpug ictf)
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::Mutate_Implementation(const FString& MutateString, APlayerController* Sender)
{
	// Forward first (chain head) — without this, mutate commands handled by
	// downstream mutators never fire in bot-hosted games.
	Super::Mutate_Implementation(MutateString, Sender);

	if (BotApiUrl.IsEmpty() || !Sender) return;

	AUTPlayerState* UTPS = Cast<AUTPlayerState>(Sender->PlayerState);
	if (!UTPS) return;

	TArray<FString> Parts;
	MutateString.ParseIntoArray(Parts, TEXT(" "), true);

	if (Parts.Num() < 1) return;

	FString Command = Parts[0].ToLower();

	if (Command == TEXT("joinpug") || Command == TEXT("leavepug") || Command == TEXT("listpug"))
	{
		// Only allow joinpug after the match is over (WaitingPostMatch or MapVoteHappening)
		if (Command == TEXT("joinpug"))
		{
			AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
			if (GS)
			{
				FName CurrentState = GS->GetMatchState();
				bool bMatchOver = (CurrentState == MatchState::WaitingPostMatch ||
				                   CurrentState == MatchState::MapVoteHappening);
				if (!bMatchOver)
				{
					// Tell the player they need to wait
					AUTPlayerController* PC = Cast<AUTPlayerController>(Sender);
					if (PC)
					{
						PC->ClientSay(UTPS, TEXT("Cannot join PUG queue while match is in progress. Wait until the match ends."), ChatDestinations::System);
					}
					UE_LOG(LogBotEvents, Log, TEXT("joinpug blocked for %s — match still in progress (state: %s)"),
						*UTPS->PlayerName, *CurrentState.ToString());
					return;
				}
			}
		}

		FString Mode = Parts.Num() > 1 ? Parts[1] : TEXT("ictf");
		FString PlayerName = UTPS->PlayerName;

		// Get UT4 player ID from UniqueId
		FString Ut4Id;
		if (UTPS->UniqueId.IsValid())
		{
			Ut4Id = UTPS->UniqueId.GetUniqueNetId()->ToString();
		}

		// Build JSON
		TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
		Json->SetStringField(TEXT("action"), Command);
		Json->SetStringField(TEXT("mode"), Mode);
		Json->SetStringField(TEXT("ut4_id"), Ut4Id);
		Json->SetStringField(TEXT("player_name"), PlayerName);

		FString Output;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
		FJsonSerializer::Serialize(Json, Writer);

		SendPost(TEXT("/pug_action"), Output);

		UE_LOG(LogBotEvents, Log, TEXT("PUG action: %s mode=%s player=%s ut4_id=%s"),
			*Command, *Mode, *PlayerName, *Ut4Id);
	}
}

// ────────────────────────────────────────────────────────────────────
// Flag Captures
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::ScoreObject_Implementation(
	AUTCarriedObject* GameObject, AUTCharacter* HolderPawn,
	AUTPlayerState* Holder, FName Reason)
{
	// Forward first (chain head) — StatSQL's flag/carry tracking lives downstream.
	Super::ScoreObject_Implementation(GameObject, HolderPawn, Holder, Reason);

	if (GetNetMode() == NM_Client || BotApiUrl.IsEmpty()) return;

	if (Reason == FName(TEXT("FlagCapture")) && Holder)
	{
		PostFlagCapture(Holder);
	}
}

// ────────────────────────────────────────────────────────────────────
// Cover Kills
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::ScoreKill_Implementation(AController* Killer, AController* Other,
	TSubclassOf<UDamageType> DamageType)
{
	// Forward down the mutator chain first — MutBotEvents runs ahead of
	// MutStatSQL/MutServerShield, which both rely on ScoreKill.
	Super::ScoreKill_Implementation(Killer, Other, DamageType);

	if (GetNetMode() == NM_Client || BotApiUrl.IsEmpty()) return;
	if (!Killer || !Other || Killer == Other) return; // ignore suicides/environment

	AUTPlayerState* KillerPS = Cast<AUTPlayerState>(Killer->PlayerState);
	AUTPlayerState* VictimPS = Cast<AUTPlayerState>(Other->PlayerState);
	if (!KillerPS || !VictimPS || KillerPS == VictimPS) return;

	// Kill-streak highlights (monster kills + sprees) — fire BEFORE the
	// team-CTF gate below so FFA / Duel / ShaftArena (no valid teams) still
	// post. The engine's IncrementKills already excluded team-kills from
	// MultiKillLevel/Spree by the time we get here.
	ScoreKill_PostHighlights(KillerPS);

	const int32 KillerTeam = KillerPS->GetTeamNum();
	const int32 VictimTeam = VictimPS->GetTeamNum();

	// A cover kill is a frag of an enemy by a teammate of the flag carrier while
	// that team is carrying. Both must be on valid, opposing teams (no team kills).
	if (KillerTeam > 1 || VictimTeam > 1 || KillerTeam == VictimTeam) return;

	FCoverCarryWindow& Window = CarryWindows[KillerTeam];
	if (!Window.bOpen) return;

	// The carrier isn't their own cover.
	const FString KillerName = KillerPS->PlayerName;
	if (KillerName == Window.CarrierName) return;

	Window.CoverKills.AddUnique(KillerName);
}

// ────────────────────────────────────────────────────────────────────
// Kill-streak highlights (multi-kills + sprees)
// ────────────────────────────────────────────────────────────────────
//
// Hooked into ScoreKill_Implementation below, fires AFTER Super:: has resolved
// (so AUTPlayerState::MultiKillLevel + Spree are already updated by the engine's
// IncrementKills path). Sends to /reward; the bot turns it into a Discord chat
// highlight. Skips in-game bots so a low-pop bot match doesn't flood chat.
//
// MultiKill rungs (UTMultiKillMessage.h):
//   1=Double, 2=Multi, 3=ULTRA, 4=MONSTER (engine caps at 4 for the announcement).
// We only post the Monster threshold (level 4) — exact equality so the kill
// AFTER the monster (level=5,6,...) doesn't re-fire.
//
// Spree rungs (UTSpreeMessage.h, engine bumps every Spree % 5 == 0):
//   1=Killing Spree, 2=Rampage, 3=Dominating, 4=Unstoppable, 5=Godlike (capped).
// We only post Dominating and above (3..5). The %5==0 gate naturally fires once
// per milestone (no spam between 15 and 20).
void AMutBotEvents::ScoreKill_PostHighlights(AUTPlayerState* KillerPS)
{
	if (KillerPS == nullptr || BotApiUrl.IsEmpty()) return;
	if (KillerPS->bIsABot) return;       // don't flood chat in bot-heavy matches

	// MONSTER KILL — fire for level 4 AND every subsequent kill in the same
	// engine multikill window (5, 6, ...). Multiplier turns those continuations
	// into "MONSTER KILL x2 / x3 / ..." on the bot side via Discord message edit
	// instead of fresh embeds. Level 4 -> Multiplier 1 -> new embed; >4 -> edit.
	if (KillerPS->MultiKillLevel >= 4)
	{
		const int32 Multiplier = KillerPS->MultiKillLevel - 3;     // 4->1, 5->2, 6->3, ...
		PostReward(KillerPS, TEXT("monster"), KillerPS->MultiKillLevel, Multiplier);
	}

	// DOMINATING and above (level 3..5). The %5==0 gate is the engine's own
	// milestone check; matches AUTGameMode::ScoreKill at UTGameMode.cpp:2846.
	// Each spree milestone is its own event — multiplier always 1.
	if (KillerPS->Spree > 0 && (KillerPS->Spree % 5) == 0)
	{
		const int32 SpreeLevel = KillerPS->Spree / 5;
		if (SpreeLevel >= 3 && SpreeLevel <= 5)
		{
			PostReward(KillerPS, TEXT("spree"), SpreeLevel, 1);
		}
	}
}

void AMutBotEvents::TryBindFlagEvents()
{
	if (bFlagEventsBound) return;

	AUTCTFGameState* CTFGS = GetWorld() ? GetWorld()->GetGameState<AUTCTFGameState>() : nullptr;
	if (!CTFGS) return; // not a CTF mode — no flags to track

	// Use GetFlagBase() accessor — never access FlagBases directly (ABI mismatch).
	bool bBoundAny = false;
	for (int32 TeamIdx = 0; TeamIdx < 2; TeamIdx++)
	{
		AUTCTFFlagBase* Base = CTFGS->GetFlagBase(TeamIdx);
		if (Base && Base->GetCarriedObject())
		{
			Base->GetCarriedObject()->OnCarriedObjectHolderChangedDelegate.AddDynamic(
				this, &AMutBotEvents::OnFlagHolderChanged);
			bBoundAny = true;
			UE_LOG(LogBotEvents, Log, TEXT("Bound cover-kill tracker to flag holder-changed delegate for team %d"), TeamIdx);
		}
	}

	if (bBoundAny)
	{
		bFlagEventsBound = true;
		CarryWindows[0] = FCoverCarryWindow();
		CarryWindows[1] = FCoverCarryWindow();
	}
}

void AMutBotEvents::OnFlagHolderChanged(AUTCarriedObject* Flag)
{
	if (!Flag) return;

	AUTPlayerState* Holder = Flag->Holder;
	if (Holder)
	{
		// Flag grabbed — open a fresh carry window for the carrier's team.
		const int32 CarrierTeam = Holder->GetTeamNum();
		if (CarrierTeam == 0 || CarrierTeam == 1)
		{
			FCoverCarryWindow& Window = CarryWindows[CarrierTeam];
			Window.bOpen = true;
			Window.CarrierName = Holder->PlayerName;
			Window.CoverKills.Empty();
		}
	}
	else
	{
		// Flag dropped or returned — stop counting covers for the carrying team.
		// A flag's carrier is always on the team opposite its home team. Leave the
		// list intact so a capture firing in the same frame can still read it.
		const int32 FlagTeam = Flag->GetTeamNum();
		if (FlagTeam == 0 || FlagTeam == 1)
		{
			CarryWindows[1 - FlagTeam].bOpen = false;
		}
	}
}

// ────────────────────────────────────────────────────────────────────
// Event Posting Functions
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::PostStateChange(const FString& State)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("state"), State);
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/state_change"), Output);
}

void AMutBotEvents::PostStateChangeWithPlayers(const FString& State)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("state"), State);
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());
	Json->SetStringField(TEXT("match_id"), GetMatchId());

	// Parse player list JSON array and attach
	FString PlayerListStr = BuildPlayerListJson();
	TArray<TSharedPtr<FJsonValue>> PlayersArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PlayerListStr);
	FJsonSerializer::Deserialize(Reader, PlayersArray);
	Json->SetArrayField(TEXT("Players"), PlayersArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/state_change"), Output);
}

void AMutBotEvents::PostFlagCapture(AUTPlayerState* Scorer)
{
	AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
	if (!GS) return;

	int32 TeamIndex = Scorer->GetTeamNum();
	FString PlayerName = Scorer->PlayerName;

	// Get team scores
	int32 ScoreRed = 0, ScoreBlue = 0;
	if (GS->Teams.IsValidIndex(0) && GS->Teams[0]) ScoreRed = GS->Teams[0]->Score;
	if (GS->Teams.IsValidIndex(1) && GS->Teams[1]) ScoreBlue = GS->Teams[1]->Score;

	// Remaining time
	float RemainingTime = GS->GetRemainingTime();

	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("score_type"), TEXT("FlagCapture"));
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetStringField(TEXT("player_name"), PlayerName);
	Json->SetNumberField(TEXT("player_team"), TeamIndex);
	Json->SetStringField(TEXT("match_id"), GetMatchId());
	Json->SetNumberField(TEXT("match_remaining_time"), RemainingTime);

	// Cover kills: teammates who fragged enemies while this team carried the flag.
	// Snapshot the open carry window for the capper's team, then reset it.
	TArray<TSharedPtr<FJsonValue>> CoverKillsArray;
	int32 NumCovers = 0;
	if (TeamIndex == 0 || TeamIndex == 1)
	{
		FCoverCarryWindow& Window = CarryWindows[TeamIndex];
		for (const FString& Name : Window.CoverKills)
		{
			CoverKillsArray.Add(MakeShareable(new FJsonValueString(Name)));
		}
		NumCovers = Window.CoverKills.Num();
		Window = FCoverCarryWindow();
	}
	Json->SetArrayField(TEXT("cover_kills"), CoverKillsArray);

	// Team scores array
	TArray<TSharedPtr<FJsonValue>> TeamsArray;

	TSharedRef<FJsonObject> RedTeam = MakeShareable(new FJsonObject());
	RedTeam->SetNumberField(TEXT("id"), 0);
	RedTeam->SetNumberField(TEXT("score"), ScoreRed);
	TeamsArray.Add(MakeShareable(new FJsonValueObject(RedTeam)));

	TSharedRef<FJsonObject> BlueTeam = MakeShareable(new FJsonObject());
	BlueTeam->SetNumberField(TEXT("id"), 1);
	BlueTeam->SetNumberField(TEXT("score"), ScoreBlue);
	TeamsArray.Add(MakeShareable(new FJsonValueObject(BlueTeam)));

	Json->SetArrayField(TEXT("teams"), TeamsArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/score"), Output);

	UE_LOG(LogBotEvents, Log, TEXT("Flag capture: %s (team %d) | Score: %d-%d | Covers: %d"),
		*PlayerName, TeamIndex, ScoreRed, ScoreBlue, NumCovers);
}

void AMutBotEvents::PostReward(AUTPlayerState* Scorer, const FString& Type, int32 Level, int32 Multiplier)
{
	if (Scorer == nullptr || BotApiUrl.IsEmpty()) return;

	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("type"),         Type);          // "monster" | "spree"
	Json->SetNumberField(TEXT("level"),        Level);         // raw engine value
	Json->SetNumberField(TEXT("multiplier"),   Multiplier);    // 1 = new embed; >1 = edit existing
	Json->SetStringField(TEXT("player_name"),  Scorer->PlayerName);
	Json->SetNumberField(TEXT("player_team"),  Scorer->GetTeamNum());
	Json->SetNumberField(TEXT("pug_id"),       PugId);
	Json->SetStringField(TEXT("match_id"),     GetMatchId());
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/reward"), Output);

	UE_LOG(LogBotEvents, Log, TEXT("Reward: %s lvl=%d x%d by %s (team %d)"),
		*Type, Level, Multiplier, *Scorer->PlayerName, Scorer->GetTeamNum());
}


void AMutBotEvents::PostMatchEnded()
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("state"), TEXT("WaitingPostMatch"));
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());
	Json->SetStringField(TEXT("match_id"), GetMatchId());

	// Team scores array
	FString TeamsStr = BuildTeamScoresJson();
	TArray<TSharedPtr<FJsonValue>> TeamsArray;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(TeamsStr);
	FJsonSerializer::Deserialize(Reader, TeamsArray);
	Json->SetArrayField(TEXT("teams"), TeamsArray);

	// Player list
	FString PlayerListStr = BuildPlayerListJson();
	TArray<TSharedPtr<FJsonValue>> PlayersArray;
	TSharedRef<TJsonReader<>> PlayerReader = TJsonReaderFactory<>::Create(PlayerListStr);
	FJsonSerializer::Deserialize(PlayerReader, PlayersArray);
	Json->SetArrayField(TEXT("Players"), PlayersArray);

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/state_change"), Output);

	UE_LOG(LogBotEvents, Log, TEXT("Match ended posted for PugId=%d"), PugId);
}

// ────────────────────────────────────────────────────────────────────
// Player Readiness Polling
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::PollPlayerReadiness()
{
	PostStateChangeWithPlayers(TEXT("WaitingToStart"));
}

void AMutBotEvents::StopReadyPolling()
{
	GetWorldTimerManager().ClearTimer(ReadyCheckTimer);
}

// Arrival observation deliberately has no dependency on passwords, teams, or
// readiness. PostPlayerInit + NotifyLogout preserve visits shorter than a poll.
void AMutBotEvents::ObserveArrival(APlayerController* PC, bool bFromLogin)
{
	if (!PC || Cast<AUTDemoRecSpectator>(PC)) return;
	AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
	if (PS && (PS->bIsABot || PS->bIsDemoRecording)) return;
	for (FBotArrivalConnection& Connection : ArrivalConnections)
	{
		if (Connection.Controller.Get() == PC)
		{
			ResolveArrival(Connection);
			return;
		}
	}
	// Enumeration/logout discovering an unrecorded connection means a login was
	// missed. Its positive identity is useful, but absence is no longer provable.
	if (!bFromLogin) ArrivalLedger.bComplete = false;
	if (ArrivalConnections.Num() >= FBotArrivalLedger::MaxPlayers)
	{
		ArrivalLedger.bComplete = false;
		return;
	}
	const int32 Index = ArrivalConnections.Emplace(PC, FPlatformTime::Seconds() - ArrivalStartedAt);
	ResolveArrival(ArrivalConnections[Index]);
}

void AMutBotEvents::ResolveArrival(FBotArrivalConnection& Connection)
{
	APlayerController* PC = Connection.Controller.Get();
	AUTPlayerState* PS = PC ? Cast<AUTPlayerState>(PC->PlayerState) : nullptr;
	if (!PS || !PS->UniqueId.IsValid()) return;
	const FString Id = FBotArrivalLedger::CanonicalId(PS->UniqueId.GetUniqueNetId()->ToString());
	if (Id.IsEmpty()) return; // Deferred identity; stays incomplete until resolved.
	if (!Connection.Id.IsEmpty() && Connection.Id != Id)
	{
		ArrivalLedger.bComplete = false;
		return;
	}
	Connection.Id = Id;
	ArrivalLedger.RecordJoin(Id, Connection.FirstSeenSeconds);
}

void AMutBotEvents::PollArrivals()
{
	PostArrivals(false);
}

void AMutBotEvents::StopArrivalPolling()
{
	bArrivalStopped = true;
	GetWorldTimerManager().ClearTimer(ArrivalTimer);
}

void AMutBotEvents::PostArrivals(bool bFinal, bool bImmediate)
{
	if (ArrivalLaunchId.IsEmpty() || bArrivalStopped || BotApiUrl.IsEmpty() || GetNetMode() == NM_Client) return;
	UWorld* World = GetWorld();
	AUTGameState* GS = World ? World->GetGameState<AUTGameState>() : nullptr;
	// BeginPlay can run in EnteringMap, before the server is ready for players.
	// Keep all login evidence from Init, but don't emit a first frame or consume
	// sequence numbers until actual warmup. UTGameMode::SetMatchState updates the
	// GameState before calling NotifyMatchStateChange, so this reads the new phase.
	if (!ArrivalLedger.QualifyState(GS ? GS->GetMatchState() : NAME_None)) return;
	const double Now = FPlatformTime::Seconds();
	// At most one request, no retry queue. Every next heartbeat contains the
	// cumulative ledger. Lifecycle/final snapshots replace an outstanding request.
	if (ArrivalRequest.IsValid())
	{
		if (!bFinal && !bImmediate && Now - ArrivalRequestStartedAt < 10.0) return;
		ArrivalRequest->OnProcessRequestComplete().Unbind();
		ArrivalRequest->CancelRequest();
		ArrivalRequest.Reset();
	}
	if (ArrivalSequence == MAX_int32)
	{
		StopArrivalPolling();
		return;
	}

	const bool bHealthy = World && GS && World->GetAuthGameMode() && !World->bIsTearingDown;
	bool bComplete = bHealthy && ArrivalLedger.bComplete;
	for (int32 Index = ArrivalConnections.Num() - 1; Index >= 0; --Index)
	{
		FBotArrivalConnection& Connection = ArrivalConnections[Index];
		if (!Connection.Controller.IsValid())
		{
			// Even a resolved controller disappearing without Logout indicates that
			// the lifecycle observation chain was interrupted.
			ArrivalLedger.bComplete = false;
			ArrivalConnections.RemoveAtSwap(Index);
			continue;
		}
		ResolveArrival(Connection);
		if (Connection.Id.IsEmpty()) bComplete = false;
	}

	TArray<TSharedPtr<FJsonValue>> Players;
	TSet<FString> CurrentIds;
	ANCReadyUpState* ReadyState = World ? ANCReadyUpState::Find(World) : nullptr;
	if (World)
	{
		int32 ControllerCount = 0;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (++ControllerCount > FBotArrivalLedger::MaxPlayers)
			{
				ArrivalLedger.bComplete = false;
				break;
			}
			APlayerController* PC = It->Get();
			if (!PC || Cast<AUTDemoRecSpectator>(PC)) continue;
			AUTPlayerState* PS = Cast<AUTPlayerState>(PC->PlayerState);
			if (PS && (PS->bIsABot || PS->bIsDemoRecording)) continue;
			ObserveArrival(PC, false);
			if (!PS || !PS->UniqueId.IsValid())
			{
				bComplete = false;
				continue;
			}
			const FString Id = FBotArrivalLedger::CanonicalId(PS->UniqueId.GetUniqueNetId()->ToString());
			if (Id.IsEmpty() || CurrentIds.Contains(Id))
			{
				bComplete = false;
				continue;
			}
			CurrentIds.Add(Id);
			UNetConnection* NetConnection = PC->GetNetConnection();
			const bool bConnected = !PC->IsPendingKillPending() && PC->Player &&
				(NetConnection ? NetConnection->State == USOCK_Open && !NetConnection->bPendingDestroy
				               : PC->IsLocalController());
			TSharedRef<FJsonObject> Player = MakeShareable(new FJsonObject());
			Player->SetStringField(TEXT("ut4_id"), Id);
			Player->SetBoolField(TEXT("spectator"), PS->bOnlySpectator);
			Player->SetBoolField(TEXT("bot"), false);
			Player->SetBoolField(TEXT("ready"), ReadyState ? ReadyState->IsPlayerReady(PS) : PS->GetTeamNum() < 2);
			Player->SetBoolField(TEXT("connected"), bConnected);
			Players.Add(MakeShareable(new FJsonValueObject(Player)));
		}
	}

	TArray<TSharedPtr<FJsonValue>> Joined;
	for (const auto& Pair : ArrivalLedger.Joined)
	{
		TSharedRef<FJsonObject> Join = MakeShareable(new FJsonObject());
		Join->SetStringField(TEXT("ut4_id"), Pair.Key);
		Join->SetNumberField(TEXT("first_seen_seconds"), Pair.Value);
		Joined.Add(MakeShareable(new FJsonValueObject(Join)));
	}
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetNumberField(TEXT("pug_id"), PugId);
	Json->SetNumberField(TEXT("version"), 1);
	Json->SetStringField(TEXT("launch_id"), ArrivalLaunchId);
	Json->SetStringField(TEXT("instance_id"), ArrivalInstanceId);
	Json->SetNumberField(TEXT("sequence"), ++ArrivalSequence);
	// ResolveArrival may discover an account during this snapshot; stamp elapsed
	// after enumeration so its first-seen time cannot exceed the envelope time.
	Json->SetNumberField(TEXT("elapsed_seconds"), FPlatformTime::Seconds() - ArrivalStartedAt);
	Json->SetStringField(TEXT("state"), GS ? GS->GetMatchState().ToString() : TEXT("Unknown"));
	Json->SetBoolField(TEXT("complete"), bComplete && ArrivalLedger.bComplete);
	Json->SetBoolField(TEXT("healthy"), bHealthy);
	Json->SetArrayField(TEXT("players"), Players);
	Json->SetArrayField(TEXT("joined"), Joined);
	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	ArrivalRequest = FHttpModule::Get().CreateRequest();
	ArrivalRequestStartedAt = Now;
	ArrivalRequest->SetURL(BotApiUrl + TEXT("/arrival"));
	ArrivalRequest->SetVerb(TEXT("POST"));
	ArrivalRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	if (!BotApiToken.IsEmpty()) ArrivalRequest->SetHeader(TEXT("api-token"), BotApiToken);
	ArrivalRequest->SetContentAsString(Output);
	TWeakObjectPtr<AMutBotEvents> WeakThis(this);
	ArrivalRequest->OnProcessRequestComplete().BindLambda(
		[WeakThis](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bConnected)
	{
		if (WeakThis.IsValid() && WeakThis->ArrivalRequest == Request)
		{
			WeakThis->ArrivalRequest.Reset();
		}
	});
	if (!ArrivalRequest->ProcessRequest()) ArrivalRequest.Reset();
}

// ────────────────────────────────────────────────────────────────────
// HTTP Sending (StatSQL pattern)
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::SendPost(const FString& Endpoint, const FString& JsonBody, int32 RetryCount)
{
	if (BotApiUrl.IsEmpty()) return;

	TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(BotApiUrl + Endpoint);
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));

	if (!BotApiToken.IsEmpty())
	{
		Request->SetHeader(TEXT("api-token"), BotApiToken);
	}

	Request->SetContentAsString(JsonBody);

	TWeakObjectPtr<AMutBotEvents> WeakThis(this);
	FString CapturedEndpoint = Endpoint;
	TSharedRef<FString> CapturedBody = MakeShareable(new FString(JsonBody));

	Request->OnProcessRequestComplete().BindLambda(
		[WeakThis, RetryCount, CapturedEndpoint, CapturedBody](
			FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnected)
	{
		if (!WeakThis.IsValid()) return;

		bool bSuccess = bConnected && Resp.IsValid() &&
			EHttpResponseCodes::IsOk(Resp->GetResponseCode());

		if (!bSuccess && RetryCount < WeakThis->MaxRetries)
		{
			float Delay = FMath::Pow(2.f, (float)RetryCount);
			UE_LOG(LogBotEvents, Warning, TEXT("POST %s failed (attempt %d/%d), retrying in %.0fs"),
				*CapturedEndpoint, RetryCount + 1, WeakThis->MaxRetries, Delay);

			FTimerHandle RetryHandle;
			WeakThis->GetWorldTimerManager().SetTimer(RetryHandle,
				[WeakThis, CapturedEndpoint, CapturedBody, RetryCount]()
			{
				if (WeakThis.IsValid())
				{
					WeakThis->SendPost(CapturedEndpoint, *CapturedBody, RetryCount + 1);
				}
			}, Delay, false);
			return;
		}

		if (!bSuccess)
		{
			int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
			UE_LOG(LogBotEvents, Error, TEXT("POST %s FAILED after %d attempts (HTTP %d)"),
				*CapturedEndpoint, WeakThis->MaxRetries, Code);
		}
		else
		{
			UE_LOG(LogBotEvents, Verbose, TEXT("POST %s succeeded"), *CapturedEndpoint);
		}
	});

	Request->ProcessRequest();
}

// ────────────────────────────────────────────────────────────────────
// JSON Helpers
// ────────────────────────────────────────────────────────────────────

FString AMutBotEvents::BuildPlayerListJson() const
{
	TArray<TSharedPtr<FJsonValue>> PlayersArray;

	UWorld* World = GetWorld();
	if (!World) return TEXT("[]");

	AUTGameState* GS = World->GetGameState<AUTGameState>();
	if (!GS) return TEXT("[]");
	ANCReadyUpState* ReadyState = ANCReadyUpState::Find(World);

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS->bOnlySpectator) continue;

		// Player ready-up has its own UTComp-style state. Legacy host-controlled
		// matches retain the old connected-and-on-a-team meaning for compatibility.
		const bool bReady = ReadyState != nullptr
			? (UTPS->bIsABot || ReadyState->IsPlayerReady(UTPS))
			: (UTPS->GetTeamNum() < 2);

		TSharedRef<FJsonObject> PlayerObj = MakeShareable(new FJsonObject());
		PlayerObj->SetStringField(TEXT("Name"), UTPS->PlayerName);
		PlayerObj->SetNumberField(TEXT("Index"), UTPS->PlayerId);
		PlayerObj->SetNumberField(TEXT("Id"), UTPS->PlayerId);
		// StatSQL/Django identity is a string, distinct from the legacy numeric
		// PlayerId above. This snapshot is diagnostic; /arrival retains the
		// server connection's UniqueId and cumulative first-login evidence.
		PlayerObj->SetStringField(TEXT("StatsID"), UTPS->StatsID);
		PlayerObj->SetBoolField(TEXT("Ready"), bReady);
		PlayerObj->SetStringField(TEXT("Password"), TEXT("")); // Not available server-side
		PlayerObj->SetNumberField(TEXT("Team"), UTPS->GetTeamNum());
		PlayerObj->SetStringField(TEXT("match_id"), GetMatchId());

		PlayersArray.Add(MakeShareable(new FJsonValueObject(PlayerObj)));
	}

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(PlayersArray, Writer);
	return Output;
}

FString AMutBotEvents::BuildTeamScoresJson() const
{
	TArray<TSharedPtr<FJsonValue>> TeamsArray;

	AUTGameState* GS = GetWorld() ? GetWorld()->GetGameState<AUTGameState>() : nullptr;
	if (!GS) return TEXT("[]");

	for (int32 i = 0; i < 2; i++)
	{
		int32 Score = 0;
		if (GS->Teams.IsValidIndex(i) && GS->Teams[i])
		{
			Score = GS->Teams[i]->Score;
		}

		TSharedRef<FJsonObject> TeamObj = MakeShareable(new FJsonObject());
		TeamObj->SetNumberField(TEXT("id"), i);
		TeamObj->SetNumberField(TEXT("score"), Score);
		TeamsArray.Add(MakeShareable(new FJsonValueObject(TeamObj)));
	}

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(TeamsArray, Writer);
	return Output;
}

FString AMutBotEvents::GetMatchId() const
{
	// Use the game's session ID as a unique match identifier
	UWorld* World = GetWorld();
	if (World && World->GetGameInstance())
	{
		return World->URL.Map + TEXT("_") + FString::FromInt(PugId);
	}
	return FString::Printf(TEXT("pug_%d"), PugId);
}

float AMutBotEvents::GetTimeSeconds() const
{
	UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.f;
}
