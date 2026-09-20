"""Run Wipeout's actual spawn-state repair and spectate guards natively.

The adapters model controller/pawn ownership and observable state transitions;
they do not reimplement the repair. Full replication and Slate input focus still
need a client/server reconnect playtest.
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
#include <set>
#include <string>
#define UE_LOG(...) do {} while (false)
constexpr int NM_Standalone = 0;
constexpr int NM_Client = 3;
const std::string NAME_Playing = "Playing";
const std::string NAME_Spectating = "Spectating";
const std::string NAME_Inactive = "Inactive";
struct UObject { virtual ~UObject() = default; };
template<class T, class U> T* Cast(U* value) { return dynamic_cast<T*>(value); }
struct AActor : UObject {
    bool PendingKill = false;
    bool bTearOff = false;
    int NetUpdates = 0;
    bool IsPendingKill() const { return PendingKill; }
    bool IsPendingKillPending() const { return PendingKill; }
    void ForceNetUpdate() { ++NetUpdates; }
};
struct AController;
struct APlayerState : AActor {};
struct APawn : AActor {
    AController* Controller = nullptr;
    APlayerState* PlayerState = nullptr;
    AController* GetController() const { return Controller; }
};
struct AUTCharacter : APawn {
    bool Dead = false;
    int Health = 125, ArmorAmount = 100, Inventory = 10;
    bool IsDead() const { return Dead; }
};
struct TeamInfo {};
struct AUTPlayerState : APlayerState {
    TeamInfo* Team = nullptr;
    bool bOnlySpectator = false, bIsInactive = false, bOutOfLives = false;
    float RespawnTime = 0.f, RespawnWaitTime = 0.f;
    int Kills = 1, Deaths = 3;
    AActor* Owner = nullptr;
    AUTCharacter* Character = nullptr;
    AActor* GetOwner() const { return Owner; }
    AUTCharacter* GetUTCharacter() const { return Character; }
    void SetOutOfLives(bool value) { bOutOfLives = value; ForceNetUpdate(); }
};
struct AController : AActor {
    APlayerState* PlayerState = nullptr;
    APawn* Pawn = nullptr;
    APawn* GetPawn() const { return Pawn; }
};
struct AUTPlayerController : AController {
    std::string State = NAME_Playing;
    AActor* ViewTarget = nullptr;
    APawn* RestartPawn = nullptr;
    int StateChanges = 0, ClientStates = 0, Restarts = 0;
    int ViewChanges = 0, ClientViews = 0, BehindViews = 0, SelfViews = 0;
    bool bSpectateBehindView = true;
    bool IsInState(const std::string& value) const { return State == value; }
    void ChangeState(const std::string& value) { State = value; ++StateChanges; }
    void ClientGotoState(const std::string&) { ++ClientStates; }
    void ClientRestart(APawn* pawn) { RestartPawn = pawn; ++Restarts; }
    void SetViewTarget(AActor* target) { ViewTarget = target; ++ViewChanges; }
    void ClientSetViewTarget(AActor* target) { ViewTarget = target; ++ClientViews; }
    void BehindView(bool value) { bSpectateBehindView = value; ++BehindViews; }
    void ServerViewSelf() { ViewTarget = this; ++SelfViews; }
};
struct PlayerSet {
    std::set<AUTPlayerState*> Values;
    bool Contains(AUTPlayerState* player) const { return Values.count(player) != 0; }
};
struct AUWipeoutGame {
    int NetMode = NM_Standalone;
    bool Authority = true, useBPSpecFunction = false;
    int BlueprintSpectates = 0;
    PlayerSet PendingRespawns, RoundEliminatedPlayers;
    AUTPlayerState* Teammate = nullptr;
    AUTPlayerState* Enemy = nullptr;
    int GetNetMode() const { return NetMode; }
    bool HasAuthority() const { return Authority; }
    bool IsPlayerWaitingToRespawn(AUTPlayerState* ps) const { return PendingRespawns.Contains(ps); }
    void BP_SpectatePSImplementation(AUTPlayerState*) { ++BlueprintSpectates; }
    AUTPlayerState* FindAliveTeammate(AUTPlayerState*) const { return Teammate; }
    AUTPlayerState* FindAliveEnemy(AUTPlayerState*) const { return Enemy; }
    bool RestoreLivePlayerState(AController* controller);
    void ForceTeamSpectate(AUTPlayerState* player);
};
'''


CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    AUWipeoutGame Game;
    AUTPlayerController PC, OtherPC;
    AUTPlayerState PS, OtherPS;
    AUTCharacter Pawn;
    TeamInfo Team;
    Fixture() {
        PC.PlayerState = &PS; PC.Pawn = &Pawn;
        PS.Owner = &PC; PS.Character = &Pawn; PS.Team = &Team;
        Pawn.Controller = &PC; Pawn.PlayerState = &PS;
    }
};
void UnchangedPawn(const Fixture& f) {
    Require(f.PC.GetPawn() == &f.Pawn && f.Pawn.Controller == &f.PC &&
            f.Pawn.PlayerState == &f.PS, "repair must not change pawn ownership");
    Require(f.Pawn.Health == 125 && f.Pawn.ArmorAmount == 100 && f.Pawn.Inventory == 10,
            "repair modified health, armor or inventory");
    Require(f.PS.Kills == 1 && f.PS.Deaths == 3, "reconnect combat history erased");
}
void NoActions(const Fixture& f) {
    Require(f.PS.NetUpdates == 0 && f.PC.StateChanges == 0 && f.PC.ClientStates == 0 &&
            f.PC.Restarts == 0 && f.PC.ViewChanges == 0 && f.PC.ClientViews == 0 &&
            f.PC.BehindViews == 0 && f.PC.SelfViews == 0 && f.Game.BlueprintSpectates == 0,
            "rejected/no-op path changed state or emitted a client action");
}
void Reconnect() {
    Fixture f;
    f.PS.bOutOfLives = true; f.PS.RespawnTime = 7.f; f.PS.RespawnWaitTime = 8.f;
    f.PC.State = NAME_Spectating;
    Require(f.Game.RestoreLivePlayerState(&f.PC), "eligible healthy reconnect was not repaired");
    Require(!f.PS.bOutOfLives && f.PS.RespawnTime == 0.f && f.PS.RespawnWaitTime == 0.f,
            "restored reconnect retained death/spectator state");
    Require(f.PS.NetUpdates == 1, "changed PlayerState must request one replication update");
    Require(f.PC.State == NAME_Playing && f.PC.Restarts == 1 && f.PC.RestartPawn == &f.Pawn,
            "repair must restart the existing pawn once");
    Require(f.PC.ViewTarget == &f.Pawn && f.PC.ClientViews == 1,
            "BP controllers with automatic camera management disabled need their pawn view restored");
    UnchangedPawn(f);
    const int stateChanges = f.PC.StateChanges, viewChanges = f.PC.ViewChanges;
    Require(!f.Game.RestoreLivePlayerState(&f.PC), "healthy repeated check must be idempotent");
    Require(f.PS.NetUpdates == 1 && f.PC.Restarts == 1 && f.PC.ClientViews == 1 &&
            f.PC.StateChanges == stateChanges && f.PC.ViewChanges == viewChanges,
            "repeated check spammed restart/view/replication");
}
void AlreadyHealthy() {
    Fixture f;
    Require(!f.Game.RestoreLivePlayerState(&f.PC), "already healthy state incorrectly repaired");
    NoActions(f); UnchangedPawn(f);
}
void PartialState() {
    for (int variant = 0; variant < 4; ++variant) {
        Fixture f;
        if (variant == 0) f.PS.RespawnTime = 2.f;
        if (variant == 1) f.PS.RespawnWaitTime = 2.f;
        if (variant == 2) f.PC.State = NAME_Inactive;
        if (variant == 3) f.PS.bOutOfLives = true;
        Require(f.Game.RestoreLivePlayerState(&f.PC), "partial stale state was missed");
        Require(!f.PS.bOutOfLives && f.PS.RespawnTime == 0.f && f.PS.RespawnWaitTime == 0.f &&
                f.PC.State == NAME_Playing && f.PC.Restarts == 1, "partial state did not normalize");
        Require(f.PS.NetUpdates == (variant == 2 ? 0 : 1),
                "unchanged PS should not need a forced update");
        UnchangedPawn(f);
    }
}
void RejectedRestore() {
    for (int variant = 0; variant < 21; ++variant) {
        Fixture f; APawn otherPawn; APlayerState nonUTPS;
        f.PS.bOutOfLives = true; f.PS.RespawnTime = 4.f; f.PS.RespawnWaitTime = 6.f;
        AController* controller = &f.PC;
        if (variant == 0) controller = nullptr;
        if (variant == 1) f.Game.NetMode = NM_Client;
        if (variant == 2) f.PC.PlayerState = nullptr;
        if (variant == 3) f.PC.PlayerState = &nonUTPS;
        if (variant == 4) f.PS.Team = nullptr;
        if (variant == 5) f.PS.bOnlySpectator = true;
        if (variant == 6) f.PS.bIsInactive = true;
        if (variant == 7) f.Game.PendingRespawns.Values.insert(&f.PS);
        if (variant == 8) f.Game.RoundEliminatedPlayers.Values.insert(&f.PS);
        if (variant == 9) f.PC.Pawn = nullptr;
        if (variant == 10) f.PC.Pawn = &otherPawn;
        if (variant == 11) f.Pawn.Dead = true;
        if (variant == 12) f.Pawn.Health = 0;
        if (variant == 13) f.Pawn.PendingKill = true;
        if (variant == 14) f.Pawn.bTearOff = true;
        if (variant == 15) f.Pawn.Controller = &f.OtherPC;
        if (variant == 16) f.Pawn.PlayerState = &f.OtherPS;
        if (variant == 17) f.Pawn.Health = -1;
        if (variant == 18) f.PC.PendingKill = true;
        if (variant == 19) f.PS.PendingKill = true;
        if (variant == 20) f.PS.Owner = &f.OtherPC;
        Require(!f.Game.RestoreLivePlayerState(controller), "ineligible player repaired");
        Require(f.PS.bOutOfLives && f.PS.RespawnTime == 4.f && f.PS.RespawnWaitTime == 6.f,
                "dead/spectator/queued/eliminated player lost its state");
        NoActions(f);
    }
}
void LiveSpectateGuard() {
    for (bool blueprint : {false, true}) {
        for (bool staleFlag : {false, true}) {
            Fixture f; f.Game.useBPSpecFunction = blueprint; f.PS.bOutOfLives = staleFlag;
            f.Game.ForceTeamSpectate(&f.PS);
            NoActions(f); UnchangedPawn(f);
            Require(f.PS.bOutOfLives == staleFlag, "spectate guard should not itself rewrite life state");
        }
    }
    Fixture f; f.PC.Pawn = nullptr;
    f.Game.ForceTeamSpectate(nullptr);
    f.Game.ForceTeamSpectate(&f.PS);
    NoActions(f);
}
void DeadSpectatePaths() {
    for (int target = 0; target < 3; ++target) {
        Fixture f; AUTCharacter targetPawn; AUTPlayerState targetPS;
        f.PS.bOutOfLives = true; f.Pawn.Dead = true; f.Pawn.Health = 0;
        targetPS.Character = &targetPawn;
        if (target == 0) f.Game.Teammate = &targetPS;
        if (target == 1) f.Game.Enemy = &targetPS;
        f.Game.ForceTeamSpectate(&f.PS);
        Require(f.PC.State == NAME_Spectating && f.PC.ClientStates == 1,
                "genuinely dead player's native spectate transition was blocked");
        Require(f.PS.bOutOfLives && f.PC.Restarts == 0, "spectating resurrected dead player");
        if (target < 2) Require(f.PC.ViewTarget == &targetPawn && !f.PC.bSpectateBehindView,
                              "dead player's teammate/enemy view changed");
        else Require(f.PC.SelfViews == 1, "no-target dead spectator lost self view fallback");
    }
    Fixture f;
    f.Game.useBPSpecFunction = true; f.PS.bOutOfLives = true; f.PC.Pawn = nullptr;
    f.Game.ForceTeamSpectate(&f.PS);
    Require(f.Game.BlueprintSpectates == 1 && f.PC.StateChanges == 0 && f.PC.Restarts == 0,
            "valid dead/pawnless player must still reach configured BP spectate callback");
}
int main(int argc, char** argv) {
    Require(argc == 2, "one case required"); const std::string name(argv[1]);
    if (name == "reconnect") Reconnect();
    else if (name == "healthy") AlreadyHealthy();
    else if (name == "partial") PartialState();
    else if (name == "rejected") RejectedRestore();
    else if (name == "live_spectate") LiveSpectateGuard();
    else if (name == "dead_spectate") DeadSpectatePaths();
    else Require(false, "unknown case");
}
'''


class WipeoutSpawnStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-wipeout-spawn-state-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        game = (PLUGIN / "Source/Private/WipeoutGame.cpp").read_text(encoding="utf-8-sig")
        code = "\n".join((
            ADAPTER,
            native_function(game, "bool AUWipeoutGame::RestoreLivePlayerState"),
            native_function(game, "void AUWipeoutGame::ForceTeamSpectate"),
            CASES,
        ))
        source = directory / "spawn_state.cpp"
        source.write_text(code, encoding="utf-8")
        cls.executable = directory / ("spawn_state.exe" if os.name == "nt" else "spawn_state")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'spawn_state.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Spawn-state adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_crash_reconnect_restores_life_and_existing_pawn_once(self):
        self.run_case("reconnect")

    def test_healthy_player_has_no_restart_or_replication_churn(self):
        self.run_case("healthy")

    def test_individual_stale_life_countdown_and_controller_states(self):
        self.run_case("partial")

    def test_dead_missing_mismatched_spectator_queued_and_eliminated_guards(self):
        self.run_case("rejected")

    def test_live_pawn_cannot_reach_native_or_blueprint_spectating(self):
        self.run_case("live_spectate")

    def test_genuinely_dead_players_keep_native_and_blueprint_spectating(self):
        self.run_case("dead_spectate")


if __name__ == "__main__":
    unittest.main()
