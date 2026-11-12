"""Compile retired gate admission plus the real weapon readiness wrapper.

Engine ownership, UHT, timers and mixed-binary play remain runtime checks.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_wipeout_healing import find_compiler, native_function

PLUGIN = Path(os.environ.get("NCP_TEST_SOURCE", Path(__file__).resolve().parents[2]))

ADAPTER = r'''
#include <cassert>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <tuple>
#include <vector>
using int32 = int32_t;
struct UObject {
    bool Alive = true;
    int Serial = 1;
    virtual ~UObject() = default;
};
template<class T> struct TWeakObjectPtr {
    T* Pointer = nullptr;
    int Serial = 0;
    TWeakObjectPtr() = default;
    TWeakObjectPtr(T* p) : Pointer(p), Serial(p ? p->Serial : 0) {}
    T* Get() const { return Pointer && Pointer->Alive && Pointer->Serial == Serial ? Pointer : nullptr; }
    bool IsValid() const { return Get() != nullptr; }
    T* operator->() const { return Get(); }
    bool operator<(const TWeakObjectPtr& other) const {
        return std::tie(Pointer, Serial) < std::tie(other.Pointer, other.Serial);
    }
};
template<class K, class V> struct TMap {
    std::map<K, V> Values;
    V* Find(const K& key) { auto i=Values.find(key); return i==Values.end() ? nullptr : &i->second; }
    void Add(const K& key, const V& value) { Values[key]=value; }
    void Remove(const K& key) { Values.erase(key); }
    void Empty() { Values.clear(); }
    struct Iterator {
        TMap& Owner;
        typename std::map<K,V>::iterator It;
        bool Removed = false;
        explicit Iterator(TMap& owner) : Owner(owner), It(owner.Values.begin()) {}
        explicit operator bool() const { return It != Owner.Values.end(); }
        const K& Key() const { return It->first; }
        V& Value() { return It->second; }
        void RemoveCurrent() { It = Owner.Values.erase(It); Removed = true; }
        void operator++() { if (!Removed) ++It; Removed = false; }
    };
    Iterator CreateIterator() { return Iterator(*this); }
};
enum { USOCK_Open = 1, USOCK_Closed = 2 };
struct UNetConnection : UObject { int State = USOCK_Open; };
struct AGameModeBase : UObject {};
struct AUTBaseGameMode : AGameModeBase {
    bool Lobby = false;
    bool IsLobbyServer() const { return Lobby; }
};
struct FTransform { static const FTransform Identity; };
const FTransform FTransform::Identity;
enum class ESpawnActorCollisionHandlingMethod { AlwaysSpawn };
struct APlayerController;
struct UWorld : UObject {
    AGameModeBase* Mode = nullptr;
    int SpawnCount = 0;
    bool FailSpawn = false;
    AGameModeBase* GetAuthGameMode() const { return Mode; }
    template<class T> T* SpawnActorDeferred(int, FTransform, APlayerController*, void*, ESpawnActorCollisionHandlingMethod);
};
struct AActor : UObject {
    UWorld* World = nullptr;
    AActor* Owner = nullptr;
    AActor* GetOwner() const { return Owner; }
};
struct AController : AActor {};
struct APlayerState { bool bIsABot = false; };
struct APlayerController : AController {
    bool Authority = true;
    bool Local = false;
    bool PendingKill = false;
    UNetConnection* Connection = nullptr;
    APlayerState* PlayerState = nullptr;
    bool HasAuthority() const { return Authority; }
    bool IsPendingKillPending() const { return PendingKill; }
    bool IsLocalController() const { return Local; }
    UWorld* GetWorld() const { return World; }
    UNetConnection* GetNetConnection() const { return Connection; }
};
struct ANCVersionGate : AActor {
    bool bAdvisorMode = false;
    int DestroyCount = 0;
    static int StaticClass() { return 1; }
    void Destroy() { ++DestroyCount; Alive=false; }
    void FinishSpawning(FTransform) {}
};
std::vector<std::unique_ptr<ANCVersionGate>> AllocatedGates;
template<class T> T* UWorld::SpawnActorDeferred(int, FTransform, APlayerController* pc, void*, ESpawnActorCollisionHandlingMethod) {
    ++SpawnCount;
    if (FailSpawn) return nullptr;
    auto gate = std::unique_ptr<T>(new T());
    gate->World = this;
    gate->Owner = pc;
    T* result = gate.get();
    AllocatedGates.emplace_back(std::move(gate));
    return result;
}
template<class T, class U> T* Cast(U* source) { return dynamic_cast<T*>(source); }
enum { ROLE_Authority = 3 };
struct AUTBot : AController {};
struct Pawn { AController* Controller = nullptr; };
struct AUTWeaponFix {
    int Role = ROLE_Authority;
    Pawn* UTOwner = nullptr;
    TWeakObjectPtr<AController> FireProtocolController;
    bool Is329FireProtocolReady() const;
};
'''

CASES = r'''
static int Passed = 0;
#define CHECK(value) do { if (!(value)) { std::cerr << "FAILED at " << __LINE__ << ": " #value "\n"; return 1; } ++Passed; } while(0)
int main() {
    UWorld world, nextWorld;
    UNetConnection connection, nextConnection;
    APlayerController pc;
    APlayerState state;
    pc.PlayerState = &state; pc.World = &world; pc.Connection = &connection;
    Pawn pawn; pawn.Controller = &pc;
    AUTWeaponFix weapon; weapon.UTOwner = &pawn;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(nullptr));
    CHECK(NCPlusVersionGate::IsProtocolConfirmed(&pc)); // No report or registry entry.
    CHECK(weapon.Is329FireProtocolReady());
    NCPlusVersionGate::SpawnFor(&pc);
    NCPlusVersionGate::SpawnFor(&pc);
    NCPlusVersionGate::SpawnAdvisorFor(&pc);
    CHECK(world.SpawnCount == 0 && AllocatedGates.empty());
    CHECK(weapon.Is329FireProtocolReady());
    pc.Connection = nullptr; CHECK(!weapon.Is329FireProtocolReady());
    pc.Connection = &connection; connection.State = USOCK_Closed;
    CHECK(!weapon.Is329FireProtocolReady()); connection.State = USOCK_Open;
    connection.Alive = false; CHECK(!weapon.Is329FireProtocolReady()); connection.Alive = true;
    pc.Authority = false; CHECK(!weapon.Is329FireProtocolReady()); pc.Authority = true;
    pc.PendingKill = true; CHECK(!weapon.Is329FireProtocolReady()); pc.PendingKill = false;
    pc.World = nullptr; CHECK(!weapon.Is329FireProtocolReady()); pc.World = &world;
    world.Alive = false; CHECK(!weapon.Is329FireProtocolReady()); world.Alive = true;
    pc.World = &nextWorld; CHECK(weapon.Is329FireProtocolReady()); // Travel needs no handshake.
    pc.Connection = &nextConnection; CHECK(weapon.Is329FireProtocolReady()); // Reconnect.
    nextConnection.State = USOCK_Closed; CHECK(!weapon.Is329FireProtocolReady());
    pc.Local = true; pc.Connection = nullptr; CHECK(weapon.Is329FireProtocolReady()); pc.Local = false;
    state.bIsABot = true; CHECK(NCPlusVersionGate::IsProtocolConfirmed(&pc)); state.bIsABot = false;
    AUTBot bot; pawn.Controller = &bot; CHECK(weapon.Is329FireProtocolReady());
    pawn.Controller = nullptr; CHECK(!weapon.Is329FireProtocolReady());
    pawn.Controller = &pc; pc.Connection = &connection;
    weapon.UTOwner = nullptr; weapon.FireProtocolController = &pc;
    CHECK(weapon.Is329FireProtocolReady()); // Existing trade-grace controller fallback.
    connection.State = USOCK_Closed; CHECK(!weapon.Is329FireProtocolReady()); connection.State = USOCK_Open;
    pc.Alive = false; CHECK(!weapon.Is329FireProtocolReady()); pc.Alive = true;
    ++pc.Serial; CHECK(!weapon.Is329FireProtocolReady()); // Cached weak identity is stale.
    weapon.UTOwner = &pawn; weapon.Role = 1; CHECK(!weapon.Is329FireProtocolReady());
    CHECK(world.SpawnCount == 0 && nextWorld.SpawnCount == 0 && AllocatedGates.empty());
    std::cout << "PASS " << Passed << " retired gate boundary checks\n";
}
'''


class Version329GateTests(unittest.TestCase):
    def test_retired_gate_live_connection_and_weapon_readiness(self):
        source = (PLUGIN / "Source/Private/NCPlusVersionGate.cpp").read_text(encoding="utf-8-sig")
        names = ("static bool IsExempt", "void SpawnFor(", "void SpawnAdvisorFor(", "bool IsProtocolConfirmed")
        code = ADAPTER + "\nnamespace NCPlusVersionGate {\n"
        code += "\n".join(native_function(source, name) for name in names) + "\n}\n"
        weapon = (PLUGIN / "Source/Private/UTWeaponFix.cpp").read_text(encoding="utf-8-sig")
        code += native_function(weapon, "bool AUTWeaponFix::Is329FireProtocolReady") + "\n" + CASES
        compiler, environment, msvc = find_compiler()
        with tempfile.TemporaryDirectory(prefix="ncp-329-version-gate-") as temporary:
            directory = Path(temporary)
            unit = directory / "version_gate.cpp"
            unit.write_text(code, encoding="utf-8")
            executable = directory / ("version_gate.exe" if os.name == "nt" else "version_gate")
            if msvc:
                command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(unit), f"/Fe{executable}"]
            else:
                command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic", str(unit), "-o", str(executable)]
            built = subprocess.run(command, cwd=directory, env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = subprocess.run([str(executable)], cwd=directory, env=environment, capture_output=True, text=True, timeout=60)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn("retired gate boundary checks", run.stdout)

    def test_version_and_existing_gate_wire_contract(self):
        public = (PLUGIN / "Source/Public/NCPlusVersionGate.h").read_text(encoding="utf-8-sig")
        source = (PLUGIN / "Source/Private/NCPlusVersionGate.cpp").read_text(encoding="utf-8-sig")
        version = (PLUGIN / "Source/Public/NetcodePlus.h").read_text(encoding="utf-8-sig")
        self.assertRegex(version, r"#define\s+NETCODE_PLUGIN_VERSION\s+329\b")
        self.assertEqual(re.findall(r"UFUNCTION\([^)]*\)\s*void\s+(\w+)\(([^)]*)\)", public),
                         [("ServerReportVersion", "int32 ClientVersion")])
        self.assertEqual(re.findall(r"UPROPERTY\([^)]*\)\s*(\w+)\s+(\w+)", public), [("bool", "bConfirmed")])
        self.assertNotIn("OwnerClientHasMoved", source)
        self.assertNotIn("OwnerIsPawnlessByDesign", source)
        self.assertNotIn("OnMoveWatch", public + source)
        end_play = native_function(source, "void ANCVersionGate::EndPlay")
        self.assertIn("ClearTimer(TimeoutHandle)", end_play)
        self.assertIn("ClearTimer(KickHandle)", end_play)
        self.assertIn("FWorldDelegates::OnWorldCleanup", source)
        self.assertIn("FGameModeEvents::GameModeLogoutEvent", source)
        self.assertNotIn("GuaranteedKick", source)
        self.assertNotIn("SpawnActorDeferred", source)
        query = native_function(source, "bool IsProtocolConfirmed")
        self.assertNotIn("ProtocolSessions", query)
        self.assertNotIn("SpawnFor", query)
        for name in ("void SpawnFor(", "void SpawnAdvisorFor("):
            self.assertNotIn("SpawnGate", native_function(source, name))


if __name__ == "__main__":
    unittest.main()
