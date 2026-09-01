#pragma once

#include "Updater.h"
#include "UpdateModels.h"

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    ref class UpdaterPopup; // forward

    public ref class UpdaterUI sealed
    {
    private:
        System::Windows::Forms::Form^ ownerForm;
        Updater^ updater;
        System::Windows::Forms::Panel^ iconPanel;
        System::Windows::Forms::ToolTip^ tooltip;
        System::Windows::Forms::Timer^ pulseTimer;
        bool pulseOn;
        int lastPulsePercent;
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

    private:
        System::Drawing::Point ComputeIconLocation();
        void EnsureIconPosition();
    };

    // The popup/explorer window — owned, FixedDialog, 360x420, anchored bottom-right
    public ref class UpdaterPopup : public System::Windows::Forms::Form
    {
    private:
        System::Windows::Forms::Form^ ownerForm;
        Updater^ updater;

        // Controls
        System::Windows::Forms::Label^ lblHeader;
        System::Windows::Forms::Label^ lblVersionLine;
        System::Windows::Forms::Label^ lblChannelLine;
        System::Windows::Forms::Label^ lblNotes;
        System::Windows::Forms::LinkLabel^ linkViewOnGithub;
        System::Windows::Forms::Button^ btnUpdate;
        System::Windows::Forms::Button^ btnDetails;
        System::Windows::Forms::Button^ btnCancelDownload;
        System::Windows::Forms::ComboBox^ comboChannel;
        System::Windows::Forms::LinkLabel^ linkCheckNow;
        System::Windows::Forms::LinkLabel^ linkBrowse;
        System::Windows::Forms::Panel^ browsePanel;
        System::Windows::Forms::ListBox^ listReleases;
        System::Windows::Forms::Label^ lblReleaseDetail;
        System::Windows::Forms::LinkLabel^ linkDetailGithub;
        System::Windows::Forms::Button^ btnUpdateSelected;
        System::Windows::Forms::Button^ btnDownloadSelected;
        System::Windows::Forms::ProgressBar^ progressBar;
        System::Windows::Forms::Label^ lblStatusBanner;

        GitHubRelease^ selectedReleaseForUpdate;
        GitHubRelease^ pendingUpdateRelease;
        bool isBrowseExpanded;

        void OnDownloadTaskCompleted(System::Threading::Tasks::Task<bool>^ t);
        void InitializeComponentPopup();
        void RefreshAll();
        void RefreshHeader();
        void RefreshBrowseList();
        void RefreshDetailPane();
        void UpdateButtonStates();

        // Handlers
        void OnCheckNowLink(System::Object^ sender, System::Windows::Forms::LinkLabelLinkClickedEventArgs^ e);
        void OnChannelChanged(System::Object^ sender, System::EventArgs^ e);
        void OnBrowseLink(System::Object^ sender, System::Windows::Forms::LinkLabelLinkClickedEventArgs^ e);
        void OnListSelected(System::Object^ sender, System::EventArgs^ e);
        void OnUpdateClick(System::Object^ sender, System::EventArgs^ e);
        void OnDetailsClick(System::Object^ sender, System::EventArgs^ e);
        void OnCancelDownloadClick(System::Object^ sender, System::EventArgs^ e);
        void OnUpdateSelectedClick(System::Object^ sender, System::EventArgs^ e);
        void OnDownloadSelectedClick(System::Object^ sender, System::EventArgs^ e);
        void OnDetailGithubClick(System::Object^ sender, System::Windows::Forms::LinkLabelLinkClickedEventArgs^ e);
        void OnViewOnGithubClick(System::Object^ sender, System::Windows::Forms::LinkLabelLinkClickedEventArgs^ e);
        void OnUpdaterStateChanged(System::Object^ sender, System::EventArgs^ e);
        void OnUpdaterReleasesUpdated(System::Object^ sender, System::EventArgs^ e);
        void OnFormDeactivate(System::Object^ sender, System::EventArgs^ e);

        bool ConfirmDowngradeIfNeeded(GitHubRelease^ target);
        void DoUpdateForRelease(GitHubRelease^ release);
        void DoUpdateForRelease_Failed(System::String^ msg);
        void DoUpdateForRelease_Launch(GitHubRelease^ release);
        void OpenUrl(System::String^ url);

    public:
        UpdaterPopup(System::Windows::Forms::Form^ owner, Updater^ updater);
        void RefreshForExternalChange();
        void SelectRelease(GitHubRelease^ release);
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
