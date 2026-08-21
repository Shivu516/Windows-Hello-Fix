#include "NotificationRegistrar.h"

#include <wtsapi32.h>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")

#ifndef GUID_LIDSWITCH_STATE_CHANGE
#define GUID_LIDSWITCH_STATE_CHANGE {0xBA3E0F4D, 0xB817, 0x4094, {0xA2, 0xD1, 0xD5, 0x63, 0x79, 0xE6, 0xA0, 0xF3}}
#endif

#ifndef GUID_POWER_BUTTON_TIMESTAMP
#define GUID_POWER_BUTTON_TIMESTAMP {0xA70AFB22, 0x3816, 0x4584, {0x9F, 0x24, 0x81, 0x0A, 0x4E, 0x27, 0x47, 0xFB}}
#endif

bool NotificationRegistrar::RegisterPowerNotifications(HWND hwnd, HPOWERNOTIFY& outLid, HPOWERNOTIFY& outButton) {
    GUID lidGuid = GUID_LIDSWITCH_STATE_CHANGE;
    GUID buttonGuid = GUID_POWER_BUTTON_TIMESTAMP;

    outLid = RegisterPowerSettingNotification(hwnd, &lidGuid, DEVICE_NOTIFY_WINDOW_HANDLE);
    outButton = RegisterPowerSettingNotification(hwnd, &buttonGuid, DEVICE_NOTIFY_WINDOW_HANDLE);
    return true;
}

void NotificationRegistrar::UnregisterPowerNotifications(HPOWERNOTIFY hLid, HPOWERNOTIFY hButton) {
    if (hLid) {
        UnregisterPowerSettingNotification(hLid);
    }
    if (hButton) {
        UnregisterPowerSettingNotification(hButton);
    }
}

bool NotificationRegistrar::RegisterSessionNotificationWithRetry(HWND hwnd) {
    for (int registrationAttempt = 0; registrationAttempt < 6; registrationAttempt++) {
        if (WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION)) {
            return true;
        }
        ::Sleep(500);
    }
    return false;
}

void NotificationRegistrar::UnregisterSessionNotification(HWND hwnd) {
    WTSUnRegisterSessionNotification(hwnd);
}
