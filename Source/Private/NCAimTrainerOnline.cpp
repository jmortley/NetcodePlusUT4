#include "NCAimTrainerOnline.h"
#include "Engine/World.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "Runtime/Online/HTTP/Public/HttpModule.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpRequest.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr int32 MaxResponseBytes = 32768;
	constexpr float RequestTimeoutSeconds = 15.0f;

	struct FTrainerOnlineConfig
	{
		FString BaseUrl = TEXT("https://ut4stats.com");
		FString Key;
		bool bEnabled = true;
	};

	FTrainerOnlineConfig ReadConfig()
	{
		FTrainerOnlineConfig Config;
		FConfigFile Ini;
		Ini.Read(FPaths::GameSavedDir() / TEXT("Config/Mod.ini"));
		Ini.GetString(TEXT("UTPUGS_STATS"), TEXT("Key"), Config.Key);
		Ini.GetString(TEXT("NCAimTrainer"), TEXT("ApiBaseUrl"), Config.BaseUrl);
		Ini.GetBool(TEXT("NCAimTrainer"), TEXT("OnlineEnabled"), Config.bEnabled);
		while (Config.BaseUrl.EndsWith(TEXT("/"))) Config.BaseUrl = Config.BaseUrl.LeftChop(1);
		// The destination is an operator setting, never a client RPC parameter.
		// Credentials must not travel over cleartext or be embedded in a URL.
		if (!Config.BaseUrl.StartsWith(TEXT("https://")) || Config.BaseUrl.Contains(TEXT("@"))
			|| Config.BaseUrl.Contains(TEXT("?")) || Config.BaseUrl.Contains(TEXT("#")))
		{
			Config.bEnabled = false;
		}
		return Config;
	}

	/** One HTTP attempt. A world timer cancels stalled requests without changing
	 *  global engine HTTP settings. The request delegate retains this object;
	 *  its weak request pointer prevents an ownership cycle. */
	struct FTrainerRequest : TSharedFromThis<FTrainerRequest>
	{
		TWeakObjectPtr<UWorld> World;
		TWeakPtr<IHttpRequest> Request;
		FTimerHandle Timeout;
		bool bFinished = false;
		TFunction<void(int32, const FString&)> Completion;

		void Finish(int32 Code, const FString& Body)
		{
			if (bFinished) return;
			bFinished = true;
			if (UWorld* W = World.Get())
			{
				W->GetTimerManager().ClearTimer(Timeout);
				Completion(Code, Body);
			}
		}
	};

	void Send(UWorld* World, const FString& Url, const FString& Key, const FString& Body,
		TFunction<void(int32, const FString&)> Completion)
	{
		TSharedRef<FTrainerRequest> Operation = MakeShareable(new FTrainerRequest);
		Operation->World = World;
		Operation->Completion = MoveTemp(Completion);
		TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
		Operation->Request = Request;
		Request->SetURL(Url);
		Request->SetVerb(Body.IsEmpty() ? TEXT("GET") : TEXT("POST"));
		Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
		if (!Body.IsEmpty())
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetHeader(TEXT("Authorization"), TEXT("Token ") + Key);
			Request->SetContentAsString(Body);
		}
		Request->OnProcessRequestComplete().BindLambda(
			[Operation](FHttpRequestPtr Req, FHttpResponsePtr Response, bool bConnected)
		{
			if (!bConnected || !Response.IsValid()) { Operation->Finish(0, FString()); return; }
			if (Response->GetContent().Num() > MaxResponseBytes) { Operation->Finish(413, FString()); return; }
			Operation->Finish(Response->GetResponseCode(), Response->GetContentAsString());
		});
		World->GetTimerManager().SetTimer(Operation->Timeout, [Operation]()
		{
			Operation->Finish(0, FString());
			TSharedPtr<IHttpRequest> Active = Operation->Request.Pin();
			if (Active.IsValid())
			{
				Active->OnProcessRequestComplete().Unbind();
				Active->CancelRequest();
			}
		}, RequestTimeoutSeconds, false);
		if (!Request->ProcessRequest()) Operation->Finish(0, FString());
	}

	bool IsMatchingAcknowledgement(const FString& Response, const FNCAimTrainerResult& Expected)
	{
		TSharedPtr<FJsonObject> Ack;
		bool bAccepted = false;
		FString Id, Scenario;
		double Revision = 0, Score = 0;
		FGuid ReturnedId, ExpectedId;
		return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response), Ack)
			&& Ack.IsValid() && Ack->TryGetBoolField(TEXT("accepted"), bAccepted) && bAccepted
			&& Ack->TryGetStringField(TEXT("run_id"), Id) && FGuid::Parse(Id, ReturnedId)
			&& FGuid::Parse(Expected.RunId, ExpectedId) && ReturnedId == ExpectedId
			&& Ack->TryGetStringField(TEXT("scenario"), Scenario)
			&& Scenario == FNCAimTrainerOnline::ScenarioSlug(Expected.Scenario)
			&& Ack->TryGetNumberField(TEXT("revision"), Revision) && Revision == 1.0
			&& Ack->TryGetNumberField(TEXT("score"), Score) && Score == Expected.Score;
	}

	void SubmitAttempt(TWeakObjectPtr<UWorld> World, const FTrainerOnlineConfig& Config,
		const FString& Body, const FNCAimTrainerResult& Expected, int32 Attempt,
		TSharedRef<TFunction<void(bool, const FString&)>> Completion)
	{
		UWorld* W = World.Get();
		if (!W) return;
		Send(W, Config.BaseUrl + TEXT("/aimtrainer_entry/"), Config.Key, Body,
			[World, Config, Body, Expected, Attempt, Completion](int32 Code, const FString& Response)
		{
			UWorld* CurrentWorld = World.Get();
			if (!CurrentWorld) return;
			if ((Code == 200 || Code == 201) && IsMatchingAcknowledgement(Response, Expected))
			{
				(*Completion)(true, TEXT("Score saved to UT4Stats"));
				return;
			}
			// Idempotent run IDs make timeout retries safe. Never retry rejected
			// credentials, invalid results, or duplicate IDs with conflicting data.
			if ((Code == 0 || Code == 429 || Code >= 500) && Attempt < 3)
			{
				FTimerHandle Retry;
				CurrentWorld->GetTimerManager().SetTimer(Retry, [World, Config, Body, Expected, Attempt, Completion]()
				{
					SubmitAttempt(World, Config, Body, Expected, Attempt + 1, Completion);
				}, Attempt == 1 ? 2.0f : 4.0f, false);
				return;
			}
			const FString Message = (Code == 401 || Code == 403)
				? TEXT("Practice result only: this host is not approved for ranked scores")
				: TEXT("Score upload unavailable; this result was not confirmed online");
			(*Completion)(false, Message);
		});
	}

	bool ReadBoundedNumber(const TSharedPtr<FJsonObject>& Object, const TCHAR* Key,
		double Min, double Max, double& Out)
	{
		return Object->TryGetNumberField(Key, Out) && FMath::IsFinite(Out) && Out >= Min && Out <= Max;
	}
}

