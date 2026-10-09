#pragma once

#include "NetcodePlus.h"
#include "GameFramework/Info.h"
#include "TimerManager.h"
#include "NCPlusXTDMReplicator.generated.h"

class AUTPlayerState;

USTRUCT(BlueprintType)
struct NETCODEPLUS_API FNCPlusXTDMPlayerStatus
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "XTDM")
	AUTPlayerState* PlayerState;
	UPROPERTY(BlueprintReadOnly, Category = "XTDM")
	bool bAlive;
	/** Same clock as AGameStateBase::GetServerWorldTimeSeconds(). Zero while alive. */
	UPROPERTY(BlueprintReadOnly, Category = "XTDM")
	float RespawnReadyServerTime;

	FNCPlusXTDMPlayerStatus() : PlayerState(nullptr), bAlive(false), RespawnReadyServerTime(0.f) {}
};

/** Independent status actor: never replace stock UT GameState, PlayerState or PlayerController. */
UCLASS()
class NETCODEPLUS_API ANCPlusXTDMReplicator : public AInfo
{
	GENERATED_BODY()
public:
	ANCPlusXTDMReplicator(const FObjectInitializer& ObjectInitializer);
	static const uint8 NoTeam = 255;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "XTDM")
	int32 TeamSize;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "XTDM")
	bool bMatchEnded;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "XTDM")
	uint8 WinningTeamIndex;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "XTDM")
	TArray<FNCPlusXTDMPlayerStatus> Players;

	static ANCPlusXTDMReplicator* Find(UWorld* World);
	const FNCPlusXTDMPlayerStatus* FindPlayer(const AUTPlayerState* PlayerState) const;
	void SetPlayerStatus(AUTPlayerState* PlayerState, bool bAlive, float RespawnReadyTime);
	void RemovePlayer(AUTPlayerState* PlayerState);
	void SetMatchResult(uint8 TeamIndex);
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
	void ApplyInitialTeamAppearance();
	FTimerHandle AppearanceReadyTimer;
};
