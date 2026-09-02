#pragma once

#include "Updater.h"
#include "UpdateModels.h"

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    ref class UpdaterPopup;

    public ref class UpdaterUI sealed
    {
    private:
        System::Windows::Forms::Form^ ownerForm;
        Updater^ updater;
        System::Windows::Forms::Panel^ iconPanel;
        System::Windows::Forms::ToolTip^ tooltip;
        System::Windows::Forms::Timer^ pulseTimer;
        bool pulseOn;
        bool iconInstalled;
        UpdaterPopup^ popup;
        void OnIconPaint(System::Object^ sender, System::Windows::Forms::PaintEventArgs^ e);
        void OnIconClick(System::Object^ sender, System::EventArgs^ e);
        void OnIconMouseEnter(System::Object^ sender, System::EventArgs^ e);
        void OnIconMouseLeave(System::Object^ sender, System::EventArgs^ e);
        void OnPulseTick(System::Object^ sender, System::EventArgs^ e);
        void OnStateChanged(System::Object^ sender, System::EventArgs^ e);
        void OnOwnerResize(System::Object^ sender, System::EventArgs^ e);
        void OnPopupClosed(System::Object^ sender, System::Windows::Forms::FormClosedEventArgs^ e);
        System::String^ GetTooltipForStatus();
        System::Drawing::Point ComputeIconLocation();
        void EnsureIconPosition();
        float GetScaleFactor();
    public:
        UpdaterUI(System::Windows::Forms::Form^ owner, Updater^ updater);
        ~UpdaterUI();
        !UpdaterUI();
        void InstallIcon();
        void RemoveIcon();
        void RefreshIcon();
        void ShowPopup();
        void ShowPopupForRelease(GitHubRelease^ release);
        bool IsPopupOpen();
    };

    public ref class UpdaterPopup : public System::Windows::Forms::Form
    {
    private:
        System::Windows::Forms::Form^ ownerForm;
        Updater^ updater;
        System::Windows::Forms::ComboBox^ cmbRelease;
        System::Windows::Forms::Button^ btnUpdate;
        System::Windows::Forms::Panel^ pnlPreview;
        System::Windows::Forms::Label^ lblReleaseInfo;
        System::Windows::Forms::Label^ lblStatusInside;
        System::Windows::Forms::Label^ lblWarningInline;
        System::Windows::Forms::RichTextBox^ rtbNotes;
        System::Windows::Forms::LinkLabel^ linkViewOnGithub;
        System::Windows::Forms::ProgressBar^ progressBar;
        GitHubRelease^ selectedRelease;
        GitHubRelease^ pendingUpdateRelease;
        bool IsDarkMode();
        void InitializeComponentPopup();
        void RefreshAll();
        void RefreshStatus();
        void RefreshReleaseList();
        void RefreshNotes();
        void UpdateButtonStates();
        void OnReleaseChanged(System::Object^ sender, System::EventArgs^ e);
        void OnUpdateClick(System::Object^ sender, System::EventArgs^ e);
        void OnViewOnGithubClick(System::Object^ sender, System::Windows::Forms::LinkLabelLinkClickedEventArgs^ e);
        void OnUpdaterStateChanged(System::Object^ sender, System::EventArgs^ e);
        void OnUpdaterReleasesUpdated(System::Object^ sender, System::EventArgs^ e);
        void OnFormDeactivate(System::Object^ sender, System::EventArgs^ e);
        void OnDownloadTaskCompleted(System::Threading::Tasks::Task<bool>^ t);
        bool ConfirmDowngradeIfNeeded(GitHubRelease^ target);
        void DoUpdateForRelease(GitHubRelease^ release);
        void DoUpdateForRelease_Failed(System::String^ msg);
        void DoUpdateForRelease_Launch(GitHubRelease^ release);
        void OpenUrl(System::String^ url);
        void OnNotesLinkClicked(System::Object^ sender, System::Windows::Forms::LinkClickedEventArgs^ e);
    public:
        UpdaterPopup(System::Windows::Forms::Form^ owner, Updater^ updater);
        void RefreshForExternalChange();
        void SelectRelease(GitHubRelease^ release);
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
