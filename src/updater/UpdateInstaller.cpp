#include "UpdateInstaller.h"
#undef GetTempPath
#include <windows.h>

using namespace System;
using namespace System::IO;
using namespace System::Net::Http;
using namespace System::Security::Cryptography;
using namespace System::Threading;
using namespace System::Diagnostics;
using namespace System::Text;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    DownloadProgress::DownloadProgress()
    {
        Percent = 0;
        BytesReceived = 0;
        TotalBytes = 0;
    }

    void UpdateInstaller::EnsureDownloadClient()
    {
        // No caching - create new client per call to avoid static managed issues
        // This method is now no-op; each Download method creates its own client
    }

    bool UpdateInstaller::IsValidUrlImpl(String^ url, bool checkHostAndPath)
    {
        if (String::IsNullOrWhiteSpace(url)) return false;
        Uri^ uri;
        if (!Uri::TryCreate(url, UriKind::Absolute, uri)) return false;
        if (uri->Scheme->ToLowerInvariant() != "https") return false;
        if (checkHostAndPath) {
            if (!uri->Host->Equals("github.com", StringComparison::OrdinalIgnoreCase)) return false;
            if (!uri->AbsolutePath->StartsWith("/Shivu516/Windows-Hello-Fix/releases/download/", StringComparison::OrdinalIgnoreCase))
                return false;
            String^ fileName = Path::GetFileName(uri->AbsolutePath);
            if (!fileName->Equals("Windows_Hello_Fix_Setup.exe", StringComparison::OrdinalIgnoreCase))
                return false;
        }
        return true;
    }

    bool UpdateInstaller::IsValidGithubAssetUrl(String^ url)
    {
        return IsValidUrlImpl(url, true);
    }

    String^ UpdateInstaller::GetStagingRoot()
    {
        try {
            String^ temp = System::IO::Path::GetTempPath();
            return System::IO::Path::Combine(temp, "WindowsHelloFix", "Updates");
        } catch (...) { return nullptr; }
    }

    String^ UpdateInstaller::CreateStagingPath()
    {
        try {
            String^ root = GetStagingRoot();
            if (String::IsNullOrEmpty(root)) return nullptr;
            String^ guid = Guid::NewGuid().ToString("N");
            String^ dir = System::IO::Path::Combine(root, guid);
            Directory::CreateDirectory(dir);
            return System::IO::Path::Combine(dir, "Windows_Hello_Fix_Setup.exe");
        } catch (...) { return nullptr; }
    }

    void UpdateInstaller::CleanupStagingPath(String^ stagedPath)
    {
        try {
            if (String::IsNullOrEmpty(stagedPath)) return;
            String^ dir = System::IO::Path::GetDirectoryName(stagedPath);
            String^ part = stagedPath + ".part";
            if (File::Exists(part)) try { File::Delete(part); } catch (...) {}
            if (File::Exists(stagedPath)) try { File::Delete(stagedPath); } catch (...) {}
            if (!String::IsNullOrEmpty(dir) && Directory::Exists(dir)) {
                try { Directory::Delete(dir, true); } catch (...) {}
            }
        } catch (...) {}
    }

    void UpdateInstaller::CleanupOldStagingFolders(int olderThanDays)
    {
        try {
            String^ root = GetStagingRoot();
            if (String::IsNullOrEmpty(root) || !Directory::Exists(root)) return;
            auto dirs = Directory::GetDirectories(root);
            DateTime cutoff = DateTime::UtcNow.AddDays(-olderThanDays);
            for each (String^ d in dirs) {
                try {
                    DateTime write = Directory::GetLastWriteTimeUtc(d);
                    if (write < cutoff) Directory::Delete(d, true);
                } catch (...) {}
            }
        } catch (...) {}
    }

    String^ UpdateInstaller::ComputeSha256Hex(String^ path)
    {
        if (String::IsNullOrEmpty(path) || !File::Exists(path)) return nullptr;
        try {
            auto sha = SHA256::Create();
            FileStream^ fs = gcnew FileStream(path, FileMode::Open, FileAccess::Read, FileShare::Read);
            try {
                array<unsigned char>^ hash = sha->ComputeHash(fs);
                StringBuilder^ sb = gcnew StringBuilder(hash->Length * 2);
                for each (unsigned char b in hash) sb->AppendFormat("{0:x2}", b);
                return sb->ToString();
            } finally { fs->Close(); }
        } catch (...) { return nullptr; }
    }

    bool UpdateInstaller::VerifyFile(String^ path, long long expectedSize, String^ expectedSha256, String^% errorDetail)
    {
        errorDetail = nullptr;
        if (String::IsNullOrEmpty(path) || !File::Exists(path)) { errorDetail = "File not found after download"; return false; }
        try {
            FileInfo^ info = gcnew FileInfo(path);
            long long actualSize = info->Length;
            if (expectedSize > 0 && actualSize != expectedSize) {
                errorDetail = String::Format("Size mismatch: expected {0}, got {1}", expectedSize, actualSize);
                return false;
            }
            if (!String::IsNullOrEmpty(expectedSha256)) {
                String^ actual = ComputeSha256Hex(path);
                if (String::IsNullOrEmpty(actual)) { errorDetail = "SHA256 compute failed"; return false; }
                String^ exp = expectedSha256->Trim()->ToLowerInvariant();
                if (exp->StartsWith("sha256:")) exp = exp->Substring(7);
                exp = exp->ToLowerInvariant()->Trim();
                if (actual->ToLowerInvariant() != exp) {
                    errorDetail = String::Format("Checksum mismatch: expected {0}, got {1}", exp, actual);
                    return false;
                }
            }
            return true;
        } catch (Exception^ ex) { errorDetail = ex->Message; return false; }
    }

    String^ UpdateInstaller::DownloadToTemp(String^ url, long long expectedSize, String^ expectedSha256, IProgress<DownloadProgress^>^ progress, CancellationToken cancelToken, String^% errorDetail)
    {
        errorDetail = nullptr;
        if (!IsValidGithubAssetUrl(url)) { errorDetail = "Invalid asset URL (allow-list)"; return nullptr; }
        String^ staged = CreateStagingPath();
        if (String::IsNullOrEmpty(staged)) { errorDetail = "Failed to create staging path"; return nullptr; }
        String^ part = staged + ".part";
        try {
            String^ dir = System::IO::Path::GetDirectoryName(staged);
            if (!String::IsNullOrEmpty(dir)) Directory::CreateDirectory(dir);

            HttpClientHandler^ handler = gcnew HttpClientHandler();
            handler->AllowAutoRedirect = true;
            handler->MaxAutomaticRedirections = 5;
            HttpClient^ localClient = gcnew HttpClient(handler, true);
            localClient->Timeout = TimeSpan::FromSeconds(120);
            localClient->DefaultRequestHeaders->UserAgent->ParseAdd("WindowsHelloFix-Updater/1.0 (https://github.com/Shivu516/Windows-Hello-Fix)");
            localClient->DefaultRequestHeaders->Accept->ParseAdd("application/octet-stream");

            HttpRequestMessage^ req = gcnew HttpRequestMessage(HttpMethod::Get, url);
            auto sendTask = localClient->SendAsync(req, HttpCompletionOption::ResponseHeadersRead, cancelToken);
            sendTask->Wait(cancelToken);
            HttpResponseMessage^ resp = sendTask->Result;
            if (!resp->IsSuccessStatusCode) {
                errorDetail = String::Format("HTTP {0} {1}", (int)resp->StatusCode, resp->ReasonPhrase);
                CleanupStagingPath(staged);
                return nullptr;
            }
            long long total = -1;
            if (resp->Content->Headers->ContentLength.HasValue) total = resp->Content->Headers->ContentLength.Value;
            else if (expectedSize > 0) total = expectedSize;

            auto streamTask = resp->Content->ReadAsStreamAsync();
            streamTask->Wait(cancelToken);
            System::IO::Stream^ netStream = streamTask->Result;
            FileStream^ fileStream = gcnew FileStream(part, FileMode::Create, FileAccess::Write, FileShare::None);
            try {
                array<unsigned char>^ buffer = gcnew array<unsigned char>(81920);
                long long received = 0;
                while (true) {
                    if (cancelToken.IsCancellationRequested) { errorDetail = "Cancelled"; CleanupStagingPath(staged); return nullptr; }
                    auto readTask = netStream->ReadAsync(buffer, 0, buffer->Length, cancelToken);
                    readTask->Wait(cancelToken);
                    int read = readTask->Result;
                    if (read <= 0) break;
                    fileStream->Write(buffer, 0, read);
                    received += read;
                    if (progress != nullptr && total > 0) {
                        int pct = (int)((received * 100) / total);
                        if (pct < 0) pct = 0; if (pct > 100) pct = 100;
                        DownloadProgress^ p = gcnew DownloadProgress();
                        p->BytesReceived = received;
                        p->TotalBytes = total;
                        p->Percent = pct;
                        progress->Report(p);
                    }
                }
                fileStream->Flush();
            } finally { fileStream->Close(); }

            String^ verifyErr;
            if (!VerifyFile(part, expectedSize, expectedSha256, verifyErr)) {
                errorDetail = "Verify failed: " + verifyErr;
                CleanupStagingPath(staged);
                return nullptr;
            }

            int retries = 0;
            while (retries < 3) {
                try {
                    if (File::Exists(staged)) File::Delete(staged);
                    File::Move(part, staged);
                    break;
                } catch (IOException^) {
                    retries++;
                    System::Threading::Thread::Sleep(500);
                    if (retries >= 3) { errorDetail = "File locked after download (antivirus?)"; CleanupStagingPath(staged); return nullptr; }
                }
            }

            if (!VerifyFile(staged, expectedSize, expectedSha256, verifyErr)) {
                errorDetail = "Final verify failed: " + verifyErr;
                CleanupStagingPath(staged);
                return nullptr;
            }

            return staged;
        } catch (OperationCanceledException^) {
            errorDetail = "Cancelled";
            CleanupStagingPath(staged);
            return nullptr;
        } catch (AggregateException^ ae) {
            Exception^ inner = ae->InnerException;
            if (inner != nullptr && dynamic_cast<OperationCanceledException^>(inner) != nullptr) {
                errorDetail = "Cancelled";
            } else {
                errorDetail = ae->Message;
                if (inner != nullptr) errorDetail = inner->Message;
            }
            CleanupStagingPath(staged);
            return nullptr;
        } catch (Exception^ ex) {
            errorDetail = ex->Message;
            CleanupStagingPath(staged);
            return nullptr;
        }
    }

    String^ UpdateInstaller::DownloadToUserPath(String^ url, String^ userPath, long long expectedSize, String^ expectedSha256, IProgress<DownloadProgress^>^ progress, CancellationToken cancelToken, String^% errorDetail)
    {
        errorDetail = nullptr;
        if (!IsValidGithubAssetUrl(url)) { errorDetail = "Invalid asset URL"; return nullptr; }
        if (String::IsNullOrEmpty(userPath)) { errorDetail = "No save path selected"; return nullptr; }
        String^ part = userPath + ".part";
        try {
            String^ dir = System::IO::Path::GetDirectoryName(userPath);
            if (!String::IsNullOrEmpty(dir)) Directory::CreateDirectory(dir);
            HttpClientHandler^ handler2 = gcnew HttpClientHandler();
            handler2->AllowAutoRedirect = true;
            HttpClient^ localClient2 = gcnew HttpClient(handler2, true);
            localClient2->Timeout = TimeSpan::FromSeconds(120);
            localClient2->DefaultRequestHeaders->UserAgent->ParseAdd("WindowsHelloFix-Updater/1.0 (https://github.com/Shivu516/Windows-Hello-Fix)");
            localClient2->DefaultRequestHeaders->Accept->ParseAdd("application/octet-stream");
            HttpRequestMessage^ req = gcnew HttpRequestMessage(HttpMethod::Get, url);
            auto sendTask = localClient2->SendAsync(req, HttpCompletionOption::ResponseHeadersRead, cancelToken);
            sendTask->Wait(cancelToken);
            HttpResponseMessage^ resp = sendTask->Result;
            if (!resp->IsSuccessStatusCode) { errorDetail = String::Format("HTTP {0}", (int)resp->StatusCode); return nullptr; }
            long long total = -1;
            if (resp->Content->Headers->ContentLength.HasValue) total = resp->Content->Headers->ContentLength.Value;
            else if (expectedSize > 0) total = expectedSize;
            auto streamTask = resp->Content->ReadAsStreamAsync();
            streamTask->Wait(cancelToken);
            System::IO::Stream^ netStream = streamTask->Result;
            FileStream^ fs = gcnew FileStream(part, FileMode::Create, FileAccess::Write, FileShare::None);
            try {
                array<unsigned char>^ buf = gcnew array<unsigned char>(81920);
                long long received = 0;
                while (true) {
                    if (cancelToken.IsCancellationRequested) { errorDetail = "Cancelled"; try { if (File::Exists(part)) File::Delete(part); } catch (...) {} return nullptr; }
                    auto rt = netStream->ReadAsync(buf, 0, buf->Length, cancelToken);
                    rt->Wait(cancelToken);
                    int n = rt->Result;
                    if (n <= 0) break;
                    fs->Write(buf, 0, n);
                    received += n;
                    if (progress != nullptr && total > 0) {
                        DownloadProgress^ p = gcnew DownloadProgress();
                        p->BytesReceived = received; p->TotalBytes = total; p->Percent = (int)((received * 100) / total);
                        progress->Report(p);
                    }
                }
                fs->Flush();
            } finally { fs->Close(); }
            String^ verifyErr;
            if (!VerifyFile(part, expectedSize, expectedSha256, verifyErr)) { errorDetail = "Verify: " + verifyErr; try { File::Delete(part); } catch (...) {} return nullptr; }
            if (File::Exists(userPath)) try { File::Delete(userPath); } catch (...) {}
            File::Move(part, userPath);
            return userPath;
        } catch (OperationCanceledException^) { errorDetail = "Cancelled"; try { if (File::Exists(part)) File::Delete(part); } catch (...) {} return nullptr; }
          catch (Exception^ ex) { errorDetail = ex->Message; try { if (File::Exists(part)) File::Delete(part); } catch (...) {} return nullptr; }
    }

    bool UpdateInstaller::LaunchInstaller(String^ stagedPath, bool silent, String^% errorDetail)
    {
        errorDetail = nullptr;
        if (String::IsNullOrEmpty(stagedPath) || !File::Exists(stagedPath)) { errorDetail = "Installer not found at staging path"; return false; }
        String^ name = System::IO::Path::GetFileName(stagedPath);
        if (!name->Equals("Windows_Hello_Fix_Setup.exe", StringComparison::OrdinalIgnoreCase)) { errorDetail = "Invalid installer filename"; return false; }
        try {
            String^ root = GetStagingRoot();
            if (!String::IsNullOrEmpty(root)) {
                String^ fullStaged = System::IO::Path::GetFullPath(stagedPath);
                String^ fullRoot = System::IO::Path::GetFullPath(root);
                if (!fullStaged->StartsWith(fullRoot, StringComparison::OrdinalIgnoreCase)) {
                    errorDetail = "Staged path not under expected temp root";
                    return false;
                }
            }
            ProcessStartInfo^ psi = gcnew ProcessStartInfo();
            psi->FileName = stagedPath;
            psi->UseShellExecute = true;
            psi->WorkingDirectory = System::IO::Path::GetDirectoryName(stagedPath);
            if (silent) psi->Arguments = "/S";
            psi->Verb = "runas";
            try {
                Process::Start(psi);
                return true;
            } catch (System::ComponentModel::Win32Exception^ wex) {
                if (wex->NativeErrorCode == 1223) { errorDetail = "User declined elevation"; return false; }
                try {
                    psi->Verb = "";
                    Process::Start(psi);
                    return true;
                } catch (Exception^ ex2) { errorDetail = ex2->Message; return false; }
            }
        } catch (Exception^ ex) { errorDetail = ex->Message; return false; }
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
