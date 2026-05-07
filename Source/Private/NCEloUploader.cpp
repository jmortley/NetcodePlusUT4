// NCEloUploader.cpp — HTTP POST machinery for the global-ELO push.
// Mirrors AMutStatSQL::SendPost (Plugins/StatSQL/Source/Private/MutStatSQL.cpp:1122)
// with the AMutStatSQL UObject-instance dependency removed: this is a free-function
// helper that the rating systems call from match-end paths.

#include "NCEloUploader.h"
#include "UnrealTournament.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"

#include "Runtime/Online/HTTP/Public/HttpModule.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpRequest.h"
#include "Runtime/Online/HTTP/Public/Interfaces/IHttpResponse.h"

DEFINE_LOG_CATEGORY_STATIC(LogNCEloUpload, Log, All);

namespace
{
	// Tunables. Same shape as MutStatSQL.cpp.
	constexpr int32 NCElo_MaxRetries = 3;
	const TCHAR* NCElo_Endpoint = TEXT("/elo_entry/");

	// Process-static config. Loaded lazily on first PostMatchResult call.
	struct FNCEloConfig
	{
		FString ApiBaseUrl = TEXT("https://ut4stats.com");
		FString ApiAuthKey;
		bool bEnabled = true;
		bool bLoaded = false;
	};

	FNCEloConfig& GetConfig()
	{
		static FNCEloConfig Config;
		return Config;
	}

	// Read Mod.ini [UTPUGS_STATS] (shared with StatSQL) plus URL option overrides.
	// Idempotent: only does I/O on the first call.
	void EnsureConfigLoaded(UWorld* World)
	{
		FNCEloConfig& Config = GetConfig();
		if (Config.bLoaded) return;
		Config.bLoaded = true;

		// --- Mod.ini ---
		const FString ModIniPath = FPaths::GameSavedDir() / TEXT("Config") / TEXT("Mod.ini");
		if (FPaths::FileExists(ModIniPath))
		{
			FConfigFile ModIni;
			ModIni.Read(ModIniPath);
			if (const FConfigSection* Section = ModIni.Find(TEXT("UTPUGS_STATS")))
			{
				if (const FConfigValue* KeyVal = Section->Find(FName(TEXT("Key"))))
				{
					if (!KeyVal->GetValue().IsEmpty())
					{
						Config.ApiAuthKey = KeyVal->GetValue();
					}
				}
				if (const FConfigValue* SendVal = Section->Find(FName(TEXT("SendStats"))))
				{
					Config.bEnabled = SendVal->GetValue().ToBool();
				}
			}
			else
			{
				UE_LOG(LogNCEloUpload, Warning, TEXT("Mod.ini missing [UTPUGS_STATS] section"));
			}
		}
		else
		{
			UE_LOG(LogNCEloUpload, Warning, TEXT("Mod.ini not found at: %s"), *ModIniPath);
		}

		// --- URL option overrides ---
		// World->URL.Op holds parsed "Key=Value" entries from the server's startup
		// URL (e.g. ?NCEloEnabled=false). Walk it once for our three keys.
		if (World)
		{
			for (const FString& Opt : World->URL.Op)
			{
				FString K, V;
				if (Opt.Split(TEXT("="), &K, &V))
				{
					if (K.Equals(TEXT("NCEloUrl"), ESearchCase::IgnoreCase) && !V.IsEmpty())
					{
						Config.ApiBaseUrl = V;
					}
					else if (K.Equals(TEXT("NCEloKey"), ESearchCase::IgnoreCase) && !V.IsEmpty())
					{
						Config.ApiAuthKey = V;
					}
					else if (K.Equals(TEXT("NCEloEnabled"), ESearchCase::IgnoreCase))
					{
						Config.bEnabled = V.ToBool();
					}
				}
			}
		}

		UE_LOG(LogNCEloUpload, Log,
			TEXT("Config loaded: URL=%s Enabled=%s Key=%s"),
			*Config.ApiBaseUrl,
			Config.bEnabled ? TEXT("true") : TEXT("false"),
			Config.ApiAuthKey.IsEmpty() ? TEXT("NOT SET") : TEXT("SET"));
	}

