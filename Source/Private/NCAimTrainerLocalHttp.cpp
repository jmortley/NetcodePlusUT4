#include "NCAimTrainerLocalHttp.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpRequest.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpResponse.h"

#if PLATFORM_WINDOWS

#include "Async/Async.h"
#include "Async/TaskGraphInterfaces.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeCounter.h"
#include "Misc/ScopeLock.h"
#pragma push_macro("E_FAIL")
#pragma push_macro("E_NOTIMPL")
#pragma push_macro("ERROR_SUCCESS")
#pragma push_macro("ERROR_IO_PENDING")
#undef E_FAIL
#undef E_NOTIMPL
#undef ERROR_SUCCESS
#undef ERROR_IO_PENDING
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winhttp.h>
#include "Windows/HideWindowsPlatformTypes.h"
#pragma pop_macro("ERROR_IO_PENDING")
#pragma pop_macro("ERROR_SUCCESS")
#pragma pop_macro("E_NOTIMPL")
#pragma pop_macro("E_FAIL")

namespace NCAimTrainerLocalHttp
{
	constexpr int32 ResponseLimit = 8192;
	class FRequest;
	TArray<TWeakPtr<IHttpRequest>> ActiveRequests;
	FThreadSafeCounter NativeContexts;
	FThreadSafeCounter QueuedCallbacks;
	FThreadSafeCounter ShutdownRequested;
	FCriticalSection DispatchLock;
	bool bShuttingDown = false;

	class FResponse : public IHttpResponse
	{
	public:
		FString Url;
		int32 Code = 0;
		TArray<uint8> Bytes;
		virtual FString GetURL() override { return Url; }
		virtual FString GetURLParameter(const FString&) override { return FString(); }
		virtual FString GetHeader(const FString&) override { return FString(); }
		virtual TArray<FString> GetAllHeaders() override { return TArray<FString>(); }
		virtual FString GetContentType() override { return TEXT("application/json"); }
		virtual int32 GetContentLength() override { return Bytes.Num(); }
		virtual const TArray<uint8>& GetContent() override { return Bytes; }
		virtual int32 GetResponseCode() override { return Code; }
		virtual FString GetContentAsString() override
		{
			if (Bytes.Num() == 0) return FString();
			FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
			return FString(Text.Length(), Text.Get());
		}
	};

	/** Native buffers remain alive until HANDLE_CLOSING, including after cancel.
	 *  Owner and handles are accessed only on the game thread. Native callbacks
	 *  retain this thread-safe state and dispatch copied data to that thread. */
	struct FState : TSharedFromThis<FState, ESPMode::ThreadSafe>
	{
		FRequest* Owner = nullptr;
		HINTERNET Session = nullptr;
		HINTERNET Connection = nullptr;
		HINTERNET Request = nullptr;
		TArray<uint8> Upload;
		uint8 ReadBuffer[ResponseLimit];

		void Close()
		{
			check(IsInGameThread());
			Owner = nullptr;
			HINTERNET ClosingRequest = Request;
			Request = nullptr;
			if (ClosingRequest) WinHttpCloseHandle(ClosingRequest);
			if (Connection) { WinHttpCloseHandle(Connection); Connection = nullptr; }
			if (Session) { WinHttpCloseHandle(Session); Session = nullptr; }
		}
	};

	struct FCallbackContext
	{
		TSharedRef<FState, ESPMode::ThreadSafe> State;
		explicit FCallbackContext(TSharedRef<FState, ESPMode::ThreadSafe> InState) : State(InState) { NativeContexts.Increment(); }
		~FCallbackContext() { NativeContexts.Decrement(); }
	};

	void CALLBACK NativeCallback(HINTERNET, DWORD_PTR ContextValue, DWORD Status, LPVOID Information, DWORD Length);

