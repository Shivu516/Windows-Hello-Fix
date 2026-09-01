#pragma once

#include "UpdateState.h"
#include "GitHubReleaseClient.h"

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    ref class UpdaterUI; // forward
    ref class DownloadProgress; // from UpdateInstaller

    public ref class Updater sealed
    {
    private:
        // Owner form (MyForm) — stored as System::Windows::Forms::Form^ to avoid circular include
        System::Windows::Forms::Form^ ownerForm;
        UpdateState^ state;
        GitHubReleaseClient^ client;
        UpdaterUI^ ui;

        // Timers
        System::Windows::Forms::Timer^ periodicTimer;
        System::Windows::Forms::Timer^ startupDelayTimer;

        // Cancellation
        System::Threading::CancellationTokenSource^ checkCts;
        System::Threading::CancellationTokenSource^ downloadCts;

        // State
        bool isArmed;
        bool isChecking;
        System::DateTime lastCheckAttemptUtc;
        System::DateTime rateLimitResetUtc;
        bool downloadInProgress;
        System::String^ stagedInstallerPath; // TEMP path after download
        // Pending marshal fields for BeginInvoke
        FetchResult^ pendingFetchResult;
        bool pendingFetchForce;
        System::String^ pendingDownloadStaged;
        bool pendingDownloadSuccess;
        System::String^ pendingDownloadErr;
        // Thread proc helpers (instance fields used by background threads)
        GitHubRelease^ pendingDownloadRelease;
        System::String^ pendingDownloadUserPath;
        bool pendingDownloadIsUserPath;

        // Constants
        static const int kStartupDelayMs = 5000;
        static const int kPeriodicMs = 6 * 60 * 60 * 1000; // 6h
        static const int kMinCheckIntervalMs = 30 * 60 * 1000; // 30m
        static const int kRetryAfterRateLimitBufferMs = 60 * 1000;

        // Logging helper (delegates to ownerForm's LogFailsafe via dynamic invoke to avoid hard core coupling)
        void Log(System::String^ evt, System::String^ targetState, bool ok);
        void LogWithDevice(System::String^ evt, System::String^ deviceInfo, System::String^ targetState, bool ok);

        // UI helpers
        void UpdateIconState();
        void EnsureUiCreated();

        // Handlers
        void OnStartupDelayTick(System::Object^ sender, System::EventArgs^ e);
        void OnPeriodicTick(System::Object^ sender, System::EventArgs^ e);

        // Core check logic (runs on ThreadPool, marshals back)
        void DoCheckAsync(bool force);

        // Helpers for Thread procs
        void CheckThreadProc(System::Object^ state);
        void DownloadThreadProc(System::Object^ state);
        void OnProgressReported(DownloadProgress^ p);

    public:
        Updater(System::Windows::Forms::Form^ owner);
        ~Updater();
        !Updater();

        // Must be called on UI thread (from main.cpp Load handler)
        void OnOwnerLoad(System::Object^ sender, System::EventArgs^ e);
        void OnOwnerClosing(System::Object^ sender, System::Windows::Forms::FormClosingEventArgs^ e);

        void Arm();
        void Disarm();

        // Public API for UI
        property UpdateState^ State { UpdateState^ get() { return state; } }
        property bool IsArmed { bool get() { return isArmed; } }

        void CheckAsync(bool force);
        void CheckAsync() { CheckAsync(false); }
        void CancelCheck();
        void CancelDownload();

        // Download selected release (upgrade/downgrade). Returns Task that completes when download verified.
        System::Threading::Tasks::Task<bool>^ DownloadReleaseAsync(GitHubRelease^ release);

        // Download to user-chosen path (Download Installer...)
        System::Threading::Tasks::Task<bool>^ DownloadToUserPathAsync(GitHubRelease^ release, System::String^ userPath);

        // Install staged file (launches NSIS). Exits current process on success if launch succeeds.
        bool LaunchStagedInstaller(bool silent);

        // Channel
        void SetChannel(UpdateChannel ch);

        // UI
        void ShowPopup();
        void ShowPopupForRelease(GitHubRelease^ release);
        bool IsPopupOpen();

        // Version
        static System::String^ GetCurrentVersionDisplay();

        // Cache
        void RefreshCacheOnChannelChange();

        // For main.cpp title fix
        void ApplyTitleFix();

        // Cleanup helper
        static void CleanupOldStaging();

        // Events
        event System::EventHandler^ StateChanged;
        event System::EventHandler^ ReleasesUpdated;

    private:
        void RaiseStateChanged();
        void RaiseReleasesUpdated();
        bool TryEnterCheckCooldown(bool force);
        void SetStatus(UpdaterStatus s, System::String^ detail);
        void OnDownloadProgress(int percent);
        void DoCheckAsync_ContinueOnUi(FetchResult^ result, bool force);
        void DoCheckAsync_ContinueOnUi_Marshaled();
        void OnDownloadCompletedUi(System::String^ staged, bool success, System::String^ errDetail);
        void OnDownloadCompletedUi_Marshaled();
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
