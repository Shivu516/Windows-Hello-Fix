#include "MyForm.h"

namespace Windows_Hello_Fix_v2_0 {

    System::Void MyForm::MyForm_FormClosing(System::Object^ sender, System::Windows::Forms::FormClosingEventArgs^ e) {
        if (e->CloseReason == CloseReason::UserClosing) {
            e->Cancel = true;
            this->Hide();
            this->ShowInTaskbar = false;

            if (!isBackgroundMode) {
                MessageBox::Show(
                    L"The program is running in the background. To close it completely, click 'Stop Monitoring Service' or kill the app in Task Manager.",
                    L"Background Service Active",
                    MessageBoxButtons::OK,
                    MessageBoxIcon::Information
                );
                isBackgroundMode = true;
            }
        }
        // If closing because Windows is shutting down, let destructor handle config reset
    }

    System::Void MyForm::btnToggle_Click(System::Object^ sender, System::EventArgs^ e) {
        auto* pCachedCameras = static_cast<std::vector<CameraDeviceInfo>*>(cachedCameras);
        auto* pSelectedInstanceId = static_cast<std::wstring*>(selectedInstanceId);

        if (this->deviceDrop->SelectedIndex == -1) {
            MessageBox::Show(L"Please select a camera device.", L"No Device Selected", MessageBoxButtons::OK, MessageBoxIcon::Warning);
            return;
        }

        if (!isMonitoring) {
            *pSelectedInstanceId = pCachedCameras->at(this->deviceDrop->SelectedIndex).instanceId;
            isMonitoring = true;
            this->deviceDrop->Enabled = false;
            this->btnToggle->Text = L"Stop Monitoring Service";
            this->lblStatus->Text = L"Status: Service Running";
            this->lblStatus->ForeColor = System::Drawing::Color::Green;
            SaveConfigState(true, msclr::interop::marshal_as<String^>(*pSelectedInstanceId));
        }
        else {
            isMonitoring = false;
            EnableTargetCameraHardware(false);

            SaveConfigState(false, msclr::interop::marshal_as<String^>(*pSelectedInstanceId));

            pSelectedInstanceId->clear();
            this->deviceDrop->Enabled = true;
            this->btnToggle->Text = L"Start Monitoring Service";
            this->lblStatus->Text = L"Status: Service Stopped";
            this->lblStatus->ForeColor = System::Drawing::Color::Gray;
        }
    }

}
