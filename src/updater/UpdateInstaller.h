#pragma once

#include "UpdateModels.h"

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    public ref class DownloadProgress sealed
    {
    public:
        property int Percent;
        property long long BytesReceived;
        property long long TotalBytes;

        DownloadProgress();
    };

    public ref class UpdateInstaller sealed
    {
    private:
        static void EnsureDownloadClient();

    public:
        static System::String^ DownloadToTemp(
            System::String^ url,
            long long expectedSize,
            System::String^ expectedSha256,
            System::IProgress<DownloadProgress^>^ progress,
            System::Threading::CancellationToken cancelToken,
            System::String^% errorDetail);

        static bool VerifyFile(System::String^ path, long long expectedSize, System::String^ expectedSha256, System::String^% errorDetail);
        static System::String^ ComputeSha256Hex(System::String^ path);
        static System::String^ CreateStagingPath();
        static System::String^ GetStagingRoot();
        static void CleanupStagingPath(System::String^ stagedPath);
        static void CleanupOldStagingFolders(int olderThanDays);
        static bool LaunchInstaller(System::String^ stagedPath, bool silent, System::String^% errorDetail);
        static System::String^ DownloadToUserPath(
            System::String^ url,
            System::String^ userPath,
            long long expectedSize,
            System::String^ expectedSha256,
            System::IProgress<DownloadProgress^>^ progress,
            System::Threading::CancellationToken cancelToken,
            System::String^% errorDetail);
        static bool IsValidGithubAssetUrl(System::String^ url);

    private:
        static bool IsValidUrlImpl(System::String^ url, bool checkHostAndPath);
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
