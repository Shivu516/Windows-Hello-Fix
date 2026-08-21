#include "EventCooldown.h"

ULONGLONG EventCooldown::s_lastPowerTick = 0;
int EventCooldown::s_lastPowerCode = -1;
ULONGLONG EventCooldown::s_lastSessionTick = 0;
int EventCooldown::s_lastSessionCode = -1;

bool EventCooldown::ShouldSuppressPowerEvent(int code, ULONGLONG nowTick) {
    if (s_lastPowerCode == code && (nowTick - s_lastPowerTick) < 1500) {
        return true;
    }
    s_lastPowerCode = code;
    s_lastPowerTick = nowTick;
    return false;
}

bool EventCooldown::ShouldSuppressSessionEvent(int code, ULONGLONG nowTick) {
    if (s_lastSessionCode == code && (nowTick - s_lastSessionTick) < 1500) {
        return true;
    }
    s_lastSessionCode = code;
    s_lastSessionTick = nowTick;
    return false;
}
