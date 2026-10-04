#include "CameraRecoveryFailsafe.h"
#include "../core/MyForm.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <msclr\marshal_cppstd.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfcore.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

// ====== NATIVE WORKER PLUMBING ======
// Both potentially blocking operations — the Media Foundation health check and the
// recovery rungs — run on dedicated native threads so the UI/message pump never blocks
// (WndProc must stay responsive for session/power events). The managed state machine
// polls each worker's done-event from a WinForms timer; on budget exhaustion it marks
// the context orphaned and abandons it. A worker is never killed mid-call: an abandoned
// worker self-cleans if it ever completes, otherwise leaks by design (bounded, one per
// timeout). Every worker writes results BEFORE its final SetEvent and touches nothing
// afterwards, so the orchestrator can free the context safely once the event fires.

struct CameraHealthWorkContext {
    std::wstring targetInstanceId;
    Windows_Hello_Fix_v2_0::CameraHealthStatus status;
    HRESULT hr;
    ULONGLONG durationMs;
    HANDLE doneEvent;
    volatile LONG orphaned;
};

struct CameraRungWorkContext {
    int rung;
    std::wstring targetId;
    bool ok;
    std::wstring detail;
    HANDLE doneEvent;
    volatile LONG orphaned;
};

// MF device symbolic links look like
//   \\?\usb#vid_04f2&pid_b829&mi_00#6&321dd860&1&0000#{e5323777-...}
// while the configured identity is the device instance ID
//   USB\VID_04F2&PID_B829&MI_00\6&321DD860&1&0000
// Match case-insensitively with '\' mapped to '#', substring direction link-contains-target.
static std::wstring NormalizeForMatch(const std::wstring& value) {
    std::wstring out = value;
    for (size_t i = 0; i < out.size(); i++) {
        wchar_t c = out[i];
        if (c == L'\\') {
            out[i] = L'#';
        } else if (c >= L'A' && c <= L'Z') {
            out[i] = c - L'A' + L'a';
        }
    }
    return out;
}

static bool SymbolicLinkMatchesTarget(const std::wstring& symbolicLink, const std::wstring& targetInstanceId) {
    if (targetInstanceId.empty()) {
        return false;
    }
    std::wstring link = NormalizeForMatch(symbolicLink);
    std::wstring target = NormalizeForMatch(targetInstanceId);
    return link.find(target) != std::wstring::npos;
}

