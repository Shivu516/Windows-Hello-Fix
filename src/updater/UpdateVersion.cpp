#include "UpdateVersion.h"
#include <msclr\marshal_cppstd.h>

using namespace System;
using namespace System::Text::RegularExpressions;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    UpdateVersion::UpdateVersion(int major, int minor, int patch, String^ prereleaseLabel, int prereleaseNumber, String^ rawTag, bool isValid)
        : major(major), minor(minor), patch(patch), prereleaseLabel(prereleaseLabel), prereleaseNumber(prereleaseNumber), rawTag(rawTag), isValid(isValid)
    {
        if (!String::IsNullOrEmpty(prereleaseLabel))
            normalizedLabelLower = prereleaseLabel->ToLowerInvariant();
        else
            normalizedLabelLower = nullptr;
    }

    int UpdateVersion::RankForLabel(String^ labelLower)
    {
        if (String::IsNullOrEmpty(labelLower)) return 100; // stable highest
        if (labelLower->StartsWith("alpha")) return 10;
        if (labelLower->StartsWith("beta")) return 20;
        if (labelLower->StartsWith("pre")) return 30; // pre, preview
        if (labelLower->Contains("preview")) return 30;
        if (labelLower->StartsWith("rc")) return 40;
        return 25; // unknown prerelease in middle
    }

    bool UpdateVersion::IsUpdaterSupported()
    {
        if (!isValid) return false;
        if (major > 2) return true;
        if (major == 2 && minor >= 1) return true;
        return false;
    }

    String^ UpdateVersion::ToDisplayString()
    {
        if (!isValid) return rawTag != nullptr ? rawTag : "unknown";
        String^ base = String::Format("v{0}.{1}.{2}", major, minor, patch);
        if (!String::IsNullOrEmpty(prereleaseLabel))
            return base + "-" + prereleaseLabel;
        return base;
    }

    int UpdateVersion::CompareTo(UpdateVersion^ other)
    {
        if (other == nullptr) return 1;
        // Invalid handling: invalid < valid, invalid-invalid by rawTag lexical
        if (!this->isValid && !other->isValid) {
            if (this->rawTag == nullptr && other->rawTag == nullptr) return 0;
            if (this->rawTag == nullptr) return -1;
            if (other->rawTag == nullptr) return 1;
            return String::Compare(this->rawTag, other->rawTag, StringComparison::OrdinalIgnoreCase);
        }
        if (!this->isValid) return -1;
        if (!other->isValid) return 1;

        if (major != other->major) return major < other->major ? -1 : 1;
        if (minor != other->minor) return minor < other->minor ? -1 : 1;
        if (patch != other->patch) return patch < other->patch ? -1 : 1;

        bool thisStable = String::IsNullOrEmpty(prereleaseLabel);
        bool otherStable = String::IsNullOrEmpty(other->prereleaseLabel);
        if (thisStable && otherStable) return 0;
        if (thisStable && !otherStable) return 1; // stable > prerelease
        if (!thisStable && otherStable) return -1;

        // Both prerelease: rank then numeric suffix then lexical
        int r1 = RankForLabel(normalizedLabelLower);
        int r2 = RankForLabel(other->normalizedLabelLower);
        if (r1 != r2) return r1 < r2 ? -1 : 1;
        if (prereleaseNumber != other->prereleaseNumber) return prereleaseNumber < other->prereleaseNumber ? -1 : 1;
        return String::Compare(normalizedLabelLower, other->normalizedLabelLower, StringComparison::OrdinalIgnoreCase);
    }

    UpdateVersion^ UpdateVersion::Parse(String^ tag)
    {
        UpdateVersion^ v;
        if (TryParse(tag, v)) return v;
        // Return invalid wrapper preserving raw
        return gcnew UpdateVersion(0, 0, 0, nullptr, 0, tag != nullptr ? tag : "", false);
    }

    bool UpdateVersion::TryParse(String^ tag, UpdateVersion^% outVersion)
    {
        outVersion = nullptr;
        if (String::IsNullOrWhiteSpace(tag)) return false;
        String^ t = tag->Trim();
        // Strip leading v/V
        if (t->Length > 0 && (t[0] == 'v' || t[0] == 'V'))
            t = t->Substring(1);
        t = t->Trim();
        if (t->Length == 0) return false;

        // Split prerelease
        String^ core = t;
        String^ pre = nullptr;
        int dash = t->IndexOf('-');
        if (dash >= 0) {
            core = t->Substring(0, dash);
            pre = t->Substring(dash + 1)->Trim();
            if (pre->Length == 0) pre = nullptr;
        }

        // core is MAJOR.MINOR.PATCH (allow 1-3 parts, missing = 0)
        array<String^>^ parts = core->Split('.');
        if (parts->Length == 0 || parts->Length > 3) return false;
        int maj = 0, min = 0, pat = 0;
        for (int i = 0; i < parts->Length; i++) {
            String^ p = parts[i]->Trim();
            if (p->Length == 0) return false;
            int val;
            if (!Int32::TryParse(p, val)) return false;
            if (val < 0) return false;
            if (i == 0) maj = val;
            else if (i == 1) min = val;
            else pat = val;
        }

        int preNum = 0;
        String^ preLabel = nullptr;
        if (!String::IsNullOrEmpty(pre)) {
            preLabel = pre;
            // Extract trailing numeric suffix: e.g. beta.1 -> 1, rc.1 ->1, beta1 ->1
            // Strategy: find last '.' or digit run
            int lastDot = pre->LastIndexOf('.');
            String^ suffix = (lastDot >= 0 && lastDot + 1 < pre->Length) ? pre->Substring(lastDot + 1) : nullptr;
            if (suffix != nullptr) {
                int n;
                if (Int32::TryParse(suffix, n))
                    preNum = n;
                else {
                    // Try trailing digits without dot: beta1
                    int idx = pre->Length - 1;
                    while (idx >= 0 && Char::IsDigit(pre[idx])) idx--;
                    if (idx + 1 < pre->Length) {
                        String^ tail = pre->Substring(idx + 1);
                        int nn;
                        if (Int32::TryParse(tail, nn)) preNum = nn;
                    }
                }
            } else {
                // Single token like "beta"
                int idx = pre->Length - 1;
                while (idx >= 0 && Char::IsDigit(pre[idx])) idx--;
                if (idx + 1 < pre->Length) {
                    String^ tail = pre->Substring(idx + 1);
                    int nn;
                    if (Int32::TryParse(tail, nn)) preNum = nn;
                }
            }
        }

        outVersion = gcnew UpdateVersion(maj, min, pat, preLabel, preNum, tag->Trim(), true);
        return true;
    }

    UpdateVersion^ UpdateVersion::Current::get()
    {
        // Avoid static managed locals - just return new instance each time (cheap, ~3 ints)
        return gcnew UpdateVersion(2, 1, 0, nullptr, 0, "v2.1.0", true);
    }

    UpdateVersion^ UpdateVersion::GetCurrentVersion()
    {
        try {
            // Try to read assembly version from manifest / entry assembly.
            // In C++/CLI, Application::ProductVersion may not be set; try Assembly.GetExecutingAssembly.
            auto asmObj = System::Reflection::Assembly::GetExecutingAssembly();
            if (asmObj != nullptr) {
                auto ver = asmObj->GetName()->Version;
                if (ver != nullptr) {
                    // ver.Major.Minor.Build (Build = patch) — manifest 2.1.0.0 → 2.1.0
                    int maj = ver->Major;
                    int min = ver->Minor;
                    int pat = ver->Build >= 0 ? ver->Build : 0;
                    // If manifest version is 0.0.* (metagen) fall back to Current
                    if (maj == 0 && min == 0) {
                        return Current;
                    }
                    String^ tag = String::Format("v{0}.{1}.{2}", maj, min, pat);
                    UpdateVersion^ v;
                    if (TryParse(tag, v)) return v;
                }
            }
        } catch (...) {}
        return Current;
    }

    String^ UpdateVersion::CurrentDisplayString()
    {
        return GetCurrentVersion()->ToDisplayString();
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
