# Module: `src/system/` — process, privilege, single-instance helpers

Native C++ free-function namespaces.

---

## PrivilegeInfo.h / PrivilegeInfo.cpp

| Field | Content |
|---|---|
| Purpose | Report current process token security context |
| Functions | `IsCurrentProcessElevatedNative()` → `TokenElevation` query (false on any failure). `GetCurrentProcessIntegrityRid()` → last SID sub-authority of `TokenIntegrityLevel` (0 on failure; two-step size query + `LocalAlloc`) |
| APIs | `OpenProcessToken`, `GetTokenInformation`, `CloseHandle`, `LocalAlloc/LocalFree`, `GetSidSubAuthority(Count)`; links advapi32 |
| Callers | ApplicationController (`Startup_Context` log line, `*_Result` camera log lines) |
| Side effects | None beyond token handles (closed on all paths) |
| Threading | Stateless, thread-safe |

## ProcessUtils.h / ProcessUtils.cpp

| Field | Content |
|---|---|
| Purpose | Force-kill helper for ghost reset |
| Function | `KillHelloFixProcess()` → `system("taskkill /F /IM Windows_Hello_Fix_v2_0.exe /T")`, returns `result == 0` |
| Inputs | None (hard-coded image name) |
| Outputs/Side effects | Spawns cmd.exe via C runtime `system()` — a console window can flash in interactive sessions; **kills every process with that image name including the caller** |
| Callers | ApplicationController ghost-reset branch only |
| Error handling | Exit-code pass-through only |

## SingleInstance.h / SingleInstance.cpp

| Field | Content |
|---|---|
| Purpose | Cross-process single-instance + GUI wake signaling |
| Functions | `CreateAppMutex(outAlreadyExists)` → `CreateMutex(NULL, TRUE, "Global\\WindowsHelloFix_AppMutex")`; already-exists from `GetLastError()==ERROR_ALREADY_EXISTS`. `TrySignalExistingInstance()` → `OpenEvent(EVENT_MODIFY_STATE)` + `SetEvent` + close + 200 ms sleep → true if signaled. `CreateWakeupEvent()` → auto-reset named event `Global\\WindowsHelloFix_WakeupEvent`. `SignalAndCloseWakeEvent(h)`. `ReleaseMutex(h)` → actually only `CloseHandle` (the Win32 `ReleaseMutex` API is never called; harmless since ownership isn't awaited). `CloseHandleIfValid(h)` → **unused helper** |
| Kernel objects | See ../ARCHITECTURE.md §7 |
| Lifetime | Handles owned by ApplicationController; released in Shutdown/finalizer paths |
| Callers | ApplicationController::Initialize, Shutdown, !ApplicationController |
| Error handling | Boolean outcomes; no logging inside module |