	/** No engine HTTP manager/configuration is used: this engine's Curl transport
	 *  can disable peer verification when its certificate bundle is unavailable. */
	class FRequest : public IHttpRequest
	{
	public:
		FString Url, Verb;
		TMap<FString, FString> Headers;
		TArray<uint8> Content;
		TArray<uint8> ResponseBytes;
		FHttpRequestCompleteDelegate Complete;
		FHttpRequestProgressDelegate Progress;
		FHttpResponsePtr Response;
		TSharedPtr<FState, ESPMode::ThreadSafe> Native;
		TSharedPtr<IHttpRequest> KeepAlive;
		EHttpRequestStatus::Type Status = EHttpRequestStatus::NotStarted;
		double StartedAt = 0;
		int32 ResponseCode = 0;
		bool bFinished = false;

		virtual ~FRequest() { CancelRequest(); }
		virtual FString GetURL() override { return Url; }
		virtual FString GetURLParameter(const FString&) override { return FString(); }
		virtual FString GetHeader(const FString& Name) override { return Headers.FindRef(Name); }
		virtual TArray<FString> GetAllHeaders() override { return TArray<FString>(); }
		virtual FString GetContentType() override { return GetHeader(TEXT("Content-Type")); }
		virtual int32 GetContentLength() override { return Content.Num(); }
		virtual const TArray<uint8>& GetContent() override { return Content; }
		virtual FString GetVerb() override { return Verb; }
		virtual void SetURL(const FString& Value) override { Url = Value; }
		virtual void SetVerb(const FString& Value) override { Verb = Value; }
		virtual void SetContent(const TArray<uint8>& Value) override { Content = Value; }
		virtual void SetContentAsString(const FString& Value) override
		{
			FTCHARToUTF8 Utf8(*Value);
			Content.Empty();
			Content.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		}
		virtual void SetHeader(const FString& Name, const FString& Value) override { Headers.Add(Name, Value); }
		virtual void AppendToHeader(const FString& Name, const FString& Value) override { SetHeader(Name, Value); }
		virtual FHttpRequestCompleteDelegate& OnProcessRequestComplete() override { return Complete; }
		virtual FHttpRequestProgressDelegate& OnRequestProgress() override { return Progress; }
		virtual EHttpRequestStatus::Type GetStatus() override { return Status; }
		virtual const FHttpResponsePtr GetResponse() const override { return Response; }
		virtual void Tick(float) override {}
		virtual float GetElapsedTime() override { return StartedAt == 0 ? 0.f : float(FPlatformTime::Seconds() - StartedAt); }

