#include "net.h"

#include <winhttp.h>

#include <atomic>
#include <thread>

#include "json.h"
#include "text.h"

namespace {

constexpr wchar_t kAgent[] = L"Hashta-Panel/1.0";
constexpr wchar_t kHost[] = L"127.0.0.1";
constexpr INTERNET_PORT kPort = 8765;

HANDLE g_job = nullptr;
HANDLE g_process = nullptr;

HWND g_notify = nullptr;
std::thread g_reader;
std::atomic<bool> g_stop{false};
HINTERNET g_session = nullptr;
HINTERNET g_connect = nullptr;
HINTERNET g_request = nullptr;
std::string g_pending;

// -- paths -----------------------------------------------------------------

std::wstring DirOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : path.substr(0, slash);
}

bool FileExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ProjectRoot() {
    wchar_t module[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, module, MAX_PATH);
    const std::wstring dir = DirOf(module);
    if (FileExists(dir + L"\\engine_server.py")) return dir;
    const std::wstring parent = DirOf(dir);
    if (FileExists(parent + L"\\engine_server.py")) return parent;
    return L"";
}

std::wstring ResolvePython() {
    wchar_t from_env[MAX_PATH] = {0};
    if (GetEnvironmentVariableW(L"HASHTA_PYTHON", from_env, MAX_PATH) > 0 &&
        FileExists(from_env)) {
        return from_env;
    }
    const std::wstring fallback =
        L"C:\\Users\\admin\\AppData\\Local\\Programs\\Python\\Python313\\pythonw.exe";
    if (FileExists(fallback)) return fallback;
    return L"";
}

// -- http ------------------------------------------------------------------

HINTERNET OpenRequest(const wchar_t* verb, const wchar_t* path,
                      HINTERNET& session, HINTERNET& connect) {
    session = WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_NO_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return nullptr;
    connect = WinHttpConnect(session, kHost, kPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        session = nullptr;
        return nullptr;
    }
    HINTERNET request =
        WinHttpOpenRequest(connect, verb, path, nullptr, WINHTTP_NO_REFERER,
                           WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        session = nullptr;
        connect = nullptr;
        return nullptr;
    }
    return request;
}

void CloseRequest(HINTERNET& request) {
    if (request) {
        WinHttpCloseHandle(request);
        request = nullptr;
    }
}

DWORD StatusCode(HINTERNET request) {
    DWORD code = 0;
    DWORD size = sizeof(code);
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                        WINHTTP_NO_HEADER_INDEX);
    return code;
}

