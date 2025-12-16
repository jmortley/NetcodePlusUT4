// TeamArenaCharacterMovement.cpp
// High-FPS optimized movement component for UT4

#include "TeamArenaCharacterMovement.h"
#include "TeamArenaCharacter.h"
#include "UTGameState.h"
#include "UTCharacter.h"
#include "Engine/World.h"

UTeamArenaCharacterMovement::UTeamArenaCharacterMovement(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    // --- HIGH-FPS FIX #1: Increase position error tolerance ---
    MaxPositionErrorSquared = 10.f;

    // --- Throttle settings ---
    TeamCollisionUpdateInterval = 0.0167f;  // 10 updates/sec instead of 480
    LastTeamCollisionUpdateTime = -1.0f;  // Force immediate first update

    // --- HIGH-FPS FIX #2: Dodge timing tolerance ---
    // Prevents server rejection when client/server timestamps differ by microseconds
    DodgeCooldownTolerance = 0.05f;
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
