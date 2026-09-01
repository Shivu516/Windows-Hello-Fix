#pragma once

#include "UpdateVersion.h"
#include "UpdateChannel.h"
#include "UpdateModels.h"

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    public enum class UpdaterStatus
    {
        Idle = 0,
        Checking = 1,
        UpToDate = 2,
        UpdateAvailable = 3,
        Downloading = 4,
        Installing = 5,
        Error = 6,
        Offline = 7,
        RateLimited = 8
    };

    public ref class UpdateState sealed
    {
    private:
        System::Object^ syncRoot;
        UpdateVersion^ installedVersion;
        UpdateChannel selectedChannel;
        System::Collections::Generic::List<GitHubRelease^>^ cachedReleases;
        System::String^ etag;
        System::String^ lastModified;
        System::DateTime lastCheckUtc;
        System::DateTime lastAttemptUtc;
        UpdaterStatus status;
        System::String^ statusDetail;
        int downloadProgress; // 0-100
        GitHubRelease^ latestForChannel;
        GitHubRelease^ pendingDownloadRelease;

        void RecalculateLatest();

    public:
        UpdateState();

        // Thread-safe accessors
        property UpdateVersion^ InstalledVersion { UpdateVersion^ get(); void set(UpdateVersion^ v); }
        property UpdateChannel SelectedChannel { UpdateChannel get(); void set(UpdateChannel c); }
        property System::Collections::Generic::List<GitHubRelease^>^ CachedReleases { System::Collections::Generic::List<GitHubRelease^>^ get(); void set(System::Collections::Generic::List<GitHubRelease^>^ v); }
        property System::String^ ETag { System::String^ get(); void set(System::String^ v); }
        property System::String^ LastModified { System::String^ get(); void set(System::String^ v); }
        property System::DateTime LastCheckUtc { System::DateTime get(); void set(System::DateTime v); }
        property System::DateTime LastAttemptUtc { System::DateTime get(); void set(System::DateTime v); }
        property UpdaterStatus Status { UpdaterStatus get(); void set(UpdaterStatus v); }
        property System::String^ StatusDetail { System::String^ get(); void set(System::String^ v); }
        property int DownloadProgress { int get(); void set(int v); }
        property GitHubRelease^ LatestForChannel { GitHubRelease^ get(); }
        property GitHubRelease^ PendingDownloadRelease { GitHubRelease^ get(); void set(GitHubRelease^ v); }

        // Derived
        bool IsUpdateAvailable();
        bool IsInstalledNewerThanLatest();
        System::Collections::Generic::List<GitHubRelease^>^ GetReleasesForSelectedChannel();
        System::Collections::Generic::List<GitHubRelease^>^ GetReleasesForChannel(UpdateChannel channel);
        GitHubRelease^ GetLatestForSelectedChannel();
        GitHubRelease^ FindReleaseByTag(System::String^ tag);

        // Cache persistence
        System::String^ GetCacheFilePath();
        System::String^ GetEtagFilePath();
        bool LoadCacheFromDisk();
        bool SaveCacheToDisk();
        bool NeedsRefresh(System::TimeSpan maxAge);

        // Helpers
        static System::String^ StatusToString(UpdaterStatus s);
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
