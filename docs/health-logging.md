# Health logging & diagnostics

The OBS2MF broker writes a fresh log per launch to
`%LOCALAPPDATA%\OBS2MF\logs\broker-<YYYYMMDD-HHMMSS>.log` (the newest 15 are kept, older ones
pruned). Alongside tray actions and source transitions, it emits a periodic **`health:`** line
(~every 60 s) and, on a crash, a `FATAL:` line, a `crash-state:` health snapshot, and a
minidump. This document defines every field on the health line and how to use them to diagnose
leaks and crashes.

## Example line

```
health: priv=3.5MB ws=12.3MB gdi=7 user=4 handles=232 | sysUsed=46% sysFree=17282MB | clients=0 cam=0 src=0 obsFrames=0 published=0
```

The same fields appear on the `crash-state:` line captured at the moment of an unhandled crash.

## Field reference

| Field | Source (Win32) | Scope | Meaning |
|---|---|---|---|
| `priv` | `PROCESS_MEMORY_COUNTERS_EX.PrivateUsage` | This process | Private committed bytes (MB) — memory belonging to the broker alone. **Primary memory-leak indicator.** |
| `ws` | `PROCESS_MEMORY_COUNTERS_EX.WorkingSetSize` | This process | Working set (MB) — memory resident in physical RAM right now. Noisy; use for footprint, not leak detection. |
| `gdi` | `GetGuiResources(GR_GDIOBJECTS)` | This process | GDI object count (DCs, bitmaps, brushes, pens, fonts, regions). Hard cap ~10,000/process. **Graphics-handle-leak indicator.** |
| `user` | `GetGuiResources(GR_USEROBJECTS)` | This process | USER object count (windows, menus, cursors, icons, hooks). Hard cap ~10,000/process. UI-handle-leak indicator. |
| `handles` | `GetProcessHandleCount` | This process | Open kernel handle count (files, threads, events, mutexes, pipe instances, reg keys). Kernel-handle-leak indicator. |
| `sysUsed` | `MEMORYSTATUSEX.dwMemoryLoad` | Whole machine | Percent of total physical RAM in use across all processes. |
| `sysFree` | `MEMORYSTATUSEX.ullAvailPhys` | Whole machine | Available physical RAM (MB) machine-wide. |
| `clients` | `FramePipeServer::ClientCount()` | Broker state | Number of apps currently streaming the camera over the frame pipe (Edge, TellerNow, Camera app…). |
| `cam` | `g_camRunning` | Broker state | `1` if the virtual camera is created and started, else `0`. |
| `src` | `g_effectiveSource` (`SourceKind`) | Broker state | Effective source being served: `0`=None, `1`=OBS, `2`=Test pattern. Reflects auto-fallback, not just the menu choice. |
| `obsFrames` | `ObsCapture::FramesReceived()` | Broker activity | Cumulative NV12 frames pulled from the OBS Virtual Camera since capture started. |
| `published` | `g_framesPublished` | Broker activity | Cumulative frames pushed into the pipe's newest-frame slot (~30/s). The producer thread's heartbeat. |

## Detailed notes

### Per-process memory: `priv` vs `ws`
- **`priv` (Private Bytes)** is memory committed to the broker alone (heaps, stacks), not shared
  with other processes. If code allocates and never frees, `priv` rises monotonically and never
  returns — this is the clearest **memory leak** signal. Over a healthy multi-hour session it
  should stay roughly flat.
- **`ws` (Working Set)** is the portion of memory currently held in physical RAM. Windows pages
  it in and out under pressure, so it fluctuates and is unreliable for leak detection. If `priv`
  keeps climbing while `ws` doesn't, the leaked memory was simply paged out — the leak is still
  real. **Watch `priv` for leaks; read `ws` as the instantaneous RAM footprint.**

### Graphics/UI handles: `gdi` and `user`
Both are capped at roughly **10,000 objects per process**. When a program creates them (loading
images, drawing, opening windows/menus) and forgets to release them, the count climbs toward the
cap and then graphics/UI calls start failing — frequently surfacing as a **misleading
"Out of memory"** error.

> This is exactly the signature of the TellerNow crash: a GDI+ `DrawImage` "Out of memory" while
> loading many check images = classic **GDI-handle exhaustion**, not literal RAM exhaustion.
> Note the broker's media source runs in **svchost**, not inside the consuming app, so a `gdi`
> leak in *TellerNow's* process cannot come from OBS2MF code. Watching the broker's `gdi`/`user`
> here confirms the broker itself is not contributing.

### Kernel handles: `handles`
Covers non-graphics resources — files, threads, sync objects, named-pipe instances, registry
keys. Given the broker's named-pipe frame server, a climbing `handles` count would flag pipe
instances or file handles not being closed as consumers connect and disconnect.

### System-wide: `sysUsed` and `sysFree`
These describe the **whole machine**, not the broker. The crash machine had only 16 GB with
~3 GB free. If `sysUsed` climbs toward ~90 %+ (or `sysFree` collapses toward zero) during a heavy
session, the system is under memory pressure and *any* graphics-heavy process (TellerNow, OBS,
the broker) can fall over — even one that isn't itself leaking. These fields let you separate
"the broker leaked" from "the machine ran out of memory and took everything down."

### Broker activity: `obsFrames` and `published`
- If `src=1` (OBS) but **`obsFrames` stops increasing** between two health lines, OBS has stalled
  or frozen even though the capture graph is nominally running.
- If **`published` stops advancing** between two lines, the broker's producer thread has hung or
  died even though the process is still alive. It is the broker's heartbeat (~1,800/min at 30 fps).

## How to read it

The **absolute numbers matter less than the trend over time.** Compare two health lines an hour
apart:

- **`priv`, `gdi`, `user`, `handles` all flat** → the broker is not leaking. If it still crashed,
  it was a victim of system-wide pressure (check `sysUsed`/`sysFree`) or an external kill.
- **One of them climbing** → the leak is localized to memory / graphics / UI / kernel handles
  respectively. Combine with the minidump and the code path for that resource.
- **`published` frozen** → producer-thread stall, independent of any leak.
- **`sysFree` collapsing while the broker's own numbers stay flat** → the culprit is elsewhere
  (most likely the consuming app's own GDI+/bitmap leak); the broker was just caught in the
  fallout.

## Crash artifacts

On an unhandled exception (access violation, unhandled C++ exception, `std::terminate`,
pure-call, or a CRT invalid parameter) the broker writes, next to the log:

- a **minidump** `broker-<timestamp>.dmp` (open in Visual Studio / WinDbg for the faulting stack),
- a `FATAL:` line naming the exception code and address, and
- a `crash-state:` health snapshot (the fields above) captured at the moment of failure.

Together these turn a previously silent crash into something diagnosable.