	// Forward decl so the lambda inside DoSend can recurse via timer.
	void DoSend(TWeakObjectPtr<UWorld> WeakWorld, const FString& Endpoint,
	            const FString& JsonBody, int32 RetryCount);

	void DoSend(TWeakObjectPtr<UWorld> WeakWorld, const FString& Endpoint,
	            const FString& JsonBody, int32 RetryCount)
	{
		UWorld* World = WeakWorld.Get();
		if (!World) return;

		const FNCEloConfig& Config = GetConfig();
		if (!Config.bEnabled || Config.ApiBaseUrl.IsEmpty())
		{
			UE_LOG(LogNCEloUpload, Warning, TEXT("Skipped — disabled or no URL configured"));
			return;
		}

		TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Config.ApiBaseUrl + Endpoint);
		Request->SetVerb(TEXT("POST"));
		Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		if (!Config.ApiAuthKey.IsEmpty())
		{
			Request->SetHeader(TEXT("Authorization"),
				FString::Printf(TEXT("Token %s"), *Config.ApiAuthKey));
		}
		Request->SetContentAsString(JsonBody);

		// Capture body by shared-ptr so retry timer doesn't dangle. Endpoint and
		// retry count are cheap to copy.
		TSharedRef<FString> CapturedBody = MakeShareable(new FString(JsonBody));
		const FString CapturedEndpoint = Endpoint;

		Request->OnProcessRequestComplete().BindLambda(
			[WeakWorld, CapturedEndpoint, CapturedBody, RetryCount]
			(FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnected)
		{
			UWorld* W = WeakWorld.Get();
			if (!W) return; // World torn down during request

			const bool bSuccess = bConnected && Resp.IsValid() &&
				EHttpResponseCodes::IsOk(Resp->GetResponseCode());

			if (!bSuccess && RetryCount < NCElo_MaxRetries)
			{
				const float Delay = FMath::Pow(2.f, (float)RetryCount); // 1s, 2s, 4s
				UE_LOG(LogNCEloUpload, Warning,
					TEXT("POST %s failed (attempt %d/%d), retrying in %.0fs"),
					*CapturedEndpoint, RetryCount + 1, NCElo_MaxRetries, Delay);

				FTimerHandle Handle;
				W->GetTimerManager().SetTimer(Handle,
					[WeakWorld, CapturedEndpoint, CapturedBody, RetryCount]()
				{
					DoSend(WeakWorld, CapturedEndpoint, *CapturedBody, RetryCount + 1);
				}, Delay, false);
				return;
			}

			if (!bSuccess)
			{
				const int32 Code = Resp.IsValid() ? Resp->GetResponseCode() : 0;
				UE_LOG(LogNCEloUpload, Error,
					TEXT("POST %s FAILED after %d attempts (HTTP %d)"),
					*CapturedEndpoint, NCElo_MaxRetries, Code);
				return;
			}

			const FString ResponseBody = Resp.IsValid() ? Resp->GetContentAsString() : FString();
			UE_LOG(LogNCEloUpload, Log,
				TEXT("POST %s OK (HTTP %d): %s"),
				*CapturedEndpoint, Resp->GetResponseCode(), *ResponseBody);
		});

		Request->ProcessRequest();
	}
} // namespace

void FNCEloUploader::PostMatchResult(UWorld* World, const FString& JsonBody)
{
	if (!World) return;
	if (JsonBody.IsEmpty())
	{
		UE_LOG(LogNCEloUpload, Warning, TEXT("PostMatchResult: empty JsonBody — skipping"));
		return;
	}

	EnsureConfigLoaded(World);
	DoSend(TWeakObjectPtr<UWorld>(World), NCElo_Endpoint, JsonBody, 0);
}
