#include "WinEventDecoder.h"
#include <wtsapi32.h>

#ifndef GUID_LIDSWITCH_STATE_CHANGE
#define GUID_LIDSWITCH_STATE_CHANGE {0xBA3E0F4D, 0xB817, 0x4094, {0xA2, 0xD1, 0xD5, 0x63, 0x79, 0xE6, 0xA0, 0xF3}}
#endif

#ifndef GUID_POWER_BUTTON_TIMESTAMP
#define GUID_POWER_BUTTON_TIMESTAMP {0xA70AFB22, 0x3816, 0x4584, {0x9F, 0x24, 0x81, 0x0A, 0x4E, 0x27, 0x47, 0xFB}}
#endif

SystemEvent WinEventDecoder::Decode(UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == 0x0011 || msg == 0x0016) {
        return DecodeSystemEvent(msg);
    }
    if (msg == 0x0218) {
        return DecodePowerEvent(static_cast<int>(wParam), lParam);
    }
    if (msg == WM_WTSSESSION_CHANGE) {
        return DecodeSessionEvent(static_cast<int>(wParam));
    }
    return SystemEvent::None;
}

SystemEvent WinEventDecoder::DecodeSystemEvent(UINT msg) {
    if (msg == 0x0011) {
        return SystemEvent::SystemQueryEnd;
    }
    if (msg == 0x0016) {
        return SystemEvent::SystemEnd;
    }
    return SystemEvent::None;
}

SystemEvent WinEventDecoder::DecodePowerEvent(int powerEvent, LPARAM lParam) {
    if (powerEvent == 0x0004) {
        return SystemEvent::PowerSuspend;
    }
    if (powerEvent == 0x8013) {
        POWERBROADCAST_SETTING* pSetting = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
        if (pSetting != nullptr) {
            GUID lidGuid = GUID_LIDSWITCH_STATE_CHANGE;
            GUID buttonGuid = GUID_POWER_BUTTON_TIMESTAMP;
            if (!IsEqualGUID(pSetting->PowerSetting, lidGuid) && !IsEqualGUID(pSetting->PowerSetting, buttonGuid)) {
                return SystemEvent::PowerSettingOther;
            }
            if (IsEqualGUID(pSetting->PowerSetting, lidGuid)) {
                return SystemEvent::PowerSettingLid;
            }
            if (IsEqualGUID(pSetting->PowerSetting, buttonGuid)) {
                return SystemEvent::PowerSettingButton;
            }
            return SystemEvent::PowerSettingOther;
        }
        // If lParam is null, treat as generic suspend path (original would not filter)
        return SystemEvent::PowerSuspend;
    }
    if (powerEvent == 0x0007) {
        return SystemEvent::PowerResumeSuspend;
    }
    if (powerEvent == 0x0012) {
        return SystemEvent::PowerResumeAutomatic;
    }
    return SystemEvent::PowerOther;
}

SystemEvent WinEventDecoder::DecodeSessionEvent(int sessionEvent) {
    if (sessionEvent == WTS_SESSION_LOCK) {
        return SystemEvent::SessionLock;
    }
    if (sessionEvent == WTS_SESSION_UNLOCK) {
        return SystemEvent::SessionUnlock;
    }
    return SystemEvent::SessionOther;
}
