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
#include "Json.h"

DEFINE_LOG_CATEGORY(LogBotEvents);

AMutBotEvents::AMutBotEvents(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PugId = -1;
	bFlagEventsBound = false;
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
}

void AMutBotEvents::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopReadyPolling();
	Super::EndPlay(EndPlayReason);
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

	// MONSTER KILL only — fire on exact threshold, not on 5+ kills (those just
	// keep saying MONSTER from the engine's POV).
	if (KillerPS->MultiKillLevel == 4)
	{
		PostReward(KillerPS, TEXT("monster"), 4);
	}

	// DOMINATING and above (level 3..5). The %5==0 gate is the engine's own
	// milestone check; matches AUTGameMode::ScoreKill at UTGameMode.cpp:2846.
	if (KillerPS->Spree > 0 && (KillerPS->Spree % 5) == 0)
	{
		const int32 SpreeLevel = KillerPS->Spree / 5;
		if (SpreeLevel >= 3 && SpreeLevel <= 5)
		{
			PostReward(KillerPS, TEXT("spree"), SpreeLevel);
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

void AMutBotEvents::PostReward(AUTPlayerState* Scorer, const FString& Type, int32 Level)
{
	if (Scorer == nullptr || BotApiUrl.IsEmpty()) return;

	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject());
	Json->SetStringField(TEXT("type"),         Type);       // "monster" | "spree"
	Json->SetNumberField(TEXT("level"),        Level);      // monster=4; spree=3..5
	Json->SetStringField(TEXT("player_name"),  Scorer->PlayerName);
	Json->SetNumberField(TEXT("player_team"),  Scorer->GetTeamNum());
	Json->SetNumberField(TEXT("pug_id"),       PugId);
	Json->SetStringField(TEXT("match_id"),     GetMatchId());
	Json->SetNumberField(TEXT("time_seconds"), GetTimeSeconds());

	FString Output;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Output);
	FJsonSerializer::Serialize(Json, Writer);

	SendPost(TEXT("/reward"), Output);

	UE_LOG(LogBotEvents, Log, TEXT("Reward: %s lvl=%d by %s (team %d)"),
		*Type, Level, *Scorer->PlayerName, Scorer->GetTeamNum());
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

	for (APlayerState* PS : GS->PlayerArray)
	{
		AUTPlayerState* UTPS = Cast<AUTPlayerState>(PS);
		if (!UTPS || UTPS->bOnlySpectator) continue;

		// With host-controlled matches, "ready" = connected and on a team.
		// GetTeamNum() returns 255 if not on a team yet.
		bool bReady = (UTPS->GetTeamNum() < 2);

		TSharedRef<FJsonObject> PlayerObj = MakeShareable(new FJsonObject());
		PlayerObj->SetStringField(TEXT("Name"), UTPS->PlayerName);
		PlayerObj->SetNumberField(TEXT("Index"), UTPS->PlayerId);
		PlayerObj->SetNumberField(TEXT("Id"), UTPS->PlayerId);
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