const TCHAR* FNCAimTrainerOnline::ScenarioSlug(int32 Scenario)
{
	return Scenario == 0 ? TEXT("strafe") : Scenario == 1 ? TEXT("headshots") : Scenario == 2 ? TEXT("instagib") : TEXT("");
}

void FNCAimTrainerOnline::Submit(UWorld* World, const FNCAimTrainerResult& Result,
	TFunction<void(bool, const FString&)> Completion)
{
	if (!World || World->GetNetMode() == NM_Client) return;
	const FTrainerOnlineConfig Config = ReadConfig();
	if (!Config.bEnabled || Config.Key.IsEmpty() || World->GetNetMode() == NM_Standalone)
	{
		Completion(false, TEXT("Practice result: shared scores require an approved online server"));
		return;
	}
	if (Result.Scenario < 0 || Result.Scenario > 2 || Result.PlayerId.IsEmpty())
	{
		Completion(false, TEXT("Practice result: no authenticated player identity"));
		return;
	}
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("run_id"), Result.RunId);
	Json->SetStringField(TEXT("player_id"), Result.PlayerId);
	Json->SetStringField(TEXT("display_name"), Result.DisplayName.Left(64));
	Json->SetStringField(TEXT("scenario"), ScenarioSlug(Result.Scenario));
	Json->SetNumberField(TEXT("revision"), 1);
	Json->SetNumberField(TEXT("score"), Result.Score);
	Json->SetNumberField(TEXT("shots"), Result.Shots);
	Json->SetNumberField(TEXT("hits"), Result.Hits);
	Json->SetNumberField(TEXT("headshots"), Result.Headshots);
	Json->SetNumberField(TEXT("targets_expired"), Result.TargetsExpired);
	Json->SetNumberField(TEXT("tracked_ms"), Result.TrackedMilliseconds);
	Json->SetNumberField(TEXT("duration_ms"), Result.DurationMilliseconds);
	FString Body;
	FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Body));
	SubmitAttempt(World, Config, Body, Result, 1,
		MakeShareable(new TFunction<void(bool, const FString&)>(MoveTemp(Completion))));
}

