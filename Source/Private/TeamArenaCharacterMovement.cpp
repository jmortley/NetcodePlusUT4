// TeamArenaCharacterMovement.cpp
// High-FPS optimized movement component for UT4

#include "TeamArenaCharacterMovement.h"
#include "TeamArenaCharacter.h"
#include "UTGameState.h"
#include "UTCharacter.h"
#include "Engine/World.h"


// This prevents visual jitter when the server sends small corrections
class FNetworkPredictionData_Client_TeamArena : public FNetworkPredictionData_Client_UTChar
{
public:
    FNetworkPredictionData_Client_TeamArena(const UCharacterMovementComponent& ClientMovement)
        : FNetworkPredictionData_Client_UTChar(ClientMovement)
    {
        // --- FIX: Set variables directly in constructor instead of overriding functions ---

        // Default is 92.f. At 600fps, jumppad launches (1200-1800 u/s) produce
        // corrections up to 240u due to tick-timing differences and ping-scaled
        // prediction error. Must smooth-blend all of these instead of clamping.
        MaxSmoothNetUpdateDist = 240.f;

        // Default is 140.f. Above this distance, corrections HARD SNAP with zero
        // smoothing. At 600fps on a jumppad with higher ping, errors can reach 200u+.
        // Set high enough that only cheats/teleports trigger hard snap.
        NoSmoothNetUpdateDist = 400.f;
    }

    typedef FNetworkPredictionData_Client_UTChar Super;
};


UTeamArenaCharacterMovement::UTeamArenaCharacterMovement(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // --- HIGH-FPS FIX #1: Increase position error tolerance ---
    // 8 units (was 3.16, then 6). At 400+ FPS with moderate ping, float drift
    // across many predicted frames can exceed 6u during fast movement/knockback.
    // 8u still well below cheat/packet-loss thresholds.
    MaxPositionErrorSquared = 64.f;

    // --- Throttle settings ---
	TeamCollisionUpdateInterval = 0.01111f;  // instead of fps dependent
    LastTeamCollisionUpdateTime = -1.0f;  // Force immediate first update
    // 50ms (was 100ms). At 400 FPS, halves accumulated prediction error per
    // correction (20 predicted frames instead of 40). Negligible bandwidth cost.
	MinTimeBetweenClientAdjustments = 0.05f;
    // 80 units (was 15, then 40). At 600fps, jumppad corrections can reach 60-80u.
    // Above this threshold, server sends corrections more aggressively (every 50ms
    // instead of 100ms), which is desirable during high-velocity events — more
    // frequent corrections means each one is smaller.
	LargeCorrectionThreshold = 80.f;
    // --- HIGH-FPS FIX #2: Dodge timing tolerance ---
    // Prevents server rejection when client/server timestamps differ by microseconds
    DodgeCooldownTolerance = 0.06f;
}

FNetworkPredictionData_Client* UTeamArenaCharacterMovement::GetPredictionData_Client() const
{
    if (!ClientPredictionData)
    {
        UTeamArenaCharacterMovement* MutableThis = const_cast<UTeamArenaCharacterMovement*>(this);
        MutableThis->ClientPredictionData = new FNetworkPredictionData_Client_TeamArena(*this);
    }
    return ClientPredictionData;
}


void UTeamArenaCharacterMovement::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    // --- HIGH-FPS FIX #3: Throttle team collision updates ---
    // Epic's code runs GetPawnIterator() + IgnoreActorWhenMoving() EVERY TICK
    // At 480 FPS with 8 players = 30,720 calls/sec. We reduce.
    
    // Temporarily force team collision flag to skip Epic's per-tick iterator
    bool bOriginalForceTeamCollision = bForceTeamCollision;
    bForceTeamCollision = true;
    
    // Call parent tick (which now skips the expensive iterator)
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    
    // Restore original value
    bForceTeamCollision = bOriginalForceTeamCollision;
    
    // Now do our throttled team collision update
    if (!bForceTeamCollision)
    {
        const float WorldTime = GetWorld()->GetTimeSeconds();
        if (WorldTime - LastTeamCollisionUpdateTime >= TeamCollisionUpdateInterval)
        {
            LastTeamCollisionUpdateTime = WorldTime;
            UpdateTeamCollisionIgnores();
        }
    }
}

