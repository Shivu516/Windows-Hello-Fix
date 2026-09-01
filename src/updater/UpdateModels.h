#pragma once

#include "UpdateVersion.h"
#include "UpdateChannel.h"

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    public ref class ReleaseAsset sealed
    {
    public:
        property System::String^ Name;
        property System::String^ BrowserDownloadUrl;
        property System::String^ ContentType;
        property long long Size;
        property System::String^ Sha256;
        property int DownloadCount;

        ReleaseAsset();
    };

    public ref class GitHubRelease sealed
    {
    public:
        property long long Id;
        property System::String^ Tag;
        property UpdateVersion^ Version;
        property System::String^ Name;
        property System::String^ HtmlUrl;
        property System::String^ Body;
        property System::DateTime PublishedAt;
        property bool IsPrerelease;
        property bool IsDraft;
        property UpdateChannel Channel;
        property System::Collections::Generic::List<ReleaseAsset^>^ Assets;
        property bool HasUpdaterSupport;

        GitHubRelease();

        ReleaseAsset^ GetAuthoritativeInstallerAsset();
        bool HasInstallerAsset();
    };

    public ref class UpdateModels sealed
    {
    public:
        static System::Collections::Generic::List<GitHubRelease^>^ ParseReleasesJson(System::String^ json);
        static GitHubRelease^ ParseSingleReleaseJson(System::String^ json);
        static System::String^ SerializeReleasesToCacheJson(System::Collections::Generic::List<GitHubRelease^>^ releases, System::String^ etag, System::String^ lastModified, System::DateTime lastCheckUtc, UpdateChannel channel);
        static bool TryDeserializeCacheJson(System::String^ json, System::Collections::Generic::List<GitHubRelease^>^% releases, System::String^% etag, System::String^% lastModified, System::DateTime% lastCheckUtc, UpdateChannel% channel);
        static bool IsValidTag(System::String^ tag);
        static System::String^ ExtractTagFromJsonObject(System::String^ objJson);

    private:
        static System::String^ GetJsonStringField(System::String^ json, System::String^ key);
        static bool GetJsonBoolField(System::String^ json, System::String^ key, bool% out);
        static long long GetJsonLongField(System::String^ json, System::String^ key, long long def);
        static System::String^ UnescapeJsonString(System::String^ s);
        static System::DateTime ParseGitHubDate(System::String^ s);
        static System::Collections::Generic::List<ReleaseAsset^>^ ParseAssetsArray(System::String^ assetsArrayJson);
        static GitHubRelease^ ParseOneReleaseObject(System::String^ objJson);
        static System::String^ EscapeForJson(System::String^ s);
        static int FindMatchingBrace(System::String^ s, int openPos, wchar_t openChar, wchar_t closeChar);
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
