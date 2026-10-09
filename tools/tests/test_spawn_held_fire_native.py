"""Compile actual pawn-restart recovery and stock held-input verifier methods.

Small adapters model possession, timers and UI state. This is not a UE build or
proof of actor-channel ordering; it exercises readiness and duplicate guards.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

try:
    from .test_wipeout_healing import PLUGIN, find_compiler, native_function
except ImportError:
    from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>
#define UE_SERVER 0
const int NM_Client = 1, NM_DedicatedServer = 2, NAME_Inactive = 0, NAME_Playing = 1, NAME_Spectating = 2;
struct UObject { virtual ~UObject() {} bool Valid = true; };
template<class T> T* Cast(UObject* value) { return dynamic_cast<T*>(value); }
template<class T> struct TWeakObjectPtr {
    T* Value = nullptr;
    T* Get() const { return Value && Value->Valid ? Value : nullptr; }
    void Reset() { Value = nullptr; }
    TWeakObjectPtr& operator=(T* value) { Value = value; return *this; }
};
struct FPlatformTime { static double Now; static double Seconds() { return Now; } };
double FPlatformTime::Now = 1.0;
struct FTimerHandle {};
struct TimerManager {
    std::vector<std::function<void()>> Next;
    std::function<void()> Pending;
    int Scheduled = 0;
    template<class T> void SetTimerForNextTick(T* object, void (T::*method)()) {
        ++Scheduled; Next.push_back([object, method]() { (object->*method)(); });
    }
    template<class T> void SetTimer(FTimerHandle&, T* object, void (T::*method)(), float rate, bool loop) {
        if (rate != .01f || loop) std::abort();
        ++Scheduled; Pending = [object, method]() { (object->*method)(); };
    }
    void ClearTimer(FTimerHandle&) { Pending = nullptr; }
    void Tick() {
        auto work = Next; Next.clear();
        if (Pending) { work.push_back(Pending); Pending = nullptr; }
        for (auto& call : work) call();
    }
    bool Empty() const { return Next.empty() && !Pending; }
};
struct DemoDriver { bool Playback = false; bool IsPlaying() const { return Playback; } };
struct AUTGameState { bool Blocked = false; bool PreventWeaponFire() const { return Blocked; } };
struct UWorld : UObject {
    bool Paused = false;
    DemoDriver* DemoNetDriver = nullptr;
    AUTGameState* GS = nullptr;
    TimerManager Timers;
    TimerManager& GetTimerManager() { return Timers; }
    bool IsPaused() const { return Paused; }
    template<class T> T* GetGameState() { return static_cast<T*>(GS); }
};
struct Console { bool Open = false; bool ConsoleActive() const { return Open; } };
struct FViewport { bool Focus = true; bool HasFocus() const { return Focus; } };
struct UGameViewportClient { bool Ignored = false; Console* ViewportConsole = nullptr; FViewport* Viewport = nullptr; bool IgnoreInput() const { return Ignored; } };
struct FQuickChatWidget { bool Open; bool IsValid() const { return Open; } };
struct UUTLocalPlayer : UObject {
    bool Menus = false, Chat = false;
    UGameViewportClient* ViewportClient = nullptr;
    virtual bool AreMenusOpen() { return Menus; }
    FQuickChatWidget GetQuickChatWidget();
};
FQuickChatWidget UUTLocalPlayer::GetQuickChatWidget() { return {Chat}; }
struct FVirtualMenuTrapLocalPlayer : UUTLocalPlayer {
    int VirtualMenuCalls = 0;
    bool AreMenusOpen() override { ++VirtualMenuCalls; return true; }
};
struct AController : UObject {};
struct AUTCharacter;
struct UUTWeaponStateInactive { static int StaticClass() { return 1; } };
struct UUTWeaponState { bool Inactive = false; bool IsA(int type) const { return type == 1 && Inactive; } };
struct AUTWeapon : UObject {
    AUTCharacter* Owner = nullptr;
    UUTWeaponState State;
    bool Firing = false, PendingKill = false, HasState = true, Unequipping = false;
    AUTCharacter* GetUTOwner() const { return Owner; }
    bool IsPendingKillPending() const { return PendingKill; }
    UUTWeaponState* GetCurrentState() { return HasState ? &State : nullptr; }
    bool IsFiring() const { return Firing; }
    bool IsUnEquipping() const { return Unequipping; }
};
struct AUTCharacter : UObject {
    AController* Controller = nullptr;
    UWorld* WorldValue = nullptr;
    AUTWeapon* Weapon = nullptr;
    AUTWeapon* PendingWeapon = nullptr;
    int NetMode = NM_Client, TauntCount = 0, Restarts = 0;
    bool Local = true, PlayerControlled = true, Dead = false, PendingKill = false;
    bool Disabled = false, Feigning = false, PendingFire[2] = { false, false };
    virtual void PawnClientRestart() { ++Restarts; }
    UWorld* GetWorld() const { return WorldValue; }
    int GetNetMode() const { return NetMode; }
    bool IsDead() const { return Dead; }
    bool IsPendingKillPending() const { return PendingKill; }
    bool IsLocallyControlled() const { return Local; }
    bool IsPlayerControlled() const { return PlayerControlled; }
    bool IsFiringDisabled() const { return Disabled; }
    bool IsFeigningDeath() const { return Feigning; }
    AUTWeapon* GetWeapon() const { return Weapon; }
    AUTWeapon* GetPendingWeapon() const { return PendingWeapon; }
    bool IsPendingFire(int mode) const { return PendingFire[mode]; }
};
struct FDeferredFireInput {
    int FireMode = 0; bool bStartFire = false;
    FDeferredFireInput() = default;
    FDeferredFireInput(int mode, bool start) : FireMode(mode), bStartFire(start) {}
};
struct Inputs { std::vector<FDeferredFireInput> Values; };
void* operator new(std::size_t, Inputs& inputs) {
    inputs.Values.emplace_back(); return &inputs.Values.back();
}
void operator delete(void*, Inputs&) {}
struct APlayerController : AController {
protected:
    // Deliberately restrict engine internals so pawn/weapon code cannot depend on them.
    AUTCharacter* AcknowledgedPawn = nullptr;
};
struct AUTPlayerController : APlayerController {
    AUTCharacter* Pawn = nullptr;
    AUTCharacter* UTCharacter = nullptr;
    UObject* Player = nullptr;
    bool Local = true, PendingKill = false, Ignored = false, Cursor = false;
    int State = NAME_Playing, Verified = 0;
    Inputs DeferredFireInputs;
private:
    bool bFirePressed = false, bAltFirePressed = false;
public:
    void FixtureAcknowledgePawn(AUTCharacter* pawn) { AcknowledgedPawn = pawn; }
    void SetHeld(bool primary, bool alternate) { bFirePressed = primary; bAltFirePressed = alternate; }
    bool IsLocalController() const { return Local; }
    bool IsPendingKillPending() const { return PendingKill; }
    AUTCharacter* GetPawn() const { return Pawn; }
    AUTCharacter* GetUTCharacter() const { return UTCharacter; }
    bool IsMoveInputIgnored() const { return Ignored; }
    bool ShouldShowMouseCursor() const { return Cursor; }
    bool IsInState(int state) const { return state == State; }
    bool HasDeferredFireInputs() const {
        for (const auto& input : DeferredFireInputs.Values) if (input.bStartFire) return true;
        return false;
    }
    void ClientVerifyFiringInputs() { ++Verified; ClientVerifyFiringInputs_Implementation(); }
    void ClientVerifyFiringInputs_Implementation();
};
struct ATeamArenaCharacter : AUTCharacter {
    using Super = AUTCharacter;
    TWeakObjectPtr<AUTPlayerController> SpawnHeldFireController;
    TWeakObjectPtr<UWorld> SpawnHeldFireWorld;
    TWeakObjectPtr<AUTWeapon> SpawnHeldFireWeapon;
    bool bSpawnHeldFireWeaponBound = false;
    FTimerHandle SpawnHeldFireHandle;
    double SpawnHeldFireDeadline = 0.0;
    void PawnClientRestart() override;
    void RetrySpawnHeldFire();
    void CancelSpawnHeldFire();
};
void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    UWorld World, OtherWorld;
    AUTGameState GS;
    DemoDriver Demo;
    Console TheConsole;
    UGameViewportClient Viewport;
    FViewport SceneViewport;
    UUTLocalPlayer LP;
    AUTPlayerController PC, OtherPC;
    ATeamArenaCharacter Pawn, OtherPawn;
    AUTWeapon Weapon, OtherWeapon;
    Fixture() {
        FPlatformTime::Now = 1.0;
        World.GS = &GS;
        Viewport.ViewportConsole = &TheConsole;
        Viewport.Viewport = &SceneViewport;
        LP.ViewportClient = &Viewport;
        PC.Player = &LP; PC.Pawn = &Pawn; PC.UTCharacter = &Pawn; PC.FixtureAcknowledgePawn(&Pawn);
        Pawn.WorldValue = &World; Pawn.Controller = &PC; Pawn.Weapon = &Weapon;
        Weapon.Owner = &Pawn;
        PC.SetHeld(true, false);
    }
    void Start() { Pawn.PawnClientRestart(); }
    void BeginClientRestart() { PC.State = NAME_Inactive; PC.FixtureAcknowledgePawn(nullptr); }
    void CompleteClientRestart() {
        // The engine acknowledges first, then enters Playing before the next tick.
        PC.FixtureAcknowledgePawn(&Pawn); PC.State = NAME_Playing;
    }
    void Tick() { World.Timers.Tick(); }
    int Starts(int mode) const {
        int result = 0;
        for (const auto& input : PC.DeferredFireInputs.Values)
            if (input.bStartFire && input.FireMode == mode) ++result;
        return result;
    }
};
'''


CASES = r'''
int main(int argc, char** argv) {
    Require(argc == 2, "case required");
    const std::string name(argv[1]);
    Fixture f;
    if (name == "held") {
        f.BeginClientRestart();
        f.Start();
        Require(f.Pawn.Restarts == 1 && f.PC.Verified == 0, "recovery ran inside PawnClientRestart");
        f.CompleteClientRestart(); f.Tick();
        Require(f.PC.Verified == 1 && f.Starts(0) == 1, "primary hold not recovered");
        Require(f.World.Timers.Empty(), "completed recovery kept polling");
        f.PC.DeferredFireInputs.Values.clear(); f.Tick();
        Require(f.PC.Verified == 1, "hold was recovered twice");
    } else if (name == "alternate") {
        f.PC.SetHeld(false, true); f.Start(); f.Tick();
        Require(f.Starts(0) == 0 && f.Starts(1) == 1, "alternate hold not recovered");
    } else if (name == "both") {
        f.PC.SetHeld(true, true); f.Start(); f.Tick();
        Require(f.Starts(0) == 1 && f.Starts(1) == 1, "stock dual-held ordering changed");
    } else if (name == "release") {
        f.Start(); f.PC.SetHeld(false, false); f.Tick();
        Require(f.Starts(0) == 0 && f.Starts(1) == 0, "released hold was replayed");
    } else if (name == "release_queue") {
        f.Start(); f.PC.SetHeld(false, true);
        f.PC.DeferredFireInputs.Values.emplace_back(0, false); f.Tick();
        Require(f.Starts(0) == 0 && f.Starts(1) == 1, "queued old release blocked current hold");
        Require(!f.PC.DeferredFireInputs.Values.front().bStartFire, "release ordering changed");
    } else if (name == "readiness") {
        f.BeginClientRestart(); f.Pawn.Weapon = nullptr;
        f.Start(); f.Tick();
        Require(f.PC.Verified == 0 && !f.World.Timers.Empty(), "missing possession was consumed");
        FPlatformTime::Now = 1.1; f.PC.FixtureAcknowledgePawn(&f.Pawn); f.Tick();
        Require(f.PC.Verified == 0, "acknowledgment bypassed Playing readiness");
        f.CompleteClientRestart(); f.Tick();
        Require(f.PC.Verified == 0, "missing weapon recovered");
        f.Pawn.Weapon = &f.Weapon; f.PC.UTCharacter = &f.OtherPawn; f.Tick();
        Require(f.PC.Verified == 0, "stale cached character received verifier input");
        f.PC.UTCharacter = &f.Pawn;
        f.Pawn.Weapon = &f.Weapon; f.Weapon.Owner = nullptr; f.Tick();
        Require(f.PC.Verified == 0, "unowned weapon recovered");
        f.Weapon.Owner = &f.Pawn; f.Weapon.HasState = false; f.Tick();
        Require(f.PC.Verified == 0, "uninitialized state recovered");
        f.Weapon.HasState = true; f.Weapon.State.Inactive = true; f.Tick();
        Require(f.PC.Verified == 0, "inactive weapon recovered");
        f.Weapon.State.Inactive = false; f.Tick();
        Require(f.Starts(0) == 1, "late ready weapon lost held input");
    } else if (name == "timeout") {
        f.Pawn.Weapon = nullptr; f.Start(); f.Tick();
        FPlatformTime::Now = 1.5; f.Pawn.Weapon = &f.Weapon; f.Tick();
        Require(f.PC.Verified == 0 && f.World.Timers.Empty(), "deadline depended on game time");
    } else if (name == "dedup") {
        f.Pawn.Weapon = nullptr; f.Start();
        FPlatformTime::Now = 1.3; f.Start();
        Require(f.World.Timers.Scheduled == 1 && f.Pawn.SpawnHeldFireDeadline == 1.5,
            "duplicate restart rearmed or extended recovery");
        f.Pawn.Weapon = &f.Weapon; f.Tick();
        f.PC.DeferredFireInputs.Values.clear(); f.Start(); f.Tick();
        Require(f.PC.Verified == 1, "completed restart recovered twice");
    } else if (name == "normal_input") {
        for (int gate = 0; gate < 6; ++gate) {
            Fixture n; n.Start();
            if (gate < 2) n.Pawn.PendingFire[gate] = true;
            if (gate == 2) n.Weapon.Firing = true;
            if (gate == 3) n.PC.DeferredFireInputs.Values.emplace_back(1, true);
            if (gate == 4) n.Pawn.PendingWeapon = &n.OtherWeapon;
            if (gate == 5) n.Weapon.Unequipping = true;
            n.Tick();
            Require(n.PC.Verified == 0 && n.World.Timers.Empty(), "normal input was duplicated");
        }
    } else if (name == "ownership") {
        for (int gate = 0; gate < 11; ++gate) {
            Fixture n; n.Start();
            if (gate == 0) n.Pawn.Controller = &n.OtherPC;
            if (gate == 1) n.PC.Pawn = &n.OtherPawn;
            if (gate == 2) n.PC.Local = false;
            if (gate == 3) n.Pawn.Local = false;
            if (gate == 4) n.Pawn.Dead = true;
            if (gate == 5) n.Pawn.PendingKill = true;
            if (gate == 6) n.PC.PendingKill = true;
            if (gate == 7) n.Pawn.WorldValue = &n.OtherWorld;
            if (gate == 8) n.Pawn.PlayerControlled = false;
            if (gate == 9) n.PC.Valid = false;
            if (gate == 10) n.Pawn.NetMode = NM_DedicatedServer;
            n.Tick();
            Require(n.PC.Verified == 0 && n.World.Timers.Empty(), "stale/nonlocal ownership recovered");
        }
    } else if (name == "blocked") {
        for (int gate = 0; gate < 18; ++gate) {
            Fixture n; n.Start();
            if (gate == 0) n.World.Paused = true;
            if (gate == 1) { n.World.DemoNetDriver = &n.Demo; n.Demo.Playback = true; }
            if (gate == 2) n.PC.Ignored = true;
            if (gate == 3) n.PC.Cursor = true;
            if (gate == 4) n.Pawn.Disabled = true;
            if (gate == 5) n.Pawn.TauntCount = 1;
            if (gate == 6) n.Pawn.Feigning = true;
            if (gate == 7) n.LP.Menus = true;
            if (gate == 8) n.LP.Chat = true;
            if (gate == 9) n.Viewport.Ignored = true;
            if (gate == 10) n.TheConsole.Open = true;
            if (gate == 11) n.GS.Blocked = true;
            if (gate == 12) n.PC.State = NAME_Spectating;
            if (gate == 13) n.PC.Player = nullptr;
            if (gate == 14) n.LP.ViewportClient = nullptr;
            if (gate == 15) n.Pawn.WorldValue = nullptr;
            if (gate == 16) n.Viewport.Viewport = nullptr;
            if (gate == 17) n.SceneViewport.Focus = false;
            n.Tick();
            Require(n.PC.Verified == 0 && n.World.Timers.Empty(), "blocked gameplay recovered or kept polling");
        }
    } else if (name == "weapon_lifetime") {
        for (int gate = 0; gate < 5; ++gate) {
            Fixture n;
            n.BeginClientRestart();
            n.Weapon.HasState = false;
            if (gate == 4) n.Pawn.Weapon = nullptr;
            n.Start(); n.Tick();
            if (gate == 4) { n.Pawn.Weapon = &n.Weapon; n.Tick(); }
            if (gate == 0 || gate == 4) { n.Pawn.Weapon = &n.OtherWeapon; n.OtherWeapon.Owner = &n.Pawn; }
            if (gate == 1) n.Pawn.Weapon = nullptr;
            if (gate == 2) n.Pawn.PendingWeapon = &n.OtherWeapon;
            if (gate == 3) n.Weapon.Valid = false;
            n.Tick();
            Require(n.PC.Verified == 0 && n.World.Timers.Empty(), "replacement/switch did not cancel before readiness");
            n.CompleteClientRestart(); n.Weapon.HasState = true; n.Tick();
            Require(n.PC.Verified == 0, "canceled weapon recovered later");
        }
    } else if (name == "qualified_localplayer") {
        for (bool menuOpen : {false, true}) {
            Fixture n; FVirtualMenuTrapLocalPlayer retailPlayer;
            retailPlayer.ViewportClient = &n.Viewport; retailPlayer.Menus = menuOpen;
            n.PC.Player = &retailPlayer; n.Start(); n.Tick();
            Require(n.Starts(0) == (menuOpen ? 0 : 1), "qualified menu check did not use base state");
            Require(retailPlayer.VirtualMenuCalls == 0, "menu guard used a retail virtual slot");
        }
    } else if (name == "admission") {
        for (int gate = 0; gate < 7; ++gate) {
            Fixture n;
            if (gate == 0) n.Pawn.WorldValue = nullptr;
            if (gate == 1) n.Pawn.NetMode = NM_DedicatedServer;
            if (gate == 2) n.Pawn.Controller = nullptr;
            if (gate == 3) n.PC.Local = false;
            if (gate == 4) n.PC.Pawn = nullptr;
            if (gate == 5) n.Pawn.Dead = true;
            if (gate == 6) n.Pawn.PendingKill = true;
            n.Start();
            Require(n.Pawn.Restarts == 1 && n.World.Timers.Empty(), "bad restart admission or skipped Super");
        }
    } else if (name == "cancel") {
        f.Start(); f.Pawn.CancelSpawnHeldFire(); f.Tick();
        Require(f.PC.Verified == 0 && f.World.Timers.Empty(), "uncancelable next-tick callback resurrected input");
    } else Require(false, "unknown case");
}
'''


class SpawnHeldFireNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-spawn-held-fire-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        native = (PLUGIN / "Source/Private/NCSpawnFireInput.cpp").read_text(encoding="utf-8-sig")
        stock = (PLUGIN.parents[1] / "Source/UnrealTournament/Private/UTPlayerController.cpp").read_text(encoding="utf-8-sig")
        signatures = ("void ATeamArenaCharacter::PawnClientRestart", "void ATeamArenaCharacter::CancelSpawnHeldFire",
                      "void ATeamArenaCharacter::RetrySpawnHeldFire")
        source = directory / "spawn_held_fire.cpp"
        source.write_text("\n".join([ADAPTER,
            native_function(stock, "void AUTPlayerController::ClientVerifyFiringInputs_Implementation")]
            + [native_function(native, signature) for signature in signatures] + [CASES]), encoding="utf-8")
        cls.executable = directory / ("spawn_held_fire.exe" if os.name == "nt" else "spawn_held_fire")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'spawn_held_fire.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic", str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Spawn held-fire adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_held_primary_recovers_after_restart_returns(self): self.run_case("held")
    def test_held_alternate_uses_stock_verifier(self): self.run_case("alternate")
    def test_dual_hold_retains_stock_order(self): self.run_case("both")
    def test_release_before_callback_does_not_fire(self): self.run_case("release")
    def test_old_queued_release_preserves_current_hold_order(self): self.run_case("release_queue")
    def test_possession_and_weapon_readiness_can_arrive_late(self): self.run_case("readiness")
    def test_half_second_deadline_uses_real_time(self): self.run_case("timeout")
    def test_repeated_restarts_cannot_rearm_or_extend_deadline(self): self.run_case("dedup")
    def test_pending_queued_firing_or_switching_input_is_not_duplicated(self): self.run_case("normal_input")
    def test_recovery_cannot_cross_ownership_life_or_world(self): self.run_case("ownership")
    def test_menu_pause_and_gameplay_vetoes_cancel(self): self.run_case("blocked")
    def test_localplayer_menu_guard_bypasses_virtual_override(self): self.run_case("qualified_localplayer")
    def test_weapon_lifetime_is_bound_before_readiness(self): self.run_case("weapon_lifetime")
    def test_nonlocal_invalid_or_dead_restarts_cannot_arm(self): self.run_case("admission")
    def test_canceled_next_tick_callback_cannot_rearm(self): self.run_case("cancel")

    def test_character_lifecycle_cancels_recovery(self):
        source = (PLUGIN / "Source/Private/TeamArenaCharacter.cpp").read_text(encoding="utf-8-sig")
        for method in ("EndPlay", "PlayDying", "Destroyed"):
            body = native_function(source, f"void ATeamArenaCharacter::{method}")
            self.assertLess(body.index("CancelSpawnHeldFire();"), body.index(f"Super::{method}"))


if __name__ == "__main__":
    unittest.main()
