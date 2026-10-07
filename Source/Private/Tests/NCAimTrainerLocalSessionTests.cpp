#include "NetcodePlus.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "NCAimTrainerLocalSession.h"
#include "../NCAimTrainerLocalHttp.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpRequest.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpResponse.h"

namespace NCAimTrainerLocalTests
{
	// A transport adapter only. Production session methods and real UE timers
	// perform serialization, acknowledgement checks, retry and cancellation.
	class FResponse : public IHttpResponse
	{
	public:
		int32 Code;
		FString Body;
		TArray<uint8> Content;
		FResponse(int32 InCode, const FString& InBody) : Code(InCode), Body(InBody)
		{
			FTCHARToUTF8 Utf8(*Body);
			Content.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		}
		virtual FString GetURL() override { return FString(); }
		virtual FString GetURLParameter(const FString&) override { return FString(); }
		virtual FString GetHeader(const FString&) override { return FString(); }
		virtual TArray<FString> GetAllHeaders() override { return TArray<FString>(); }
		virtual FString GetContentType() override { return TEXT("application/json"); }
		virtual int32 GetContentLength() override { return Content.Num(); }
		virtual const TArray<uint8>& GetContent() override { return Content; }
		virtual int32 GetResponseCode() override { return Code; }
		virtual FString GetContentAsString() override { return Body; }
	};

