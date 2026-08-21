#pragma once

enum class SystemEvent {
    None,
    SystemQueryEnd,
    SystemEnd,
    PowerSuspend,
    PowerSettingLid,
    PowerSettingButton,
    PowerSettingOther,
    PowerResumeSuspend,
    PowerResumeAutomatic,
    PowerOther,
    SessionLock,
    SessionUnlock,
    SessionOther
};
