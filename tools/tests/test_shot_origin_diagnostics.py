"""Execute the real origin observer and pawn override against the stock lookup.

The small adapter supplies containers/world state only. Both lookup algorithms
and the observer scope are compiled from production sources. These tests do not
replace an Unreal build or a packet-loss playtest.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, STOCK, find_compiler, native_function


ADAPTER = r'''
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>
using int32 = int32_t;
constexpr int32 INDEX_NONE = -1;
struct FVector {
    float X, Y, Z;
    FVector(float x = 0.f, float y = 0.f, float z = 0.f) : X(x), Y(y), Z(z) {}
    bool operator==(const FVector& other) const {
        return X == other.X && Y == other.Y && Z == other.Z;
    }
    static const FVector ZeroVector;
};
const FVector FVector::ZeroVector;
struct FSavedPosition {
    FVector Position;
    bool bTeleported = false;
    bool bShotSpawned = false;
    float Time = 0.f;
    float TimeStamp = 0.f;
    bool operator==(const FSavedPosition& other) const {
        return Position == other.Position && bTeleported == other.bTeleported
            && bShotSpawned == other.bShotSpawned && Time == other.Time
            && TimeStamp == other.TimeStamp;
    }
};
template<class T> struct TArray : std::vector<T> {
    int32 Num() const { return static_cast<int32>(this->size()); }
};
struct UWorld {
    float Time = 10.f;
    float GetTimeSeconds() const { return Time; }
};
class AUTCharacter {
public:
    virtual ~AUTCharacter() = default;
    UWorld World;
    FVector Location = FVector(100.f, 200.f, 300.f);
    TArray<FSavedPosition> SavedPositions;
    float MaxShotSynchDelay = 0.25f;
    const UWorld* GetWorld() const { return &World; }
    FVector GetActorLocation() const { return Location; }
    virtual FVector GetDelayedShotPosition();
};
struct ATeamArenaCharacter : AUTCharacter {
    using Super = AUTCharacter;
    FVector GetDelayedShotPosition() override;
};
'''


CASES = r'''
void Require(bool value, const char* message) {
    if (!value) { std::cerr << message << '\n'; std::exit(1); }
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }
FSavedPosition Sample(float time, float stamp, float x, bool shot, bool teleport = false) {
    FSavedPosition result;
    result.Position = FVector(x, 3.f, 4.f);
    result.Time = time; result.TimeStamp = stamp;
    result.bShotSpawned = shot; result.bTeleported = teleport;
    return result;
}
FVector CheckedLookup(ATeamArenaCharacter& character) {
    const auto history = character.SavedPositions;
    const FVector location = character.Location;
    const float now = character.World.Time;
    const float cutoff = character.MaxShotSynchDelay;
    const FVector expected = character.AUTCharacter::GetDelayedShotPosition();
    const FVector actual = character.GetDelayedShotPosition();
    Require(actual == expected, "NCP override changed the stock returned origin");
    Require(character.SavedPositions == history, "observation mutated saved movement history");
    Require(character.Location == location && character.World.Time == now
        && character.MaxShotSynchDelay == cutoff, "observation changed character/world state");
    return actual;
}
void NewestMarker() {
    ATeamArenaCharacter character;
    character.SavedPositions.push_back(Sample(9.8f, 101.f, 1.f, true));
    character.SavedPositions.push_back(Sample(9.875f, 102.f, 2.f, true));
    character.SavedPositions.push_back(Sample(9.9375f, 103.f, 3.f, false));
    FNCShotOriginScope scope(&character);
    Require(CheckedLookup(character) == character.SavedPositions[1].Position,
        "newest shot marker must supply the origin");
    Require(scope.LookupCount == 1 && scope.SavedCount == 3 && scope.MarkerIndex == 1,
        "wrong observed lookup count/history size/index");
    Require(scope.bResultMatches && scope.MarkerServerTime == 9.875f
        && scope.MarkerMoveStamp == 102.f && Near(scope.MarkerAgeMs, 125.f),
        "marker metadata must describe the actual stock result");
    Require(!scope.bReachedAgeCutoff && !scope.bMarkerOverAge
        && !scope.bNewerTeleport && !scope.bMarkerTeleported, "unexpected marker flags");
}
void Fallbacks() {
    for (int kind = 0; kind < 3; ++kind) {
        ATeamArenaCharacter character;
        if (kind == 1) {
            character.SavedPositions.push_back(Sample(9.875f, 1.f, 1.f, false));
            character.SavedPositions.push_back(Sample(9.9375f, 2.f, 2.f, false));
        }
        if (kind == 2) {
            character.SavedPositions.push_back(Sample(9.f, 1.f, 1.f, true));
            character.SavedPositions.push_back(Sample(9.5f, 2.f, 2.f, false));
            character.SavedPositions.push_back(Sample(9.9375f, 3.f, 3.f, false));
        }
        FNCShotOriginScope scope(&character);
        Require(CheckedLookup(character) == character.Location, "missing eligible marker did not fall back");
        Require(scope.LookupCount == 1 && scope.MarkerIndex == INDEX_NONE && scope.bResultMatches,
            "fallback must have no chosen marker");
        Require(scope.LookupPosition == character.Location && scope.MarkerAgeMs == -1.f
            && scope.MarkerServerTime == -1.f && scope.MarkerMoveStamp == -1.f,
            "fallback invented a marker/time");
        Require(scope.bReachedAgeCutoff == (kind == 2), "age-cutoff reason mismatch");
    }
}
void FlagBeforeAge() {
    for (float time : {9.75f, 9.5f}) {
        ATeamArenaCharacter character;
        character.SavedPositions.push_back(Sample(time, 22.f, 5.f, true));
        character.SavedPositions.push_back(Sample(9.9375f, 23.f, 6.f, false));
        FNCShotOriginScope scope(&character);
        Require(CheckedLookup(character) == character.SavedPositions[0].Position,
            "diagnostic must preserve stock flag-before-age selection");
        Require(scope.MarkerIndex == 0 && scope.bResultMatches && !scope.bReachedAgeCutoff,
            "stock-selected marker was mislabeled fallback");
        Require(scope.bMarkerOverAge == (time == 9.5f), "strict age boundary changed");
    }
}
void TeleportsRemainDescriptive() {
    ATeamArenaCharacter character;
    character.SavedPositions.push_back(Sample(9.875f, 10.f, 5.f, true));
    character.SavedPositions.push_back(Sample(9.9375f, 11.f, 9.f, false, true));
    {
        FNCShotOriginScope scope(&character);
        CheckedLookup(character);
        Require(scope.MarkerIndex == 0 && scope.bNewerTeleport && !scope.bMarkerTeleported
            && scope.bResultMatches, "newer teleport must be reported without changing lookup");
    }
    character.SavedPositions[1].bShotSpawned = true;
    {
        FNCShotOriginScope scope(&character);
        CheckedLookup(character);
        Require(scope.MarkerIndex == 1 && scope.bMarkerTeleported && !scope.bNewerTeleport
            && scope.bResultMatches, "selected teleport marker metadata is incorrect");
    }
}
void DuplicateValues() {
    ATeamArenaCharacter character;
    character.SavedPositions.push_back(Sample(9.8f, 7.f, 9.f, true));
    character.SavedPositions.push_back(Sample(9.875f, 7.f, 9.f, true));
    character.SavedPositions.push_back(Sample(9.875f, 7.f, 9.f, true));
    FNCShotOriginScope scope(&character);
    CheckedLookup(character);
    Require(scope.MarkerIndex == 2 && scope.MarkerMoveStamp == 7.f && scope.bResultMatches,
        "duplicate position/stamp/time must identify newest index, not first equal value");
}
void ScopeIsolation() {
    ATeamArenaCharacter first, second;
    first.SavedPositions.push_back(Sample(9.875f, 1.f, 2.f, true));
    second.SavedPositions.push_back(Sample(9.875f, 3.f, 4.f, true));
    CheckedLookup(first); // Disabled: no scope exists.
    {
        FNCShotOriginScope outer(&first);
        CheckedLookup(second);
        FNCShotOriginScope::ObserveStockLookup(nullptr, FVector());
        Require(outer.LookupCount == 0, "mismatched/null owner leaked into scope");
        {
            FNCShotOriginScope inner(&second);
            CheckedLookup(first);
            Require(outer.LookupCount == 0 && inner.LookupCount == 0,
                "nested scope must mask the parent even for parent's owner");
            CheckedLookup(second);
            Require(inner.LookupCount == 1 && inner.MarkerIndex == 0, "nested owner not observed");
            {
                FNCShotOriginScope inactive(nullptr);
                CheckedLookup(second);
                CheckedLookup(first);
                Require(inactive.LookupCount == 0 && inner.LookupCount == 1 && outer.LookupCount == 0,
                    "inactive nested query failed to mask enclosing observations");
            }
        }
        CheckedLookup(first);
        Require(outer.LookupCount == 1 && outer.MarkerIndex == 0,
            "parent scope not restored after nested destruction");
    }
    CheckedLookup(first); // Scope cleanup must not retain the destroyed observer.
    FNCShotOriginScope fresh(&first);
    Require(fresh.LookupCount == 0 && fresh.MarkerIndex == INDEX_NONE, "new scope inherited stale state");
    CheckedLookup(first);
    Require(fresh.LookupCount == 1, "fresh scope did not observe exactly one call");
}
void ResultMismatch() {
    for (bool marker : {false, true}) {
        ATeamArenaCharacter character;
        if (marker) character.SavedPositions.push_back(Sample(9.875f, 1.f, 2.f, true));
        const auto before = character.SavedPositions;
        FNCShotOriginScope scope(&character);
        const FVector unexpected(-100.f, -200.f, -300.f);
        FNCShotOriginScope::ObserveStockLookup(&character, unexpected);
        Require(scope.LookupCount == 1 && !scope.bResultMatches && scope.LookupPosition == unexpected,
            "a non-stock result was falsely presented as confirmed");
        Require(scope.MarkerIndex == (marker ? 0 : INDEX_NONE), "mismatch lost candidate metadata");
        Require(character.SavedPositions == before, "mismatch observation mutated history");
    }
}
void MultipleLookupsKeepFirst() {
    ATeamArenaCharacter character;
    character.SavedPositions.push_back(Sample(9.875f, 1.f, 2.f, true));
    FNCShotOriginScope scope(&character);
    CheckedLookup(character);
    const FVector first = scope.LookupPosition;
    character.SavedPositions.push_back(Sample(9.9375f, 2.f, 3.f, true));
    Require(CheckedLookup(character) == character.SavedPositions[1].Position,
        "second real lookup must still return its current stock result");
    Require(scope.LookupCount == 2 && scope.MarkerIndex == 0 && scope.SavedCount == 1
        && scope.MarkerMoveStamp == 1.f && scope.LookupPosition == first,
        "multiple lookup ambiguity must count both and retain first observation");
}
void HistoryParityMatrix() {
    // Vary flags, teleports, duplicate timestamps/positions and cutoff placement.
    // Execute the actual stock and overridden methods for every fixture.
    for (int flags = 0; flags < 256; ++flags) {
        ATeamArenaCharacter character;
        for (int i = 0; i < 8; ++i)
            character.SavedPositions.push_back(Sample(9.125f + i * 0.125f,
                static_cast<float>(i / 2), static_cast<float>(i % 3),
                (flags & (1 << i)) != 0, (i + flags) % 5 == 0));
        CheckedLookup(character); // Observer disabled must preserve result too.
        FNCShotOriginScope scope(&character);
        const FVector result = CheckedLookup(character);
        Require(scope.LookupCount == 1 && scope.bResultMatches && scope.LookupPosition == result,
            "observer disagrees with production stock lookup in parity matrix");
    }
}
int main(int argc, char** argv) {
    Require(argc == 2, "one case required"); const std::string name(argv[1]);
    if (name == "newest") NewestMarker();
    else if (name == "fallbacks") Fallbacks();
    else if (name == "age") FlagBeforeAge();
    else if (name == "teleports") TeleportsRemainDescriptive();
    else if (name == "duplicates") DuplicateValues();
    else if (name == "scopes") ScopeIsolation();
    else if (name == "mismatch") ResultMismatch();
    else if (name == "multiple") MultipleLookupsKeepFirst();
    else if (name == "parity") HistoryParityMatrix();
    else Require(false, "unknown case");
}
'''


def without_unreal_includes(source):
    return re.sub(r'^\s*#(?:include\b[^\n]*|pragma\s+once)\s*$', '', source, flags=re.MULTILINE)


class ShotOriginDiagnosticsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-shot-origin-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        private = PLUGIN / "Source/Private"
        header = (private / "NCShotOriginDiagnostics.h").read_text(encoding="utf-8-sig")
        observer = (private / "NCShotOriginDiagnostics.cpp").read_text(encoding="utf-8-sig")
        stock = STOCK.read_text(encoding="utf-8-sig")
        character = (private / "TeamArenaCharacter.cpp").read_text(encoding="utf-8-sig")
        code = "\n".join((
            ADAPTER, without_unreal_includes(header), without_unreal_includes(observer),
            native_function(stock, "FVector AUTCharacter::GetDelayedShotPosition()"),
            native_function(character, "FVector ATeamArenaCharacter::GetDelayedShotPosition()"),
            CASES,
        ))
        source = directory / "shot_origin.cpp"
        source.write_text(code, encoding="utf-8")
        cls.executable = directory / ("shot_origin.exe" if os.name == "nt" else "shot_origin")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'shot_origin.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Origin adapter compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_newest_marker_and_actual_metadata(self):
        self.run_case("newest")

    def test_empty_unmarked_and_age_cutoff_fallbacks(self):
        self.run_case("fallbacks")

    def test_stock_flag_before_age_and_exact_boundary_are_preserved(self):
        self.run_case("age")

    def test_newer_and_selected_teleports_are_observed_without_changing_origin(self):
        self.run_case("teleports")

    def test_duplicate_positions_stamps_and_times_keep_newest_index(self):
        self.run_case("duplicates")

    def test_scope_off_owner_isolation_nested_masking_and_restoration(self):
        self.run_case("scopes")

    def test_result_mismatches_remain_unconfirmed(self):
        self.run_case("mismatch")

    def test_multiple_lookups_count_ambiguity_and_retain_first_record(self):
        self.run_case("multiple")

    def test_stock_return_and_history_parity_across_256_histories(self):
        self.run_case("parity")


if __name__ == "__main__":
    unittest.main()
