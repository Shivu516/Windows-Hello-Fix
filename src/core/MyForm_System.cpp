#include "MyForm.h"

namespace Windows_Hello_Fix_v2_0 {

    bool MyForm::IsRestoreCameraCommand(array<System::String^>^ args) {
        for (int i = 0; i < args->Length; i++) {
            if (args[i]->Equals(L"/restore-camera", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"/enable-camera", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"--enable-camera", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"/repair-camera", System::StringComparison::OrdinalIgnoreCase)) {
                return true;
            }
        }

        return false;
    }

    bool MyForm::IsDisableCameraCommand(array<System::String^>^ args) {
        for (int i = 0; i < args->Length; i++) {
            if (args[i]->Equals(L"/disable-camera", System::StringComparison::OrdinalIgnoreCase) ||
                args[i]->Equals(L"--disable-camera", System::StringComparison::OrdinalIgnoreCase)) {
                return true;
            }
        }

        return false;
    }

    void MyForm::ListenForWakeupSignal() {
        while (keepListening && hWakeupEvent != NULL) {
            DWORD waitResult = WaitForSingleObject(hWakeupEvent, INFINITE);
            if (waitResult == WAIT_OBJECT_0 && keepListening) {
                if (this->InvokeRequired) {
                    this->Invoke(gcnew MethodInvoker(this, &MyForm::BringWindowToFrontDelegate));
                }
                else {
                    BringWindowToFrontDelegate();
                }
            }
        }
    }

    void MyForm::BringWindowToFrontDelegate() {
        this->Opacity = 1.0;
        this->Show();
        this->Visible = true;
        this->ShowInTaskbar = true;
        this->WindowState = FormWindowState::Normal;
        this->BringToFront();
        this->Activate();
        this->Refresh();
    }

}
