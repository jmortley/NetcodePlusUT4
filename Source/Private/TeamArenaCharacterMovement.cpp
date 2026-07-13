// TeamArenaCharacterMovement.cpp
// High-FPS optimized movement component for UT4

#include "TeamArenaCharacterMovement.h"
#include "TeamArenaCharacter.h"
#include "UTGameState.h"
#include "UTCharacter.h"
#include "Engine/World.h"
#include "Components/CapsuleComponent.h"   // GetScaledCapsuleSize in ComputeSlideVectorUT


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

        // Saved-move buffer cap. In THIS engine fork the fields are named
        // MaxSavedMoveCount / MaxFreeMoveCount (CharacterMovementComponent.h:2449-2450) —
        // NOT the modern UE4 names MaxSavedMoves/MaxFreeMoves (which is why an earlier
        // attempt with those names failed to compile, C2065).
        //
        // Engine default ~96. At 480+ fps a hitch backlogs ~1 saved move per frame faster
        // than the server acks, so below ~700 CreateSavedMove() hits the cap, logs "hit
        // limit of N saved moves", and drops moves -> movement desync. 900 ~= 1.9s of stall
        // headroom. Client-side prediction data only (no replication / no version bump) ->
        // needs a client roll. Replaces the UE4-Engine DLL binary patch for TeamArena pawns.
        MaxSavedMoveCount = 900;
        MaxFreeMoveCount  = 900;
    }

    typedef FNetworkPredictionData_Client_UTChar Super;
};


UTeamArenaCharacterMovement::UTeamArenaCharacterMovement(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // --- HIGH-FPS FIX #1: Increase position error tolerance ---
    // 12 units. At 720fps with moderate ping, knockback replay divergence
    // from rockets/combos can exceed 8u. 12u covers light-to-medium impulses
    // without giving up too much cheat detection (~1728u/s undetected drift).
    MaxPositionErrorSquared = 144.f;

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
        // New rates (matches UT2004 UTComp's 0.011 NetMoveDelta):
        //   90Hz (11ms) — airborne, high-speed (>1500 u/s), or important moves
        //   40Hz (25ms) — normal ground movement
        //
        // Dodges and shots still bypass this entirely via CanDelaySendingMove.
        bool bNeedsHighRate = NewMove->IsImportantMove(ClientData->LastAckedMove)
                           || IsFalling()
                           || Velocity.SizeSquared() > 1500.f * 1500.f;
        float NetMoveDelta = bNeedsHighRate ? 0.011f : 0.025f;

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
    // TODO: Hard clamp for extreme corrections (IG+ style) — disabled for now,
    // needs more testing with water volumes and movement mode transitions.
    Super::SmoothClientPosition(DeltaSeconds);
}

// ── BSP slope-edge stick fix ─────────────────────────────────────────────────
// Copy of UUTCharacterMovement::ComputeSlideVectorUT (UTCharacterMovement.cpp:1651,
// engine fork CL-3525360 — frozen, no drift risk) with ONE branch changed.
//
// Stock reduces only Result.Z in the slope-dodge-boost branch. Result comes from a
// plane projection (Result|Normal == 0), so a Z-only reduction points the slide
// vector back INTO any upward-facing surface (Result|Normal < 0) and the very next
// move re-collides with the face it just left. On flat faces TwoWallAdjust
// self-heals; on BSP seams/convex edges the re-impact returns edge normals, the
// delta collapses, and PhysFalling rebuilds velocity from actual displacement —
// the player hard-sticks mid-slope (timiimit/UT4UU-Public#4, the BunnyTrack slope
// bug). Epic's own comment in the full-clamp branch below names the failure mode.
//
// Fix: keep the exact stock Z limit (slope-dodge boost + uphill cap unchanged) but
// apply it by rescaling the WHOLE vector so the slide stays plane-parallel, then
// return the clipped portion as horizontal motion along the surface — the same
// idiom the full-clamp branch (and engine HandleSlopeBoosting) already uses.
//
// Runs in client prediction AND server move replay: shared simulation, so it must
// ship client+server together (NETCODE_PLUGIN_VERSION bump; the gate enforces it).

