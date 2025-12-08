// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#pragma once
#include "NetcodePlus.h"
#include "UnrealTournament.h"
#include "UTWeapon.h"
#include "UTWeaponFix.generated.h"

/**
 * Enhanced weapon base class combining three critical fixes:
 * 
 * 1. Transaction Validation: Fixes high-FPS desync with unique event indices
 * 2. Epic's Lag Compensation: Uses built-in GetRewindLocation() for hit validation
 * 3. Split Prediction: Separates visual (0ms) from hit validation (120ms) time
 * 
 * Key improvements:
 * - Transactional fire events with unique event indexing per fire mode
 * - Server-authoritative validation with client correction feedback loop
 * - Prevention of simultaneous fire mode activation
 * - Strict cooldown enforcement on server side
 * - Automatic desync recovery via ClientConfirmFireEvent RPC
 * - Forgiving hit detection using split prediction's hit validation time
 * 
 * Works with TeamArenaPredictionPC and TeamArenaCharacter for complete hybrid system.
 */
UCLASS(Abstract)
class NETCODEPLUS_API AUTWeaponFix : public AUTWeapon
{
    GENERATED_BODY()
public:
    AUTWeaponFix(const FObjectInitializer& ObjectInitializer);

    virtual void BeginPlay() override;

    //~ Begin AUTWeapon Interface
    virtual void StartFire(uint8 FireModeNum) override;
    virtual void StopFire(uint8 FireModeNum) override;
    virtual void PostInitProperties() override;
    virtual void Tick(float DeltaTime) override;
    virtual void DetachFromOwner_Implementation() override;
    virtual bool PutDown() override;
    virtual void FireInstantHit(bool bDealDamage, FHitResult* OutHit) override;
    virtual void FireShot() override;
    virtual FRotator GetAdjustedAim_Implementation(FVector StartFireLoc) override;
    virtual void HitScanTrace(const FVector& StartLocation, const FVector& EndTrace,
        float TraceRadius, FHitResult& Hit, float PredictionTime) override;
    virtual AUTProjectile* SpawnNetPredictedProjectile(TSubclassOf<AUTProjectile> ProjectileClass, FVector SpawnLocation, FRotator SpawnRotation) override;
    virtual void FireCone() override;
    virtual FVector GetFireStartLoc(uint8 FireMode = 255) override;
    virtual FRotator GetBaseFireRotation() override;
    //~ End AUTWeapon Interface
    
     /**
     * Checks if a fire mode is currently on cooldown.
     *
     * @param FireModeNum - Fire mode to check
     * @param CurrentTime - Current world time
     * @return true if cooldown is still active (cannot fire yet)
     */
    bool IsFireModeOnCooldown(uint8 FireModeNum, float CurrentTime);
    void OnRetryTimer(uint8 FireModeNum);

protected:
    /**
     * Server-side authoritative fire event index for each fire mode.
     * This is the ground truth that clients must sync to.
     * Replicated to clients for verification.
     */
    bool bIsTransactionalFire;
    bool bHandlingRetry;
    FTimerHandle RetryFireHandle[2];
    UPROPERTY(Transient)
    FRotator CachedTransactionalRotation;

    UPROPERTY(Replicated)
    TArray<int32> AuthoritativeFireEventIndex;

    /**
     * Client-side fire event counter for each fire mode.
     * Incremented locally on each fire attempt, then validated by server.
     */
    UPROPERTY()
    TArray<int32> ClientFireEventIndex;

    /**
     * Timestamp of last successful fire for each mode (server time).
     * Used for refire rate validation on server.
     */
    UPROPERTY()
    TArray<float> LastFireTime;

    /**
     * Replicated active state for each fire mode (0 = inactive, 1 = active).
     * Used to prevent simultaneous fire modes and sync state to non-owning clients.
     */
    UPROPERTY(ReplicatedUsing = OnRep_FireModeState)
    TArray<uint8> FireModeActiveState;

    /**
     * Currently active fire mode (255 = none active).
     * Prevents race conditions from rapid mode switching.
     */
    UPROPERTY()
    uint8 CurrentlyFiringMode;

    /**
     * Server RPC to request firing with full validation.
     *
     * @param FireModeNum - Which fire mode to activate
     * @param InFireEventIndex - Unique sequence number for this fire event
     * @param ClientTimestamp - Client's GetWorld()->GetTimeSeconds() when fire was initiated
     * @param bClientPredicted - Whether client has already predicted this shot
     */
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerStartFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp, bool bClientPredicted, FRotator ClientViewRot, AUTCharacter* ClientHitChar, uint8 ZOffset);

    /**
     * Server RPC to stop firing.
     *
     * @param FireModeNum - Which fire mode to deactivate
     * @param InFireEventIndex - Final event index from client
     * @param ClientTimestamp - Client's timestamp when fire was stopped
     */
    UFUNCTION(Server, Reliable, WithValidation)
    void ServerStopFireFixed(uint8 FireModeNum, int32 InFireEventIndex, float ClientTimestamp);

    /**
     * Client RPC to confirm a fire event or correct client's event index.
     * This is the critical desync recovery mechanism - server pushes corrections.
     *
     * @param FireModeNum - Which fire mode this applies to
     * @param InAuthorizedEventIndex - Server's authoritative event index (what client should sync to)
     */
    UFUNCTION(Client, Reliable)
    void ClientConfirmFireEvent(uint8 FireModeNum, int32 InAuthorizedEventIndex);

    /**
     * Validates a fire request from the client.
     * Performs multi-layer checks:
     * - Event sequence validity (no duplicate or out-of-order events)
     * - Timestamp sanity (reject if >1s desync)
     * - Refire rate compliance (server-authoritative cooldown)
     *
     * @return true if request is valid and should be processed
     */
    bool ValidateFireRequest(uint8 FireModeNum, int32 InEventIndex, float ClientTime);



    /**
     * Generates next event index for client-side fire prediction.
     * Uses int32 to prevent overflow issues (stock code used uint8).
     *
     * @return Next sequential event index
     */
    int32 GetNextClientFireEventIndex(uint8 FireModeNum);

    /**
     * Validates that a fire event index is in valid sequence.
     * Allows small lookahead (10 events) to handle network reordering,
     * but rejects old/duplicate events.
     *
     * @return true if event index is valid in sequence
     */
    bool IsFireEventSequenceValid(uint8 FireModeNum, int32 InEventIndex);

    /**
     * Replication notify for fire mode state changes.
     * Updates CurrentlyFiringMode on non-owning clients.
     */
    UFUNCTION()
    void OnRep_FireModeState();

    /**
     * Get the hit validation time from the player controller.
     * Uses split prediction's hit validation time if available (120ms default).
     * Falls back to standard GetPredictionTime() if not using split prediction.
     *
     * @return Time in seconds to rewind pawns for hit validation
     */
    virtual float GetHitValidationPredictionTime() const;

    /** Setup replication */
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

    /** Impressive Add On */
    virtual void OnServerHitScanResult(const FHitResult& Hit, float PredictionTime);
};