		virtual bool ProcessRequest() override
		{
			check(IsInGameThread());
			const bool bStart = Url == TEXT("https://ut4stats.com/aimtrainer_local/start/");
			if (bShuttingDown || Status != EHttpRequestStatus::NotStarted || Verb != TEXT("POST") || Content.Num() > 65536
				|| (!bStart && Url != TEXT("https://ut4stats.com/aimtrainer_local/checkpoint/"))) return false;
			const FString Authorization = GetHeader(TEXT("Authorization"));
			if (!Authorization.StartsWith(TEXT("Bearer ")) || Authorization.Len() <= 7 || Authorization.Len() > 8192
				|| Authorization.Contains(TEXT("\r")) || Authorization.Contains(TEXT("\n"))) return false;
			// WinHTTP can deliver HANDLE_CLOSING after CloseHandle returns. Pin the
			// callback's code until process exit so a delayed native callback cannot
			// execute an unloaded plugin, including at editor/module shutdown.
			HMODULE CallbackModule = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
				reinterpret_cast<LPCWSTR>(&NativeCallback), &CallbackModule)) return false;
			KeepAlive = AsShared();
			ActiveRequests.RemoveAll([](const TWeakPtr<IHttpRequest>& Item) { return !Item.IsValid(); });
			ActiveRequests.Add(KeepAlive);
			StartedAt = FPlatformTime::Seconds();
			Status = EHttpRequestStatus::Processing;
			Native = MakeShareable(new FState);
			Native->Owner = this;
			Native->Upload = Content;
			Native->Session = WinHttpOpen(L"NetcodePlus-AimTrainer/9", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
				WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
			DWORD Protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
			if (!Native->Session || !WinHttpSetTimeouts(Native->Session, 4000, 4000, 4000, 4000)
				|| !WinHttpSetOption(Native->Session, WINHTTP_OPTION_SECURE_PROTOCOLS, &Protocols, sizeof(Protocols)))
				return SetupFailed();
			Native->Connection = WinHttpConnect(Native->Session, L"ut4stats.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
			if (!Native->Connection) return SetupFailed();
			Native->Request = WinHttpOpenRequest(Native->Connection, L"POST",
				bStart ? L"/aimtrainer_local/start/" : L"/aimtrainer_local/checkpoint/",
				nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
			if (!Native->Request) return SetupFailed();
			DWORD Disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
			DWORD AutoLogon = WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH;
			// Keep WinHTTP's default chain, hostname, date and CA validation. Never
			// set SECURITY_FLAG_IGNORE_* or reuse engine/global verification flags.
			if (!WinHttpSetOption(Native->Request, WINHTTP_OPTION_DISABLE_FEATURE, &Disabled, sizeof(Disabled))
				|| !WinHttpSetOption(Native->Request, WINHTTP_OPTION_AUTOLOGON_POLICY, &AutoLogon, sizeof(AutoLogon)))
				return SetupFailed();
			FCallbackContext* Context = new FCallbackContext(Native.ToSharedRef());
			DWORD_PTR ContextValue = reinterpret_cast<DWORD_PTR>(Context);
			if (!WinHttpSetOption(Native->Request, WINHTTP_OPTION_CONTEXT_VALUE, &ContextValue, sizeof(ContextValue)))
			{
				delete Context;
				return SetupFailed();
			}
			const DWORD Notifications = WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES
				| WINHTTP_CALLBACK_FLAG_SECURE_FAILURE;
			if (WinHttpSetStatusCallback(Native->Request, NativeCallback, Notifications, 0) == WINHTTP_INVALID_STATUS_CALLBACK)
			{
				delete Context;
				return SetupFailed();
			}
			// HANDLE_CLOSING now exclusively owns Context destruction. Upload is
			// retained there until native I/O has stopped, even on immediate cancel.
			const FString HeaderBlock = TEXT("Accept: application/json\r\nContent-Type: application/json\r\nAuthorization: ")
				+ Authorization + TEXT("\r\n");
			if (!WinHttpSendRequest(Native->Request, *HeaderBlock, DWORD(HeaderBlock.Len()), Native->Upload.GetData(),
				DWORD(Native->Upload.Num()), DWORD(Native->Upload.Num()), ContextValue)) return SetupFailed();
			return true;
		}

		bool SetupFailed()
		{
			CancelRequest();
			return false;
		}

		virtual void CancelRequest() override
		{
			check(IsInGameThread());
			// Keep this alive if clearing the self-reference releases its last owner.
			TSharedPtr<IHttpRequest> ThisRequest = KeepAlive;
			if (!bFinished) Status = EHttpRequestStatus::Failed;
			bFinished = true;
			if (Native.IsValid()) { Native->Close(); Native.Reset(); }
			Headers.Empty();
			KeepAlive.Reset();
		}

		void Finish(bool bConnected)
		{
			if (bFinished) return;
			TSharedPtr<IHttpRequest> ThisRequest = KeepAlive;
			bFinished = true;
			Status = bConnected ? EHttpRequestStatus::Succeeded : EHttpRequestStatus::Failed;
			if (Native.IsValid()) { Native->Close(); Native.Reset(); }
			Headers.Empty();
			if (bConnected)
			{
				TSharedPtr<FResponse, ESPMode::ThreadSafe> Result = MakeShareable(new FResponse);
				Result->Url = Url;
				Result->Code = ResponseCode;
				Result->Bytes = MoveTemp(ResponseBytes);
				Response = Result;
			}
			FHttpRequestCompleteDelegate Callback = Complete;
			Callback.ExecuteIfBound(ThisRequest, Response, bConnected);
			KeepAlive.Reset();
		}

		void HandleStatus(DWORD Notification, const TArray<uint8>& Data)
		{
			check(IsInGameThread());
			if (bFinished || !Native.IsValid() || !Native->Request) return;
			switch (Notification)
			{
			case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
				if (!WinHttpReceiveResponse(Native->Request, nullptr)) Finish(false);
				break;
			case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
			{
				DWORD Code = 0, Size = sizeof(Code);
				if (!WinHttpQueryHeaders(Native->Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &Code, &Size, WINHTTP_NO_HEADER_INDEX)) { Finish(false); return; }
				ResponseCode = int32(Code);
				if (ResponseCode != 200 && ResponseCode != 201) { Finish(true); return; }
				DWORD ContentLength = 0;
				Size = sizeof(ContentLength);
				if (WinHttpQueryHeaders(Native->Request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
					WINHTTP_HEADER_NAME_BY_INDEX, &ContentLength, &Size, WINHTTP_NO_HEADER_INDEX) && ContentLength > ResponseLimit)
				{
					ResponseCode = 413;
					Finish(true);
					return;
				}
				ReadNext();
				break;
			}
			case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
				if (Data.Num() == 0) { Finish(true); return; }
				if (ResponseBytes.Num() + Data.Num() > ResponseLimit)
				{
					ResponseCode = 413;
					ResponseBytes.Empty();
					Finish(true);
					return;
				}
				ResponseBytes.Append(Data);
				ReadNext();
				break;
			case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
			case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
				Finish(false);
				break;
			}
		}

		void ReadNext()
		{
			if (!WinHttpReadData(Native->Request, Native->ReadBuffer, sizeof(Native->ReadBuffer), nullptr)) Finish(false);
		}
	};

	void CALLBACK NativeCallback(HINTERNET, DWORD_PTR ContextValue, DWORD Status, LPVOID Information, DWORD Length)
	{
		FCallbackContext* Context = reinterpret_cast<FCallbackContext*>(ContextValue);
		if (!Context) return;
		if (Status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { delete Context; return; }
		// Shutdown may outlive the game-thread task graph. After cancellation,
		// only native context release remains; never enqueue more engine work.
		// Admission and enqueue share the shutdown lock, so no callback can pass
		// the flag check and enqueue after shutdown has drained the queue.
		FScopeLock DispatchGuard(&DispatchLock);
		if (ShutdownRequested.GetValue() != 0) return;
		TSharedRef<FState, ESPMode::ThreadSafe> State = Context->State;
		TArray<uint8> Data;
		if (Status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE && Length > 0)
		{
			if (Length > ResponseLimit || !Information) Status = WINHTTP_CALLBACK_STATUS_REQUEST_ERROR;
			else Data.Append(static_cast<const uint8*>(Information), int32(Length));
		}
		QueuedCallbacks.Increment();
		AsyncTask(ENamedThreads::GameThread, [State, Status, Data]()
		{
			if (State->Owner) State->Owner->HandleStatus(Status, Data);
			QueuedCallbacks.Decrement();
		});
	}
}

#endif // PLATFORM_WINDOWS

TSharedPtr<IHttpRequest> CreateNCAimTrainerLocalRequest()
{
#if PLATFORM_WINDOWS
	return MakeShareable(new NCAimTrainerLocalHttp::FRequest);
#else
	return nullptr;
#endif
}

bool ShutdownNCAimTrainerLocalHttp()
{
#if PLATFORM_WINDOWS
	using namespace NCAimTrainerLocalHttp;
	check(IsInGameThread());
	bShuttingDown = true;
	{
		FScopeLock DispatchGuard(&DispatchLock);
		ShutdownRequested.Increment();
	}
	for (const TWeakPtr<IHttpRequest>& Item : ActiveRequests)
	{
		TSharedPtr<IHttpRequest> Request = Item.Pin();
		if (Request.IsValid())
		{
			Request->OnProcessRequestComplete().Unbind();
			Request->OnRequestProgress().Unbind();
			Request->CancelRequest();
		}
	}
	ActiveRequests.Empty();
	const double Deadline = FPlatformTime::Seconds() + 5.0;
	while ((NativeContexts.GetValue() > 0 || QueuedCallbacks.GetValue() > 0) && FPlatformTime::Seconds() < Deadline)
	{
		if (FTaskGraphInterface::IsRunning()) FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FPlatformProcess::SleepNoStats(.001f);
	}
	return NativeContexts.GetValue() == 0 && QueuedCallbacks.GetValue() == 0;
#else
	return true;
#endif
}