// Health-check worker — Level 2+3 per the research: activate the configured target through
// the camera stack and read one frame (the only user-mode proof that the pipeline works).
// No category attribute is set: default VIDCAP enumeration lists normal video cameras
// (including the configured RGB target) and does not list IR/sensor cameras used by
// Windows Hello; the symbolic-link match guarantees no other camera is ever activated.
// MFStartup/MFShutdown are paired on this thread; cleanup runs on every exit path.
static DWORD WINAPI CameraHealthWorkerProc(LPVOID param) {
    CameraHealthWorkContext* ctx = static_cast<CameraHealthWorkContext*>(param);
    ULONGLONG startTick = GetTickCount64();
    ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::Error;
    ctx->hr = S_OK;

    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        ctx->hr = hr;
    } else {
        IMFAttributes* pAttributes = nullptr;
        IMFActivate** ppActivates = nullptr;
        UINT32 activateCount = 0;
        IMFMediaSource* pSource = nullptr;
        IMFSourceReader* pReader = nullptr;

        do {
            hr = MFCreateAttributes(&pAttributes, 2);
            if (FAILED(hr)) { ctx->hr = hr; break; }

            hr = pAttributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
            if (FAILED(hr)) { ctx->hr = hr; break; }

            hr = MFEnumDeviceSources(pAttributes, &ppActivates, &activateCount);
            if (FAILED(hr)) { ctx->hr = hr; break; }

            int matchedIndex = -1;
            for (UINT32 i = 0; i < activateCount && matchedIndex < 0; i++) {
                LPWSTR symbolicLink = nullptr;
                UINT32 symbolicLinkLength = 0;
                if (SUCCEEDED(ppActivates[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink, &symbolicLinkLength))) {
                    if (symbolicLink != nullptr && SymbolicLinkMatchesTarget(symbolicLink, ctx->targetInstanceId)) {
                        matchedIndex = static_cast<int>(i);
                    }
                    CoTaskMemFree(symbolicLink);
                }
            }

            if (matchedIndex < 0) {
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::NotFound;
                ctx->hr = S_OK;
                break;
            }

            hr = ppActivates[matchedIndex]->ActivateObject(IID_IMFMediaSource, (void**)&pSource);
            if (hr == E_ACCESSDENIED) {
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::PrivacyDenied;
                ctx->hr = hr;
                break;
            }
            if (FAILED(hr)) {
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::Wedged;
                ctx->hr = hr;
                break;
            }

            hr = MFCreateSourceReaderFromMediaSource(pSource, nullptr, &pReader);
            if (FAILED(hr)) {
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::Wedged;
                ctx->hr = hr;
                break;
            }

            // Synchronous single-frame read: blocks until the first frame or an error.
            // A wedged camera can block indefinitely — the orchestrator owns the deadline
            // and abandons this thread past its budget.
            hr = pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::Healthy;
                ctx->hr = S_OK;
            } else if (hr == E_ACCESSDENIED) {
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::PrivacyDenied;
                ctx->hr = hr;
            } else if (hr == MF_E_VIDEO_DEVICE_LOCKED || hr == MF_E_HW_MFT_FAILED_START_STREAMING) {
                // Device responds but another client holds it — busy is healthy, never
                // recovered over.
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::Busy;
                ctx->hr = hr;
            } else if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED || hr == HRESULT_FROM_WIN32(ERROR_DEVICE_REMOVED)) {
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::NotFound;
                ctx->hr = hr;
            } else {
                ctx->status = Windows_Hello_Fix_v2_0::CameraHealthStatus::Wedged;
                ctx->hr = hr;
            }
        } while (false);

        // Cleanup on every path (doc-mandated: IMFMediaSource::Shutdown prevents the
        // system from holding device references).
        if (pReader != nullptr) {
            pReader->Release();
        }
        if (pSource != nullptr) {
            pSource->Shutdown();
            pSource->Release();
        }
        if (ppActivates != nullptr) {
            for (UINT32 i = 0; i < activateCount; i++) {
                ppActivates[i]->Release();
            }
            CoTaskMemFree(ppActivates);
        }
        if (pAttributes != nullptr) {
            pAttributes->Release();
        }
        MFShutdown();
    }

    ctx->durationMs = GetTickCount64() - startTick;
    if (InterlockedCompareExchange(&ctx->orphaned, 0, 0) == 0) {
        SetEvent(ctx->doneEvent);
    } else {
        CloseHandle(ctx->doneEvent);
        delete ctx;
    }
    return 0;
}

// Recovery-rung worker — calls only the src/core primitives (single device/system
// authority); rung 3 deliberately reuses the existing full-cycle pipeline untouched.
static DWORD WINAPI CameraRungWorkerProc(LPVOID param) {
    CameraRungWorkContext* ctx = static_cast<CameraRungWorkContext*>(param);
    ctx->ok = false;

    switch (ctx->rung) {
        case 1:
            ctx->ok = RestartCameraHardware(ctx->targetId);
            break;
        case 2:
            ctx->ok = RestartCameraFrameServerService();
            break;
        case 3:
            ctx->ok = RecoverCameraHardware(ctx->targetId, true);
            break;
        case 4: {
            std::wstring parentId;
            std::wstring siblingStatus;
            ctx->ok = ReenumerateCameraParent(ctx->targetId, parentId, siblingStatus);
            ctx->detail = L"Parent=" + parentId + L" | " + siblingStatus;
            break;
        }
        default:
            break;
    }

    if (InterlockedCompareExchange(&ctx->orphaned, 0, 0) == 0) {
        SetEvent(ctx->doneEvent);
    } else {
        CloseHandle(ctx->doneEvent);
        delete ctx;
    }
    return 0;
}

namespace Windows_Hello_Fix_v2_0 {

