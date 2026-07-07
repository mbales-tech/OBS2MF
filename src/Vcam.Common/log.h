// OBS2MF - minimal thread-safe rotating file logger (header-only).
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <string>
#include <vector>
#include <algorithm>
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

    // Start a fresh, uniquely-named log for this launch: `dir\prefix-YYYYMMDD-HHMMSS.log`.
    // Every launch gets its own file (nothing is appended to or overwritten from a prior run),
    // and older `prefix-*.log` files are pruned so at most `keep` remain. `dir` is remembered
    // (Dir()) so callers can drop sibling files such as crash dumps next to the active log.
    void InitSession(const std::wstring& dir, const std::wstring& prefix,
                     size_t keep = 15, size_t maxBytes = 16 * 1024 * 1024) {
        {
            std::lock_guard<std::mutex> lock(_mtx);
            _maxBytes = maxBytes;
            _dir = dir;
            _prefix = prefix;
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            PruneOld(keep); // trim previous sessions before adding this one

            SYSTEMTIME st; GetLocalTime(&st);
            wchar_t name[MAX_PATH];
            _snwprintf_s(name, _countof(name), _TRUNCATE,
                L"%s\\%s-%04u%02u%02u-%02u%02u%02u.log",
                dir.c_str(), prefix.c_str(),
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            _path = name;
        } // release before LogRaw re-locks (std::mutex is non-recursive)
        LogRaw(L"---- log opened ----");
    }

    const std::wstring& Dir() const { return _dir; }

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

    // Delete oldest `prefix-*.log` files so that after this launch creates a new one, at
    // most `keep` session logs exist. Filenames embed a zero-padded YYYYMMDD-HHMMSS stamp,
    // so a lexical sort is chronological. Caller holds _mtx.
    void PruneOld(size_t keep) {
        if (_dir.empty() || _prefix.empty()) return;
        std::wstring pattern = _dir + L"\\" + _prefix + L"-*.log";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        std::vector<std::wstring> files;
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                files.push_back(fd.cFileName);
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        std::sort(files.begin(), files.end());
        const size_t keepOld = keep > 0 ? keep - 1 : 0; // leave room for the new session file
        if (files.size() <= keepOld) return;
        for (size_t i = 0, n = files.size() - keepOld; i < n; ++i)
            DeleteFileW((_dir + L"\\" + files[i]).c_str());
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
    std::wstring _dir;
    std::wstring _prefix;
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
