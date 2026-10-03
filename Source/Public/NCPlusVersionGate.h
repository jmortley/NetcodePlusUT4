// Owner-only protocol handshake. Only an exact version report authorizes 329
// match RPCs; pawn movement and spectator status are not protocol evidence.
// The report RPC/net-field layout deliberately stays unchanged so older builds
// can report their version and receive a useful, non-banning disconnect reason.
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

	/** Actor's work is finished; actual protocol permission lives in the
	 *  connection/world-bound registry, never in this flag alone. */
	UPROPERTY(Transient)
	bool bConfirmed;

	int32 AdvisorNagCount;
	float TimeoutSec;
	FTimerHandle TimeoutHandle;
	FTimerHandle KickHandle;
};

namespace NCPlusVersionGate
{
	/** Server-only gate. Local authority PCs and bots do not handshake. */
	NETCODEPLUS_API void SpawnFor(class APlayerController* PC);
	NETCODEPLUS_API void SpawnAdvisorFor(class APlayerController* PC);

	/** Fail closed for unknown, pending, mismatched, or stale remote sessions.
	 *  Unknown match sessions start a fresh handshake (including seamless travel).
	 *  Confirmation requires the same weak controller, world and connection.
	 *  This is a compatibility gate, not authentication against a modified client. */
	NETCODEPLUS_API bool IsProtocolConfirmed(class APlayerController* PC);

	NETCODEPLUS_API void RegisterHubAdvisor();
	NETCODEPLUS_API void UnregisterHubAdvisor();
}
