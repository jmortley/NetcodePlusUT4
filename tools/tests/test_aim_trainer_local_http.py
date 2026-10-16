"""Execute native trainer HTTP response completion with a WinHTTP adapter.

Production header capture and completion methods are compiled unchanged. This
does not exercise real WinHTTP I/O, TLS, callback threading, or shutdown.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_wipeout_healing import PLUGIN, find_compiler, native_function


ADAPTER = r'''
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>
using int32 = int;
using uint8 = unsigned char;
using DWORD = uint32_t;
using WCHAR = wchar_t;
#define TEXT(x) L##x
#define ARRAY_COUNT(x) (sizeof(x) / sizeof((x)[0]))
#define check(x) do { if (!(x)) std::abort(); } while (false)
bool IsInGameThread() { return true; }
enum class ESearchCase { IgnoreCase };
struct FString : std::wstring {
    using std::wstring::wstring;
    FString() = default;
    FString(const std::wstring& value) : std::wstring(value) {}
    bool IsEmpty() const { return empty(); }
    bool Equals(const FString& other, ESearchCase) const {
        return size() == other.size() && std::equal(begin(), end(), other.begin(),
            [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
    }
};
template<class T> struct TArray : std::vector<T> {
    using std::vector<T>::vector;
    int Num() const { return int(this->size()); }
    void Add(const T& value) { this->push_back(value); }
    void Empty() { this->clear(); }
    void Append(const TArray& values) { this->insert(this->end(), values.begin(), values.end()); }
};
enum class ESPMode { ThreadSafe };
template<class T, ESPMode Mode = ESPMode::ThreadSafe> struct TSharedPtr : std::shared_ptr<T> {
    using std::shared_ptr<T>::shared_ptr;
    bool IsValid() const { return this->get() != nullptr; }
    void Reset() { this->reset(); }
};
template<class T> TSharedPtr<T> MakeShareable(T* pointer) { return TSharedPtr<T>(pointer); }
template<class T> typename std::remove_reference<T>::type&& MoveTemp(T&& value) {
    return std::move(value);
}
struct IHttpResponse {
    virtual ~IHttpResponse() = default;
    virtual FString GetHeader(const FString&) = 0;
    virtual TArray<FString> GetAllHeaders() = 0;
};
class FResponse : public IHttpResponse {
public:
    FString Url, RetryAfter, Date;
    int32 Code = 0;
    TArray<uint8> Bytes;
    /*RESPONSE_METHODS*/
};
struct IHttpRequest {};
using FHttpResponsePtr = TSharedPtr<FResponse>;
struct FHttpRequestCompleteDelegate {
    std::function<void(TSharedPtr<IHttpRequest>, FHttpResponsePtr, bool)> Callback;
    void ExecuteIfBound(TSharedPtr<IHttpRequest> request, FHttpResponsePtr response, bool connected) {
        if (Callback) Callback(request, response, connected);
    }
};
namespace EHttpRequestStatus { enum Type { Processing, Succeeded, Failed }; }
constexpr DWORD WINHTTP_QUERY_FLAG_NUMBER = 0x80000000u;
constexpr DWORD WINHTTP_QUERY_STATUS_CODE = 1, WINHTTP_QUERY_CONTENT_LENGTH = 2;
constexpr DWORD WINHTTP_QUERY_RETRY_AFTER = 3, WINHTTP_QUERY_DATE = 4;
constexpr DWORD WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE = 10;
constexpr DWORD WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE = 11;
constexpr DWORD WINHTTP_CALLBACK_STATUS_READ_COMPLETE = 12;
constexpr DWORD WINHTTP_CALLBACK_STATUS_REQUEST_ERROR = 13;
constexpr DWORD WINHTTP_CALLBACK_STATUS_SECURE_FAILURE = 14;
constexpr const wchar_t* WINHTTP_HEADER_NAME_BY_INDEX = nullptr;
constexpr DWORD* WINHTTP_NO_HEADER_INDEX = nullptr;
constexpr int32 ResponseLimit = 8192;
struct FNativeResponse {
    DWORD Code = 429, ContentLength = 0;
    FString RetryAfter = L"60", Date = L"Mon, 12 Oct 2026 20:00:00 GMT";
    bool Closed = false, StatusReadable = true;
    int Reads = 0, HeaderQueries = 0;
};
using HINTERNET = FNativeResponse*;
bool WinHttpQueryHeaders(HINTERNET request, DWORD query, const wchar_t*,
                        void* buffer, DWORD* size, DWORD*) {
    check(request && !request->Closed);
    if (query == (WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER)) {
        if (!request->StatusReadable) return false;
        check(*size == sizeof(DWORD));
        *static_cast<DWORD*>(buffer) = request->Code;
        return true;
    }
    if (query == (WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER)) {
        check(*size == sizeof(DWORD));
        *static_cast<DWORD*>(buffer) = request->ContentLength;
        return true;
    }
    ++request->HeaderQueries;
    const FString& value = query == WINHTTP_QUERY_RETRY_AFTER ? request->RetryAfter : request->Date;
    if (value.empty()) return false;
    const DWORD required = DWORD((value.size() + 1) * sizeof(wchar_t));
    if (*size < required) { *size = required; return false; }
    std::memcpy(buffer, value.c_str(), required);
    *size = required - sizeof(wchar_t);
    return true;
}
bool WinHttpReceiveResponse(HINTERNET request, void*) { check(!request->Closed); return true; }
bool WinHttpReadData(HINTERNET request, void*, DWORD, DWORD*) {
    check(!request->Closed); ++request->Reads; return true;
}
struct FState {
    HINTERNET Request = nullptr;
    uint8 ReadBuffer[ResponseLimit];
    void Close() { Request->Closed = true; Request = nullptr; }
};
class FRequest {
public:
    FString Url = L"https://ut4stats.com/aimtrainer_local/start/";
    TArray<FString> Headers;
    TArray<uint8> ResponseBytes;
    FHttpRequestCompleteDelegate Complete;
    FHttpResponsePtr Response;
    TSharedPtr<FState> Native;
    TSharedPtr<IHttpRequest> KeepAlive;
    EHttpRequestStatus::Type Status = EHttpRequestStatus::Processing;
    int32 ResponseCode = 0;
    FString ResponseRetryAfter, ResponseDate;
    bool bFinished = false;
    /*REQUEST_METHODS*/
};
void Require(bool condition, const char* message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
struct Fixture {
    FNativeResponse Server;
    FRequest Request;
    FHttpResponsePtr Received;
    int Completions = 0;
    bool Connected = false;
    Fixture() {
        Request.Native = MakeShareable(new FState);
        Request.Native->Request = &Server;
        Request.KeepAlive = MakeShareable(new IHttpRequest);
        Request.Headers.Add(L"Authorization: Bearer test-only");
        Request.Complete.Callback = [this](TSharedPtr<IHttpRequest> request, FHttpResponsePtr response, bool connected) {
            Require(request.IsValid(), "request lifetime ended before completion");
            Require(Server.Closed, "completion ran before native handle closure");
            Received = response; Connected = connected; ++Completions;
        };
    }
    void Headers() { Request.HandleStatus(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, {}); }
    void EndBody() { Request.HandleStatus(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, {}); }
};
'''


CASES = r'''
int main(int argc, char** argv) {
    Require(argc == 2, "case required");
    const std::string name = argv[1];
    if (name == "rate_limit") {
        Fixture f; f.Headers();
        Require(f.Completions==0 && f.Request.Response.IsValid() && f.Request.Response->Code==429
            && f.Request.Response->GetHeader(L"Retry-After")==L"60", "incomplete error body hid known cooldown");
        f.Request.HandleStatus(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, {'{','"','e','r','r','o','r','"',':','"','l','i','m','i','t','"','}'});
        f.EndBody();
        Require(f.Completions == 1 && f.Connected && f.Received.IsValid(), "429 did not complete as an HTTP response");
        Require(f.Received->Code == 429, "429 response code was lost");
        Require(f.Received->GetHeader(L"retry-after") == L"60", "Retry-After disappeared during completion");
        Require(f.Received->GetHeader(L"RETRY-AFTER") == L"60", "header lookup is case-sensitive");
        Require(f.Received->GetHeader(L"dAtE") == f.Server.Date, "Date was not retained for absolute Retry-After");
        Require(f.Received->GetHeader(L"Authorization").IsEmpty(), "request credentials exposed as response headers");
        const auto headers = f.Received->GetAllHeaders();
        Require(headers.Num() == 2 && headers[0] == L"Retry-After: 60"
                && headers[1] == L"Date: Mon, 12 Oct 2026 20:00:00 GMT", "enumeration lost retry metadata");
        Require(f.Server.Reads == 2 && f.Server.HeaderQueries == 2 && !f.Received->Bytes.empty(), "rejected response lost its bounded reason body or headers");
        Require(!f.Request.Native.IsValid() && !f.Request.KeepAlive.IsValid() && f.Request.Headers.empty(), "completion retained request resources");
        f.Request.HandleStatus(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, {});
        Require(f.Completions == 1, "late native event completed the same request twice");
    } else if (name == "success") {
        for (DWORD code : {200u, 201u}) {
            Fixture f; f.Server.Code = code; f.Server.ContentLength = 2; f.Headers();
            Require(f.Completions == 0 && f.Server.Reads == 1 && !f.Server.Closed, "success finished before receiving JSON");
            f.Request.HandleStatus(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, {'{', '}'});
            Require(f.Completions == 0 && f.Server.Reads == 2, "body chunk was not accumulated");
            f.Request.HandleStatus(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, {});
            Require(f.Completions == 1 && f.Connected && f.Received->Code == int32(code), "success status was lost");
            Require(f.Received->Bytes == TArray<uint8>({'{', '}'}), "success JSON changed");
            Require(f.Received->GetHeader(L"Retry-After") == L"60", "success metadata was discarded");
        }
    } else if (name == "absent_oversized") {
        for (bool oversized : {false, true}) {
            Fixture f;
            f.Server.RetryAfter = oversized ? FString(10000, L'9') : FString();
            f.Server.Date = FString(); f.Headers(); f.EndBody();
            Require(f.Completions == 1 && f.Connected && f.Received->Code == 429, "bad header prevented rejection delivery");
            Require(f.Received->GetHeader(L"Retry-After").IsEmpty(), "missing or oversized header escaped the bounded reader");
            Require(f.Received->GetAllHeaders().empty(), "empty headers were enumerated");
            Require(f.Server.HeaderQueries == 2, "header retrieval retried with an unbounded allocation");
        }
    } else if (name == "absolute_date") {
        Fixture f; f.Server.RetryAfter = L"Mon, 12 Oct 2026 20:01:00 GMT"; f.Headers(); f.EndBody();
        Require(f.Received->GetHeader(L"Retry-After") == f.Server.RetryAfter
            && f.Received->GetHeader(L"Date") == f.Server.Date, "HTTP-date retry information was transformed or lost");
    } else if (name == "interrupted_error_body") {
        Fixture f; f.Headers();
        f.Request.HandleStatus(WINHTTP_CALLBACK_STATUS_REQUEST_ERROR, {});
        Require(f.Completions==1 && f.Connected && f.Received->Code==429
            && f.Received->GetHeader(L"Retry-After")==L"60", "body failure erased an explicit rejection");
    } else if (name == "oversized_error_body") {
        Fixture f; f.Server.ContentLength=ResponseLimit+1; f.Headers();
        Require(f.Completions==1 && f.Received->Code==429 && f.Received->Bytes.empty()
            && f.Received->GetHeader(L"Retry-After")==L"60", "oversized error changed status or lost cooldown");
        Fixture chunked; chunked.Headers();
        chunked.Request.HandleStatus(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, TArray<uint8>(ResponseLimit+1, 'x'));
        Require(chunked.Completions==1 && chunked.Received->Code==429 && chunked.Received->Bytes.empty(),
            "oversized streamed body escaped bounds");
    } else if (name == "transport_failure") {
        Fixture f; f.Server.StatusReadable = false; f.Headers();
        Require(f.Completions == 1 && !f.Connected && !f.Received.IsValid(), "transport failure manufactured an HTTP response");
        Require(f.Request.Status == EHttpRequestStatus::Failed && f.Server.HeaderQueries == 0, "failed status query continued reading headers");
    } else Require(false, "unknown case");
}
'''


class AimTrainerLocalHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler, cls.environment, msvc = find_compiler()
        temporary = tempfile.TemporaryDirectory(prefix="ncp-local-http-")
        cls.addClassCleanup(temporary.cleanup)
        directory = Path(temporary.name)
        native = (PLUGIN / "Source/Private/NCAimTrainerLocalHttp.cpp").read_text(encoding="utf-8-sig")
        # Select response methods before the request's similarly named methods.
        response = native[native.index("class FResponse"):native.index("struct FState")]
        program = ADAPTER.replace("/*RESPONSE_METHODS*/", "\n".join(native_function(response, signature) for signature in (
            "virtual FString GetHeader", "virtual TArray<FString> GetAllHeaders")))
        program = program.replace("/*REQUEST_METHODS*/", "\n".join(native_function(native, signature) for signature in (
            "void Finish(bool", "void CaptureErrorHeaders", "FString ReadResponseHeader", "void HandleStatus", "void ReadNext")))
        source = directory / "local_http.cpp"
        source.write_text(program + CASES, encoding="utf-8")
        cls.executable = directory / ("local_http.exe" if os.name == "nt" else "local_http")
        command = ([compiler, "/nologo", "/EHsc", "/W4", "/WX", "/std:c++14", str(source),
                    f"/Fe{cls.executable}", f"/Fo{directory / 'local_http.obj'}"] if msvc else
                   [compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.executable)])
        result = subprocess.run(command, cwd=directory, env=cls.environment, capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], env=self.environment,
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


for _case in ("rate_limit", "success", "absent_oversized", "absolute_date", "transport_failure", "interrupted_error_body", "oversized_error_body"):
    setattr(AimTrainerLocalHttpTests, "test_" + _case, lambda self, case=_case: self.run_case(case))


if __name__ == "__main__":
    unittest.main()