    static System::String^ HealthStatusText(CameraHealthStatus status) {
        switch (status) {
            case CameraHealthStatus::Healthy: return L"Healthy";
            case CameraHealthStatus::NotFound: return L"NotFound";
            case CameraHealthStatus::PrivacyDenied: return L"PrivacyDenied";
            case CameraHealthStatus::Busy: return L"Busy";
            case CameraHealthStatus::Wedged: return L"Wedged";
            case CameraHealthStatus::Timeout: return L"Timeout";
            case CameraHealthStatus::Error: return L"Error";
        }
        return L"Unknown";
    }

    // File-local logging helper (free function: MyForm::DiagLevel needs the complete type,
    // which is available in this .cpp but not in the forward-declaring header).
    static void WriteCamRecLog(MyForm^ owner, MyForm::DiagLevel level, System::String^ eventName,
        System::String^ opId, System::String^ details, std::wstring targetInstanceId,
        System::String^ targetState, bool verificationPass)
    {
        if (owner == nullptr) return;
        try {
            owner->LogFailsafeExWithDevice(level, L"FAILSAFE", eventName, opId, details, targetInstanceId, targetState, verificationPass);
        } catch (...) {}
    }

    CameraRecoveryFailsafe::CameraRecoveryFailsafe(MyForm^ ownerForm)
        : owner(ownerForm)
        , state(RecoveryState::Idle)
        , currentRung(0)
        , incidentOp(nullptr)
        , incidentStartTick(0)
        , workerDeadlineTick(0)
        , lastHealthStatus(CameraHealthStatus::Error)
        , lastHealthHr(S_OK)
        , lastCheckTick(0)
        , parkedUntilTick(0)
        , prevExpectedDisabledObserved(false)
        , notFoundRecheckQueued(false)
        , isArmed(false)
        , pendingGapAction(GapAction::None)
        , healthContext(nullptr)
        , healthThread(nullptr)
        , rungContext(nullptr)
        , rungThread(nullptr)
    {
        transitionTimer = gcnew System::Windows::Forms::Timer();
        transitionTimer->Interval = kTransitionPollMs;
        transitionTimer->Tick += gcnew System::EventHandler(this, &CameraRecoveryFailsafe::OnTransitionTick);

        backupTimer = gcnew System::Windows::Forms::Timer();
        backupTimer->Interval = kBackupPollMs;
        backupTimer->Tick += gcnew System::EventHandler(this, &CameraRecoveryFailsafe::OnBackupTick);

        startupTimer = gcnew System::Windows::Forms::Timer();
        startupTimer->Interval = kStartupCheckMs;
        startupTimer->Tick += gcnew System::EventHandler(this, &CameraRecoveryFailsafe::OnStartupTick);

        resultTimer = gcnew System::Windows::Forms::Timer();
        resultTimer->Interval = kResultPollMs;
        resultTimer->Tick += gcnew System::EventHandler(this, &CameraRecoveryFailsafe::OnResultTick);

        gapTimer = gcnew System::Windows::Forms::Timer();
        gapTimer->Interval = kGapMs;
        gapTimer->Tick += gcnew System::EventHandler(this, &CameraRecoveryFailsafe::OnGapTick);
    }

    CameraRecoveryFailsafe::~CameraRecoveryFailsafe()
    {
        Disarm();
        this->!CameraRecoveryFailsafe();
    }

    CameraRecoveryFailsafe::!CameraRecoveryFailsafe()
    {
        try { Disarm(); } catch (...) {}
    }

    void CameraRecoveryFailsafe::Arm()
    {
        if (isArmed) return;
        if (owner != nullptr && owner->IsSystemEndingActive()) return;

        isArmed = true;
        state = RecoveryState::Idle;
        currentRung = 0;
        incidentOp = nullptr;
        lastCheckTick = 0;
        parkedUntilTick = 0;
        prevExpectedDisabledObserved = false;
        notFoundRecheckQueued = false;
        pendingGapAction = GapAction::None;

        try {
            WriteCamRecLog(owner, MyForm::DiagLevel::Info, L"CameraRecovery_Start",
                owner->NewOperationId(L"CAMREC"), L"", std::wstring(), L"Enabled", true);
        } catch (...) {}

        transitionTimer->Interval = kTransitionPollMs;
        transitionTimer->Start();
        backupTimer->Interval = kBackupPollMs;
        backupTimer->Start();
        startupTimer->Interval = kStartupCheckMs;
        startupTimer->Start();
        resultTimer->Stop();
        gapTimer->Stop();
    }

