#include "UpdateModels.h"
#include <msclr\marshal_cppstd.h>

using namespace System;
using namespace System::Collections::Generic;
using namespace System::Text;
using namespace System::Globalization;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    ReleaseAsset::ReleaseAsset()
    {
        Size = 0;
        DownloadCount = 0;
        Name = nullptr;
        BrowserDownloadUrl = nullptr;
        ContentType = nullptr;
        Sha256 = nullptr;
    }

    GitHubRelease::GitHubRelease()
    {
        Id = 0;
        IsPrerelease = false;
        IsDraft = false;
        Channel = UpdateChannel::Unknown;
        HasUpdaterSupport = false;
        Assets = gcnew System::Collections::Generic::List<ReleaseAsset^>();
        PublishedAt = System::DateTime::MinValue;
        Tag = nullptr;
        Name = nullptr;
        HtmlUrl = nullptr;
        Body = nullptr;
        Version = nullptr;
    }

    ReleaseAsset^ GitHubRelease::GetAuthoritativeInstallerAsset()
    {
        if (Assets == nullptr || Assets->Count == 0) return nullptr;
        ReleaseAsset^ best = nullptr;
        int count = 0;
        for each (ReleaseAsset^ a in Assets) {
            if (a == nullptr || String::IsNullOrEmpty(a->Name)) continue;
            if (!a->Name->Equals("Windows_Hello_Fix_Setup.exe", StringComparison::OrdinalIgnoreCase)) continue;
            // Validate URL prefix if present
            if (!String::IsNullOrEmpty(a->BrowserDownloadUrl)) {
                if (!a->BrowserDownloadUrl->StartsWith("https://github.com/Shivu516/Windows-Hello-Fix/releases/download/", StringComparison::OrdinalIgnoreCase))
                    continue;
            }
            // Allow content_type application/x-msdownload or application/octet-stream or empty (legacy)
            count++;
            if (best == nullptr) best = a;
            else {
                // Prefer larger size (more complete) or with sha256
                bool bestHasSha = !String::IsNullOrEmpty(best->Sha256);
                bool curHasSha = !String::IsNullOrEmpty(a->Sha256);
                if (curHasSha && !bestHasSha) best = a;
                else if (a->Size > best->Size) best = a;
            }
        }
        // If multiple authoritative names, we return best but caller can check count>1 for warning
        return best;
    }

    bool GitHubRelease::HasInstallerAsset()
    {
        return GetAuthoritativeInstallerAsset() != nullptr;
    }

    // ---- JSON helpers ----

    String^ UpdateModels::UnescapeJsonString(String^ s)
    {
        if (s == nullptr) return nullptr;
        // Common unescape: \n \r \t \" \\ \/
        StringBuilder^ sb = gcnew StringBuilder(s->Length);
        for (int i = 0; i < s->Length; i++) {
            wchar_t c = s[i];
            if (c == L'\\' && i + 1 < s->Length) {
                wchar_t n = s[i + 1];
                if (n == L'n') { sb->Append(L'\n'); i++; }
                else if (n == L'r') { sb->Append(L'\r'); i++; }
                else if (n == L't') { sb->Append(L'\t'); i++; }
                else if (n == L'"') { sb->Append(L'"'); i++; }
                else if (n == L'\\') { sb->Append(L'\\'); i++; }
                else if (n == L'/') { sb->Append(L'/'); i++; }
                else if (n == 'u' && i + 5 < s->Length) {
                    String^ hex = s->Substring(i + 2, 4);
                    try {
                        int code = Int32::Parse(hex, NumberStyles::HexNumber);
                        sb->Append((wchar_t)code);
                        i += 5;
                    } catch (...) { sb->Append(c); }
                } else { sb->Append(c); }
            } else {
                sb->Append(c);
            }
        }
        return sb->ToString();
    }

    String^ UpdateModels::EscapeForJson(String^ s)
    {
        if (s == nullptr) return "";
        StringBuilder^ sb = gcnew StringBuilder();
        for each (wchar_t c in s) {
            if (c == '"') sb->Append("\\\"");
            else if (c == '\\') sb->Append("\\\\");
            else if (c == '\n') sb->Append("\\n");
            else if (c == '\r') sb->Append("\\r");
            else if (c == '\t') sb->Append("\\t");
            else if (c < 0x20) sb->AppendFormat("\\u{0:X4}", (int)c);
            else sb->Append(c);
        }
        return sb->ToString();
    }

    int UpdateModels::FindMatchingBrace(String^ s, int openPos, wchar_t openChar, wchar_t closeChar)
    {
        if (openPos < 0 || openPos >= s->Length || s[openPos] != openChar) return -1;
        int depth = 0;
        bool inString = false;
        bool escape = false;
        for (int i = openPos; i < s->Length; i++) {
            wchar_t c = s[i];
            if (escape) { escape = false; continue; }
            if (c == '\\' && inString) { escape = true; continue; }
            if (c == '"') { inString = !inString; continue; }
            if (inString) continue;
            if (c == openChar) depth++;
            else if (c == closeChar) {
                depth--;
                if (depth == 0) return i;
            }
        }
        return -1;
    }

    String^ UpdateModels::GetJsonStringField(String^ json, String^ key)
    {
        if (json == nullptr || key == nullptr) return nullptr;
        String^ pattern = "\"" + key + "\"";
        int idx = json->IndexOf(pattern, StringComparison::Ordinal);
        if (idx < 0) return nullptr;
        int colon = json->IndexOf(':', idx + pattern->Length);
        if (colon < 0) return nullptr;
        int p = colon + 1;
        while (p < json->Length && Char::IsWhiteSpace(json[p])) p++;
        if (p >= json->Length) return nullptr;
        if (json[p] == 'n') { // null
            if (p + 3 < json->Length && json->Substring(p, 4) == "null") return nullptr;
            return nullptr;
        }
        if (json[p] != '"') return nullptr; // not a string
        int start = p + 1;
        StringBuilder^ sb = gcnew StringBuilder();
        bool esc = false;
        for (int i = start; i < json->Length; i++) {
            wchar_t c = json[i];
            if (esc) {
                // keep escaped as two chars for Unescape to handle
                sb->Append(L'\\');
                sb->Append(c);
                esc = false;
            } else if (c == L'\\') {
                esc = true;
            } else if (c == L'"') {
                return UnescapeJsonString(sb->ToString());
            } else {
                sb->Append(c);
            }
        }
        return nullptr;
    }

    bool UpdateModels::GetJsonBoolField(String^ json, String^ key, bool% out)
    {
        String^ pattern = "\"" + key + "\"";
        int idx = json->IndexOf(pattern, StringComparison::Ordinal);
        if (idx < 0) return false;
        int colon = json->IndexOf(':', idx + pattern->Length);
        if (colon < 0) return false;
        int p = colon + 1;
        while (p < json->Length && Char::IsWhiteSpace(json[p])) p++;
        if (p + 3 < json->Length && json->Substring(p, 4)->ToLowerInvariant() == "true") { out = true; return true; }
        if (p + 4 < json->Length && json->Substring(p, 5)->ToLowerInvariant() == "false") { out = false; return true; }
        return false;
    }

    long long UpdateModels::GetJsonLongField(String^ json, String^ key, long long def)
    {
        String^ pattern = "\"" + key + "\"";
        int idx = json->IndexOf(pattern, StringComparison::Ordinal);
        if (idx < 0) return def;
        int colon = json->IndexOf(':', idx + pattern->Length);
        if (colon < 0) return def;
        int p = colon + 1;
        while (p < json->Length && Char::IsWhiteSpace(json[p])) p++;
        int start = p;
        while (p < json->Length && (Char::IsDigit(json[p]) || json[p] == '-')) p++;
        if (p == start) return def;
        String^ num = json->Substring(start, p - start);
        long long val;
        if (Int64::TryParse(num, val)) return val;
        return def;
    }

    DateTime UpdateModels::ParseGitHubDate(String^ s)
    {
        if (String::IsNullOrEmpty(s)) return DateTime::MinValue;
        try {
            // GitHub uses ISO8601 e.g. 2026-06-08T06:13:00Z
            return DateTime::Parse(s, nullptr, DateTimeStyles::RoundtripKind);
        } catch (...) {
            try { return DateTime::Parse(s); } catch (...) { return DateTime::MinValue; }
        }
    }

    List<ReleaseAsset^>^ UpdateModels::ParseAssetsArray(String^ assetsArrayJson)
    {
        List<ReleaseAsset^>^ list = gcnew List<ReleaseAsset^>();
        if (String::IsNullOrEmpty(assetsArrayJson)) return list;
        String^ s = assetsArrayJson->Trim();
        if (s->Length < 2 || s[0] != '[') return list;
        int pos = 1;
        while (pos < s->Length) {
            while (pos < s->Length && Char::IsWhiteSpace(s[pos]) || pos < s->Length && s[pos] == ',') pos++;
            if (pos >= s->Length || s[pos] == ']') break;
            if (s[pos] != '{') { pos++; continue; }
            int end = FindMatchingBrace(s, pos, '{', '}');
            if (end < 0) break;
            String^ obj = s->Substring(pos, end - pos + 1);
            ReleaseAsset^ a = gcnew ReleaseAsset();
            a->Name = GetJsonStringField(obj, "name");
            a->BrowserDownloadUrl = GetJsonStringField(obj, "browser_download_url");
            a->ContentType = GetJsonStringField(obj, "content_type");
            a->Size = GetJsonLongField(obj, "size", 0);
            a->DownloadCount = (int)GetJsonLongField(obj, "download_count", 0);
            String^ digest = GetJsonStringField(obj, "digest");
            if (!String::IsNullOrEmpty(digest)) {
                if (digest->StartsWith("sha256:", StringComparison::OrdinalIgnoreCase))
                    a->Sha256 = digest->Substring(7);
                else
                    a->Sha256 = digest;
                // Normalize lower
                if (a->Sha256 != nullptr) a->Sha256 = a->Sha256->Trim()->ToLowerInvariant();
            }
            list->Add(a);
            pos = end + 1;
        }
        return list;
    }

    bool UpdateModels::IsValidTag(String^ tag)
    {
        if (String::IsNullOrWhiteSpace(tag)) return false;
        UpdateVersion^ v;
        return UpdateVersion::TryParse(tag, v) && v->IsValid;
    }

    String^ UpdateModels::ExtractTagFromJsonObject(String^ objJson)
    {
        return GetJsonStringField(objJson, "tag_name");
    }

    GitHubRelease^ UpdateModels::ParseOneReleaseObject(String^ objJson)
    {
        if (String::IsNullOrEmpty(objJson)) return nullptr;
        GitHubRelease^ r = gcnew GitHubRelease();
        r->Id = GetJsonLongField(objJson, "id", 0);
        r->Tag = GetJsonStringField(objJson, "tag_name");
        r->Name = GetJsonStringField(objJson, "name");
        r->HtmlUrl = GetJsonStringField(objJson, "html_url");
        r->Body = GetJsonStringField(objJson, "body");
        String^ published = GetJsonStringField(objJson, "published_at");
        if (String::IsNullOrEmpty(published)) published = GetJsonStringField(objJson, "created_at");
        r->PublishedAt = ParseGitHubDate(published);
        bool pr = false, dr = false;
        if (GetJsonBoolField(objJson, "prerelease", pr)) r->IsPrerelease = pr;
        if (GetJsonBoolField(objJson, "draft", dr)) r->IsDraft = dr;

        // Version parse
        if (!String::IsNullOrEmpty(r->Tag)) {
            r->Version = UpdateVersion::Parse(r->Tag);
            r->HasUpdaterSupport = r->Version->IsUpdaterSupported();
        } else {
            r->Version = gcnew UpdateVersion(0, 0, 0, nullptr, 0, r->Tag, false);
            r->HasUpdaterSupport = false;
        }
        r->Channel = ChannelHelper::FromRelease(r->IsPrerelease, r->Tag, r->Name);

        // Assets: find "assets": [
        int assetsIdx = objJson->IndexOf("\"assets\"", StringComparison::Ordinal);
        if (assetsIdx >= 0) {
            int colon = objJson->IndexOf(':', assetsIdx);
            if (colon >= 0) {
                int arrStart = objJson->IndexOf('[', colon);
                if (arrStart >= 0) {
                    int arrEnd = FindMatchingBrace(objJson, arrStart, '[', ']');
                    if (arrEnd >= 0) {
                        String^ arrJson = objJson->Substring(arrStart, arrEnd - arrStart + 1);
                        r->Assets = ParseAssetsArray(arrJson);
                    }
                }
            }
        }
        return r;
    }

    List<GitHubRelease^>^ UpdateModels::ParseReleasesJson(String^ json)
    {
        List<GitHubRelease^>^ list = gcnew List<GitHubRelease^>();
        if (String::IsNullOrEmpty(json)) return list;
        String^ s = json->Trim();
        if (s->Length == 0) return list;
        // Could be array [ ... ] or single object { ... }
        if (s[0] == '[') {
            int pos = 1;
            while (pos < s->Length) {
                while (pos < s->Length && (Char::IsWhiteSpace(s[pos]) || s[pos] == ',')) pos++;
                if (pos >= s->Length || s[pos] == ']') break;
                if (s[pos] != '{') { pos++; continue; }
                int end = FindMatchingBrace(s, pos, '{', '}');
                if (end < 0) break;
                String^ obj = s->Substring(pos, end - pos + 1);
                GitHubRelease^ r = ParseOneReleaseObject(obj);
                if (r != nullptr && !r->IsDraft) // filter drafts per spec
                    list->Add(r);
                pos = end + 1;
            }
        } else if (s[0] == '{') {
            GitHubRelease^ r = ParseOneReleaseObject(s);
            if (r != nullptr && !r->IsDraft) list->Add(r);
        }
        return list;
    }

    GitHubRelease^ UpdateModels::ParseSingleReleaseJson(String^ json)
    {
        auto list = ParseReleasesJson(json);
        if (list->Count > 0) return list[0];
        return nullptr;
    }

    String^ UpdateModels::SerializeReleasesToCacheJson(List<GitHubRelease^>^ releases, String^ etag, String^ lastModified, DateTime lastCheckUtc, UpdateChannel channel)
    {
        StringBuilder^ sb = gcnew StringBuilder();
        sb->Append("{");
        sb->AppendFormat("\"etag\":\"{0}\",", EscapeForJson(etag != nullptr ? etag : ""));
        sb->AppendFormat("\"lastModified\":\"{0}\",", EscapeForJson(lastModified != nullptr ? lastModified : ""));
        sb->AppendFormat("\"lastCheckUtc\":\"{0}\",", lastCheckUtc.ToString("o"));
        sb->AppendFormat("\"channel\":\"{0}\",", EscapeForJson(ChannelHelper::ToPersistedString(channel)));
        sb->Append("\"releases\":[");
        for (int i = 0; i < releases->Count; i++) {
            auto r = releases[i];
            sb->Append("{");
            sb->AppendFormat("\"id\":{0},", r->Id);
            sb->AppendFormat("\"tag\":\"{0}\",", EscapeForJson(r->Tag));
            sb->AppendFormat("\"name\":\"{0}\",", EscapeForJson(r->Name));
            sb->AppendFormat("\"html_url\":\"{0}\",", EscapeForJson(r->HtmlUrl));
            sb->AppendFormat("\"body\":\"{0}\",", EscapeForJson(r->Body));
            sb->AppendFormat("\"published_at\":\"{0}\",", r->PublishedAt.ToString("o"));
            sb->AppendFormat("\"prerelease\":{0},", r->IsPrerelease ? "true" : "false");
            sb->AppendFormat("\"draft\":{0},", r->IsDraft ? "true" : "false");
            sb->Append("\"assets\":[");
            for (int j = 0; j < r->Assets->Count; j++) {
                auto a = r->Assets[j];
                sb->Append("{");
                sb->AppendFormat("\"name\":\"{0}\",", EscapeForJson(a->Name));
                sb->AppendFormat("\"browser_download_url\":\"{0}\",", EscapeForJson(a->BrowserDownloadUrl));
                sb->AppendFormat("\"content_type\":\"{0}\",", EscapeForJson(a->ContentType));
                sb->AppendFormat("\"size\":{0},", a->Size);
                sb->AppendFormat("\"digest\":\"{0}\",", EscapeForJson(a->Sha256 != nullptr ? "sha256:" + a->Sha256 : ""));
                sb->AppendFormat("\"download_count\":{0}", a->DownloadCount);
                sb->Append("}");
                if (j + 1 < r->Assets->Count) sb->Append(",");
            }
            sb->Append("]");
            sb->Append("}");
            if (i + 1 < releases->Count) sb->Append(",");
        }
        sb->Append("]");
        sb->Append("}");
        return sb->ToString();
    }

    bool UpdateModels::TryDeserializeCacheJson(String^ json, List<GitHubRelease^>^% releases, String^% etag, String^% lastModified, DateTime% lastCheckUtc, UpdateChannel% channel)
    {
        releases = gcnew List<GitHubRelease^>();
        etag = nullptr; lastModified = nullptr; lastCheckUtc = DateTime::MinValue; channel = UpdateChannel::Stable;
        if (String::IsNullOrEmpty(json)) return false;
        try {
            etag = GetJsonStringField(json, "etag");
            lastModified = GetJsonStringField(json, "lastModified");
            String^ lastCheck = GetJsonStringField(json, "lastCheckUtc");
            if (!String::IsNullOrEmpty(lastCheck)) {
                try { lastCheckUtc = DateTime::Parse(lastCheck, nullptr, DateTimeStyles::RoundtripKind); } catch (...) {}
            }
            String^ ch = GetJsonStringField(json, "channel");
            if (!String::IsNullOrEmpty(ch)) {
                UpdateChannel parsed;
                if (ChannelHelper::TryParsePersisted(ch, parsed)) channel = parsed;
            }
            int relIdx = json->IndexOf("\"releases\"", StringComparison::Ordinal);
            if (relIdx < 0) return true; // no releases array but other fields parsed
            int colon = json->IndexOf(':', relIdx);
            int arrStart = json->IndexOf('[', colon);
            int arrEnd = FindMatchingBrace(json, arrStart, '[', ']');
            if (arrStart < 0 || arrEnd < 0) return true;
            String^ arrJson = json->Substring(arrStart, arrEnd - arrStart + 1);
            // Reuse ParseAssets-style but for releases
            // Build a JSON array string with objects that mimic GitHub shape: map our cache fields to parseable
            // For simplicity, parse manually: split objects
            int pos = 1;
            while (pos < arrJson->Length) {
                while (pos < arrJson->Length && (Char::IsWhiteSpace(arrJson[pos]) || arrJson[pos] == ',')) pos++;
                if (pos >= arrJson->Length || arrJson[pos] == ']') break;
                if (arrJson[pos] != '{') { pos++; continue; }
                int end = FindMatchingBrace(arrJson, pos, '{', '}');
                if (end < 0) break;
                String^ obj = arrJson->Substring(pos, end - pos + 1);
                // Convert cache obj to GitHub-like obj for ParseOneReleaseObject
                // Cache stores html_url as html_url, etc.
                // ParseOneReleaseObject expects tag_name, name, html_url, body, published_at, prerelease, draft, assets (with browser_download_url etc.)
                // Our cache already matches those keys (we wrote html_url, body, published_at, prerelease, draft) — need to ensure mapping
                // The cache writes "tag" not "tag_name" — bridge it
                String^ tag = GetJsonStringField(obj, "tag");
                if (!String::IsNullOrEmpty(tag)) {
                    // Inject tag_name if missing
                    if (obj->IndexOf("\"tag_name\"", StringComparison::Ordinal) < 0) {
                        // Replace first occurrence of "\"tag\""
                        int tagIdx = obj->IndexOf("\"tag\"", StringComparison::Ordinal);
                        if (tagIdx >= 0) {
                            String^ before = obj->Substring(0, tagIdx);
                            String^ after = obj->Substring(tagIdx + 5); // len "\"tag\"" =5
                            obj = before + "\"tag_name\"" + after;
                        }
                    }
                }
                GitHubRelease^ r = ParseOneReleaseObject(obj);
                if (r != nullptr) releases->Add(r);
                pos = end + 1;
            }
            return true;
        } catch (...) { return false; }
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
