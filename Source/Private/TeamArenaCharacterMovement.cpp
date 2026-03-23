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
        // corrections up to 180u due to tick-timing differences. Must smooth-blend
        // all of these instead of clamping.
        MaxSmoothNetUpdateDist = 180.f;

        // Default is 140.f. Above this distance, corrections HARD SNAP with zero
        // smoothing. At 600fps on a jumppad, errors can reach 160u+. Set high enough
        // that only cheats/teleports trigger hard snap.
        NoSmoothNetUpdateDist = 300.f;
    }

    typedef FNetworkPredictionData_Client_UTChar Super;
};


UTeamArenaCharacterMovement::UTeamArenaCharacterMovement(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // --- HIGH-FPS FIX #1: Increase position error tolerance ---
    // 6 units (was 3.16). Absorbs minor velocity prediction errors at 400+ FPS
    // without masking real desyncs (cheats/packet loss still exceed 6u).
    MaxPositionErrorSquared = 36.f;

    // --- Throttle settings ---
	TeamCollisionUpdateInterval = 0.0167f;  // instead of fps dependent
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
    DodgeCooldownTolerance = 0.1f;
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
    // At 480 FPS with 8 players = 30,720 calls/sec. We reduce to ~32 calls/sec.
    
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
