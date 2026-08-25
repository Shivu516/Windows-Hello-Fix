#include "MyForm.h"
#include "src/application/CommandLine.h"

using namespace System;
using namespace System::Windows::Forms;
using namespace Windows_Hello_Fix_v2_0;

[STAThreadAttribute]
int main(array<String^>^ args)
{
    Application::EnableVisualStyles();
    Application::SetCompatibleTextRenderingDefault(false);

    MyForm form;

    // v2.0 behavior: hide for background/disable/enable/restore/repair (case-sensitive match as in CommandLine::ShouldHideWindow)
    bool runHidden = CommandLine::ShouldHideWindow(args);

    if (runHidden) {
        // Make the window completely invisible while keeping WndProc alive
        form.Opacity = 0;
        form.ShowInTaskbar = false;
        form.WindowState = FormWindowState::Minimized;
    }

    Application::Run(% form);

    return 0;
}
