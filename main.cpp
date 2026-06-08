#include "MyForm.h"

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

    Application::Run(% form);

    return 0;
}
