"""Compile the 329 version gate's actual identity/registry/admission code.

The adapter covers weak-object identity, same-controller travel, connection
replacement, mismatch terminality, hub isolation, and unknown/pending denial.
It does not validate UE actor replication, UHT layout, timeout delivery, or an
old client's RPC decoding; the documented mixed-version playtest remains needed.
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
void EnsureProtocolLifecycle() {}
'''

CASES = r'''
static int Passed = 0;
#define CHECK(value) do { if (!(value)) { std::cerr << "FAILED at " << __LINE__ << ": " #value "\n"; return 1; } ++Passed; } while(0)
int main() {
    AUTBaseGameMode match;
    AUTBaseGameMode lobby;
    lobby.Lobby = true;
    UWorld world, nextWorld, hub;
    world.Mode = &match;
    nextWorld.Mode = &match;
    hub.Mode = &lobby;
    UNetConnection connection, nextConnection;
    APlayerController pc;
    APlayerState state;
    pc.PlayerState = &state;
    pc.World = &world;
    pc.Connection = &connection;
    auto entry = [&]() { return ProtocolSessions.Find(TWeakObjectPtr<APlayerController>(&pc)); };
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(nullptr));
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    CHECK(world.SpawnCount == 1 && entry() && entry()->State == EProtocolState::Pending);
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc) && world.SpawnCount == 1);
    ANCVersionGate* gate = entry()->Gate.Get();
    CHECK(GateOwnsSession(gate));
    CHECK(RecordVersion(*entry(), &pc, gate, 329));
    gate->Destroy();
    CHECK(NCPlusVersionGate::IsProtocolConfirmed(&pc)); // Confirmation outlives gate actor.
    CHECK(!RecordVersion(*entry(), &pc, gate, 328)); // Report outcome is terminal.
    pc.World = &nextWorld;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    CHECK(nextWorld.SpawnCount == 1);
    CHECK(!GateOwnsSession(gate));
    CHECK(!RecordVersion(*entry(), &pc, gate, 329)); // Old world report cannot approve.
    gate = entry()->Gate.Get();
    CHECK(RecordVersion(*entry(), &pc, gate, 328));
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    CHECK(nextWorld.SpawnCount == 1); // Wrong version cannot restart its timeout.
    CHECK(!RecordVersion(*entry(), &pc, gate, 329)); // Can't undo mismatch in place.
    pc.Connection = &nextConnection;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    CHECK(nextWorld.SpawnCount == 2);
    CHECK(!GateOwnsSession(gate));
    gate = entry()->Gate.Get();
    CHECK(RecordVersion(*entry(), &pc, gate, 329));
    CHECK(NCPlusVersionGate::IsProtocolConfirmed(&pc));
    pc.Connection = nullptr;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc) && !entry());
    pc.Connection = &nextConnection;
    pc.World = &hub;
    NCPlusVersionGate::SpawnAdvisorFor(&pc);
    CHECK(entry() && entry()->bAdvisor);
    gate = entry()->Gate.Get();
    CHECK(RecordVersion(*entry(), &pc, gate, 329));
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc) && hub.SpawnCount == 1);
    hub.Mode = &match; // Even same-world advisor -> match needs a new report.
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    CHECK(hub.SpawnCount == 2 && !entry()->bAdvisor);
    CHECK(!RecordVersion(*entry(), &pc, gate, 329));
    gate = entry()->Gate.Get();
    CHECK(RecordVersion(*entry(), &pc, gate, 329));
    CHECK(NCPlusVersionGate::IsProtocolConfirmed(&pc));
    CleanupProtocolWorld(&hub, false, false);
    CHECK(!entry());
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    gate = entry()->Gate.Get();
    OnProtocolLogout(&match, &pc);
    CHECK(!entry() && !gate->Alive);
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    gate = entry()->Gate.Get();
    gate->Destroy(); // Unexpected loss of pending gate must retry.
    const int BeforeRespawn = hub.SpawnCount;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    CHECK(hub.SpawnCount == BeforeRespawn + 1 && entry()->Gate.Get() != gate);
    gate = entry()->Gate.Get();
    CHECK(RecordVersion(*entry(), &pc, gate, 329));
    pc.Authority = false;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    pc.Local = true;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc)); // Client cannot exempt itself.
    pc.Authority = true;
    CHECK(NCPlusVersionGate::IsProtocolConfirmed(&pc));
    pc.Local = false;
    state.bIsABot = true;
    CHECK(NCPlusVersionGate::IsProtocolConfirmed(&pc));
    state.bIsABot = false;
    pc.PendingKill = true;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    pc.PendingKill = false;
    ++pc.Serial; // Same address, different weak identity = reconnect/new UObject.
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    CHECK(entry() && entry()->State == EProtocolState::Pending);
    nextConnection.Alive = false;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    PruneProtocolSessions();
    CHECK(!entry());
    nextConnection.Alive = true;
    hub.FailSpawn = true;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc) && !entry());
    hub.FailSpawn = false;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc) && entry());
    CHECK(RecordVersion(*entry(), &pc, entry()->Gate.Get(), 329));
    CHECK(NCPlusVersionGate::IsProtocolConfirmed(&pc));
    nextConnection.State = USOCK_Closed;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc) && !entry());
    nextConnection.State = USOCK_Open;
    CHECK(!NCPlusVersionGate::IsProtocolConfirmed(&pc));
    std::cout << "PASS " << Passed << " protocol boundary checks\n";
}
'''


class Version329GateTests(unittest.TestCase):
    def test_exact_gate_identity_registry_and_travel(self):
        source = (PLUGIN / "Source/Private/NCPlusVersionGate.cpp").read_text(encoding="utf-8-sig")
        definitions = source[source.index("    enum class EProtocolState"):source.index("    FDelegateHandle ProtocolCleanupHandle;")]
        names = ("bool SessionIdentityMatches", "bool IsConfirmedSession", "bool RecordVersion",
                 "void PruneProtocolSessions", "bool GateOwnsSession", "void CleanupProtocolWorld",
                 "void OnProtocolLogout")
        code = ADAPTER + "\n#define NETCODE_PLUGIN_VERSION 329\n" + definitions
        code += "\n".join(native_function(source, name) for name in names)
        code += "\nnamespace NCPlusVersionGate {\n"
        names = ("static bool IsExempt", "static void SpawnGate", "void SpawnFor(",
                 "void SpawnAdvisorFor(", "bool IsProtocolConfirmed")
        code += "\n".join(native_function(source, name) for name in names) + "\n}\n" + CASES
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
            self.assertIn("PASS 52 protocol boundary checks", run.stdout)

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


if __name__ == "__main__":
    unittest.main()