    void CameraRecoveryFailsafe::Disarm()
    {
        isArmed = false;
        state = RecoveryState::Idle;
        currentRung = 0;
        pendingGapAction = GapAction::None;
        try { if (transitionTimer != nullptr) transitionTimer->Stop(); } catch (...) {}
        try { if (backupTimer != nullptr) backupTimer->Stop(); } catch (...) {}
        try { if (startupTimer != nullptr) startupTimer->Stop(); } catch (...) {}
        try { if (resultTimer != nullptr) resultTimer->Stop(); } catch (...) {}
        try { if (gapTimer != nullptr) gapTimer->Stop(); } catch (...) {}
        OrphanHealthWorker();
        OrphanRungWorker();
    }

    void CameraRecoveryFailsafe::OnOwnerLoad(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        try { Arm(); } catch (...) {}
    }

    void CameraRecoveryFailsafe::OnOwnerClosing(System::Object^ /*sender*/, System::Windows::Forms::FormClosingEventArgs^ e)
    {
        // Hide-to-background closes are cancelled by MyForm (CloseReason::UserClosing) —
        // the daemon keeps running, so the watchdog keeps running too. Disarm only on
        // real shutdowns (application exit, Windows shutdown, task-manager close).
        if (e != nullptr && e->CloseReason == System::Windows::Forms::CloseReason::UserClosing) {
            return;
        }
        try { Disarm(); } catch (...) {}
    }

    bool CameraRecoveryFailsafe::IsExpectedEnabled()
    {
        if (owner == nullptr) return false;
        if (owner->IsSystemEndingActive()) return false;
        if (!owner->IsMonitoringActive()) return false;
        return owner->IsCameraExpectedEnabled();
    }

    bool CameraRecoveryFailsafe::TryGetTargetId(std::wstring& targetId)
    {
        if (owner == nullptr) return false;
        return owner->TryGetFailsafeTargetId(targetId);
    }

    void CameraRecoveryFailsafe::OrphanHealthWorker()
    {
        if (healthContext != nullptr) {
            InterlockedExchange(&static_cast<CameraHealthWorkContext*>(healthContext)->orphaned, 1);
            healthContext = nullptr;
        }
        healthThread = nullptr;
    }

    void CameraRecoveryFailsafe::OrphanRungWorker()
    {
        if (rungContext != nullptr) {
            InterlockedExchange(&static_cast<CameraRungWorkContext*>(rungContext)->orphaned, 1);
            rungContext = nullptr;
        }
        rungThread = nullptr;
    }

    void CameraRecoveryFailsafe::OnTransitionTick(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        if (!isArmed) return;
        if (owner == nullptr) return;
        if (owner->IsSystemEndingActive()) return;

        if (!owner->IsMonitoringActive() || !IsExpectedEnabled()) {
            // Locked/suspended/shutdown/monitoring-off — nothing runs here. Remember the
            // expected-disabled episode so the coming unlock/resume counts as an enable
            // transition, and clear exhaustion parking (new episode = fresh allowance).
            prevExpectedDisabledObserved = true;
            parkedUntilTick = 0;
            return;
        }

        if (state != RecoveryState::Idle) return;

        std::wstring targetId;
        if (!TryGetTargetId(targetId) || targetId.empty()) return;

        bool pnpDisabled = false;
        if (!GetCameraHardwareDisabledState(targetId, pnpDisabled)) return;
        if (pnpDisabled) {
            prevExpectedDisabledObserved = true;
            return;
        }

        if (prevExpectedDisabledObserved) {
            // Camera just became expected-enabled AND PnP-enabled (unlock / resume /
            // startup enable) — the primary health-check trigger.
            prevExpectedDisabledObserved = false;
            RequestHealthCheck(L"EnableTransition", false);
        }
    }

    void CameraRecoveryFailsafe::OnBackupTick(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        if (!isArmed) return;
        RequestHealthCheck(L"Backup", false);
    }

    void CameraRecoveryFailsafe::OnStartupTick(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        startupTimer->Stop();
        if (!isArmed) return;
        RequestHealthCheck(L"Startup", false);
    }

