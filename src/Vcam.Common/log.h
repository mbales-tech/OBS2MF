// OBS2MF - minimal thread-safe rotating file logger (header-only).
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <string>
#include <mutex>
#include <cstdio>
#include <cstdarg>

namespace obs2mf {

class Logger {
public:
    // path: full log file path. maxBytes: rotate to .1 when exceeded.
    void Init(const std::wstring& path, size_t maxBytes = 2 * 1024 * 1024) {
        {
            std::lock_guard<std::mutex> lock(_mtx);
            _path = path;
            _maxBytes = maxBytes;
            // ensure directory exists
            auto slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                std::wstring dir = path.substr(0, slash);
                SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            }
        } // release before LogRaw re-locks (std::mutex is non-recursive)
        LogRaw(L"---- log opened ----");
    }

    void Logf(const wchar_t* fmt, ...) {
        wchar_t buf[2048];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
        va_end(args);
        LogRaw(buf);
    }

    const std::wstring& Path() const { return _path; }

private:
    void LogRaw(const wchar_t* msg) {
        std::lock_guard<std::mutex> lock(_mtx);
        if (_path.empty()) return;
        Rotate();
        HANDLE h = CreateFileW(_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return;
        SetFilePointer(h, 0, nullptr, FILE_END);

        SYSTEMTIME st; GetLocalTime(&st);
        wchar_t line[2176];
        int n = _snwprintf_s(line, _countof(line), _TRUNCATE,
            L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%u] %s\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            GetCurrentProcessId(), msg);
        if (n > 0) {
            // write as UTF-8 for readability in any editor
            int bytes = WideCharToMultiByte(CP_UTF8, 0, line, n, nullptr, 0, nullptr, nullptr);
            std::string utf8(bytes, '\0');
            WideCharToMultiByte(CP_UTF8, 0, line, n, utf8.data(), bytes, nullptr, nullptr);
            DWORD written = 0;
            WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
        }
        CloseHandle(h);
    }

    void Rotate() {
        LARGE_INTEGER size{};
        HANDLE h = CreateFileW(_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            GetFileSizeEx(h, &size);
            CloseHandle(h);
        }
        if ((size_t)size.QuadPart > _maxBytes) {
            std::wstring bak = _path + L".1";
            DeleteFileW(bak.c_str());
            MoveFileW(_path.c_str(), bak.c_str());
        }
    }

    std::mutex _mtx;
    std::wstring _path;
    size_t _maxBytes = 2 * 1024 * 1024;
};

// Helper: expand a known folder + subpath into a full path.
inline std::wstring KnownFolderPath(REFKNOWNFOLDERID id, const wchar_t* sub) {
    PWSTR p = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) {
        result = p;
        CoTaskMemFree(p);
        result += L"\\";
        result += sub;
    }
    return result;
}

} // namespace obs2mf
