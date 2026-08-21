#pragma once

#include <windows.h>
#include "SystemEvent.h"

class WinEventDecoder {
public:
    static SystemEvent Decode(UINT msg, WPARAM wParam, LPARAM lParam);
    static SystemEvent DecodePowerEvent(int powerEvent, LPARAM lParam);
    static SystemEvent DecodeSessionEvent(int sessionEvent);
    static SystemEvent DecodeSystemEvent(UINT msg);
};
