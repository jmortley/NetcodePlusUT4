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
#include "UTCTFGameState.h"
#include "Json.h"

DEFINE_LOG_CATEGORY(LogBotEvents);

AMutBotEvents::AMutBotEvents(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bReplicates = true;
	bAlwaysRelevant = true;
	PugId = -1;
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
// Flag Captures
// ────────────────────────────────────────────────────────────────────

void AMutBotEvents::ScoreObject_Implementation(
	AUTCarriedObject* GameObject, AUTCharacter* HolderPawn,
	AUTPlayerState* Holder, FName Reason)
{
	if (GetNetMode() == NM_Client || BotApiUrl.IsEmpty()) return;

	if (Reason == FName(TEXT("FlagCapture")) && Holder)
	{
		PostFlagCapture(Holder);
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

	UE_LOG(LogBotEvents, Log, TEXT("Flag capture: %s (team %d) | Score: %d-%d"),
		*PlayerName, TeamIndex, ScoreRed, ScoreBlue);
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
