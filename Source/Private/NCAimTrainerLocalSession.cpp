#include "NCAimTrainerLocalSession.h"
// Stock UT headers require the full engine types, even when this file starts a unity batch.
#include "UnrealTournament.h"
#include "NCAimTrainerOnline.h"
#include "NCAimTrainerScenarioPolicy.h"
#include "NCAimTrainerLocalHttp.h"
#include "Engine/World.h"
#include "UTLocalPlayer.h"
#include "OnlineSubsystemUtils.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpRequest.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace NCAimTrainerLocal
{
	// Account tokens must never be sent to the configurable server-upload URL.
	const TCHAR* const StartUrl = TEXT("https://ut4stats.com/aimtrainer_local/start/");
	const TCHAR* const CheckpointUrl = TEXT("https://ut4stats.com/aimtrainer_local/checkpoint/");
	constexpr int32 MaxResponseBytes = 8192;
	constexpr int32 MaxCheckpointBytes = 65536;
	constexpr int32 MaxBatchEvents = 256;
	constexpr int32 MaxRunEvents = 4096;
	constexpr int32 MaxPendingCheckpoints = 4;
	constexpr float TimeoutSeconds = 4.0f;

	bool OnlineEnabled()
	{
		FConfigFile Ini;
		Ini.Read(FPaths::GameSavedDir() / TEXT("Config/Mod.ini"));
		bool bEnabled = true;
		Ini.GetBool(TEXT("NCAimTrainer"), TEXT("OnlineEnabled"), bEnabled);
		return bEnabled;
	}

	bool IsBoundedText(const FString& Value, int32 MaxLength, bool bAllowSpaces)
	{
		if (Value.IsEmpty() || Value.Len() > MaxLength) return false;
		for (int32 Index = 0; Index < Value.Len(); ++Index)
		{
			if (Value[Index] < (bAllowSpaces ? 32 : 33) || Value[Index] == 127) return false;
		}
		return true;
	}

	bool ReadInteger(const TSharedPtr<FJsonObject>& Json, const TCHAR* Field, int32 Min, int32 Max, int32& Out)
	{
		double Value = 0;
		if (!Json->TryGetNumberField(Field, Value) || !FMath::IsFinite(Value)
			|| Value < Min || Value > Max || Value != double(int32(Value))) return false;
		Out = int32(Value);
		return true;
	}

	IOnlineIdentityPtr GetIdentity(UWorld* World, int32 ControllerId)
	{
		UUTLocalPlayer* LocalPlayer = World ? Cast<UUTLocalPlayer>(World->GetFirstLocalPlayerFromController()) : nullptr;
		if (!LocalPlayer || LocalPlayer->GetControllerId() != ControllerId) return nullptr;
		return Online::GetIdentityInterface(World);
	}
}

