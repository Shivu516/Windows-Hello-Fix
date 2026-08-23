# Module: `src/config/` — persistence and diagnostic log

Managed (C++/CLI) static classes. Only writers/readers of
`%APPDATA%\Windows Hello Fix\`.

---

## ConfigPaths.h / ConfigPaths.cpp

| Field | Content |
|---|---|
| Purpose | Resolve persistent file paths |
| Functions | `GetConfigFilePath()` → `%APPDATA%\Windows Hello Fix\config.txt`; `GetDiagnosticLogFilePath()` → same dir + `diagnostic.log` |
| Side effects | `Directory::CreateDirectory` on every call (idempotent); diagnostic path derives its directory from the config path with a fallback re-derivation |
| APIs | `Environment::GetFolderPath(ApplicationData)`, `System::IO::Path`, `System::IO::Directory` |
| Error handling | None — exceptions propagate to callers (which all swallow) |
| Callers | ConfigStore only |

## ConfigStore.h / ConfigStore.cpp

| Field | Content |
|---|---|
| Purpose | config.txt read/write + the application's single file logger |
| State | `static Object^ s_diagnosticLogSync` — lock object serializing all log writes |

**Functions**

- `WriteDiagnosticLog(eventName, targetState, verificationPass)`:
  `Monitor::Enter` → append line
  `<timestamp> \| Event=<name> \| Target=<state> \| Verify=PASS\|FAIL`
  → close → swallow any exception → release lock. Timestamp format
  `yyyy-MM-dd HH:mm:ss.fff`.
- `WriteDiagnosticLogWithDevice(eventName, std::wstring id, state, pass)`:
  marshals id and appends `\| Device=<id>` **into the event-name field**, then
  delegates.
- `SaveConfigState(monitoring, deviceInstanceId)`: overwrite config.txt with
  two lines `monitoring=0\|1` and `device=<id>`; exceptions swallowed.
- `LoadConfigState(out deviceInstanceId)` → returns bool "monitoring active":
  reads exactly two lines; line1 must equal `monitoring=1` (trimmed); line2
  must start with `device=`; value is trimmed then passed through native
  `TrimTrailingChars` (guards against `\r\n` corruption in C++ comparisons).
  Missing file or parse issues → false, empty id.
- `EnsureConfigFileExists(deviceInstanceId)`: creates config via
  SaveConfigState(false, …) when absent.

**Inputs/outputs**: text files described above; no registry, no network.

**Threading**: log writes thread-safe; config read/write not locked but only
accessed from UI thread today.

**Callers**: ApplicationController (log lines everywhere, save/load),
MyForm_Load (`LoadConfigState` ×2, `EnsureConfigFileExists`).

**Logging behavior note**: every event name/target/verify string used by the
app flows through this class — catalog in ../LOGGING.md.

**Error handling pattern**: blanket `catch (...) {}` — silent failure is a
deliberate design choice here; a broken `%APPDATA%` degrades to no logs and
default config without crashing.