// Matches the file-local MAX_STEP_SIDE_Z in UTCharacterMovement.cpp:14 (internal
// linkage there, not referenceable from this module).
static const float NCP_MAX_STEP_SIDE_Z = 0.08f;

FVector UTeamArenaCharacterMovement::ComputeSlideVectorUT(const float DeltaTime, const FVector& InDelta, const float Time, const FVector& Normal, const FHitResult& Hit)
{
    const bool bFalling = IsFalling();
    FVector Delta = InDelta;
    FVector Result = UMovementComponent::ComputeSlideVector(Delta, Time, Normal, Hit);

    // prevent boosting up slopes
    if (bFalling && Result.Z > 0.f)
    {
        float PawnRadius, PawnHalfHeight;
        CharacterOwner->GetCapsuleComponent()->GetScaledCapsuleSize(PawnRadius, PawnHalfHeight);
        if (Delta.Z < 0.f && (Hit.ImpactNormal.Z < NCP_MAX_STEP_SIDE_Z))
        {
            // (stock) We were moving downward, but a slide was going to send us upward. We want
            // to aim straight down for the next move to make sure we get the most upward-facing
            // opposing normal.
            Result = FVector(0.f, 0.f, Delta.Z);
        }
        else if (bAllowSlopeDodgeBoost && (((CharacterOwner->GetActorLocation() - Hit.ImpactPoint).Size2D() > 0.93f * PawnRadius) || (Hit.ImpactNormal.Z > 0.2f)))
        {
            if (Result.Z > Delta.Z * Time)
            {
                // CHANGED vs stock: stock did `Result.Z = Max(Result.Z * SlopeDodgeScaling,
                // Delta.Z*Time)` and left XY alone, which tilts the slide into the plane and
                // re-collides with the surface it just left (the BSP slope-edge stick). We take
                // the exact same Z, then cancel the into-plane component by adjusting the
                // horizontal part along the slope's horizontal normal only — cross-slope motion
                // is untouched. Algebraically identical to "rescale the whole vector and give
                // the clipped remainder back tangentially", but with no normalize and no
                // GetSafeNormal2D degeneracy: for near-vertical normals (|Normal.XY|^2 <
                // SMALL_NUMBER) that idiom silently restored FULL XY and re-tilted the vector
                // into the plane, so we fall back to a uniform rescale there instead — still
                // exactly plane-parallel.
                // Deliberately NOT gated on ZLimit like the clamp branch below: ZLimit <= 0
                // (descending into the slope) is the main slope-dodge-boost case, and stock
                // still applies the 0.93 scaling there.
                const FVector SlideResult = Result;
                const float TargetZ = FMath::Max(Result.Z * SlopeDodgeScaling, Delta.Z * Time);
                Result.Z = TargetZ;

                const FVector HorizontalNormal(Normal.X, Normal.Y, 0.f);
                const float HorizontalNormalSq = HorizontalNormal.SizeSquared();
                if (HorizontalNormalSq >= SMALL_NUMBER)
                {
                    Result -= HorizontalNormal * ((Result | Normal) / HorizontalNormalSq);
                }
                else
                {
                    // Uniform rescale keeps the projection's direction (parallel by
                    // construction); the divisor is > 0 via the enclosing Result.Z > 0 gate.
                    Result = SlideResult * (TargetZ / SlideResult.Z);
                    Result.Z = TargetZ;
                }
            }
        }
        else
        {
            // (stock) Don't move any higher than we originally intended.
            const float ZLimit = Delta.Z * Time;
            if (Result.Z > ZLimit && ZLimit > KINDA_SMALL_NUMBER)
            {
                FVector SlideResult = Result;

                // Rescale the entire vector (not just the Z component) otherwise we change the
                // direction and likely head right back into the impact.
                const float UpPercent = ZLimit / Result.Z;
                Result *= UpPercent;

                // Make remaining portion of original result horizontal and parallel to impact normal.
                const FVector RemainderXY = (SlideResult - Result) * FVector(1.f, 1.f, 0.f);
                const FVector NormalXY = Normal.GetSafeNormal2D();
                const FVector Adjust = UCharacterMovementComponent::ComputeSlideVector(RemainderXY, 1.f, NormalXY, Hit);
                Result += Adjust;
            }
        }
    }
    return Result;
}
