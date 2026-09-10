#pragma once

#include "NetcodePlus.h"
#include "UTTeamGameMode.h"
#include "UTTeamDMGameMode.h"
#include "NCPlusXTDMGameMode.generated.h"

class ANCPlusXTDMReplicator;
class ANCReadyUpState;
class ANCAccuracyStatsReplicator;

/** Four-team instagib. Stock UT GameState/PlayerState/PlayerController networking is retained. */
UCLASS(Config = Game)
class NETCODEPLUS_API ANCPlusXTDMGameMode : public AUTTeamDMGameMode
{
	GENERATED_BODY()
public:
	ANCPlusXTDMGameMode(const FObjectInitializer& ObjectInitializer);

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category = "XTDM", meta = (ClampMin = "2", ClampMax = "4"))
	int32 TeamSize;
	/** Explicit server testing override; normal matches require every configured seat and every human ready. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category = "XTDM")
	bool bAllowIncompleteTeams;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Config, Category = "XTDM")
	float XTDMSpawnProtectionTime;
	/** Optional class settings. Empty settings are loaded only when this game mode is launched. */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "XTDM|Assets")
	TSubclassOf<APawn> InstagibCharacterClass;
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "XTDM|Assets")
	TSubclassOf<AUTInventory> InstagibRifleClass;
	UPROPERTY(Transient, BlueprintReadOnly, Category = "XTDM")
	ANCPlusXTDMReplicator* XTDMState;

	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void InitGameState() override;
	virtual void BeginPlay() override;
	virtual APlayerController* Login(UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void GenericPlayerInitialization(AController* Player) override;
	virtual bool FindInactivePlayer(APlayerController* Player) override;
	virtual void Logout(AController* Exiting) override;
	virtual bool ChangeTeam(AController* Player, uint8 NewTeam = 255, bool bBroadcast = true) override;
	virtual bool ShouldBalanceTeams(bool bInitialTeam) const override { return false; }
	virtual uint8 PickBalancedTeam(AUTPlayerState* PS, uint8 RequestedTeam) override;
	virtual bool ReadyToStartMatch_Implementation() override;
	virtual void HandleMatchHasStarted() override;
	virtual void HandlePlayerIntro() override;
	virtual void CheckCountDown() override;
	virtual void HandleMatchHasEnded() override;
	virtual void RestartPlayer(AController* NewPlayer) override;
	virtual void Killed(AController* Killer, AController* KilledPlayer, APawn* KilledPawn, TSubclassOf<UDamageType> DamageType) override;
	virtual void GiveDefaultInventory(APawn* PlayerPawn) override;
	virtual void DiscardInventory(APawn* Other, AController* Killer = nullptr) override;
	virtual bool CheckRelevance_Implementation(AActor* Other) override;
	virtual bool OverridePickupQuery_Implementation(APawn* Other, TSubclassOf<AUTInventory> ItemClass, AActor* Pickup, bool& bAllowPickup) override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual AActor* FindPlayerStart_Implementation(AController* Player, const FString& IncomingName = TEXT("")) override;
	virtual APawn* SpawnDefaultPawnFor_Implementation(AController* Player, AActor* StartSpot) override;
	virtual bool CheckScore_Implementation(AUTPlayerState* Scorer) override;
	virtual AUTPlayerState* IsThereAWinner_Implementation(bool& bTied) override;
	virtual void CheckGameTime() override;
	virtual void EndGame(AUTPlayerState* Winner, FName Reason) override;
	virtual void SetEndGameFocus(AUTPlayerState* Winner) override;
	virtual void PlayEndOfMatchMessage() override;
	virtual bool SupportsInstantReplay() const override { return false; }
	virtual bool UTIsHandlingReplays() override { return false; }
	virtual void UpdateSkillRating() override {}
	virtual void SetEloFor(AUTPlayerState* PS, bool bRanked, int32 NewEloValue, bool bIncrementMatchCount) override {}
	virtual void SendEndOfGameStats(FName Reason) override;

private:
	struct FTeamReservation
	{
		uint8 Team = 255;
		TWeakObjectPtr<AUTPlayerState> Player;
	};
	TMap<FString, uint8> DraftTeams;
	TMap<FString, FTeamReservation> Reservations;
	TArray<TWeakObjectPtr<APlayerStart>> SpawnStarts;
	TMap<TWeakObjectPtr<APlayerStart>, float> LastStartUse;
	TSet<TWeakObjectPtr<APawn>> GrantedInventory;
	TWeakObjectPtr<ANCReadyUpState> ReadyState;
	TWeakObjectPtr<ANCAccuracyStatsReplicator> AccuracyState;
	uint8 FinalWinningTeam;
	bool bRosterLocked;
	bool bConfigurationValid;
	bool bStartsCached;

	static FString PlayerIdentity(const AUTPlayerState* PS);
	bool ParseDraft(const FString& Options, FString& ErrorMessage);
	int32 OccupiedOrReservedSeats(uint8 Team, const AUTPlayerState* Requester) const;
	bool IsRosterComplete() const;
	bool IsTeamLocked() const;
	void EnsureStatusActors();
	void RefreshPlayerStatus(AController* Player);
	void CachePlayerStarts();
	AUTTeamInfo* FindLeadingTeam(bool& bTied) const;
	AUTPlayerState* FindTeamRepresentative(const AUTTeamInfo* Team) const;
	void RestoreFinalResult();
};
