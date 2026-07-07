# OBS2MF

A Windows 11 **Media Foundation virtual camera** that exposes the **OBS Virtual Camera**
(or a built-in animated test pattern) to any app — including Media Foundation apps and
browsers — as a system camera named **"OBS2MF"** (shown by Windows as
*"OBS2MF (Windows Virtual Camera)"*), with a system-tray control panel.

Built for Windows 11 (x64) with Microsoft-only dependencies. Requires the Windows 11 SDK
(10.0.26100+) and Visual Studio 2026.

## Architecture

```
OBS Studio ──(OBS Virtual Camera)──►  Vcam.Broker.exe  (tray app, your session)
                                        • captures OBS Virtual Camera via DirectShow (NV12 -> 1280x720)
                                        • animated test pattern
                                        • MFCreateVirtualCamera (Session / CurrentUser)
                                        • named-pipe frame server  \\.\pipe\OBS2MF.Frames
                                                 │
Apps / Edge ──► Frame Server (svchost) ──► Vcam.MediaSource.dll  (64-bit COM, HKLM)
                                        • IMFMediaSource, NV12-only
                                        • pipe client -> injects broker frames
                                        • "waiting for service" fallback pattern
```

- `MFCreateVirtualCamera` is the only exposure that reaches Media Foundation apps; a pure
  DirectShow softcam is not reliably visible to them. The media source runs inside the Frame
  Server service, so frames are handed to it from the broker over a named pipe (a non-elevated
  tray app can't create `Global\` shared memory).
- Output is **NV12** (verified accepted by the target 32-bit app and browsers — MJPEG is not
  required).
- OBS's virtual camera is a **DirectShow-only** device (invisible to Media Foundation), so the
  broker captures it with a DirectShow graph and forwards frames over the pipe.

## Layout

| Path | What |
|---|---|
| `src/Vcam.Common/` | Header-only shared code: `version.h`, `guids.h` (frozen CLSID), `ipc.h` (pipe protocol), `log.h` |
| `src/Vcam.MediaSource/` | 64-bit COM DLL (the virtual-camera media source) — derived from smourier/VCamSample (MIT) |
| `src/Vcam.Broker/` | 64-bit tray app: DirectShow OBS capture, test pattern, pipe server, camera lifetime, tray UI (source switch, auto-start, log), logging |
| `installer/OBS2MF.nsi` | NSIS installer |

## Build

```powershell
# one-time NuGet restore (WIL + C++/WinRT), installs into src\packages
nuget restore OBS2MF.sln
# build
msbuild OBS2MF.sln /t:Build /p:Configuration=Release /p:Platform=x64
```
Outputs: `x64\Release\Vcam.MediaSource.dll` and `x64\Release\Vcam.Broker.exe`.

## Installer

```powershell
& "C:\Program Files (x86)\NSIS\makensis.exe" installer\OBS2MF.nsi
```
Produces `installer\OBS2MF-Setup-<version>.exe`, which installs to `%ProgramFiles%\OBS2MF`,
registers the media source DLL (HKLM, elevated), and adds **all-users** Start-menu + Desktop
shortcuts. On upgrade/uninstall it stops the Windows Camera Frame Server so the in-use DLL can
be replaced/removed.

## Tray controls

Launch `Vcam.Broker.exe` (Start-menu / Desktop shortcut). The camera exists only while the
broker runs (Session lifetime). Right-click the tray icon:

- **Camera active / stopped** — create or remove the virtual camera.
- **Source: OBS Virtual Camera** *(default)* — live OBS; auto-falls back to the test pattern
  when OBS's virtual camera isn't running.
- **Source: Test pattern** — animated NV12 color bars.
- **Start with Windows** — per-user auto-start at sign-in (`HKCU\...\Run`).
- **Open log file** — opens the current session log (see below).
- **Exit**.

## Logging & diagnostics

Each launch writes a fresh log to `%LOCALAPPDATA%\OBS2MF\logs\broker-<YYYYMMDD-HHMMSS>.log`
(the 15 most recent are kept; older ones are pruned, so files never grow unbounded or get
overwritten). Beyond tray actions, the broker logs:

- an `env:` line at startup (Windows build, total/free RAM, exe path);
- a periodic `health:` line (~every 60 s) with process private/working-set memory, **GDI /
  USER object counts**, handle count, system memory load, consumer count, and frame counters —
  the signal for spotting a slow leak over a long session;
- source transitions (OBS going live / auto-fallback to the test pattern) and consumer
  connect/disconnect events.

On an unhandled crash (access violation, C++ exception, `std::terminate`, pure-call, or a CRT
invalid parameter) the broker writes a **minidump** next to the log
(`%LOCALAPPDATA%\OBS2MF\logs\broker-<timestamp>.dmp`) plus a `FATAL:` line and a final
`crash-state:` health snapshot, so a previously silent crash is now diagnosable.

See [docs/health-logging.md](docs/health-logging.md) for a full definition of every `health:`
field (`priv`, `ws`, `gdi`, `user`, `handles`, …) and how to use them to diagnose leaks.

## Releasing

Installers are published as **GitHub Release assets** (download page:
`https://github.com/mbales-tech/OBS2MF/releases`). One-time: `gh auth login`. Then, after
bumping the version in `src/Vcam.Common/version.h` **and** `installer/OBS2MF.nsi`:

```powershell
pwsh -File scripts/release.ps1
```

This reads the version from `version.h`, rebuilds the solution + installer, creates the
`v<version>` tag/Release, and uploads `OBS2MF-Setup-<version>.exe` as the asset.

## Manual dev registration (without the installer)

The media source DLL must live where the Frame Server service accounts can read it (not under
`C:\Users\<you>`), and must be registered in HKLM as administrator:

```powershell
Start-Process regsvr32 -Verb RunAs -ArgumentList '/s','"C:\Users\Public\OBS2MF\Vcam.MediaSource.dll"'
```

Then run `Vcam.Broker.exe`. The camera exists only while the broker runs (Session lifetime).

Logs: `%LOCALAPPDATA%\OBS2MF\logs\broker-<timestamp>.log` (tray → *Open log file*). See
**Logging & diagnostics** above.

## Versioning

`src/Vcam.Common/version.h` is the single source of the SemVer number. The CLSID in
`guids.h` is **frozen** and must never change (the camera's identity depends on it).