void UTeamArenaCharacterMovement::UpdateTeamCollisionIgnores()
{
    AUTCharacter* UTOwner = Cast<AUTCharacter>(CharacterOwner);
    if (!UTOwner)
    {
        return;
    }

    AUTGameState* GS = GetWorld()->GetGameState<AUTGameState>();
    if (!GS || GS->bTeamCollision)
    {
        return;
    }

    UPrimitiveComponent* Capsule = Cast<UPrimitiveComponent>(UpdatedComponent);
    if (!Capsule)
    {
        return;
    }

    // This is Epic's original logic, just not running 480 times per second
    for (FConstPawnIterator It = GetWorld()->GetPawnIterator(); It; ++It)
    {
        AUTCharacter* Char = It->IsValid() ? Cast<AUTCharacter>((*It).Get()) : nullptr;
        if (Char)
        {
            bool bShouldIgnore = GS->OnSameTeam(UTOwner, Char) && 
                                 (UTOwner != Char) && 
                                 !Char->IsPendingKillPending() && 
                                 !Char->IsDead() && 
                                 !Char->IsRagdoll();
            Capsule->IgnoreActorWhenMoving(Char, bShouldIgnore);
        }
    }
}

void UTeamArenaCharacterMovement::UTCallServerMove()
{
    AUTCharacter* UTCharacterOwner = Cast<AUTCharacter>(CharacterOwner);
    FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
    if (!UTCharacterOwner || !ClientData || (ClientData->SavedMoves.Num() == 0))
    {
        return;
    }
    APlayerController* PC = Cast<APlayerController>(CharacterOwner->GetController());

    // Decide whether to hold off on move
    const FSavedMovePtr& NewMove = ClientData->SavedMoves.Last();
    if (CanDelaySendingMove(NewMove))
    {
        // --- HIGH-FPS FIX #5: Adaptive move send rate ---
        // Stock UT4: 60Hz important, 30Hz normal. At 400+ FPS the 30Hz floor
        // leaves 66u gaps at sprint speed — too much room for float drift.
        //
        // New rates:
        //   60Hz (17ms) — airborne, high-speed (>1500 u/s), or important moves
        //   40Hz (25ms) — normal ground movement
        //
        // Dodges and shots still bypass this entirely via CanDelaySendingMove.
        bool bNeedsHighRate = NewMove->IsImportantMove(ClientData->LastAckedMove)
                           || IsFalling()
                           || Velocity.SizeSquared() > 1500.f * 1500.f;
        float NetMoveDelta = bNeedsHighRate ? 0.017f : 0.025f;

        if (((NewMove->TimeStamp - ClientData->ClientUpdateTime) * CharacterOwner->GetWorldSettings()->GetEffectiveTimeDilation() < NetMoveDelta))
        {
            return;
        }
    }

    // Find the oldest unacknowledged sent important move (OldMove).
    // Don't include the last move because it may be combined with the next new move.
    FSavedMovePtr OldMovePtr = NULL;
    if (ClientData->LastAckedMove.IsValid())
    {
        bool bHaveCriticalMove = false;
        for (int32 i = 0; i < ClientData->SavedMoves.Num() - 1; i++)
        {
            const FSavedMovePtr& CurrentMove = ClientData->SavedMoves[i];
            if (CurrentMove->TimeStamp > ClientData->ClientUpdateTime)
            {
                break;
            }
            else if (CurrentMove->IsImportantMove(ClientData->LastAckedMove))
            {
                bool bNewCriticalMove = ((FSavedMove_UTCharacter*)(CurrentMove.Get()))->IsCriticalMove(ClientData->LastAckedMove);
                if (bNewCriticalMove || !bHaveCriticalMove)
                {
                    OldMovePtr = CurrentMove;
                    bHaveCriticalMove = bNewCriticalMove;
                }
            }
        }
    }

    // Send old move if it exists
    if (OldMovePtr.IsValid())
    {
        const FSavedMove_Character* OldMove = OldMovePtr.Get();
        UTCharacterOwner->UTServerMoveOld(OldMove->TimeStamp, OldMove->Acceleration, OldMove->SavedControlRotation.Yaw, OldMove->GetCompressedFlags());
    }

    for (int32 i = 0; i < ClientData->SavedMoves.Num() - 1; i++)
    {
        const FSavedMovePtr& MoveToSend = ClientData->SavedMoves[i];
        if (MoveToSend.IsValid() && (MoveToSend->TimeStamp > ClientData->ClientUpdateTime))
        {
            ClientData->ClientUpdateTime = MoveToSend->TimeStamp;
            if (((FSavedMove_UTCharacter*)(MoveToSend.Get()))->NeedsRotationSent())
            {
                UTCharacterOwner->UTServerMoveSaved(MoveToSend->TimeStamp, MoveToSend->Acceleration, MoveToSend->GetCompressedFlags(), MoveToSend->SavedControlRotation.Yaw, MoveToSend->SavedControlRotation.Pitch);
            }
            else
            {
                UTCharacterOwner->UTServerMoveQuick(MoveToSend->TimeStamp, MoveToSend->Acceleration, MoveToSend->GetCompressedFlags());
            }
        }
    }

    if (NewMove.IsValid() && (NewMove->TimeStamp > ClientData->ClientUpdateTime))
    {
        UPrimitiveComponent* ClientMovementBase = NewMove->EndBase.Get();
        bool bUseRelativeLocation = MovementBaseUtility::UseRelativeLocation(ClientMovementBase);
        const FVector SendLocation = bUseRelativeLocation ? NewMove->SavedRelativeLocation : NewMove->SavedLocation;
        if (!bUseRelativeLocation)
        {
            ClientMovementBase = NULL;
            NewMove->EndBoneName = NAME_None;
        }
        ClientData->ClientUpdateTime = NewMove->TimeStamp;
        UTCharacterOwner->UTServerMove
            (
            NewMove->TimeStamp,
            NewMove->Acceleration,
            SendLocation,
            NewMove->GetCompressedFlags(),
            NewMove->SavedControlRotation.Yaw,
            NewMove->SavedControlRotation.Pitch,
            ClientMovementBase,
            NewMove->EndBoneName,
            NewMove->MovementMode
            );
    }

    APlayerCameraManager* PlayerCameraManager = (PC ? PC->PlayerCameraManager : NULL);
    if (PlayerCameraManager != NULL && PlayerCameraManager->bUseClientSideCameraUpdates)
    {
        PlayerCameraManager->bShouldSendClientSideCameraUpdate = true;
    }
}