TSharedPtr<FNCAimTrainerLocalSession> FNCAimTrainerLocalSession::Start(UWorld* InWorld, int32 Scenario, const FString& RequestId,
	TFunction<void(TSharedPtr<FNCAimTrainerLocalSession>, const FString&)> InCompletion, bool bInMovementPractice)
{
	using namespace NCAimTrainerLocal;
	FGuid ParsedId;
	if (!InWorld || InWorld->GetNetMode() != NM_Standalone || !NCAimTrainerScenarioPolicy::IsValidScenario(Scenario)
		|| !FGuid::Parse(RequestId, ParsedId) || !ParsedId.IsValid()
		|| RequestId != ParsedId.ToString(EGuidFormats::DigitsWithHyphens).ToLower())
	{
		InCompletion(nullptr, TEXT("Practice only: local upload could not start"));
		return nullptr;
	}
	if (!OnlineEnabled())
	{
		InCompletion(nullptr, TEXT("Practice only: online scores are disabled"));
		return nullptr;
	}
	UUTLocalPlayer* LocalPlayer = Cast<UUTLocalPlayer>(InWorld->GetFirstLocalPlayerFromController());
	const int32 LocalControllerId = LocalPlayer ? LocalPlayer->GetControllerId() : INDEX_NONE;
	IOnlineIdentityPtr Identity = GetIdentity(InWorld, LocalControllerId);
	TSharedPtr<const FUniqueNetId> AccountId = Identity.IsValid() ? Identity->GetUniquePlayerId(LocalControllerId) : nullptr;
	if (!Identity.IsValid() || Identity->GetLoginStatus(LocalControllerId) != ELoginStatus::LoggedIn
		|| !AccountId.IsValid() || !AccountId->IsValid() || !IsBoundedText(AccountId->ToString(), 128, false))
	{
		InCompletion(nullptr, TEXT("Practice only: sign in to your UT account to save local scores"));
		return nullptr;
	}
	TSharedRef<FNCAimTrainerLocalSession> Session = MakeShareable(new FNCAimTrainerLocalSession);
	Session->World = InWorld;
	Session->ControllerId = LocalControllerId;
	Session->RunId = RequestId;
	Session->bMovementPractice = bInMovementPractice;
	Session->PlayerId = AccountId->ToString();
	Session->StartCompletion = MoveTemp(InCompletion);
	Session->BuildStartBody(Scenario);
	TWeakPtr<FNCAimTrainerLocalSession> WeakSession = Session;
	Session->WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddLambda(
		[WeakSession](UWorld* CleanupWorld, bool, bool)
	{
		TSharedPtr<FNCAimTrainerLocalSession> Current = WeakSession.Pin();
		if (Current.IsValid() && Current->World.Get() == CleanupWorld) Current->Cancel();
	});
	Session->SendStartAttempt();
	return Session->IsHealthy() ? TSharedPtr<FNCAimTrainerLocalSession>(Session) : nullptr;
}

void FNCAimTrainerLocalSession::BuildStartBody(int32 Scenario)
{
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("request_id"), RunId);
	Json->SetStringField(TEXT("scenario"), FNCAimTrainerOnline::ScenarioSlug(Scenario));
	Json->SetNumberField(TEXT("revision"), FNCAimTrainerOnline::PresetRevisionForScenario(Scenario));
	Json->SetBoolField(TEXT("movement"), bMovementPractice);
	FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&StartBody));
}

FNCAimTrainerLocalSession::~FNCAimTrainerLocalSession()
{
	Cancel();
}

bool FNCAimTrainerLocalSession::IsHealthy() const
{
	UWorld* CurrentWorld = World.Get();
	return bHealthy && !bCancelled && CurrentWorld && CurrentWorld->GetNetMode() == NM_Standalone;
}

void FNCAimTrainerLocalSession::SendStartAttempt()
{
	using namespace NCAimTrainerLocal;
	if (!IsHealthy()) { Cancel(); return; }
	if (!OnlineEnabled()) { Fail(TEXT("Practice only: online scores are disabled")); return; }
	IOnlineIdentityPtr Identity = GetIdentity(World.Get(), ControllerId);
	TSharedPtr<const FUniqueNetId> AccountId = Identity.IsValid() ? Identity->GetUniquePlayerId(ControllerId) : nullptr;
	if (!Identity.IsValid() || Identity->GetLoginStatus(ControllerId) != ELoginStatus::LoggedIn
		|| !AccountId.IsValid() || !AccountId->IsValid() || AccountId->ToString() != PlayerId)
	{
		Fail(TEXT("Practice only: your UT account is no longer signed in"));
		return;
	}
	// The provider token lives only in this call and the request header. Retries
	// obtain it again from the same world/account; it is never saved or logged.
	const FString AccountToken = Identity->GetAuthToken(ControllerId);
	if (!IsBoundedText(AccountToken, 8192, false))
	{
		Fail(TEXT("Practice only: your UT account could not authenticate"));
		return;
	}
	SendRequest(StartUrl, AccountToken, StartBody);
}

