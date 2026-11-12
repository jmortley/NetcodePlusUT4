"""Compile the actual trainer movement gates with a native movement adapter.

Exercises local and decoded-input paths, fixed-position protection, and a
world-space dodge basis independent of camera yaw. Collision and prediction
still need the normal Unreal packaged playtest.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cstdlib>
#include <iostream>
#include <string>
struct FVector {
    float X, Y, Z;
    FVector(float x=0, float y=0, float z=0) : X(x), Y(y), Z(z) {}
};
enum EMovementMode { MOVE_None, MOVE_Walking, MOVE_Falling };
struct ACharacter { bool bPressedJump = false, bIsCrouched = false; float Yaw = 0.f; };
struct MockTimerManager { int Clears = 0; void ClearTimer(int&) { ++Clears; } };
struct MockWorld { MockTimerManager Timers; MockTimerManager& GetTimerManager() { return Timers; } };
struct UTeamArenaCharacterMovement {
    bool bConstrainToPlane = false;
    EMovementMode MovementMode = MOVE_None;
    bool bPressedDodgeForward = false, bPressedDodgeBack = false;
    bool bPressedDodgeLeft = false, bPressedDodgeRight = false;
    bool bPressedSlide = false, bWantsToCrouch = false;
    bool bWantsFloorSlide = false, bWantsWallSlide = false;
    bool bIsFloorSliding = false, bIsDodging = false, bWasFloorSliding = false, bIsDodgeLanding = false;
    int CurrentMultiJumpCount = 0, CurrentWallDodgeCount = 0, FloorSlideTapHandle = 0;
    FVector Velocity, PlaneNormal, PlaneOrigin;
    MockWorld World;
    bool StockDodgeAllowed = true, StockJumpAllowed = true, StockCrouchAllowed = true;
    int Maintenance = 0, Jumps = 0, Dodges = 0, Slides = 0;
    FVector LastDodge, LastCross;
    ACharacter* CharacterOwner = nullptr;
    MockWorld* GetWorld() { return &World; }
    void StopMovementImmediately() { Velocity = FVector(); }
    void ClearFloorSlideTap() { bWantsFloorSlide = false; }
    void UpdateWallSlide(bool enabled) { bWantsWallSlide = enabled; }
    void ClearFallingStateFlags() {
        bIsFloorSliding = bIsDodging = false; CurrentMultiJumpCount = CurrentWallDodgeCount = 0;
    }
    void UnCrouch(bool) { if (CharacterOwner) CharacterOwner->bIsCrouched = false; }
    void SetPlaneConstraintNormal(FVector value) { PlaneNormal = value; }
    void SetPlaneConstraintOrigin(FVector value) { PlaneOrigin = value; }
    void SetPlaneConstraintEnabled(bool enabled) { bConstrainToPlane = enabled; }
    void SetMovementMode(EMovementMode mode) { MovementMode = mode; }
    void DisableMovement() { MovementMode = MOVE_None; }
    virtual ~UTeamArenaCharacterMovement() = default;
    virtual bool CanDodge() { return StockDodgeAllowed; }
    virtual bool CanJump() { return StockJumpAllowed; }
    virtual bool CanCrouchInCurrentState() const { return StockCrouchAllowed; }
    virtual void GetDodgeDirection(FVector&, FVector&) const {}
    void ClearDodgeInput() {
        bPressedDodgeForward = bPressedDodgeBack = bPressedDodgeLeft = bPressedDodgeRight = bPressedSlide = false;
    }
    // Minimal observable stock boundary: the production wrapper must sanitize
    // both local buttons and flags reconstructed from a remote saved move.
    virtual void CheckJumpInput(float) {
        ++Maintenance;
        if (bPressedSlide) ++Slides;
        if (CharacterOwner && CharacterOwner->bPressedJump) {
            if (CanJump()) { ++Jumps; MovementMode = MOVE_Falling; }
        } else if ((bPressedDodgeForward || bPressedDodgeBack || bPressedDodgeLeft || bPressedDodgeRight) && CanDodge()) {
            ++Dodges; GetDodgeDirection(LastDodge, LastCross); MovementMode = MOVE_Falling;
        }
    }
};
struct UNCAimTrainerMovement : UTeamArenaCharacterMovement {
    using Super = UTeamArenaCharacterMovement;
    void ResetTrainerMovement(bool, float = -1800.f);
    void CheckJumpInput(float) override;
    void GetDodgeDirection(FVector&, FVector&) const override;
    bool CanDodge() override;
    bool CanJump() override;
    bool CanCrouchInCurrentState() const override;
};
'''

CASES = r'''
void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
void FixedInput() {
    for (bool constrained : {false, true}) {
        ACharacter pawn; pawn.bPressedJump = true;
        UNCAimTrainerMovement move; move.CharacterOwner = &pawn;
        move.bConstrainToPlane = constrained;
        move.bPressedDodgeLeft = move.bPressedDodgeRight = move.bPressedDodgeForward = move.bPressedDodgeBack = true;
        move.bPressedSlide = move.bWantsToCrouch = true;
        move.CheckJumpInput(.016f);
        Require(move.Jumps == 0 && move.Dodges == 0 && move.Slides == 0, "fixed mode admitted movement");
        Require(!pawn.bPressedJump && !move.bWantsToCrouch && !move.bPressedDodgeLeft && !move.bPressedDodgeRight,
                "fixed mode retained requests for a later frame");
        Require(!move.CanJump() && !move.CanDodge() && !move.CanCrouchInCurrentState(), "direct fixed-mode gate bypass");
        Require(move.Maintenance == 1, "stock bookkeeping was skipped");
    }
}
void RemoteForward() {
    ACharacter pawn;
    UNCAimTrainerMovement move; move.CharacterOwner = &pawn;
    move.bConstrainToPlane = true; move.MovementMode = MOVE_Walking;
    for (int input = 0; input < 2; ++input) {
        move.bPressedDodgeForward = input == 0; move.bPressedDodgeBack = input == 1;
        move.CheckJumpInput(.016f);
        Require(move.Dodges == 0 && move.MovementMode == MOVE_Walking,
                "decoded forward/back dodge caused a vertical or sideways hop");
    }
    move.bPressedDodgeForward = true; move.bPressedDodgeRight = true;
    move.CheckJumpInput(.016f);
    Require(move.Dodges == 1 && move.LastDodge.Y == 1.f && move.LastDodge.X == 0.f,
            "forbidden direction defeated an allowed side dodge");
}
void Direction() {
    for (float yaw : {0.f, 45.f, 90.f, 180.f, 270.f}) {
        for (bool left : {false, true}) {
            ACharacter pawn; pawn.Yaw = yaw;
            UNCAimTrainerMovement move; move.CharacterOwner = &pawn;
            move.bConstrainToPlane = true; move.MovementMode = MOVE_Walking;
            move.bPressedDodgeLeft = left; move.bPressedDodgeRight = !left;
            move.CheckJumpInput(.016f);
            Require(move.Dodges == 1 && move.LastDodge.X == 0.f && move.LastDodge.Z == 0.f
                    && move.LastDodge.Y == (left ? -1.f : 1.f), "side dodge lost full lane direction while looking away");
            Require(move.LastCross.X == 1.f && move.LastCross.Y == 0.f && move.LastCross.Z == 0.f,
                    "invalid perpendicular dodge basis");
        }
    }
}
void PracticeActions() {
    ACharacter pawn; pawn.bPressedJump = true;
    UNCAimTrainerMovement move; move.CharacterOwner = &pawn;
    move.bConstrainToPlane = true; move.MovementMode = MOVE_Walking;
    move.bWantsToCrouch = true;
    move.CheckJumpInput(.016f);
    Require(move.Jumps == 1 && move.MovementMode == MOVE_Falling, "practice jump suppressed");
    Require(move.bWantsToCrouch && move.CanCrouchInCurrentState(), "practice crouch suppressed");
    pawn.bPressedJump = false; move.bPressedSlide = true;
    move.CheckJumpInput(.016f);
    Require(move.Slides == 1 && move.MovementMode == MOVE_Falling, "normal slide request or airborne mode changed");
}
void StockRules() {
    UNCAimTrainerMovement move;
    move.bConstrainToPlane = true; move.MovementMode = MOVE_Walking;
    move.StockDodgeAllowed = move.StockJumpAllowed = move.StockCrouchAllowed = false;
    Require(!move.CanDodge() && !move.CanJump() && !move.CanCrouchInCurrentState(),
            "practice bypassed cooldown, jump availability or crouch rules");
    move.StockDodgeAllowed = move.StockJumpAllowed = move.StockCrouchAllowed = true;
    move.bConstrainToPlane = false;
    Require(!move.CanDodge() && !move.CanJump() && !move.CanCrouchInCurrentState(), "unconstrained pawn admitted movement");
    move.CheckJumpInput(.016f); // no owner must remain safe
}
void ResetPosture() {
    for (bool practice : {false, true}) {
        ACharacter pawn; pawn.bPressedJump = pawn.bIsCrouched = true;
        UNCAimTrainerMovement move; move.CharacterOwner = &pawn;
        move.Velocity = FVector(5.f, 1200.f, 300.f);
        move.bPressedDodgeLeft = move.bPressedSlide = move.bWantsToCrouch = true;
        move.bWantsFloorSlide = move.bWantsWallSlide = true;
        move.bIsFloorSliding = move.bIsDodging = move.bWasFloorSliding = move.bIsDodgeLanding = true;
        move.CurrentMultiJumpCount = 2; move.CurrentWallDodgeCount = 3;
        move.ResetTrainerMovement(practice);
        Require(move.Velocity.X == 0.f && move.Velocity.Y == 0.f && move.Velocity.Z == 0.f,
                "fresh run retained velocity");
        Require(!pawn.bPressedJump && !pawn.bIsCrouched && !move.bWantsToCrouch,
                "fresh run retained owner posture or requests");
        Require(!move.bPressedDodgeLeft && !move.bPressedSlide && !move.bWantsFloorSlide && !move.bWantsWallSlide
                && !move.bIsFloorSliding && !move.bIsDodging && !move.bWasFloorSliding && !move.bIsDodgeLanding,
                "fresh run retained dodge or slide state");
        Require(move.CurrentMultiJumpCount == 0 && move.CurrentWallDodgeCount == 0 && move.World.Timers.Clears == 1,
                "fresh run retained jump counts or delayed slide timer");
        Require(move.bConstrainToPlane == practice && move.MovementMode == (practice ? MOVE_Walking : MOVE_None),
                "fresh run mode did not match movement option");
        Require(move.PlaneNormal.X == 1.f && move.PlaneNormal.Y == 0.f && move.PlaneNormal.Z == 0.f
                && move.PlaneOrigin.X == -1800.f && move.PlaneOrigin.Z == 50108.f,
                "fresh run plane was based on a possibly displaced pawn");
    }
}
void ScenarioLane() {
    ACharacter pawn;
    UNCAimTrainerMovement move; move.CharacterOwner = &pawn;
    for (bool practice : {false, true}) {
        move.ResetTrainerMovement(practice, -800.f);
        Require(move.PlaneOrigin.X == -800.f && move.PlaneNormal.X == 1.f
                && move.bConstrainToPlane == practice, "rocket lane reset retained the distant firing plane");
        move.ResetTrainerMovement(practice);
        Require(move.PlaneOrigin.X == -1800.f && move.bConstrainToPlane == practice,
                "normal preset retained the closer rocket firing plane");
    }
}
int main(int argc, char** argv) {
    Require(argc == 2, "case required"); const std::string name(argv[1]);
    if (name == "fixed") FixedInput();
    else if (name == "remote") RemoteForward();
    else if (name == "direction") Direction();
    else if (name == "actions") PracticeActions();
    else if (name == "stock") StockRules();
    else if (name == "reset") ResetPosture();
    else if (name == "scenario_lane") ScenarioLane();
    else Require(false, "unknown case");
}
'''


class AimTrainerMovementTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-aim-trainer-movement-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerCharacter.cpp").read_text(encoding="utf-8-sig")
        signatures = (
            "void UNCAimTrainerMovement::ResetTrainerMovement",
            "void UNCAimTrainerMovement::CheckJumpInput",
            "void UNCAimTrainerMovement::GetDodgeDirection",
            "bool UNCAimTrainerMovement::CanDodge",
            "bool UNCAimTrainerMovement::CanJump",
            "bool UNCAimTrainerMovement::CanCrouchInCurrentState",
        )
        source = directory / "trainer_movement.cpp"
        source.write_text("\n".join([ADAPTER] + [native_function(native, s) for s in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("trainer_movement.exe" if os.name == "nt" else "trainer_movement")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'trainer_movement.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Movement adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_fixed_position_cannot_jump_dodge_crouch_or_slide(self): self.run_case("fixed")
    def test_saved_move_forward_and_backward_dodges_are_rejected(self): self.run_case("remote")
    def test_side_dodge_keeps_direction_while_aiming_around(self): self.run_case("direction")
    def test_practice_retains_jump_crouch_slide_and_airborne_mode(self): self.run_case("actions")
    def test_practice_respects_stock_movement_rules(self): self.run_case("stock")
    def test_fresh_run_resets_slide_jump_and_crouched_owner_state(self): self.run_case("reset")
    def test_explicit_rocket_lane_and_default_lane_are_restored(self): self.run_case("scenario_lane")


if __name__ == "__main__":
    unittest.main()