bool UTeamArenaCharacterMovement::CanDodge()
{
    // --- HIGH-FPS FIX #4: Add tolerance to dodge cooldown ---
    // Problem: Client thinks dodge ready at t=0.35000, server calculates t=0.34998
    // Server rejects, client hitches on correction.
    // Solution: Server-side tolerance (client still predicts strictly)
    
    float Tolerance = 0.0f;
    if (GetNetMode() != NM_Client)
    {
        // Only apply tolerance on server/authority
        // This way client predicts at exact timing, server accepts with tolerance
        Tolerance = DodgeCooldownTolerance;
    }

    return !bIsFloorSliding &&
           bCanDodge &&
           CanEverJump() &&
           (GetCurrentMovementTime() > (DodgeResetTime - Tolerance)) &&
           !IsRootedByWeapon();
}

void UTeamArenaCharacterMovement::SmoothClientPosition(float DeltaSeconds)
{
    Super::SmoothClientPosition(DeltaSeconds);

    // Hard clamp — extreme corrections (teleport, packet loss, jump pad edge case)
    // should snap instead of smoothing over multiple frames. IG+ does this at 100u;
    // we use 250u since UE4 corrections accumulate differently.
    FNetworkPredictionData_Client_Character* ClientData = GetPredictionData_Client_Character();
    if (ClientData && ClientData->MeshTranslationOffset.SizeSquared() > 250.f * 250.f)
    {
        ClientData->MeshTranslationOffset = FVector::ZeroVector;
    }
}