void FNCAimTrainerOnline::Fetch(UWorld* World, int32 Scenario,
	TFunction<void(bool, const TArray<FNCAimTrainerLeaderboardRow>&)> Completion)
{
	if (!World || World->GetNetMode() == NM_Client) return;
	const FTrainerOnlineConfig Config = ReadConfig();
	if (!Config.bEnabled || Scenario < 0 || Scenario > 2)
	{
		Completion(false, TArray<FNCAimTrainerLeaderboardRow>());
		return;
	}
	const FString Url = Config.BaseUrl + FString::Printf(
		TEXT("/aimtrainer_leaderboard/?scenario=%s&revision=1&limit=10"), ScenarioSlug(Scenario));
	Send(World, Url, FString(), FString(),
		[Scenario, Completion](int32 Code, const FString& Body)
	{
		TArray<FNCAimTrainerLeaderboardRow> Rows;
		TSharedPtr<FJsonObject> Json;
		const TArray<TSharedPtr<FJsonValue>>* JsonRows = nullptr;
		FString ReturnedScenario;
		double Revision = 0;
		if (Code != 200 || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Json)
			|| !Json.IsValid() || !Json->TryGetStringField(TEXT("scenario"), ReturnedScenario)
			|| ReturnedScenario != ScenarioSlug(Scenario)
			|| !ReadBoundedNumber(Json, TEXT("revision"), 1, 1, Revision)
			|| !Json->TryGetArrayField(TEXT("rows"), JsonRows) || JsonRows->Num() > 10)
		{
			Completion(false, Rows);
			return;
		}
		for (const TSharedPtr<FJsonValue>& Value : *JsonRows)
		{
			const TSharedPtr<FJsonObject>* Row = nullptr;
			double Rank = 0, Score = 0, Accuracy = 0;
			FString Name;
			if (!Value.IsValid() || !Value->TryGetObject(Row) || !Row->IsValid()
				|| !(*Row)->TryGetStringField(TEXT("display_name"), Name)
				|| !ReadBoundedNumber(*Row, TEXT("rank"), 1, 10, Rank)
				|| !ReadBoundedNumber(*Row, TEXT("score"), 0, 60000, Score)
				|| !ReadBoundedNumber(*Row, TEXT("accuracy_pct"), 0, 100, Accuracy))
			{
				Completion(false, TArray<FNCAimTrainerLeaderboardRow>());
				return;
			}
			FNCAimTrainerLeaderboardRow Entry;
			Entry.Rank = int32(Rank);
			Entry.DisplayName = Name.Left(32).Replace(TEXT("\n"), TEXT(" ")).Replace(TEXT("\r"), TEXT(" "));
			Entry.Score = int32(Score);
			Entry.AccuracyPercent = float(Accuracy);
			Rows.Add(Entry);
		}
		Completion(true, Rows);
	});
}
