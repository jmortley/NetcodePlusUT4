#include "NCAimTrainerCharacter.h"
#include "TimerManager.h"

ANCAimTrainerCharacter::ANCAimTrainerCharacter(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer.SetDefaultSubobjectClass<UNCAimTrainerMovement>(ACharacter::CharacterMovementComponentName))
{
}

UNCAimTrainerMovement::UNCAimTrainerMovement(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
}

void UNCAimTrainerMovement::ResetTrainerMovement(bool bPractice)
{
    StopMovementImmediately();
    ClearDodgeInput();
    ClearFloorSlideTap();
    UpdateWallSlide(false);
    if (GetWorld()) { GetWorld()->GetTimerManager().ClearTimer(FloorSlideTapHandle); }
    ClearFallingStateFlags();
    bWasFloorSliding = false;
    bIsDodgeLanding = false;
    bWantsToCrouch = false;
    if (CharacterOwner) { CharacterOwner->bPressedJump = false; }
    // Crouched state skips owner replication in UT. Both authority and the
    // locally controlled pawn must expand their own capsule on a fresh run.
    UnCrouch(false);
    SetPlaneConstraintNormal(FVector(1.f, 0.f, 0.f));
    SetPlaneConstraintOrigin(FVector(-1800.f, 0.f, 50108.f));
    SetPlaneConstraintEnabled(bPractice);
    if (bPractice) { SetMovementMode(MOVE_Walking); }
    else { DisableMovement(); }
}

void UNCAimTrainerMovement::CheckJumpInput(float DeltaTime)
{
    // Tap actions and decoded saved moves set these independently of MoveForward.
    // Reject them here on both the predicting client and the authoritative pawn.
    bPressedDodgeForward = false;
    bPressedDodgeBack = false;
    if (!bConstrainToPlane || MovementMode == MOVE_None)
    {
        ClearDodgeInput();
        bWantsToCrouch = false;
        if (CharacterOwner) { CharacterOwner->bPressedJump = false; }
    }
    // Preserve normal jump, slide, dodge cooldown and movement event handling.
    Super::CheckJumpInput(DeltaTime);
}

void UNCAimTrainerMovement::GetDodgeDirection(FVector& OutDodgeDir, FVector& OutDodgeCross) const
{
    // Plane projection alone would shorten a dodge as the view turns, or even
    // turn a forward double-tap into a lateral dodge. Keep the input basis fixed.
    OutDodgeDir = FVector(0.f, bPressedDodgeLeft ? -1.f : (bPressedDodgeRight ? 1.f : 0.f), 0.f);
    OutDodgeCross = FVector(1.f, 0.f, 0.f);
}

bool UNCAimTrainerMovement::CanDodge()
{
    return bConstrainToPlane && MovementMode != MOVE_None && Super::CanDodge();
}

bool UNCAimTrainerMovement::CanJump()
{
    return bConstrainToPlane && MovementMode != MOVE_None && Super::CanJump();
}

bool UNCAimTrainerMovement::CanCrouchInCurrentState() const
{
    return bConstrainToPlane && MovementMode != MOVE_None && Super::CanCrouchInCurrentState();
}
