#pragma once

#include "NetcodePlus.h"
#include "TeamArenaCharacter.h"
#include "TeamArenaCharacterMovement.h"
#include "NCAimTrainerCharacter.generated.h"

/** Trainer-only movement. The constrained practice lane uses world Y even
 * when the player looks sideways; ordinary NCP pawns keep their stock dodges. */
UCLASS()
class NETCODEPLUS_API UNCAimTrainerMovement : public UTeamArenaCharacterMovement
{
    GENERATED_BODY()
public:
    UNCAimTrainerMovement(const FObjectInitializer& ObjectInitializer);
    /** Setup boundaries only. Calling this during progress updates cancels jumps. */
    void ResetTrainerMovement(bool bPractice);
    virtual void CheckJumpInput(float DeltaTime) override;
    virtual void GetDodgeDirection(FVector& OutDodgeDir, FVector& OutDodgeCross) const override;
    virtual bool CanDodge() override;
    virtual bool CanJump() override;
    virtual bool CanCrouchInCurrentState() const override;
};

/** Only the aim trainer game mode spawns this trainee pawn. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANCAimTrainerCharacter : public ATeamArenaCharacter
{
    GENERATED_BODY()
public:
    ANCAimTrainerCharacter(const FObjectInitializer& ObjectInitializer);
};

/** Separate CDO: native crouch/skin restoration must retain the IG dimensions. */
UCLASS(NotBlueprintable)
class NETCODEPLUS_API ANCAimTrainerInstagibCharacter : public ANCAimTrainerCharacter
{
    GENERATED_BODY()
public:
    ANCAimTrainerInstagibCharacter(const FObjectInitializer& ObjectInitializer);
};
