#include "MyForm.h"
#include "src/watchdog/RecoveryLoopFailsafe.h"

using namespace System;
using namespace System::Windows::Forms;
using namespace Windows_Hello_Fix_v2_0;

[STAThreadAttribute]
int main(array<String^>^ args)
{
    Application::EnableVisualStyles();
    Application::SetCompatibleTextRenderingDefault(false);

    MyForm form;

    // Check for the background argument passed by Task Scheduler
    bool runHidden = false;
    for (int i = 0; i < args->Length; i++) {
        if (args[i] == L"--background" || args[i] == L"/background" ||
            args[i] == L"--disable-camera" || args[i] == L"/disable-camera" ||
            args[i] == L"--enable-camera" || args[i] == L"/enable-camera" ||
            args[i] == L"/restore-camera" || args[i] == L"/repair-camera") {
            runHidden = true;
            break;
        }
    }

    if (runHidden) {
        // Make the window completely invisible while keeping WndProc alive
        form.Opacity = 0;
        form.ShowInTaskbar = false;
        form.WindowState = FormWindowState::Minimized;
    }

    // RecoveryLoopFailsafe: owned outside src/core (extreme core-preservation).
    // Only for long-lived daemon - never for short-lived command workers.
    bool isCommandWorker = false;
    for (int i = 0; i < args->Length; i++) {
        if (args[i]->Equals(L"--disable-camera", StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"/disable-camera", StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"--enable-camera", StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"/enable-camera", StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"/restore-camera", StringComparison::OrdinalIgnoreCase) ||
            args[i]->Equals(L"/repair-camera", StringComparison::OrdinalIgnoreCase)) {
            isCommandWorker = true;
            break;
        }
    }

    RecoveryLoopFailsafe^ recoveryLoop = nullptr;
    if (!isCommandWorker) {
        try {
            recoveryLoop = gcnew RecoveryLoopFailsafe(%form);
            form.Load += gcnew EventHandler(recoveryLoop, &RecoveryLoopFailsafe::OnOwnerLoad);
            form.FormClosing += gcnew FormClosingEventHandler(recoveryLoop, &RecoveryLoopFailsafe::OnOwnerClosing);
        } catch (...) {
            recoveryLoop = nullptr;
        }
    }

    Application::Run(%form);

    // Ensure Disarm on exit if loop was armed (also handled by FormClosing).
    try { if (recoveryLoop != nullptr) recoveryLoop->Disarm(); } catch (...) {}

    return 0;
}
