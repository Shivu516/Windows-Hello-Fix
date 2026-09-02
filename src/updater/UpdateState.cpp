#include "UpdateState.h"

using namespace System;
using namespace System::IO;
using namespace System::Collections::Generic;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    UpdateState::UpdateState()
        : selectedChannel(UpdateChannel::Stable), status(UpdaterStatus::Idle), downloadProgress(0)
    {
        syncRoot = gcnew Object();
        installedVersion = UpdateVersion::GetCurrentVersion();
        cachedReleases = gcnew List<GitHubRelease^>();
        etag = nullptr;
        lastModified = nullptr;
        lastCheckUtc = DateTime::MinValue;
        lastAttemptUtc = DateTime::MinValue;
        statusDetail = "";
        latestForChannel = nullptr;
        pendingDownloadRelease = nullptr;
    }

    UpdateVersion^ UpdateState::InstalledVersion::get() { System::Threading::Monitor::Enter(syncRoot); try { return installedVersion; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::InstalledVersion::set(UpdateVersion^ v) { System::Threading::Monitor::Enter(syncRoot); try { installedVersion = v; RecalculateLatest(); } finally { System::Threading::Monitor::Exit(syncRoot); } }

    UpdateChannel UpdateState::SelectedChannel::get() { System::Threading::Monitor::Enter(syncRoot); try { return selectedChannel; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::SelectedChannel::set(UpdateChannel c) { System::Threading::Monitor::Enter(syncRoot); try { selectedChannel = c; RecalculateLatest(); } finally { System::Threading::Monitor::Exit(syncRoot); } }

    List<GitHubRelease^>^ UpdateState::CachedReleases::get() { System::Threading::Monitor::Enter(syncRoot); try { return cachedReleases; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::CachedReleases::set(List<GitHubRelease^>^ v) { System::Threading::Monitor::Enter(syncRoot); try { cachedReleases = v != nullptr ? v : gcnew List<GitHubRelease^>(); RecalculateLatest(); } finally { System::Threading::Monitor::Exit(syncRoot); } }

    String^ UpdateState::ETag::get() { System::Threading::Monitor::Enter(syncRoot); try { return etag; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::ETag::set(String^ v) { System::Threading::Monitor::Enter(syncRoot); try { etag = v; } finally { System::Threading::Monitor::Exit(syncRoot); } }

    String^ UpdateState::LastModified::get() { System::Threading::Monitor::Enter(syncRoot); try { return lastModified; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::LastModified::set(String^ v) { System::Threading::Monitor::Enter(syncRoot); try { lastModified = v; } finally { System::Threading::Monitor::Exit(syncRoot); } }

    DateTime UpdateState::LastCheckUtc::get() { System::Threading::Monitor::Enter(syncRoot); try { return lastCheckUtc; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::LastCheckUtc::set(DateTime v) { System::Threading::Monitor::Enter(syncRoot); try { lastCheckUtc = v; } finally { System::Threading::Monitor::Exit(syncRoot); } }

    DateTime UpdateState::LastAttemptUtc::get() { System::Threading::Monitor::Enter(syncRoot); try { return lastAttemptUtc; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::LastAttemptUtc::set(DateTime v) { System::Threading::Monitor::Enter(syncRoot); try { lastAttemptUtc = v; } finally { System::Threading::Monitor::Exit(syncRoot); } }

    UpdaterStatus UpdateState::Status::get() { System::Threading::Monitor::Enter(syncRoot); try { return status; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::Status::set(UpdaterStatus v) { System::Threading::Monitor::Enter(syncRoot); try { status = v; } finally { System::Threading::Monitor::Exit(syncRoot); } }

    String^ UpdateState::StatusDetail::get() { System::Threading::Monitor::Enter(syncRoot); try { return statusDetail; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::StatusDetail::set(String^ v) { System::Threading::Monitor::Enter(syncRoot); try { statusDetail = v != nullptr ? v : ""; } finally { System::Threading::Monitor::Exit(syncRoot); } }

    int UpdateState::DownloadProgress::get() { System::Threading::Monitor::Enter(syncRoot); try { return downloadProgress; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::DownloadProgress::set(int v) { System::Threading::Monitor::Enter(syncRoot); try { downloadProgress = v; } finally { System::Threading::Monitor::Exit(syncRoot); } }

    GitHubRelease^ UpdateState::LatestForChannel::get() { System::Threading::Monitor::Enter(syncRoot); try { return latestForChannel; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    GitHubRelease^ UpdateState::PendingDownloadRelease::get() { System::Threading::Monitor::Enter(syncRoot); try { return pendingDownloadRelease; } finally { System::Threading::Monitor::Exit(syncRoot); } }
    void UpdateState::PendingDownloadRelease::set(GitHubRelease^ v) { System::Threading::Monitor::Enter(syncRoot); try { pendingDownloadRelease = v; } finally { System::Threading::Monitor::Exit(syncRoot); } }

    void UpdateState::RecalculateLatest()
    {
        // Called with lock already held
        latestForChannel = nullptr;
        if (cachedReleases == nullptr || cachedReleases->Count == 0) return;
        GitHubRelease^ best = nullptr;
        for each (GitHubRelease^ r in cachedReleases) {
            if (r == nullptr || r->IsDraft) continue;
            if (!ChannelHelper::IsIncludedBySelection(selectedChannel, r->Channel)) continue;
            if (r->Version == nullptr || !r->Version->IsValid) continue;
            // Only consider releases with installer asset for "latest available for update"
            // But for display, we still want latest even without asset? Spec says release list shows all; latest available for update should have asset.
            // We'll consider both, but prefer with asset.
            if (best == nullptr) best = r;
            else {
                int cmp = r->Version->CompareTo(best->Version);
                if (cmp > 0) best = r;
                else if (cmp == 0) {
                    // Prefer with asset
                    bool bestHas = best->HasInstallerAsset();
                    bool curHas = r->HasInstallerAsset();
                    if (curHas && !bestHas) best = r;
                }
            }
        }
        latestForChannel = best;
    }

    bool UpdateState::IsUpdateAvailable()
    {
        System::Threading::Monitor::Enter(syncRoot);
        try {
            if (latestForChannel == nullptr || installedVersion == nullptr) return false;
            if (latestForChannel->Version == nullptr || !latestForChannel->Version->IsValid) return false;
            return latestForChannel->Version->CompareTo(installedVersion) > 0;
        } finally { System::Threading::Monitor::Exit(syncRoot); }
    }

    bool UpdateState::IsInstalledNewerThanLatest()
    {
        System::Threading::Monitor::Enter(syncRoot);
        try {
            if (latestForChannel == nullptr || installedVersion == nullptr) return false;
            if (latestForChannel->Version == nullptr || !latestForChannel->Version->IsValid) return false;
            return installedVersion->CompareTo(latestForChannel->Version) > 0;
        } finally { System::Threading::Monitor::Exit(syncRoot); }
    }

    List<GitHubRelease^>^ UpdateState::GetReleasesForSelectedChannel()
    {
        return GetReleasesForChannel(SelectedChannel);
    }

    List<GitHubRelease^>^ UpdateState::GetAllReleasesSorted()
    {
        System::Threading::Monitor::Enter(syncRoot);
        try {
            List<GitHubRelease^>^ filtered = gcnew List<GitHubRelease^>();
            if (cachedReleases != nullptr) {
                for each (GitHubRelease^ r in cachedReleases) {
                    if (r == nullptr || r->IsDraft) continue;
                    filtered->Add(r);
                }
                for (int i = 0; i < filtered->Count; i++) {
                    for (int j = i + 1; j < filtered->Count; j++) {
                        GitHubRelease^ a = filtered[i];
                        GitHubRelease^ b = filtered[j];
                        int cmp = 0;
                        if (a == nullptr && b == nullptr) cmp = 0;
                        else if (a == nullptr) cmp = 1;
                        else if (b == nullptr) cmp = -1;
                        else if (a->Version == nullptr && b->Version == nullptr) cmp = 0;
                        else if (a->Version == nullptr) cmp = 1;
                        else if (b->Version == nullptr) cmp = -1;
                        else cmp = b->Version->CompareTo(a->Version);
                        if (cmp > 0) {
                            GitHubRelease^ tmp = filtered[i];
                            filtered[i] = filtered[j];
                            filtered[j] = tmp;
                        }
                    }
                }
            }
            return filtered;
        } finally { System::Threading::Monitor::Exit(syncRoot); }
    }

    List<GitHubRelease^>^ UpdateState::GetReleasesForChannel(UpdateChannel channel)
    {
        System::Threading::Monitor::Enter(syncRoot);
        try {
            List<GitHubRelease^>^ filtered = gcnew List<GitHubRelease^>();
            if (cachedReleases != nullptr) {
                for each (GitHubRelease^ r in cachedReleases) {
                    if (r == nullptr || r->IsDraft) continue;
                    if (ChannelHelper::IsIncludedBySelection(channel, r->Channel)) filtered->Add(r);
                }
                // Sort descending by version - manual bubble sort to avoid lambda capture issues
                for (int i = 0; i < filtered->Count; i++) {
                    for (int j = i + 1; j < filtered->Count; j++) {
                        GitHubRelease^ a = filtered[i];
                        GitHubRelease^ b = filtered[j];
                        int cmp = 0;
                        if (a == nullptr && b == nullptr) cmp = 0;
                        else if (a == nullptr) cmp = 1;
                        else if (b == nullptr) cmp = -1;
                        else if (a->Version == nullptr && b->Version == nullptr) cmp = 0;
                        else if (a->Version == nullptr) cmp = 1;
                        else if (b->Version == nullptr) cmp = -1;
                        else cmp = b->Version->CompareTo(a->Version);
                        if (cmp > 0) {
                            GitHubRelease^ tmp = filtered[i];
                            filtered[i] = filtered[j];
                            filtered[j] = tmp;
                        }
                    }
                }
            }
            return filtered;
        } finally { System::Threading::Monitor::Exit(syncRoot); }
    }

    GitHubRelease^ UpdateState::GetLatestForSelectedChannel()
    {
        System::Threading::Monitor::Enter(syncRoot);
        try { return latestForChannel; } finally { System::Threading::Monitor::Exit(syncRoot); }
    }

    GitHubRelease^ UpdateState::FindReleaseByTag(String^ tag)
    {
        if (String::IsNullOrEmpty(tag)) return nullptr;
        System::Threading::Monitor::Enter(syncRoot);
        try {
            if (cachedReleases != nullptr) {
                for each (GitHubRelease^ r in cachedReleases) {
                    if (r != nullptr && r->Tag != nullptr && r->Tag->Equals(tag, StringComparison::OrdinalIgnoreCase))
                        return r;
                }
            }
            return nullptr;
        } finally { System::Threading::Monitor::Exit(syncRoot); }
    }

    String^ UpdateState::GetCacheFilePath()
    {
        try {
            String^ dir = Path::Combine(Environment::GetFolderPath(Environment::SpecialFolder::ApplicationData), "Windows Hello Fix");
            return Path::Combine(dir, "updater_cache.json");
        } catch (...) { return nullptr; }
    }

    String^ UpdateState::GetEtagFilePath()
    {
        try {
            String^ dir = Path::Combine(Environment::GetFolderPath(Environment::SpecialFolder::ApplicationData), "Windows Hello Fix");
            return Path::Combine(dir, "updater_etag.txt");
        } catch (...) { return nullptr; }
    }

    bool UpdateState::LoadCacheFromDisk()
    {
        try {
            String^ path = GetCacheFilePath();
            if (String::IsNullOrEmpty(path) || !File::Exists(path)) return false;
            String^ json = File::ReadAllText(path);
            List<GitHubRelease^>^ rels;
            String^ et;
            String^ lm;
            DateTime lc;
            UpdateChannel ch;
            if (UpdateModels::TryDeserializeCacheJson(json, rels, et, lm, lc, ch)) {
                // Detect corrupted cache from prior 92r bug (StringBuilder Append int) — discard if any body contains 92r92n
                bool corrupted = false;
                if (rels != nullptr) {
                    for each (GitHubRelease^ r in rels) {
                        if (r != nullptr && r->Body != nullptr && r->Body->Contains("92r92n")) { corrupted = true; break; }
                    }
                }
                if (corrupted) {
                    try { File::Delete(path); } catch (...) {}
                    return false;
                }
                System::Threading::Monitor::Enter(syncRoot);
                try {
                    cachedReleases = rels != nullptr ? rels : gcnew List<GitHubRelease^>();
                    etag = et;
                    lastModified = lm;
                    lastCheckUtc = lc;
                    // Persisted channel overrides current? We keep current selection if user changed it after load
                    // But if load is initial, apply persisted
                    if (lastCheckUtc == DateTime::MinValue) {
                        // Don't overwrite selection on first load if caller already set preference? We'll apply anyway
                        selectedChannel = ch;
                    } else {
                        // If file has channel, adopt it
                        selectedChannel = ch;
                    }
                    RecalculateLatest();
                } finally { System::Threading::Monitor::Exit(syncRoot); }
                return true;
            }
            return false;
        } catch (...) { return false; }
    }

    bool UpdateState::SaveCacheToDisk()
    {
        try {
            String^ path = GetCacheFilePath();
            if (String::IsNullOrEmpty(path)) return false;
            String^ dir = Path::GetDirectoryName(path);
            if (!String::IsNullOrEmpty(dir)) Directory::CreateDirectory(dir);
            System::Threading::Monitor::Enter(syncRoot);
            String^ json;
            try {
                json = UpdateModels::SerializeReleasesToCacheJson(cachedReleases, etag, lastModified, lastCheckUtc, selectedChannel);
            } finally { System::Threading::Monitor::Exit(syncRoot); }
            // Atomic write: temp + move
            String^ tmp = path + ".tmp";
            File::WriteAllText(tmp, json);
            if (File::Exists(path)) File::Delete(path);
            File::Move(tmp, path);
            return true;
        } catch (...) { return false; }
    }

    bool UpdateState::NeedsRefresh(TimeSpan maxAge)
    {
        System::Threading::Monitor::Enter(syncRoot);
        try {
            if (lastCheckUtc == DateTime::MinValue) return true;
            TimeSpan age = DateTime::UtcNow - lastCheckUtc;
            return age > maxAge;
        } finally { System::Threading::Monitor::Exit(syncRoot); }
    }

    String^ UpdateState::StatusToString(UpdaterStatus s)
    {
        switch (s) {
        case UpdaterStatus::Idle: return "Idle";
        case UpdaterStatus::Checking: return "Checking";
        case UpdaterStatus::UpToDate: return "UpToDate";
        case UpdaterStatus::UpdateAvailable: return "UpdateAvailable";
        case UpdaterStatus::Downloading: return "Downloading";
        case UpdaterStatus::Installing: return "Installing";
        case UpdaterStatus::Error: return "Error";
        case UpdaterStatus::Offline: return "Offline";
        case UpdaterStatus::RateLimited: return "RateLimited";
        default: return "Unknown";
        }
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
