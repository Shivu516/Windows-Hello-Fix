#pragma once

#include "UpdateModels.h"
#include "UpdateState.h"

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    public enum class FetchStatus
    {
        Success = 0,
        NotModified = 1,
        NetworkError = 2,
        RateLimited = 3,
        ServerError = 4,
        Malformed = 5,
        Cancelled = 6
    };

    public ref class FetchResult sealed
    {
    public:
        property FetchStatus Status;
        property System::Collections::Generic::List<GitHubRelease^>^ Releases;
        property System::String^ ETag;
        property System::String^ LastModified;
        property System::String^ ErrorDetail;
        property int RateLimitRemaining;
        property System::DateTime RateLimitReset;
        property int HttpStatusCode;

        FetchResult();
    };

    public ref class GitHubReleaseClient sealed
    {
    private:
        System::Net::Http::HttpClient^ httpClient;
        System::String^ owner;
        System::String^ repo;
        void EnsureClient();
        FetchResult^ DoFetch(System::String^ url, System::String^ etag, System::String^ lastModified, System::Threading::CancellationToken cancelToken);
        static System::DateTime ParseRateLimitReset(System::String^ headerValue);

    public:
        GitHubReleaseClient();
        GitHubReleaseClient(System::String^ owner, System::String^ repo);

        FetchResult^ FetchReleases(System::String^ etag, System::String^ lastModified, System::Threading::CancellationToken cancelToken);
        FetchResult^ FetchLatestRelease(System::String^ etag, System::Threading::CancellationToken cancelToken);
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