    void CameraRecoveryFailsafe::OnGapTick(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        gapTimer->Stop();
        if (!isArmed) {
            pendingGapAction = GapAction::None;
            return;
        }
        GapAction action = pendingGapAction;
        pendingGapAction = GapAction::None;

        if (action == GapAction::PostRungCheck && currentRung > 0) {
            RequestHealthCheck(System::String::Format(L"PostRung{0}", currentRung), true);
        } else if (action == GapAction::NotFoundRecheck) {
            RequestHealthCheck(L"NotFoundRecheck", true);
        }
    }

    void CameraRecoveryFailsafe::RequestHealthCheck(System::String^ trigger, bool bypassCooldown)
    {
        if (!isArmed || state != RecoveryState::Idle) return;
        if (owner == nullptr) return;
        if (owner->IsSystemEndingActive()) return;
        if (!owner->IsMonitoringActive()) return;
        if (!IsExpectedEnabled()) return;
        if (GetTickCount64() < parkedUntilTick) return;

        ULONGLONG nowTick = GetTickCount64();
        if (!bypassCooldown && lastCheckTick != 0 && (nowTick - lastCheckTick) < kCheckCooldownMs) {
            return;
        }

        std::wstring targetId;
        if (!TryGetTargetId(targetId) || targetId.empty()) return;

        // PnP pre-check: a disabled/absent devnode belongs to the PnP-level watchdogs;
        // this failsafe only judges a PnP-enabled camera.
        bool pnpDisabled = false;
        bool pnpKnown = GetCameraHardwareDisabledState(targetId, pnpDisabled);
        if (pnpKnown && pnpDisabled) {
            prevExpectedDisabledObserved = true;
            WriteCamRecLog(owner, MyForm::DiagLevel::Debug, L"CameraRecovery_SkippedPnpDisabled", incidentOp,
                System::String::Format(L"Trigger={0}", trigger), targetId, L"NoChange", true);
            if (currentRung > 0) {
                // Post-rung the devnode is still/again disabled — the rung did not stick.
                AdvanceOrExhaust();
            }
            return;
        }
        prevExpectedDisabledObserved = false;
        lastCheckTick = nowTick;

        if (currentRung == 0) {
            // New routine check — fresh operation id. Post-rung checks keep the incident id.
            incidentOp = owner->NewOperationId(L"CAMREC");
        }

        SpawnHealthWorker(targetId, trigger);
    }

    void CameraRecoveryFailsafe::SpawnHealthWorker(std::wstring targetId, System::String^ trigger)
    {
        if (healthContext != nullptr) return;

        CameraHealthWorkContext* ctx = new CameraHealthWorkContext();
        ctx->targetInstanceId = targetId;
        ctx->status = CameraHealthStatus::Error;
        ctx->hr = S_OK;
        ctx->durationMs = 0;
        ctx->orphaned = 0;
        ctx->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ctx->doneEvent == NULL) {
            delete ctx;
            WriteCamRecLog(owner, MyForm::DiagLevel::Error, L"CameraRecovery_HealthCheckResult", incidentOp,
                System::String::Format(L"Trigger={0} | Error=WorkerSpawnFailed", trigger), targetId, L"NoChange", false);
            return;
        }

        HANDLE thread = CreateThread(nullptr, 0, CameraHealthWorkerProc, ctx, 0, nullptr);
        if (thread == NULL) {
            CloseHandle(ctx->doneEvent);
            delete ctx;
            WriteCamRecLog(owner, MyForm::DiagLevel::Error, L"CameraRecovery_HealthCheckResult", incidentOp,
                System::String::Format(L"Trigger={0} | Error=WorkerSpawnFailed", trigger), targetId, L"NoChange", false);
            return;
        }

        healthContext = ctx;
        healthThread = thread;
        workerDeadlineTick = GetTickCount64() + kHealthBudgetMs;
        state = RecoveryState::Checking;
        resultTimer->Start();

