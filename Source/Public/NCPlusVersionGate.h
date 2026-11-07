// Retired version gate. Keep the legacy actor and report RPC declarations
// unchanged for existing 329 clients. Version reports no longer authorize
// gameplay or disconnect players; new gate actors are not spawned.
#pragma once

#include "NetcodePlus.h"
#include "GameFramework/Info.h"
#include "NCPlusVersionGate.generated.h"

UCLASS()
class NETCODEPLUS_API ANCVersionGate : public AInfo
{
	GENERATED_UCLASS_BODY()

public:
	virtual void BeginPlay() override;
	virtual void PostNetInit() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Keep this wire signature unchanged across the 328 -> 329 boundary. */
	UFUNCTION(Reliable, Server, WithValidation)
	void ServerReportVersion(int32 ClientVersion);

	/** Hub-only advice, never authorization to send match protocol RPCs.
	 *  Plain server-side member; no new replicated properties/net fields. */
	bool bAdvisorMode;

protected:
	void OnTimeout();
	void OnKickDeadline();
	void KickOwner(const FString& Reason);
	void OnAdvisorCheck();
	void WhisperOwner(const FString& Msg);

	/** Legacy actor completion flag, retained without changing class layout.
	 *  Neither this flag nor the old registry authorizes gameplay. */
	UPROPERTY(Transient)
	bool bConfirmed;

	int32 AdvisorNagCount;
	float TimeoutSec;
	FTimerHandle TimeoutHandle;
	FTimerHandle KickHandle;
};

namespace NCPlusVersionGate
{
	/** Retained no-op entry points; version enforcement is retired. */
	NETCODEPLUS_API void SpawnFor(class APlayerController* PC);
	NETCODEPLUS_API void SpawnAdvisorFor(class APlayerController* PC);

	/** Legacy name: validates the current authority controller and live remote
	 *  connection, with existing local/bot exemptions. No version report, cached
	 *  confirmation or new gate actor is required. RPC ownership and gameplay
	 *  transaction validation remain separate checks. */
	NETCODEPLUS_API bool IsProtocolConfirmed(class APlayerController* PC);

	NETCODEPLUS_API void RegisterHubAdvisor();
	NETCODEPLUS_API void UnregisterHubAdvisor();
}
