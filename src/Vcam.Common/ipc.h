// OBS2MF - broker <-> media source frame transport over a named pipe.
//
// Why a named pipe (not Global\ shared memory): the tray/broker runs non-elevated and
// lacks SeCreateGlobalPrivilege, so it cannot create Global\ kernel objects that the
// Frame Server (svchost, session 0) could open. A named pipe name is global and can be
// created by a non-elevated process, then opened by the service across the session
// boundary once its DACL grants the service accounts.
//
// Protocol (byte stream, pull model):
//   client -> server : 4-byte request magic 'REQ1'
//   server -> client : FrameHeader, then FrameHeader.byteLength payload bytes
// The server keeps only the newest frame; each request returns that frame (or a header
// with flag_valid clear if none). NV12 payload is tightly packed: stride == width,
// Y plane (stride*height) followed by interleaved UV plane (stride*(height/2)).
#pragma once

#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>

namespace obs2mf {

inline constexpr wchar_t kFramePipeName[] = L"\\\\.\\pipe\\OBS2MF.Frames";
inline constexpr uint32_t kFrameMagic = 0x314D424F;   // 'OBM1'
inline constexpr uint32_t kRequestMagic = 0x31514552; // 'REQ1'
inline constexpr uint32_t kProtoVersion = 1;
inline constexpr uint32_t kFourccNV12 = 0x3231564E;   // 'NV12'

enum SourceKind : uint32_t { SourceNone = 0, SourceOBS = 1, SourceTest = 2, SourceOff = 3 };
enum FrameFlags : uint32_t { flag_valid = 0x1 };

#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic;        // kFrameMagic
    uint32_t version;      // kProtoVersion
    uint32_t fourcc;       // kFourccNV12
    uint32_t width;
    uint32_t height;
    uint32_t stride;       // Y bytes-per-row (== width for our packing)
    uint32_t byteLength;   // payload length in bytes
    uint32_t sequence;
    uint64_t timestampQpc; // QueryPerformanceCounter at capture (system-wide comparable)
    uint32_t sourceKind;   // SourceKind
    uint32_t flags;        // FrameFlags
};
#pragma pack(pop)

// DACL granting SYSTEM, LOCAL SERVICE, NETWORK SERVICE, interactive users and admins.
// SYSTEM = Frame Server Monitor, LOCAL SERVICE = Frame Server, interactive = the broker.
inline bool BuildPipeSecurity(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sdOut) {
    const wchar_t* sddl = L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;NS)(A;;GA;;;IU)(A;;GA;;;BA)";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr))
        return false;
    sdOut = sd;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;
    return true;
}

inline bool ReadExact(HANDLE h, void* buf, DWORD n) {
    BYTE* p = static_cast<BYTE*>(buf);
    DWORD got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(h, p + got, n - got, &r, nullptr) || r == 0) return false;
        got += r;
    }
    return true;
}

inline bool WriteExact(HANDLE h, const void* buf, DWORD n) {
    const BYTE* p = static_cast<const BYTE*>(buf);
    DWORD put = 0;
    while (put < n) {
        DWORD w = 0;
        if (!WriteFile(h, p + put, n - put, &w, nullptr) || w == 0) return false;
        put += w;
    }
    return true;
}

// ---- Client (media source) ---------------------------------------------------
class FramePipeClient {
public:
    ~FramePipeClient() { Close(); }

    // Returns true and fills hdr/payload with the newest frame. On any failure the
    // connection is dropped so the next call transparently reconnects.
    bool Fetch(FrameHeader& hdr, std::vector<BYTE>& payload) {
        if (!EnsureConnected()) return false;
        uint32_t req = kRequestMagic;
        if (!WriteExact(_pipe, &req, sizeof(req))) { Close(); return false; }
        if (!ReadExact(_pipe, &hdr, sizeof(hdr))) { Close(); return false; }
        if (hdr.magic != kFrameMagic) { Close(); return false; }
        if (hdr.byteLength) {
            payload.resize(hdr.byteLength);
            if (!ReadExact(_pipe, payload.data(), hdr.byteLength)) { Close(); return false; }
        } else {
            payload.clear();
        }
        return (hdr.flags & flag_valid) != 0;
    }

    void Close() {
        if (_pipe != INVALID_HANDLE_VALUE) { CloseHandle(_pipe); _pipe = INVALID_HANDLE_VALUE; }
    }

private:
    bool EnsureConnected() {
        if (_pipe != INVALID_HANDLE_VALUE) return true;
        HANDLE h = CreateFileW(kFramePipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD mode = PIPE_READMODE_BYTE;
        SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
        _pipe = h;
        return true;
    }
    HANDLE _pipe = INVALID_HANDLE_VALUE;
};

// ---- Server (broker) ---------------------------------------------------------
// Holds the newest frame and serves it to any number of clients. Thread-safe Publish.
class FramePipeServer {
public:
    ~FramePipeServer() { Stop(); }

    bool Start() {
        if (_running.exchange(true)) return true;
        _accept = std::thread([this] { AcceptLoop(); });
        return true;
    }

    void Stop() {
        if (!_running.exchange(false)) return;
        // Poke the accept loop out of ConnectNamedPipe by connecting once.
        HANDLE h = CreateFileW(kFramePipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (_accept.joinable()) _accept.join();
    }

    void Publish(const FrameHeader& hdr, const void* payload) {
        std::lock_guard<std::mutex> lock(_mtx);
        _hdr = hdr;
        _hdr.magic = kFrameMagic;
        _hdr.version = kProtoVersion;
        const BYTE* p = static_cast<const BYTE*>(payload);
        _payload.assign(p, p + hdr.byteLength);
    }

    int ClientCount() const { return _clients.load(); }

private:
    void AcceptLoop() {
        while (_running.load()) {
            SECURITY_ATTRIBUTES sa{};
            PSECURITY_DESCRIPTOR sd = nullptr;
            SECURITY_ATTRIBUTES* psa = BuildPipeSecurity(sa, sd) ? &sa : nullptr;
            HANDLE pipe = CreateNamedPipeW(
                kFramePipeName,
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                PIPE_UNLIMITED_INSTANCES,
                4 * 1024 * 1024, 64 * 1024, 0, psa);
            if (sd) LocalFree(sd);
            if (pipe == INVALID_HANDLE_VALUE) { Sleep(200); continue; }

            BOOL ok = ConnectNamedPipe(pipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (!_running.load()) { CloseHandle(pipe); break; }
            if (!ok) { CloseHandle(pipe); continue; }

            std::thread([this, pipe] { Serve(pipe); }).detach();
        }
    }

    void Serve(HANDLE pipe) {
        _clients.fetch_add(1);
        for (;;) {
            uint32_t req = 0;
            if (!ReadExact(pipe, &req, sizeof(req))) break;
            if (req != kRequestMagic) break;
            FrameHeader hdr;
            std::vector<BYTE> data;
            {
                std::lock_guard<std::mutex> lock(_mtx);
                hdr = _hdr;
                data = _payload;
            }
            hdr.byteLength = static_cast<uint32_t>(data.size());
            if (!WriteExact(pipe, &hdr, sizeof(hdr))) break;
            if (!data.empty() && !WriteExact(pipe, data.data(), (DWORD)data.size())) break;
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
        _clients.fetch_sub(1);
    }

    std::atomic<bool> _running{false};
    std::atomic<int> _clients{0};
    std::thread _accept;
    std::mutex _mtx;
    FrameHeader _hdr{};
    std::vector<BYTE> _payload;
};

} // namespace obs2mf