        WriteCamRecLog(owner, MyForm::DiagLevel::Info, L"CameraRecovery_HealthCheck", incidentOp,
            System::String::Format(L"Trigger={0}", trigger), targetId, L"NoChange", true);
    }

    void CameraRecoveryFailsafe::OnResultTick(System::Object^ /*sender*/, System::EventArgs^ /*e*/)
    {
        if (!isArmed) return;

        if (state == RecoveryState::Checking && healthContext != nullptr) {
            CameraHealthWorkContext* ctx = static_cast<CameraHealthWorkContext*>(healthContext);

            if (WaitForSingleObject(ctx->doneEvent, 0) == WAIT_OBJECT_0) {
                CameraHealthStatus status = ctx->status;
                HRESULT hr = ctx->hr;
                int durationMs = static_cast<int>(ctx->durationMs);
                CloseHandle(ctx->doneEvent);
                CloseHandle(static_cast<HANDLE>(healthThread));
                delete ctx;
                healthContext = nullptr;
                healthThread = nullptr;
                resultTimer->Stop();

                lastHealthStatus = status;
                lastHealthHr = hr;

                std::wstring deviceId;
                TryGetTargetId(deviceId);
                MyForm::DiagLevel level = MyForm::DiagLevel::Info;
                bool pass = true;
                if (status == CameraHealthStatus::NotFound || status == CameraHealthStatus::PrivacyDenied || status == CameraHealthStatus::Busy) {
                    level = MyForm::DiagLevel::Warn;
                } else if (status != CameraHealthStatus::Healthy) {
                    level = MyForm::DiagLevel::Error;
                    pass = false;
                }
                WriteCamRecLog(owner, level, L"CameraRecovery_HealthCheckResult", incidentOp,
                    System::String::Format(L"Result={0} | HRESULT=0x{1:X8} | DurationMs={2}",
                        HealthStatusText(status), static_cast<int>(hr), durationMs),
                    deviceId, L"NoChange", pass);

                HandleHealthResult(status, hr);
            }
            else if (GetTickCount64() > workerDeadlineTick) {
                // Hard budget exceeded — abandon the worker (never killed mid-call; it
                // self-cleans if it ever completes, otherwise leaks by design).
                InterlockedExchange(&ctx->orphaned, 1);
                healthContext = nullptr;
                healthThread = nullptr;
                resultTimer->Stop();

                lastHealthStatus = CameraHealthStatus::Timeout;
                lastHealthHr = HRESULT_FROM_WIN32(WAIT_TIMEOUT);

                std::wstring deviceId;
                TryGetTargetId(deviceId);
                WriteCamRecLog(owner, MyForm::DiagLevel::Error, L"CameraRecovery_HealthCheckResult", incidentOp,
                    System::String::Format(L"Result=Timeout | BudgetMs={0}", kHealthBudgetMs),
                    deviceId, L"NoChange", false);

                HandleHealthResult(CameraHealthStatus::Timeout, lastHealthHr);
            }
            return;
        }

        if (state == RecoveryState::Recovering && rungContext != nullptr) {
            CameraRungWorkContext* ctx = static_cast<CameraRungWorkContext*>(rungContext);

            if (WaitForSingleObject(ctx->doneEvent, 0) == WAIT_OBJECT_0) {
                bool ok = ctx->ok;
                int rung = ctx->rung;
                System::String^ detail = msclr::interop::marshal_as<System::String^>(ctx->detail);
                CloseHandle(ctx->doneEvent);
                CloseHandle(static_cast<HANDLE>(rungThread));
                delete ctx;
                rungContext = nullptr;
                rungThread = nullptr;
                resultTimer->Stop();

                LONG stage = InterlockedCompareExchange(&g_lastHardwareToggleStage, 0, 0);
                LONG setupErr = InterlockedCompareExchange(&g_lastSetupApiError, 0, 0);
                LONG cfgMgr = InterlockedCompareExchange(&g_lastConfigManagerResult, 0, 0);
                LONG propFlags = InterlockedCompareExchange(&g_lastPropChangeFlags, 0, 0);
                LONG serviceState = InterlockedCompareExchange(&g_lastServiceState, 0, 0);
                LONG serviceErr = InterlockedCompareExchange(&g_lastServiceError, 0, 0);

                System::String^ details = System::String::Format(
                    L"Rung={0} | Stage={1} | SetupErr={2} | CfgMgr={3} | PropFlags=0x{4:X8} | ServiceState={5} | ServiceErr={6}",
                    rung, static_cast<int>(stage), static_cast<int>(setupErr), static_cast<int>(cfgMgr),
                    static_cast<int>(propFlags), static_cast<int>(serviceState), static_cast<int>(serviceErr));
                if (detail != nullptr && detail->Length > 0) {
                    details = details + L" | " + detail;
                }

                std::wstring deviceId;
                TryGetTargetId(deviceId);
                WriteCamRecLog(owner, ok ? MyForm::DiagLevel::Info : MyForm::DiagLevel::Error,
                    L"CameraRecovery_RungResult", incidentOp, details, deviceId, L"Enabled", ok);

                HandleRungResult(ok);
            }
            else if (GetTickCount64() > workerDeadlineTick) {
                InterlockedExchange(&ctx->orphaned, 1);
                rungContext = nullptr;
                rungThread = nullptr;
                resultTimer->Stop();

                WriteCamRecLog(owner, MyForm::DiagLevel::Error, L"CameraRecovery_RungTimeout", incidentOp,
                    System::String::Format(L"Rung={0}", currentRung), std::wstring(), L"Enabled", false);
                // A hung device/service operation means the stack is in an unknown state —
                // abort instead of stacking further operations onto it.
                AbortIncident(L"RungTimeout");
            }
        }
    }

    void CameraRecoveryFailsafe::HandleHealthResult(CameraHealthStatus status, HRESULT hr)
    {
        state = RecoveryState::Idle;
        lastHealthStatus = status;
        lastHealthHr = hr;

        // Session state may have changed while the worker was out — never act on a camera
        // that is no longer expected-enabled (Windows Hello may own the device again).
        if (!IsExpectedEnabled()) {
            std::wstring deviceId;
            TryGetTargetId(deviceId);
            WriteCamRecLog(owner, MyForm::DiagLevel::Warn, L"CameraRecovery_SkippedExpectedDisabled", incidentOp,
                L"Phase=ResultHandling", deviceId, L"NoChange", true);
            currentRung = 0;
            notFoundRecheckQueued = false;
            return;
        }

        std::wstring deviceId;
        TryGetTargetId(deviceId);

        bool deviceResponsive = (status == CameraHealthStatus::Healthy
            || status == CameraHealthStatus::Busy
            || status == CameraHealthStatus::PrivacyDenied);

        if (deviceResponsive) {
            if (currentRung > 0) {
                // Recovery ladder succeeded at this rung (Busy/Privacy after a rung still
                // proves the device responds).
                WriteCamRecLog(owner, MyForm::DiagLevel::Info, L"CameraRecovery_Recovered", incidentOp,
                    System::String::Format(L"Rung={0} | TotalMs={1} | Result={2}",
                        currentRung, static_cast<int>(GetTickCount64() - incidentStartTick), HealthStatusText(status)),
                    deviceId, L"Enabled", true);
                currentRung = 0;
                notFoundRecheckQueued = false;
            }
            return;
        }

        if (status == CameraHealthStatus::NotFound) {
            if (currentRung > 0) {
                // Device may be mid re-detection after a recovery rung — treat as rung failure.
                AdvanceOrExhaust();
                return;
            }
            if (!notFoundRecheckQueued) {
                // Do not hammer: exactly one delayed re-check; persistent NotFound is
                // logged and left to the routine triggers.
                notFoundRecheckQueued = true;
                pendingGapAction = GapAction::NotFoundRecheck;
                gapTimer->Interval = kNotFoundRecheckMs;
                gapTimer->Start();
            } else {
                notFoundRecheckQueued = false;
                WriteCamRecLog(owner, MyForm::DiagLevel::Warn, L"CameraRecovery_NotFoundPersistent", incidentOp,
                    L"", deviceId, L"NoChange", false);
            }
            return;
        }

        // Wedged / Timeout / Error — recovery candidate.
        if (currentRung > 0) {
            AdvanceOrExhaust();
        } else {
            incidentStartTick = GetTickCount64();
            AdvanceToRung(1);
        }
    }

    void CameraRecoveryFailsafe::HandleRungResult(bool ok)
    {
        state = RecoveryState::Idle;

        if (!ok) {
            AdvanceOrExhaust();
            return;
        }

        // Rung executed — bounded settle window, then a fresh health check with a fresh
        // target lookup (device identity may have changed across restart/re-enumeration).
        pendingGapAction = GapAction::PostRungCheck;
        gapTimer->Interval = kGapMs;
        gapTimer->Start();
    }

    void CameraRecoveryFailsafe::AdvanceOrExhaust()
    {
        if (!isArmed) return;
        if (currentRung >= 4) {
            ParkAfterExhaustion();
            return;
        }
        AdvanceToRung(currentRung + 1);
    }

    void CameraRecoveryFailsafe::AdvanceToRung(int rung)
    {
        if (!isArmed) return;
        if (owner == nullptr) return;

        if (owner->IsSystemEndingActive()) { AbortIncident(L"SystemEnding"); return; }
        if (!owner->IsMonitoringActive()) { AbortIncident(L"MonitoringOff"); return; }
        if (!IsExpectedEnabled()) { AbortIncident(L"ExpectedDisabled"); return; }

        std::wstring targetId;
        if (!TryGetTargetId(targetId) || targetId.empty()) { AbortIncident(L"NoTarget"); return; }

        currentRung = rung;
        state = RecoveryState::Recovering;

        WriteCamRecLog(owner, MyForm::DiagLevel::Info, L"CameraRecovery_RungStart", incidentOp,
            System::String::Format(L"Rung={0}", rung), targetId, L"Enabled", true);

        CameraRungWorkContext* ctx = new CameraRungWorkContext();
        ctx->rung = rung;
        ctx->targetId = targetId;
        ctx->ok = false;
        ctx->orphaned = 0;
        ctx->doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (ctx->doneEvent == NULL) {
            delete ctx;
            AbortIncident(L"WorkerSpawnFailed");
            return;
        }

        HANDLE thread = CreateThread(nullptr, 0, CameraRungWorkerProc, ctx, 0, nullptr);
        if (thread == NULL) {
            CloseHandle(ctx->doneEvent);
            delete ctx;
            AbortIncident(L"WorkerSpawnFailed");
            return;
        }

        rungContext = ctx;
        rungThread = thread;
        int budgetMs = kRung4BudgetMs;
        if (rung == 1) budgetMs = kRung1BudgetMs;
        else if (rung == 2) budgetMs = kRung2BudgetMs;
        else if (rung == 3) budgetMs = kRung3BudgetMs;
        workerDeadlineTick = GetTickCount64() + budgetMs;
        resultTimer->Start();
    }

    void CameraRecoveryFailsafe::AbortIncident(System::String^ reason)
    {
        currentRung = 0;
        notFoundRecheckQueued = false;
        pendingGapAction = GapAction::None;
        try { gapTimer->Stop(); } catch (...) {}
        state = RecoveryState::Idle;

        std::wstring deviceId;
        TryGetTargetId(deviceId);
        WriteCamRecLog(owner, MyForm::DiagLevel::Warn, L"CameraRecovery_Aborted", incidentOp,
            System::String::Format(L"Reason={0}", reason), deviceId, L"NoChange", false);
    }

    void CameraRecoveryFailsafe::ParkAfterExhaustion()
    {
        ULONGLONG nowTick = GetTickCount64();
        parkedUntilTick = nowTick + kParkMs;
        lastCheckTick = nowTick;

        System::String^ rebootNote = L"";
        if (lastHealthHr == MF_E_REBOOT_REQUIRED) {
            rebootNote = L" | MF_E_REBOOT_REQUIRED_persists | Recommendation=RebootWindows";
        }

        std::wstring deviceId;
        TryGetTargetId(deviceId);
        WriteCamRecLog(owner, MyForm::DiagLevel::Error, L"CameraRecovery_RecoveryExhausted", incidentOp,
            System::String::Format(L"FinalResult={0} | HRESULT=0x{1:X8} | ParkMs={2}{3}",
                HealthStatusText(lastHealthStatus), static_cast<int>(lastHealthHr),
                static_cast<int>(kParkMs), rebootNote),
            deviceId, L"Enabled", false);

        currentRung = 0;
        notFoundRecheckQueued = false;
        state = RecoveryState::Idle;
    }

} // namespace Windows_Hello_Fix_v2_0
