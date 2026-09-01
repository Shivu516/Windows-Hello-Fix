#include "GitHubReleaseClient.h"

using namespace System;
using namespace System::Net::Http;
using namespace System::Threading;
using namespace System::Collections::Generic;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    FetchResult::FetchResult()
    {
        Status = FetchStatus::NetworkError;
        Releases = gcnew List<GitHubRelease^>();
        ETag = nullptr;
        LastModified = nullptr;
        ErrorDetail = nullptr;
        RateLimitRemaining = -1;
        RateLimitReset = DateTime::MinValue;
        HttpStatusCode = 0;
    }

    GitHubReleaseClient::GitHubReleaseClient() : owner("Shivu516"), repo("Windows-Hello-Fix"), httpClient(nullptr)
    {
        EnsureClient();
    }

    GitHubReleaseClient::GitHubReleaseClient(String^ owner, String^ repo) : owner(owner), repo(repo), httpClient(nullptr)
    {
        if (String::IsNullOrEmpty(owner)) this->owner = "Shivu516";
        if (String::IsNullOrEmpty(repo)) this->repo = "Windows-Hello-Fix";
        EnsureClient();
    }

    void GitHubReleaseClient::EnsureClient()
    {
        if (httpClient != nullptr) return;
        HttpClientHandler^ handler = gcnew HttpClientHandler();
        httpClient = gcnew HttpClient(handler, true);
        httpClient->Timeout = TimeSpan::FromSeconds(15);
        httpClient->DefaultRequestHeaders->UserAgent->ParseAdd("WindowsHelloFix-Updater/1.0 (https://github.com/Shivu516/Windows-Hello-Fix)");
        httpClient->DefaultRequestHeaders->Accept->ParseAdd("application/vnd.github+json");
        try { httpClient->DefaultRequestHeaders->Add("X-GitHub-Api-Version", "2022-11-28"); } catch (...) {}
    }

    FetchResult^ GitHubReleaseClient::FetchReleases(String^ etag, String^ lastModified, CancellationToken cancelToken)
    {
        String^ url = String::Format("https://api.github.com/repos/{0}/{1}/releases?per_page=20&page=1", owner, repo);
        return DoFetch(url, etag, lastModified, cancelToken);
    }

    FetchResult^ GitHubReleaseClient::FetchLatestRelease(String^ etag, CancellationToken cancelToken)
    {
        String^ url = String::Format("https://api.github.com/repos/{0}/{1}/releases/latest", owner, repo);
        return DoFetch(url, etag, nullptr, cancelToken);
    }

    DateTime GitHubReleaseClient::ParseRateLimitReset(String^ headerValue)
    {
        if (String::IsNullOrEmpty(headerValue)) return DateTime::MinValue;
        try {
            long long secs;
            if (Int64::TryParse(headerValue->Trim(), secs)) {
                DateTime epoch(1970, 1, 1, 0, 0, 0, DateTimeKind::Utc);
                return epoch.AddSeconds((double)secs);
            }
        } catch (...) {}
        return DateTime::MinValue;
    }

    FetchResult^ GitHubReleaseClient::DoFetch(String^ url, String^ etag, String^ lastModified, CancellationToken cancelToken)
    {
        FetchResult^ result = gcnew FetchResult();
        if (cancelToken.IsCancellationRequested) { result->Status = FetchStatus::Cancelled; return result; }
        EnsureClient();
        try {
            HttpRequestMessage^ req = gcnew HttpRequestMessage(HttpMethod::Get, url);
            if (!String::IsNullOrEmpty(etag)) {
                String^ et = etag->Trim();
                try { req->Headers->TryAddWithoutValidation("If-None-Match", et); } catch (...) {}
            }
            if (!String::IsNullOrEmpty(lastModified)) {
                try { req->Headers->TryAddWithoutValidation("If-Modified-Since", lastModified); } catch (...) {}
            }

            // Synchronous wait but with cancellation
            auto sendTask = httpClient->SendAsync(req, HttpCompletionOption::ResponseHeadersRead, cancelToken);
            sendTask->Wait(cancelToken);
            HttpResponseMessage^ resp = sendTask->Result;

            result->HttpStatusCode = (int)resp->StatusCode;

            try {
                if (resp->Headers->Contains("X-RateLimit-Remaining")) {
                    auto vals = resp->Headers->GetValues("X-RateLimit-Remaining");
                    for each (String^ v in vals) { int rem; if (Int32::TryParse(v, rem)) result->RateLimitRemaining = rem; }
                }
                if (resp->Headers->Contains("X-RateLimit-Reset")) {
                    auto vals = resp->Headers->GetValues("X-RateLimit-Reset");
                    for each (String^ v in vals) { result->RateLimitReset = ParseRateLimitReset(v); }
                }
                if (resp->Headers->ETag != nullptr && resp->Headers->ETag->Tag != nullptr) result->ETag = resp->Headers->ETag->Tag;
                else if (resp->Headers->Contains("ETag")) {
                    auto vals = resp->Headers->GetValues("ETag");
                    for each (String^ v in vals) { result->ETag = v; break; }
                }
                if (resp->Content != nullptr && resp->Content->Headers->Contains("Last-Modified")) {
                    auto vals = resp->Content->Headers->GetValues("Last-Modified");
                    for each (String^ v in vals) { result->LastModified = v; break; }
                }
            } catch (...) {}

            if (resp->StatusCode == System::Net::HttpStatusCode::NotModified) {
                result->Status = FetchStatus::NotModified;
                return result;
            }
            if ((int)resp->StatusCode == 429) {
                result->Status = FetchStatus::RateLimited;
                try {
                    String^ body = resp->Content->ReadAsStringAsync()->Result;
                    result->ErrorDetail = "Rate limited: " + body;
                } catch (...) { result->ErrorDetail = "Rate limited"; }
                return result;
            }
            if ((int)resp->StatusCode >= 500) {
                result->Status = FetchStatus::ServerError;
                try { result->ErrorDetail = resp->Content->ReadAsStringAsync()->Result; } catch (...) {}
                return result;
            }
            if ((int)resp->StatusCode >= 400) {
                result->Status = FetchStatus::NetworkError;
                try { result->ErrorDetail = String::Format("HTTP {0}: {1}", (int)resp->StatusCode, resp->ReasonPhrase); } catch (...) {}
                return result;
            }

            String^ json = resp->Content->ReadAsStringAsync()->Result;
            if (String::IsNullOrEmpty(json)) {
                result->Status = FetchStatus::Malformed;
                result->ErrorDetail = "Empty response";
                return result;
            }
            try {
                auto releases = UpdateModels::ParseReleasesJson(json);
                result->Releases = releases != nullptr ? releases : gcnew List<GitHubRelease^>();
                result->Status = FetchStatus::Success;
            } catch (Exception^ ex) {
                result->Status = FetchStatus::Malformed;
                result->ErrorDetail = "JSON parse: " + ex->Message;
            }
            return result;
        } catch (OperationCanceledException^) {
            result->Status = FetchStatus::Cancelled;
            result->ErrorDetail = "Cancelled";
            return result;
        } catch (AggregateException^ ae) {
            Exception^ inner = ae->InnerException;
            if (inner != nullptr && dynamic_cast<OperationCanceledException^>(inner) != nullptr) {
                result->Status = FetchStatus::Cancelled;
                result->ErrorDetail = "Cancelled";
                return result;
            }
            if (inner != nullptr && dynamic_cast<HttpRequestException^>(inner) != nullptr) {
                result->Status = FetchStatus::NetworkError;
                result->ErrorDetail = inner->Message;
                return result;
            }
            result->Status = FetchStatus::NetworkError;
            result->ErrorDetail = ae->Message;
            return result;
        } catch (HttpRequestException^ ex) {
            result->Status = FetchStatus::NetworkError;
            result->ErrorDetail = ex->Message;
            if (ex->Message != nullptr && (ex->Message->Contains("NameResolution") || ex->Message->Contains("No such host") || ex->Message->Contains("Unable to connect")))
                result->ErrorDetail = "Offline: " + ex->Message;
            return result;
        } catch (Exception^ ex) {
            result->Status = FetchStatus::NetworkError;
            result->ErrorDetail = ex->Message;
            return result;
        }
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
