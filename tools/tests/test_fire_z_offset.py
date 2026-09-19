"""Compile the production fire-height encoder against minimal math adapters.

Checks compatibility with the existing byte-minus-127 decoder. Unreal firing
and network delivery still require the normal client/server playtest.
"""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
using uint8 = uint8_t;
struct FMath {
    static bool IsFinite(float value) { return std::isfinite(value); }
    static bool IsNearlyEqual(float a, float b, float tolerance) {
        return std::fabs(a - b) <= tolerance;
    }
    static float Clamp(float value, float low, float high) {
        return std::max(low, std::min(value, high));
    }
};
'''

CASES = r'''
void Require(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
void ExplicitHeights() {
    struct Example { float Height; int Byte; int Decoded; };
    const Example examples[] = {
        {83.f, 210, 83}, {83.25f, 210, 83}, {83.5f, 211, 84},
        {70.f, 197, 70}, {0.f, 127, 0}, {-0.5f, 127, 0},
        {-1.f, 126, -1}, {-126.f, 1, -126}, {-126.5f, 1, -126},
        {-1000.f, 1, -126}, {128.f, 255, 128}, {1000.f, 255, 128}
    };
    for (const auto& e : examples) {
        const uint8 encoded = EncodeClientFireZOffset(e.Height, 83.f, true);
        Require(encoded == e.Byte, "explicit height changed the existing codec");
        Require(encoded != 0 && int(encoded) - 127 == e.Decoded,
            "old server decoder would use a fallback or wrong height");
    }
    // The server may already have changed stance. An explicit standing height
    // must remain 83 regardless of that server-side default.
    const uint8 standing = EncodeClientFireZOffset(83.f, 83.f, true);
    for (int serverDefault : {32, 64, 83}) {
        Require((standing ? int(standing) - 127 : serverDefault) == 83,
            "explicit height still depends on server stance");
    }
    Require(EncodeClientFireZOffset(std::numeric_limits<float>::max(), 83.f, true) == 255,
        "finite upper extreme did not saturate");
    Require(EncodeClientFireZOffset(-std::numeric_limits<float>::max(), 83.f, true) == 1,
        "finite lower extreme became the zero sentinel");
}
void LegacyToggle() {
    for (float height : {82.f, 82.5f, 83.f, 83.5f, 84.f}) {
        Require(EncodeClientFireZOffset(height, 83.f, false) == 0,
            "disabled switch did not restore near-default omission");
    }
    Require(EncodeClientFireZOffset(70.f, 83.f, false) == 197,
        "legacy landing offset changed");
    Require(EncodeClientFireZOffset(0.f, 83.f, false) == 127,
        "zero physical height became a missing value");
    Require(EncodeClientFireZOffset(-1000.f, 83.f, false) == 0,
        "legacy lower saturation changed");
    Require(EncodeClientFireZOffset(1000.f, 83.f, false) == 255,
        "legacy upper saturation changed");
}
void InvalidViewData() {
    for (float value : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()}) {
        for (bool enabled : {false, true}) {
            Require(EncodeClientFireZOffset(value, 83.f, enabled) == 0,
                "invalid view height did not use safe server fallback");
        }
    }
}
int main(int argc, char** argv) {
    Require(argc == 2, "one test case required");
    const std::string name(argv[1]);
    if (name == "explicit") ExplicitHeights();
    else if (name == "legacy") LegacyToggle();
    else if (name == "invalid") InvalidViewData();
    else Require(false, "unknown case");
}
'''


class FireZOffsetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-fire-z-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        production = (PLUGIN / "Source/Private/UTWeaponFix.cpp").read_text(encoding="utf-8-sig")
        source = directory / "fire_z.cpp"
        source.write_text(ADAPTER + native_function(production,
            "static uint8 EncodeClientFireZOffset") + CASES, encoding="utf-8")
        cls.executable = directory / ("fire_z.exe" if os.name == "nt" else "fire_z")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14",
                       str(source), f"/Fe{cls.executable}", f"/Fo{directory / 'fire_z.obj'}"]
        else:
            command = [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_explicit_quantized_height_and_sentinel_boundaries(self):
        self.run_case("explicit")

    def test_disabled_toggle_keeps_legacy_encoding(self):
        self.run_case("legacy")

    def test_invalid_view_data_falls_back_without_float_to_byte_conversion(self):
        self.run_case("invalid")


if __name__ == "__main__":
    unittest.main()