	class FRequest : public IHttpRequest
	{
	public:
		FString Url, Verb, Body;
		TMap<FString, FString> Headers;
		TArray<uint8> Content;
		FHttpRequestCompleteDelegate Complete;
		FHttpRequestProgressDelegate Progress;
		FHttpResponsePtr Response;
		bool bProcessed = false, bCancelled = false;
		virtual FString GetURL() override { return Url; }
		virtual FString GetURLParameter(const FString&) override { return FString(); }
		virtual FString GetHeader(const FString& Key) override { return Headers.FindRef(Key); }
		virtual TArray<FString> GetAllHeaders() override { return TArray<FString>(); }
		virtual FString GetContentType() override { return GetHeader(TEXT("Content-Type")); }
		virtual int32 GetContentLength() override { return Content.Num(); }
		virtual const TArray<uint8>& GetContent() override { return Content; }
		virtual FString GetVerb() override { return Verb; }
		virtual void SetURL(const FString& Value) override { Url = Value; }
		virtual void SetVerb(const FString& Value) override { Verb = Value; }
		virtual void SetContent(const TArray<uint8>& Value) override { Content = Value; }
		virtual void SetContentAsString(const FString& Value) override { Body = Value; }
		virtual void SetHeader(const FString& Key, const FString& Value) override { Headers.Add(Key, Value); }
		virtual void AppendToHeader(const FString& Key, const FString& Value) override { SetHeader(Key, Value); }
		virtual bool ProcessRequest() override { bProcessed = true; return true; }
		virtual FHttpRequestCompleteDelegate& OnProcessRequestComplete() override { return Complete; }
		virtual FHttpRequestProgressDelegate& OnRequestProgress() override { return Progress; }
		virtual void CancelRequest() override { bCancelled = true; }
		virtual EHttpRequestStatus::Type GetStatus() override { return EHttpRequestStatus::Processing; }
		virtual const FHttpResponsePtr GetResponse() const override { return Response; }
		virtual void Tick(float) override {}
		virtual float GetElapsedTime() override { return 0.f; }
		void Reply(int32 Code, const FString& Value)
		{
			Response = MakeShareable(new FResponse(Code, Value));
			// Preserve the invoking delegate while production cancels/unbinds it.
			FHttpRequestCompleteDelegate Callback = Complete;
			Callback.ExecuteIfBound(AsShared(), Response, true);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FNCAimTrainerLocalSessionTest,
	"NetcodePlus.AimTrainer.LocalSession.Transport",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FNCAimTrainerLocalSessionTest::RunTest(const FString& Parameters)
{
	using namespace NCAimTrainerLocalTests;
	FConfigFile Ini;
	Ini.Read(FPaths::GameSavedDir() / TEXT("Config/Mod.ini"));
	bool bEnabled = true;
	Ini.GetBool(TEXT("NCAimTrainer"), TEXT("OnlineEnabled"), bEnabled);
	if (!bEnabled)
	{
		AddWarning(TEXT("LocalSession transport test skipped because OnlineEnabled=false; no setting was changed."));
		return true;
	}
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	TestNotNull(TEXT("Test world exists"), World);
	if (!World) return false;
	TArray<TSharedPtr<FRequest>> Requests;
	const FString Id = TEXT("01234567-89ab-cdef-0123-456789abcdef");
	const FString Ack0 = FString::Printf(TEXT("{\"accepted\":true,\"run_id\":\"%s\",\"sequence\":0,\"final\":false,\"score\":0,\"scope\":\"local_checkpoints\",\"movement\":false}"), *Id);
	uint64 TestFrame = GFrameCounter;
	auto TickTimers = [World, &TestFrame](float Seconds)
	{
		// Avoid changing the application's frame counter outside this isolated
		// timer tick. The temporary world is destroyed before the test returns.
		TGuardValue<uint64> FrameGuard(GFrameCounter, ++TestFrame);
		World->GetTimerManager().Tick(Seconds);
	};
	auto NewSession = [&]() -> TSharedPtr<FNCAimTrainerLocalSession>
	{
		Requests.Empty();
		TSharedPtr<FNCAimTrainerLocalSession> Session = MakeShareable(new FNCAimTrainerLocalSession);
		Session->World = World;
		Session->bStarting = false;
		Session->RunId = Id;
		Session->RunToken = TEXT("fixture-run-token");
		Session->ExpiresAt = FPlatformTime::Seconds() + 180;
		Session->TestRequestFactory = [&Requests]() -> TSharedRef<IHttpRequest>
		{
			TSharedRef<FRequest> Request = MakeShareable(new FRequest);
			Requests.Add(Request);
			return Request;
		};
		return Session;
	};
	// Exercise the actual v11 start encoder and decoder without an online
	// account or live provider, including the movement-board identity.
	TSharedPtr<FNCAimTrainerLocalSession> StartSession = NewSession();
	StartSession->bStarting = true;
	StartSession->PlayerId = TEXT("0123456789abcdef0123456789abcdef");
	StartSession->bMovementPractice = true;
	StartSession->BuildStartBody(3);
	TSharedPtr<FJsonObject> StartJson;
	TestTrue(TEXT("Start payload is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StartSession->StartBody), StartJson));
	if (StartJson.IsValid())
	{
		TestEqual(TEXT("Precision popup has an independent scenario"), StartJson->GetStringField(TEXT("scenario")), FString(TEXT("precision_popup")));
		TestEqual(TEXT("New runs use preset revision 11"), StartJson->GetNumberField(TEXT("revision")), 11.0);
		TestTrue(TEXT("Start payload includes movement mode"), StartJson->GetBoolField(TEXT("movement")));
	}
	const TCHAR* SactfSlugs[] = { TEXT("sactf_headshots"), TEXT("sactf_popup") };
	for (int32 Scenario = 4; Scenario < 6; ++Scenario)
	{
		StartSession->BuildStartBody(Scenario);
		TSharedPtr<FJsonObject> SactfJson;
		TestTrue(TEXT("SACTF start payload is JSON"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StartSession->StartBody), SactfJson));
		if (SactfJson.IsValid())
		{
			TestEqual(TEXT("SACTF scenarios submit independent slugs"), SactfJson->GetStringField(TEXT("scenario")), FString(SactfSlugs[Scenario - 4]));
			TestEqual(TEXT("SACTF uses revision 11"), SactfJson->GetNumberField(TEXT("revision")), 11.0);
			TestTrue(TEXT("SACTF preserves movement board identity"), SactfJson->GetBoolField(TEXT("movement")));
		}
	}
	const FString StartAck = FString::Printf(TEXT("{\"run_id\":\"%s\",\"player_id\":\"0123456789abcdef0123456789abcdef\",\"display_name\":\"Fixture\",\"run_token\":\"fixture-run-token\",\"checkpoint_ms\":5000,\"expires_in\":180,\"movement\":true}"), *Id);
	StartSession->HandleResponse(StartSession->RequestSerial, 201, StartAck);
	TestTrue(TEXT("Matching movement start acknowledgement is accepted"), StartSession->IsHealthy() && !StartSession->bStarting);
	StartSession->BeginRecording();
	Requests.Last()->Reply(200, Ack0);
	TestFalse(TEXT("Fixed-position acknowledgement cannot confirm a movement checkpoint"), StartSession->IsHealthy());
	StartSession->Cancel();
	StartSession = NewSession();
	StartSession->bStarting = true;
	StartSession->PlayerId = TEXT("0123456789abcdef0123456789abcdef");
	StartSession->HandleResponse(StartSession->RequestSerial, 201, StartAck);
	TestFalse(TEXT("Movement start acknowledgement cannot confirm a fixed-position run"), StartSession->IsHealthy());
	StartSession->Cancel();
	StartSession.Reset();
	int32 Completions = 0;
	TSharedPtr<FNCAimTrainerLocalSession> Session = NewSession();
	Session->SetCompletion([&Completions](bool, const FString&, int32) { ++Completions; });
	Session->BeginRecording();
	TestEqual(TEXT("BeginRecording posts exactly one clock checkpoint"), Requests.Num(), 1);
	TestFalse(TEXT("The timed run cannot start before the clock acknowledgement"), Session->IsRecordingReady());
	TickTimers(0.f);
	TickTimers(4.01f);
	TestTrue(TEXT("Four-second timeout cancels stalled transport"), Requests[0]->bCancelled);
	TickTimers(.99f);
	TestEqual(TEXT("First retry waits one second"), Requests.Num(), 1);
	TickTimers(.02f);
	TestEqual(TEXT("First retry sends once"), Requests.Num(), 2);
	TickTimers(4.01f);
	TickTimers(1.99f);
	TestEqual(TEXT("Second retry waits two seconds"), Requests.Num(), 2);
	TickTimers(.02f);
	TestEqual(TEXT("Third attempt is the last"), Requests.Num(), 3);
	TestEqual(TEXT("Retry preserves serialized checkpoint bytes"), Requests.Last()->Body, Requests[0]->Body);
	TickTimers(4.01f);
	TestFalse(TEXT("Repeated timeout fails the local run"), Session->IsHealthy());
	TestFalse(TEXT("A failed anchor cannot start the timed run"), Session->IsRecordingReady());
	TestEqual(TEXT("Failure completion fires once"), Completions, 1);
	TickTimers(30.f);
	TestEqual(TEXT("No fourth attempt"), Requests.Num(), 3);
	Session->Cancel();

	Completions = 0;
	Session = NewSession();
	Session->SetCompletion([&Completions](bool, const FString&, int32) { ++Completions; });
	Session->BeginRecording();
	FHttpRequestCompleteDelegate LateReply = Requests[0]->Complete;
	Session->Cancel();
	FHttpResponsePtr Reply = MakeShareable(new FResponse(200, Ack0));
	LateReply.ExecuteIfBound(Requests[0], Reply, true);
	TickTimers(30.f);
	TestTrue(TEXT("Cancel stops in-flight transport"), Requests[0]->bCancelled);
	TestEqual(TEXT("Cancel suppresses late completions"), Completions, 0);
	TestEqual(TEXT("Cancel clears timeout and retry work"), Requests.Num(), 1);

	Session = NewSession();
	Session->BeginRecording();
	Requests[0]->Reply(500, TEXT("{}"));
	Session->Cancel();
	TickTimers(30.f);
	TestEqual(TEXT("Cancel during backoff prevents another post"), Requests.Num(), 1);

	Session = NewSession();
	Session->BeginRecording();
	Requests[0]->Reply(401, TEXT("{}"));
	TickTimers(30.f);
	TestEqual(TEXT("Rejected credentials are never retried"), Requests.Num(), 1);
	TestFalse(TEXT("Rejected credentials fail the session"), Session->IsHealthy());
	Session->Cancel();

	Session = NewSession();
	Session->BeginRecording();
	Requests[0]->Reply(200, Ack0.Replace(TEXT("local_checkpoints"), TEXT("approved_servers")));
	TestFalse(TEXT("Another leaderboard scope cannot acknowledge a local run"), Session->IsHealthy());
	TestFalse(TEXT("Wrong-scope acknowledgement does not establish the clock"), Session->IsRecordingReady());
	Session->Cancel();

	Session = NewSession();
	Session->BeginRecording();
	Requests[0]->Reply(200, Ack0);
	TestTrue(TEXT("Matching clock checkpoint is accepted"), Session->IsHealthy());
	TestTrue(TEXT("Only an accepted clock checkpoint permits the timed run"), Session->IsRecordingReady());
	Session->NextSequence = 12;
	Session->LastElapsedUs = 55000000;
	int32 ReturnedScore = -1;
	Completions = 0;
	Session->SetCompletion([&](bool bSuccess, const FString&, int32 Score)
	{
		TestTrue(TEXT("Valid final acknowledgement succeeds"), bSuccess);
		ReturnedScore = Score;
		++Completions;
	});
	Session->QueueCheckpoint(60000000, TArray<FNCAimTrainerLocalEvent>(), true);
	const FString FinalAck = FString::Printf(TEXT("{\"accepted\":true,\"run_id\":\"%s\",\"sequence\":12,\"final\":true,\"score\":123,\"scope\":\"local_checkpoints\",\"movement\":false}"), *Id);
	Requests.Last()->Reply(200, FinalAck);
	TestEqual(TEXT("Final score comes from the service acknowledgement"), ReturnedScore, 123);
	TestEqual(TEXT("Success completion fires once"), Completions, 1);
	TestTrue(TEXT("Run bearer is discarded after completion"), Session->RunToken.IsEmpty());
	Session->Cancel();
	Session.Reset();
	World->DestroyWorld(false);

	// Exercise the real secure factory's preflight without any network calls,
	// including the public IHttpRequest surface's rejection of redirected URLs.
#if PLATFORM_WINDOWS
	const TCHAR* RejectedUrls[] = {
		TEXT("http://ut4stats.com/aimtrainer_local/start/"),
		TEXT("https://example.com/aimtrainer_local/start/"),
		TEXT("https://ut4stats.com/aimtrainer_entry/"),
		TEXT("https://ut4stats.com/aimtrainer_local/start/?redirect=example.com"),
		TEXT("https://ut4stats.com@example.com/aimtrainer_local/start/")
	};
	for (const TCHAR* RejectedUrl : RejectedUrls)
	{
		TSharedPtr<IHttpRequest> Request = CreateNCAimTrainerLocalRequest();
		Request->SetURL(RejectedUrl);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Authorization"), TEXT("Bearer fixture-token"));
		Request->SetContentAsString(TEXT("{}"));
		TestFalse(TEXT("Secure transport rejects every non-allowlisted endpoint before I/O"), Request->ProcessRequest());
	}
	TSharedPtr<IHttpRequest> InjectedHeader = CreateNCAimTrainerLocalRequest();
	InjectedHeader->SetURL(TEXT("https://ut4stats.com/aimtrainer_local/start/"));
	InjectedHeader->SetVerb(TEXT("POST"));
	InjectedHeader->SetHeader(TEXT("Authorization"), TEXT("Bearer fixture\r\nHost: example.com"));
	InjectedHeader->SetContentAsString(TEXT("{}"));
	TestFalse(TEXT("Header injection is rejected before I/O"), InjectedHeader->ProcessRequest());
#else
	TestFalse(TEXT("Unsupported platforms never fall back to unverified engine HTTP"), CreateNCAimTrainerLocalRequest().IsValid());
#endif
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
