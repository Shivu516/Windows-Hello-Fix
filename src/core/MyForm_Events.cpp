#include "MyForm.h"

namespace Windows_Hello_Fix_v2_0 {

    void MyForm::WndProc(System::Windows::Forms::Message% m) {
        static bool isAlreadyDisabled = false; // Inter-process hardware state lock tracker
        static ULONGLONG lastSessionEventTick = 0;
        static int lastSessionEventCode = -1;
        static ULONGLONG lastPowerEventTick = 0;
        static int lastPowerEventCode = -1;
        ULONGLONG nowTick = GetTickCount64();

        // 1. System Shutdown / Logoff
        if (m.Msg == 0x0016 || m.Msg == 0x0011) {
            isSystemEnding = true;
            String^ opId = NewOperationId(L"SYSEND");
            WriteDiagnosticLogEx(DiagLevel::Info, L"SYSTEM", L"SystemEnd_Begin", opId, L"", L"Disabled", true);
            if (isMonitoring) {
                ULONGLONG actionStart = GetTickCount64();
                bool shutdownDisableResult = DisableTargetCameraHardware(true, opId);
                WriteDiagnosticLogEx(shutdownDisableResult ? DiagLevel::Info : DiagLevel::Error, L"SYSTEM", L"SystemEnd_Disable", opId,
                    String::Format(L"TriggerToStart={0}ms | TriggerToComplete={1}ms",
                        (int)(actionStart - nowTick), (int)(GetTickCount64() - nowTick)),
                    L"Disabled", shutdownDisableResult);
            }
            WTSUnRegisterSessionNotification(static_cast<HWND>(this->Handle.ToPointer()));
        }

        // 2. Power Broadcast Events (Sleep / Resume / Hardware Notifications)
        else if (m.Msg == 0x0218) { // WM_POWERBROADCAST
            int powerEvent = m.WParam.ToInt32();

            if (lastPowerEventCode == powerEvent && (nowTick - lastPowerEventTick) < 1500) {
                WriteDiagnosticLogEx(DiagLevel::Debug, L"POWER", L"PowerEvent_DedupIgnored", L"",
                    String::Format(L"Code={0} | ElapsedSinceLast={1}ms", powerEvent, (int)(nowTick - lastPowerEventTick)),
                    L"NoChange", true);
                Form::WndProc(m);
                return;
            }
            lastPowerEventCode = powerEvent;
            lastPowerEventTick = nowTick;

            // Trap Sleep Broadcast or Low-Level Power Intercept Events
            if (powerEvent == 0x0004 || powerEvent == 0x8013) {
                if (isMonitoring && !isAlreadyDisabled) {

                    if (powerEvent == 0x8013) {
                        POWERBROADCAST_SETTING* pSetting = reinterpret_cast<POWERBROADCAST_SETTING*>(m.LParam.ToPointer());
                        if (pSetting != nullptr) {
                            GUID lidGuid = GUID_LIDSWITCH_STATE_CHANGE;
                            GUID buttonGuid = GUID_POWER_BUTTON_TIMESTAMP;

                            // Lid close event or physical Power button action caught instantly!
                            if (!IsEqualGUID(pSetting->PowerSetting, lidGuid) && !IsEqualGUID(pSetting->PowerSetting, buttonGuid)) {
                                WriteDiagnosticLogEx(DiagLevel::Debug, L"POWER", L"PowerSetting_IrrelevantGuid", L"",
                                    String::Format(L"Code={0}", powerEvent), L"NoChange", true);
                                Form::WndProc(m);
                                return;
                            }
                        }
                    }

                    // Enforce structural hardware state lock
                    String^ opId = NewOperationId(L"POWER");
                    isAlreadyDisabled = true;
                    ULONGLONG actionStart = GetTickCount64();
                    bool powerDisableResult = DisableTargetCameraHardware(true, opId);
                    WriteDiagnosticLogEx(powerDisableResult ? DiagLevel::Info : DiagLevel::Error, L"POWER", L"PowerEvent_Disable", opId,
                        String::Format(L"Code={0} | TriggerToStart={1}ms | TriggerToComplete={2}ms",
                            powerEvent, (int)(actionStart - nowTick), (int)(GetTickCount64() - nowTick)),
                        L"Disabled", powerDisableResult);

                    // CRITICAL TIME WINDOW BYPASS (500ms safety window)
                    ::Sleep(500);
                }
            }
            // System Waking Up (PBT_APMRESUMESUSPEND = 0x0007 or PBT_APMRESUMEAUTOMATIC = 0x0012)
            else if (powerEvent == 0x0007 || powerEvent == 0x0012) {
                if (isMonitoring) {
                    String^ opId = NewOperationId(L"POWER");
                    // Force a brief delay to allow systemic device trees to rebuild
                    System::Threading::Thread::Sleep(1000);
                    ULONGLONG actionStart = GetTickCount64();
                    bool powerEnableResult = EnableTargetCameraHardware(false, opId);
                    WriteDiagnosticLogEx(powerEnableResult ? DiagLevel::Info : DiagLevel::Error, L"POWER", L"PowerEvent_Enable", opId,
                        String::Format(L"Code={0} | TriggerToStart={1}ms | TriggerToComplete={2}ms",
                            powerEvent, (int)(actionStart - nowTick), (int)(GetTickCount64() - nowTick)),
                        L"Enabled", powerEnableResult);
                    isAlreadyDisabled = false; // Release lock on verified wake
                }
            }
        }

        // 3. Session Lock / Unlock Events
        else if (m.Msg == WM_WTSSESSION_CHANGE) {
            int sessionEvent = m.WParam.ToInt32();

            WriteDiagnosticLogEx(DiagLevel::Debug, L"SESSION", L"SessionEvent_Received", L"",
                String::Format(L"Code={0} | Monitoring={1}",
                    sessionEvent, isMonitoring ? L"Active" : L"Off"),
                L"NoChange", true);

            if (lastSessionEventCode == sessionEvent && (nowTick - lastSessionEventTick) < 1500) {
                WriteDiagnosticLogEx(DiagLevel::Debug, L"SESSION", L"SessionEvent_DedupIgnored", L"",
                    String::Format(L"Code={0} | LastCode={1} | ElapsedSinceLast={2}ms",
                        sessionEvent, lastSessionEventCode, (int)(nowTick - lastSessionEventTick)),
                    L"NoChange", true);
                Form::WndProc(m);
                return;
            }
            lastSessionEventCode = sessionEvent;
            lastSessionEventTick = nowTick;

            if (!isMonitoring) {
                WriteDiagnosticLogEx(DiagLevel::Debug, L"SESSION", L"SessionEvent_Ignored_MonitoringOff", L"",
                    String::Format(L"Code={0}", sessionEvent), L"NoChange", true);
            }
            else if (sessionEvent == WTS_SESSION_LOCK) {
                String^ opId = NewOperationId(L"LOCK");
                ULONGLONG actionStart = GetTickCount64();
                bool lockDisableResult = DisableTargetCameraHardware(true, opId);
                WriteDiagnosticLogEx(lockDisableResult ? DiagLevel::Info : DiagLevel::Error, L"LOCK", L"SessionLock_Disable", opId,
                    String::Format(L"TriggerToStart={0}ms | TriggerToComplete={1}ms",
                        (int)(actionStart - nowTick), (int)(GetTickCount64() - nowTick)),
                    L"Disabled", lockDisableResult);
            }
            else if (sessionEvent == WTS_SESSION_UNLOCK) {
                String^ opId = NewOperationId(L"UNLOCK");
                ULONGLONG actionStart = GetTickCount64();
                bool unlockEnableResult = EnableTargetCameraHardware(false, opId);
                WriteDiagnosticLogEx(unlockEnableResult ? DiagLevel::Info : DiagLevel::Error, L"LOCK", L"SessionUnlock_Enable", opId,
                    String::Format(L"TriggerToStart={0}ms | TriggerToComplete={1}ms",
                        (int)(actionStart - nowTick), (int)(GetTickCount64() - nowTick)),
                    L"Enabled", unlockEnableResult);
            }
        }

        Form::WndProc(m);
    }

}