void FNCAimTrainerLocalSession::SendRequest(const FString& Url, const FString& Token, const FString& Body)
{
	if (!IsHealthy()) { Cancel(); return; }
	bBusy = true;
	const int32 Serial = ++RequestSerial;
	TSharedPtr<IHttpRequest> Active =
#if WITH_DEV_AUTOMATION_TESTS
		TestRequestFactory ? TSharedPtr<IHttpRequest>(TestRequestFactory()) :
#endif
		CreateNCAimTrainerLocalRequest();
	if (!Active.IsValid())
	{
		Fail(TEXT("Practice only: secure local score uploads are unavailable on this platform"));
		return;
	}
	Request = Active;
	Active->SetURL(Url);
	Active->SetVerb(TEXT("POST"));
	Active->SetHeader(TEXT("Accept"), TEXT("application/json"));
	Active->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Active->SetHeader(TEXT("Authorization"), TEXT("Bearer ") + Token);
	Active->SetContentAsString(Body);
	TWeakPtr<FNCAimTrainerLocalSession> WeakSession = AsShared();
	// Start has no owner until its callback runs. Later requests keep only a weak
	// reference so dropping the run owner cancels outstanding network work.
	TSharedPtr<FNCAimTrainerLocalSession> StartingSession;
	if (bStarting) StartingSession = AsShared();
	Active->OnProcessRequestComplete().BindLambda(
		[WeakSession, StartingSession, Serial](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		TSharedPtr<FNCAimTrainerLocalSession> Session = WeakSession.Pin();
		if (!Session.IsValid()) return;
		if (!bConnected || !Response.IsValid()) Session->HandleResponse(Serial, 0, FString());
		else if (Response->GetContent().Num() > NCAimTrainerLocal::MaxResponseBytes)
			Session->HandleResponse(Serial, 413, FString());
		else Session->HandleResponse(Serial, Response->GetResponseCode(), Response->GetContentAsString());
	});
	Active->OnRequestProgress().BindLambda([WeakSession, Serial](FHttpRequestPtr, int32, int32 Received)
	{
		TSharedPtr<FNCAimTrainerLocalSession> Session = WeakSession.Pin();
		if (Session.IsValid() && Received > NCAimTrainerLocal::MaxResponseBytes)
			Session->HandleResponse(Serial, 413, FString());
	});
	World.Get()->GetTimerManager().SetTimer(Timeout, [WeakSession, StartingSession, Serial]()
	{
		TSharedPtr<FNCAimTrainerLocalSession> Session = WeakSession.Pin();
		if (Session.IsValid()) Session->HandleResponse(Serial, 0, FString());
	}, NCAimTrainerLocal::TimeoutSeconds, false);
	if (!Active->ProcessRequest()) HandleResponse(Serial, 0, FString());
}

void FNCAimTrainerLocalSession::StopRequest()
{
	++RequestSerial;
	if (UWorld* CurrentWorld = World.Get()) CurrentWorld->GetTimerManager().ClearTimer(Timeout);
	TSharedPtr<IHttpRequest> Active = Request.Pin();
	Request.Reset();
	if (Active.IsValid())
	{
		Active->OnProcessRequestComplete().Unbind();
		Active->OnRequestProgress().Unbind();
		Active->CancelRequest();
	}
}