bool PostJson(const wchar_t* path, const std::string& body) {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = OpenRequest(L"POST", path, session, connect);
    if (!request) return false;
    WinHttpSetTimeouts(session, 1500, 1500, 3000, 5000);

    bool ok = WinHttpSendRequest(request, L"Content-Type: application/json",
                                 (DWORD)-1L, (void*)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(request, nullptr);
    if (ok) {
        const DWORD code = StatusCode(request);
        ok = code >= 200 && code < 300;
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

// -- event stream ----------------------------------------------------------

bool StreamConnect() {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = OpenRequest(L"GET", L"/events", session, connect);
    if (!request) return false;

    WinHttpSetTimeouts(session, 3000, 3000, 5000, 30000);
    const bool sent = WinHttpSendRequest(
        request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(request, nullptr) ||
        StatusCode(request) != 200) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }

    g_session = session;
    g_connect = connect;
    g_request = request;
    g_pending.clear();
    return true;
}

void StreamClose() {
    CloseRequest(g_request);
    if (g_connect) {
        WinHttpCloseHandle(g_connect);
        g_connect = nullptr;
    }
    if (g_session) {
        WinHttpCloseHandle(g_session);
        g_session = nullptr;
    }
    g_pending.clear();
}

void DispatchLine(const std::string& line) {
    if (line.rfind("data: ", 0) != 0) return;
    auto* payload = new std::string(line.substr(6));
    if (!PostMessageW(g_notify, WM_APP_EVENT, 0, (LPARAM)payload)) {
        delete payload;
    }
}

bool StreamPump() {
    DWORD available = 0;
    if (!WinHttpQueryDataAvailable(g_request, &available)) return false;
    if (available == 0) return false;

    std::string chunk((size_t)available, '\0');
    DWORD read = 0;
    if (!WinHttpReadData(g_request, chunk.data(), available, &read)) return false;
    chunk.resize((size_t)read);
    g_pending += chunk;

    size_t newline = 0;
    while ((newline = g_pending.find('\n')) != std::string::npos) {
        std::string line = g_pending.substr(0, newline);
        g_pending.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        DispatchLine(line);
    }
    return true;
}

void ReaderMain() {
    bool announced = false;
    while (!g_stop) {
        if (!g_request && !StreamConnect()) {
            Sleep(400);
            continue;
        }
        if (!announced) {
            PostMessageW(g_notify, WM_APP_NET, 1, 0);
            announced = true;
        }
        while (!g_stop && StreamPump()) {
        }
        if (g_stop) break;

        StreamClose();
        if (announced) {
            PostMessageW(g_notify, WM_APP_NET, 0, 0);
            announced = false;
        }
        Sleep(400);
    }
    StreamClose();
}

}  // namespace

// -- engine process --------------------------------------------------------

bool EngineStart() {
    const std::wstring root = ProjectRoot();
    const std::wstring python = ResolvePython();
    if (root.empty() || python.empty()) return false;

    g_job = CreateJobObjectW(nullptr, nullptr);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &limits,
                                sizeof(limits));
    }

    const std::wstring logs = root + L"\\logs";
    CreateDirectoryW(logs.c_str(), nullptr);
    const std::wstring log_path = logs + L"\\panel-engine.log";
    SECURITY_ATTRIBUTES inherit = {};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE log = CreateFileW(log_path.c_str(), GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                 OPEN_EXISTING, 0, nullptr);
    if (log == INVALID_HANDLE_VALUE || null_in == INVALID_HANDLE_VALUE) {
        if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
        if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
        if (g_job) {
            CloseHandle(g_job);
            g_job = nullptr;
        }
        return false;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_in;
    startup.hStdOutput = log;
    startup.hStdError = log;

    std::wstring command = L"\"" + python + L"\" \"" + root +
                           L"\\engine_server.py\"";
    PROCESS_INFORMATION process = {};
    const BOOL started =
        CreateProcessW(python.c_str(), command.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, root.c_str(), &startup,
                       &process);

    CloseHandle(log);
    CloseHandle(null_in);

    if (!started) {
        if (g_job) {
            CloseHandle(g_job);
            g_job = nullptr;
        }
        return false;
    }

    if (g_job && !AssignProcessToJobObject(g_job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(g_job);
        g_job = nullptr;
        return false;
    }
    CloseHandle(process.hThread);
    g_process = process.hProcess;
    return true;
}

std::wstring PanelRoot() {
    return ProjectRoot();
}

void EngineStop() {
    if (!g_process) {
        if (g_job) {
            CloseHandle(g_job);
            g_job = nullptr;
        }
        return;
    }

    PostJson(L"/api/shutdown", "{}");
    if (WaitForSingleObject(g_process, 8000) != WAIT_OBJECT_0) {
        if (g_job) TerminateJobObject(g_job, 1);
        WaitForSingleObject(g_process, 3000);
    }

    CloseHandle(g_process);
    g_process = nullptr;
    if (g_job) {
        CloseHandle(g_job);
        g_job = nullptr;
    }
}

// -- commands --------------------------------------------------------------

bool ApiSend(const std::wstring& text) {
    const std::string body = "{\"text\": \"" + json::Quote(Narrow(text)) + "\"}";
    return PostJson(L"/api/send", body);
}

bool ApiStopTurn() {
    return PostJson(L"/api/stop", "{}");
}

bool ApiReset() {
    return PostJson(L"/api/reset", "{}");
}

bool ApiState(std::string& out) {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = OpenRequest(L"GET", L"/api/state", session, connect);
    if (!request) return false;
    WinHttpSetTimeouts(session, 1500, 1500, 3000, 5000);

    bool ok = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 nullptr, 0, 0, 0) &&
              WinHttpReceiveResponse(request, nullptr) &&
              StatusCode(request) == 200;
    if (ok) {
        out.clear();
        for (;;) {
            DWORD available = 0;
            if (!WinHttpQueryDataAvailable(request, &available) || available == 0)
                break;
            std::string chunk((size_t)available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read)) {
                ok = false;
                break;
            }
            out.append(chunk.data(), (size_t)read);
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return ok;
}

void EventsStart(HWND notify) {
    g_notify = notify;
    g_stop = false;
    g_reader = std::thread(ReaderMain);
}

void EventsStop() {
    g_stop = true;
    StreamClose();
    if (g_reader.joinable()) g_reader.join();
}
