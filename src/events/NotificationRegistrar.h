#pragma once

#include <windows.h>

class NotificationRegistrar {
public:
    static bool RegisterPowerNotifications(HWND hwnd, HPOWERNOTIFY& outLid, HPOWERNOTIFY& outButton);
    static void UnregisterPowerNotifications(HPOWERNOTIFY hLid, HPOWERNOTIFY hButton);

    static bool RegisterSessionNotificationWithRetry(HWND hwnd);
    static void UnregisterSessionNotification(HWND hwnd);
};
