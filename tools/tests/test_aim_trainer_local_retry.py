"""Compile the production local-upload cooldown policy with engine type adapters.

This covers retry delays and the process-lifetime monotonic deadline. The real
UE transport/session test covers response headers, callbacks, request admission,
and cancellation; these adapters do not claim to execute WinHTTP or UE timers.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler


ADAPTER = r'''
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
using int32 = int;
struct FString : std::string {
    using std::string::string;
    FString() = default;
    FString(const std::string& value):std::string(value){}
    bool IsEmpty() const { return empty(); }
    int32 Len() const { return int32(size()); }
    FString Trim() const { const auto first=find_first_not_of(" \t\r\n"); return first==npos ? FString() : FString(substr(first)); }
    FString TrimTrailing() const { const auto last=find_last_not_of(" \t\r\n"); return last==npos ? FString() : FString(substr(0,last+1)); }
};
struct FMath {
    static bool IsFinite(double value) { return std::isfinite(value); }
    static double Max(double a,double b) { return std::max(a,b); }
};
struct FTimespan { double Seconds; double GetTotalSeconds() const { return Seconds; } };
struct FDateTime {
    double Seconds=0;
    static FDateTime UtcNow() { return {1791824400.0}; }
    static bool ParseHttpDate(const FString& value,FDateTime& out) {
        std::tm stamp{};
        std::istringstream stream(value);
        stream >> std::get_time(&stamp,"%a, %d %b %Y %H:%M:%S GMT");
        if(stream.fail()) return false;
        #ifdef _WIN32
        out.Seconds=double(_mkgmtime(&stamp));
        #else
        out.Seconds=double(timegm(&stamp));
        #endif
        return true;
    }
    FTimespan operator-(const FDateTime& other) const { return {Seconds-other.Seconds}; }
};
'''

CASES = r'''
void Require(bool pass,const char* reason) { if(!pass) { std::cerr<<reason; std::exit(1); } }
int main(int argc,char** argv) {
    if(argc!=2) return 2;
    using namespace NCAimTrainerLocalRetry;
    const std::string name=argv[1];
    if(name=="server_delay") {
        Require(DelaySeconds(429,"60","")==60.0,"429 lost the server's60-second delay");
        Require(DelaySeconds(429,"  7\t","")==7.0,"server's shorter retry delay was ignored");
        Require(DelaySeconds(503,"120","")==120.0,"hinted503 did not honor Retry-After");
        Require(DelaySeconds(429,"3600","")==3600.0,"longer server delay was truncated");
    } else if(name=="explicit_zero") {
        Require(DelaySeconds(429,"0","")==0.0,"explicit zero must not impose a local quota");
    } else if(name=="missing_or_bad") {
        const char* invalid[]={"","-1","1.5","garbage","60 seconds","nan","inf"};
        for(const char* value:invalid) {
            Require(DelaySeconds(429,value,"")==60.0,"malformed429 created a retry burst");
            Require(DelaySeconds(503,value,"")==0.0,"unhinted503 added a restart restriction");
        }
        const FString huge(std::string(400,'9'));
        Require(DelaySeconds(429,huge,"")==60.0,"numeric overflow escaped bounded fallback");
    } else if(name=="http_date") {
        Require(DelaySeconds(429,"Mon, 12 Oct 2026 18:01:00 GMT","Mon, 12 Oct 2026 18:00:00 GMT")==60.0,
                "HTTP-date cooldown must use the service clock despite client clock skew");
        Require(DelaySeconds(503,"Mon, 12 Oct 2026 18:02:00 GMT","Mon, 12 Oct 2026 18:00:00 GMT")==120.0,
                "503 HTTP-date hint was lost");
    } else if(name=="expired_date") {
        Require(DelaySeconds(429,"Mon, 12 Oct 2026 18:00:00 GMT","Mon, 12 Oct 2026 18:00:01 GMT")==0.0,
                "expired valid delay must not become a new60-second restriction");
    } else if(name=="cooldown_boundary") {
        FStartCooldown cooldown;
        Require(cooldown.Remaining(10.0)==0.0,"fresh process restricted restarts without rejection");
        cooldown.Defer(10.0,60.0);
        Require(cooldown.Remaining(13.0)==57.0,"a three-second restart bypassed Retry-After");
        Require(cooldown.Remaining(69.99)>0.0,"early request admitted");
        Require(cooldown.Remaining(70.0)==0.0&&cooldown.Remaining(100.0)==0.0,"deadline did not release restart");
    } else if(name=="cooldown_extension") {
        FStartCooldown cooldown;
        cooldown.Defer(10.0,60.0);
        cooldown.Defer(11.0,5.0);
        Require(cooldown.Until==70.0,"later shorter rejection cut an existing cooldown short");
        cooldown.Defer(20.0,80.0);
        Require(cooldown.Remaining(30.0)==70.0,"new later deadline was ignored");
    } else if(name=="restarts_do_not_extend") {
        FStartCooldown cooldown;
        cooldown.Defer(10.0,60.0);
        for(int restart=0;restart<20;++restart) {
            const double now=10.0+restart*3.0;
            Require(cooldown.Remaining(now)==70.0-now,"checking restart availability shifted cooldown");
        }
        Require(cooldown.Remaining(70.0)==0.0,"restarts prolonged a fixed server cooldown");
    } else return 2;
}
'''


class AimTrainerLocalRetryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        cls.temporary = tempfile.TemporaryDirectory(prefix="ncp-trainer-local-retry-")
        cls.addClassCleanup(cls.temporary.cleanup)
        directory = Path(cls.temporary.name)
        (directory / "Misc").mkdir()
        (directory / "CoreMinimal.h").write_text("#pragma once\n" + ADAPTER, encoding="utf-8")
        (directory / "Misc/DateTime.h").write_text("#pragma once\n", encoding="utf-8")
        policy = (PLUGIN / "Source/Private/NCAimTrainerLocalRetry.h").as_posix()
        source = directory / "local_retry.cpp"
        source.write_text(f'#include "{policy}"\n' + CASES, encoding="utf-8")
        cls.executable = directory / ("local_retry.exe" if os.name == "nt" else "local_retry")
        if msvc:
            command = [compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", f"/I{directory}", str(source),
                       f"/Fe{cls.executable}", f"/Fo{directory / 'local_retry.obj'}"]
        else:
            command = [compiler, "-std=c++14", "-Wall", "-Wextra", "-Werror", "-pedantic", f"-I{directory}",
                       str(source), "-o", str(cls.executable)]
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(f"Local retry policy compilation failed:\n{result.stdout}\n{result.stderr}")

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment, capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_numeric_server_delays(self): self.run_case("server_delay")
    def test_explicit_zero_does_not_add_local_delay(self): self.run_case("explicit_zero")
    def test_missing_malformed_and_overflow_hints(self): self.run_case("missing_or_bad")
    def test_http_date_uses_server_clock(self): self.run_case("http_date")
    def test_expired_http_date_does_not_add_local_delay(self): self.run_case("expired_date")
    def test_cooldown_prevents_three_second_restart_then_expires(self): self.run_case("cooldown_boundary")
    def test_rejections_can_extend_but_not_shorten_cooldown(self): self.run_case("cooldown_extension")
    def test_restart_checks_do_not_prolong_cooldown(self): self.run_case("restarts_do_not_extend")


if __name__ == "__main__":
    unittest.main()