void FNCAimTrainerLocalSession::HandleResponse(int32 Serial, int32 Code, const FString& Body)
{
	using namespace NCAimTrainerLocal;
	if (Serial != RequestSerial || bCancelled || !bHealthy) return;
	StopRequest();
	if (!IsHealthy()) { Cancel(); return; }
	if ((Code == 0 || Code == 429 || Code >= 500) && Attempt < 3)
	{
		ScheduleRetry();
		return;
	}
	bBusy = false;
	if (Code != 200 && Code != 201)
	{
		Fail((Code == 401 || Code == 403)
			? TEXT("Practice only: local score authentication was not accepted")
			: TEXT("Practice only: local score upload was not confirmed"));
		return;
	}
	TSharedPtr<FJsonObject> Json;
	FString ReturnedId;
	bool bReturnedMovement = false;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Body), Json) || !Json.IsValid()
		|| !Json->TryGetStringField(TEXT("run_id"), ReturnedId) || ReturnedId != RunId
		|| !Json->TryGetBoolField(TEXT("movement"), bReturnedMovement) || bReturnedMovement != bMovementPractice)
	{
		Fail(TEXT("Practice only: the score service returned an invalid response"));
		return;
	}
	if (bStarting)
	{
		FString ReturnedPlayerId, ReturnedName, ReturnedToken;
		int32 CheckpointMs = 0, ExpiresIn = 0;
		if (!Json->TryGetStringField(TEXT("player_id"), ReturnedPlayerId) || ReturnedPlayerId != PlayerId
			|| !Json->TryGetStringField(TEXT("display_name"), ReturnedName) || !IsBoundedText(ReturnedName, 64, true)
			|| !Json->TryGetStringField(TEXT("run_token"), ReturnedToken) || !IsBoundedText(ReturnedToken, 512, false)
			|| !ReadInteger(Json, TEXT("checkpoint_ms"), 5000, 5000, CheckpointMs)
			|| !ReadInteger(Json, TEXT("expires_in"), 1, 180, ExpiresIn))
		{
			Fail(TEXT("Practice only: the score service could not verify this UT account"));
			return;
		}
		DisplayName = ReturnedName;
		RunToken = ReturnedToken;
		ExpiresAt = FPlatformTime::Seconds() + ExpiresIn;
		StartBody.Empty();
		FinishStart(true, TEXT("Local score account verified"));
		return;
	}
	bool bAccepted = false, bReturnedFinal = false;
	FString Scope;
	int32 Sequence = 0, Score = 0;
	if (Pending.Num() == 0 || !Json->TryGetBoolField(TEXT("accepted"), bAccepted) || !bAccepted
		|| !Json->TryGetBoolField(TEXT("final"), bReturnedFinal) || bReturnedFinal != Pending[0].bFinal
		|| !Json->TryGetStringField(TEXT("scope"), Scope) || Scope != TEXT("local_checkpoints")
		|| !ReadInteger(Json, TEXT("sequence"), Pending[0].Sequence, Pending[0].Sequence, Sequence)
		|| !ReadInteger(Json, TEXT("score"), 0, 60000, Score))
	{
		Fail(TEXT("Practice only: a local checkpoint was not confirmed"));
		return;
	}
	// The game starts its 60-second clock only after the service confirms this
	// anchor. A lost initial POST must not make checkpoint 1 arrive too early
	// relative to the server's eventual first receipt of checkpoint 0.
	if (Pending[0].Sequence == 0) bAnchorAcknowledged = true;
	Pending.RemoveAt(0);
	Attempt = 1;
	if (bReturnedFinal)
	{
		RunToken.Empty();
		FinalScore = Score;
		bResultReady = bResultSuccess = true;
		DeliverCompletion();
	}
	else if (Pending.Num() > 0) SendCheckpointAttempt();
}

void FNCAimTrainerLocalSession::ScheduleRetry()
{
	const float Delay = Attempt == 1 ? 1.0f : 2.0f;
	++Attempt;
	TWeakPtr<FNCAimTrainerLocalSession> WeakSession = AsShared();
	TSharedPtr<FNCAimTrainerLocalSession> StartingSession;
	if (bStarting) StartingSession = AsShared();
	World.Get()->GetTimerManager().SetTimer(Retry, [WeakSession, StartingSession]()
	{
		TSharedPtr<FNCAimTrainerLocalSession> Session = WeakSession.Pin();
		if (!Session.IsValid() || !Session->IsHealthy()) return;
		if (Session->bStarting) Session->SendStartAttempt();
		else Session->SendCheckpointAttempt();
	}, Delay, false);
}

