#pragma once

#include <windows.h>

class EventCooldown {
public:
    static bool ShouldSuppressPowerEvent(int code, ULONGLONG nowTick);
    static bool ShouldSuppressSessionEvent(int code, ULONGLONG nowTick);

private:
    static ULONGLONG s_lastPowerTick;
    static int s_lastPowerCode;
    static ULONGLONG s_lastSessionTick;
    static int s_lastSessionCode;
};
