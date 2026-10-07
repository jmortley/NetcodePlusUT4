#pragma once

#include "CoreMinimal.h"
#include "TimerManager.h"

class UWorld;
class IHttpRequest;

/** Local observations only; the service computes the score from accepted events. */
struct NETCODEPLUS_API FNCAimTrainerLocalEvent
{
	enum EType { Shot, Hit, Expire, Sample };
	EType Type = Shot;
	int32 TimeUs = 0;
	int32 Target = 0;
	int32 Appearance = 0;
	bool bHead = false;
	bool bFiring = false;
	bool bContact = false;
};

/** One standalone, account-authenticated run. All methods run on the game thread.
 *  Owners must Cancel before abandoning/restarting a run. No hub key is used. */
class NETCODEPLUS_API FNCAimTrainerLocalSession : public TSharedFromThis<FNCAimTrainerLocalSession>
{
public:
	// The pending handle allows cancellation before authentication completes.
	static TSharedPtr<FNCAimTrainerLocalSession> Start(UWorld* World, int32 Scenario, const FString& RequestId,
		TFunction<void(TSharedPtr<FNCAimTrainerLocalSession>, const FString&)> Completion, bool bMovementPractice = false);
	~FNCAimTrainerLocalSession();
	void BeginRecording();
	bool IsRecordingReady() const { return bAnchorAcknowledged && IsHealthy(); }
	bool QueueCheckpoint(int32 ElapsedMicroseconds, const TArray<FNCAimTrainerLocalEvent>& Events, bool bFinal);
	bool IsHealthy() const;
	FString GetFailureReason() const { return FailureReason; }
	const FString& GetPlayerId() const { return PlayerId; }
	const FString& GetDisplayName() const { return DisplayName; }
	void Cancel();
	void SetCompletion(TFunction<void(bool, const FString&, int32)> Callback);

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FNCAimTrainerLocalSessionTest;
	TFunction<TSharedRef<IHttpRequest>()> TestRequestFactory;
#endif
	struct FCheckpoint
	{
		FString Body;
		int32 Sequence = 0;
		bool bFinal = false;
		double QueuedAt = 0;
	};

	FNCAimTrainerLocalSession() {}
	void BuildStartBody(int32 Scenario);
	void SendStartAttempt();
	void SendCheckpointAttempt();
	void SendRequest(const FString& Url, const FString& Token, const FString& Body);
	void HandleResponse(int32 Serial, int32 Code, const FString& Body);
	void StopRequest();
	void ScheduleRetry();
	void Fail(const FString& Reason);
	void FinishStart(bool bSuccess, const FString& Message);
	void DeliverCompletion();

	TWeakObjectPtr<UWorld> World;
	TWeakPtr<IHttpRequest> Request;
	FTimerHandle Timeout;
	FTimerHandle Retry;
	FDelegateHandle WorldCleanupHandle;
	TArray<FCheckpoint> Pending;
	FString StartBody;
	FString RunId;
	FString RunToken;
	FString PlayerId;
	FString DisplayName;
	FString FailureReason;
	TFunction<void(TSharedPtr<FNCAimTrainerLocalSession>, const FString&)> StartCompletion;
	TFunction<void(bool, const FString&, int32)> Completion;
	int32 ControllerId = INDEX_NONE;
	int32 Attempt = 1;
	int32 RequestSerial = 0;
	int32 NextSequence = 0;
	int32 LastElapsedUs = 0;
	int32 LastEventUs = 0;
	int32 EventCount = 0;
	int32 FinalScore = 0;
	double ExpiresAt = 0;
	bool bMovementPractice = false;
	bool bStarting = true;
	bool bHealthy = true;
	bool bCancelled = false;
	bool bRecording = false;
	bool bAnchorAcknowledged = false;
	bool bBusy = false;
	bool bFinalQueued = false;
	bool bResultReady = false;
	bool bResultSuccess = false;
	bool bCompletionDelivered = false;
};