void FNCAimTrainerLocalSession::FinishStart(bool bSuccess, const FString& Message)
{
	bStarting = false;
	Attempt = 1;
	if (StartCompletion)
	{
		TFunction<void(TSharedPtr<FNCAimTrainerLocalSession>, const FString&)> Callback = MoveTemp(StartCompletion);
		TSharedPtr<FNCAimTrainerLocalSession> Result;
		if (bSuccess) Result = AsShared();
		Callback(Result, Message);
	}
}

void FNCAimTrainerLocalSession::BeginRecording()
{
	if (!IsHealthy() || bStarting || bRecording || bResultReady) return;
	bRecording = true;
	QueueCheckpoint(0, TArray<FNCAimTrainerLocalEvent>(), false);
}

bool FNCAimTrainerLocalSession::QueueCheckpoint(int32 ElapsedMicroseconds,
	const TArray<FNCAimTrainerLocalEvent>& Events, bool bFinal)
{
	using namespace NCAimTrainerLocal;
	if (!IsHealthy() || bStarting || !bRecording || bResultReady || bFinalQueued) return false;
	if (Pending.Num() >= MaxPendingCheckpoints)
	{
		Fail(TEXT("Practice only: local checkpoints could not keep up with this run"));
		return false;
	}
	const bool bValidTime = NextSequence == 0 ? ElapsedMicroseconds == 0 && Events.Num() == 0 && !bFinal
		: NextSequence == 12 ? ElapsedMicroseconds == 60000000 && bFinal
		: NextSequence < 12 && !bFinal && ElapsedMicroseconds >= NextSequence * 5000000
			&& ElapsedMicroseconds <= NextSequence * 5000000 + 250000;
	if (!bValidTime || Events.Num() > MaxBatchEvents || EventCount + Events.Num() > MaxRunEvents)
	{
		Fail(TEXT("Practice only: local checkpoint timing or event limits were exceeded"));
		return false;
	}
	TArray<TSharedPtr<FJsonValue>> JsonEvents;
	int32 PreviousEventUs = LastEventUs;
	for (const FNCAimTrainerLocalEvent& Event : Events)
	{
		if (Event.TimeUs < LastElapsedUs || Event.TimeUs < PreviousEventUs || Event.TimeUs > ElapsedMicroseconds
			|| ((Event.Type == FNCAimTrainerLocalEvent::Hit || Event.Type == FNCAimTrainerLocalEvent::Expire)
				&& (Event.Target < 0 || Event.Target > 5 || Event.Appearance <= 0)))
		{
			Fail(TEXT("Practice only: local event timing was invalid"));
			return false;
		}
		TSharedRef<FJsonObject> JsonEvent = MakeShareable(new FJsonObject);
		JsonEvent->SetNumberField(TEXT("t"), Event.TimeUs);
		switch (Event.Type)
		{
		case FNCAimTrainerLocalEvent::Shot:
			JsonEvent->SetStringField(TEXT("type"), TEXT("shot"));
			break;
		case FNCAimTrainerLocalEvent::Hit:
		case FNCAimTrainerLocalEvent::Expire:
			JsonEvent->SetStringField(TEXT("type"), Event.Type == FNCAimTrainerLocalEvent::Hit ? TEXT("hit") : TEXT("expire"));
			JsonEvent->SetNumberField(TEXT("target"), Event.Target);
			JsonEvent->SetNumberField(TEXT("appearance"), Event.Appearance);
			if (Event.Type == FNCAimTrainerLocalEvent::Hit) JsonEvent->SetBoolField(TEXT("head"), Event.bHead);
			break;
		case FNCAimTrainerLocalEvent::Sample:
			JsonEvent->SetStringField(TEXT("type"), TEXT("sample"));
			JsonEvent->SetBoolField(TEXT("firing"), Event.bFiring);
			JsonEvent->SetBoolField(TEXT("contact"), Event.bContact);
			break;
		default:
			Fail(TEXT("Practice only: a local event was invalid"));
			return false;
		}
		JsonEvents.Add(MakeShareable(new FJsonValueObject(JsonEvent)));
		PreviousEventUs = Event.TimeUs;
	}
	TSharedRef<FJsonObject> Json = MakeShareable(new FJsonObject);
	Json->SetStringField(TEXT("run_id"), RunId);
	Json->SetNumberField(TEXT("sequence"), NextSequence);
	Json->SetNumberField(TEXT("elapsed_us"), ElapsedMicroseconds);
	Json->SetArrayField(TEXT("events"), JsonEvents);
	Json->SetBoolField(TEXT("final"), bFinal);
	FCheckpoint Checkpoint;
	FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Checkpoint.Body));
	if (FTCHARToUTF8(*Checkpoint.Body).Length() > MaxCheckpointBytes)
	{
		Fail(TEXT("Practice only: a local checkpoint exceeded the upload limit"));
		return false;
	}
	Checkpoint.Sequence = NextSequence++;
	Checkpoint.bFinal = bFinal;
	Checkpoint.QueuedAt = FPlatformTime::Seconds();
	Pending.Add(MoveTemp(Checkpoint));
	LastElapsedUs = ElapsedMicroseconds;
	LastEventUs = PreviousEventUs;
	EventCount += Events.Num();
	bFinalQueued = bFinal;
	if (!bBusy) SendCheckpointAttempt();
	return IsHealthy();
}

