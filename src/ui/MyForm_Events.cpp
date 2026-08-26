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
            WriteDiagnosticLog(L"SystemEnd_Begin", L"Disabled", true);
            if (isMonitoring) {
                bool shutdownDisableResult = DisableTargetCameraHardware(true);
                WriteDiagnosticLog(L"SystemEnd_Disable", L"Disabled", shutdownDisableResult);
            }
            WTSUnRegisterSessionNotification(static_cast<HWND>(this->Handle.ToPointer()));
        }

        // 2. Power Broadcast Events (Sleep / Resume / Hardware Notifications)
        else if (m.Msg == 0x0218) { // WM_POWERBROADCAST
            int powerEvent = m.WParam.ToInt32();

            if (lastPowerEventCode == powerEvent && (nowTick - lastPowerEventTick) < 1500) {
                WriteDiagnosticLog(L"PowerEvent_DedupIgnored", L"NoChange", true);
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
                                WriteDiagnosticLog(L"PowerSetting_IrrelevantGuid", L"NoChange", true);
                                Form::WndProc(m);
                                return;
                            }
                        }
                    }

                    // Enforce structural hardware state lock
                    isAlreadyDisabled = true;
                    bool powerDisableResult = DisableTargetCameraHardware(true);
                    WriteDiagnosticLog(L"PowerEvent_Disable", L"Disabled", powerDisableResult);

                    // CRITICAL TIME WINDOW BYPASS (500ms safety window)
                    ::Sleep(500);
                }
            }
            // System Waking Up (PBT_APMRESUMESUSPEND = 0x0007 or PBT_APMRESUMEAUTOMATIC = 0x0012)
            else if (powerEvent == 0x0007 || powerEvent == 0x0012) {
                if (isMonitoring) {
                    // Force a brief delay to allow systemic device trees to rebuild
                    System::Threading::Thread::Sleep(1000);
                    bool powerEnableResult = EnableTargetCameraHardware(false);
                    WriteDiagnosticLog(L"PowerEvent_Enable", L"Enabled", powerEnableResult);
                    isAlreadyDisabled = false; // Release lock on verified wake
                }
            }
        }

        // 3. Session Lock / Unlock Events
        else if (m.Msg == WM_WTSSESSION_CHANGE) {
            int sessionEvent = m.WParam.ToInt32();

            WriteDiagnosticLog(
                String::Format(L"SessionEvent_Received_Code={0}", sessionEvent),
                isMonitoring ? L"ActiveMonitoring" : L"MonitoringOff",
                true
            );

            if (lastSessionEventCode == sessionEvent && (nowTick - lastSessionEventTick) < 1500) {
                WriteDiagnosticLog(L"SessionEvent_DedupIgnored", L"NoChange", true);
                Form::WndProc(m);
                return;
            }
            lastSessionEventCode = sessionEvent;
            lastSessionEventTick = nowTick;

            if (!isMonitoring) {
                WriteDiagnosticLog(L"SessionEvent_Ignored_MonitoringOff", L"NoChange", true);
            }
            else if (sessionEvent == WTS_SESSION_LOCK) {
                bool lockDisableResult = DisableTargetCameraHardware(true);
                WriteDiagnosticLog(L"SessionLock_Disable", L"Disabled", lockDisableResult);
            }
            else if (sessionEvent == WTS_SESSION_UNLOCK) {
                bool unlockEnableResult = EnableTargetCameraHardware(false);
                WriteDiagnosticLog(L"SessionUnlock_Enable", L"Enabled", unlockEnableResult);
            }
        }

        Form::WndProc(m);
    }

}