void FNCAimTrainerLocalSession::SendCheckpointAttempt()
{
	if (!IsHealthy()) { Cancel(); return; }
	if (Pending.Num() == 0 || bResultReady) return;
	if (!NCAimTrainerLocal::OnlineEnabled()) { Fail(TEXT("Practice only: online scores are disabled")); return; }
	const double Now = FPlatformTime::Seconds();
	if (Now >= ExpiresAt || Now - Pending[0].QueuedAt > 15.0)
	{
		Fail(TEXT("Practice only: the local score upload expired"));
		return;
	}
	// Pending[0].Body never changes, including on retries after a lost reply.
	SendRequest(NCAimTrainerLocal::CheckpointUrl, RunToken, Pending[0].Body);
}

void FNCAimTrainerLocalSession::Fail(const FString& Reason)
{
	if (bCancelled || !bHealthy || bResultReady) return;
	bHealthy = false;
	bBusy = false;
	FailureReason = Reason;
	StopRequest();
	if (UWorld* CurrentWorld = World.Get()) CurrentWorld->GetTimerManager().ClearTimer(Retry);
	Pending.Empty();
	StartBody.Empty();
	RunToken.Empty();
	bResultReady = true;
	if (bStarting) FinishStart(false, Reason);
	else DeliverCompletion();
}

void FNCAimTrainerLocalSession::SetCompletion(TFunction<void(bool, const FString&, int32)> Callback)
{
	if (bCancelled || bCompletionDelivered) return;
	Completion = MoveTemp(Callback);
	DeliverCompletion();
}

void FNCAimTrainerLocalSession::DeliverCompletion()
{
	if (bCancelled || !bResultReady || bCompletionDelivered || !Completion) return;
	bCompletionDelivered = true;
	TFunction<void(bool, const FString&, int32)> Callback = MoveTemp(Completion);
	Callback(bResultSuccess, bResultSuccess ? TEXT("Score saved to the local checkpoint leaderboard") : FailureReason, FinalScore);
}

void FNCAimTrainerLocalSession::Cancel()
{
	if (bCancelled) return;
	bCancelled = true;
	bHealthy = false;
	bBusy = false;
	StopRequest();
	if (UWorld* CurrentWorld = World.Get()) CurrentWorld->GetTimerManager().ClearTimer(Retry);
	FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
	WorldCleanupHandle.Reset();
	Pending.Empty();
	StartBody.Empty();
	RunToken.Empty();
	StartCompletion = nullptr;
	Completion = nullptr;
}
