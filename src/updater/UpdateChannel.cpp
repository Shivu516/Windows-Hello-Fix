#include "UpdateChannel.h"

using namespace System;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    UpdateChannel ChannelHelper::FromRelease(bool isPrerelease, String^ tag, String^ name)
    {
        String^ t = tag != nullptr ? tag->ToLowerInvariant() : "";
        String^ n = name != nullptr ? name->ToLowerInvariant() : "";

        // Stable: prerelease == false and tag has no prerelease segment (no '-' containing label)
        // We treat any prerelease==true as not Stable.
        if (!isPrerelease) {
            // Check if tag itself contains prerelease identifiers despite flag false (mis-tagged) -> treat as Beta/Pre
            if (t->Contains("-beta") || t->Contains("-b") || t->Contains(".beta")) {
                return UpdateChannel::Beta;
            }
            if (t->Contains("-rc") || t->Contains("-pre") || t->Contains("preview") || t->Contains("-alpha") || t->Contains(".rc")) {
                return UpdateChannel::PreRelease;
            }
            return UpdateChannel::Stable;
        }

        // prerelease == true
        // Heuristics on tag/name
        if (t->Contains("beta") || n->Contains("beta") || t->Contains("-b.") || t->Contains("-b1") || t->Contains("-b2")) {
            // Distinguish pre/rc that also might contain beta? beta wins
            return UpdateChannel::Beta;
        }
        if (t->Contains("-rc") || t->Contains(".rc") || t->Contains("-pre") || t->Contains("preview") || t->Contains("alpha") || n->Contains("rc") || n->Contains("preview") || n->Contains("pre-release") || n->Contains("pre_release")) {
            return UpdateChannel::PreRelease;
        }
        // Unknown prerelease -> conservative PreRelease
        return UpdateChannel::PreRelease;
    }

    String^ ChannelHelper::ToDisplayString(UpdateChannel channel)
    {
        switch (channel) {
        case UpdateChannel::Stable: return "Stable";
        case UpdateChannel::Beta: return "Beta";
        case UpdateChannel::PreRelease: return "Pre-Release";
        default: return "Unknown";
        }
    }

    String^ ChannelHelper::ToPersistedString(UpdateChannel channel)
    {
        switch (channel) {
        case UpdateChannel::Stable: return "Stable";
        case UpdateChannel::Beta: return "Beta";
        case UpdateChannel::PreRelease: return "PreRelease";
        default: return "Stable";
        }
    }

    bool ChannelHelper::TryParsePersisted(String^ s, UpdateChannel% out)
    {
        if (String::IsNullOrWhiteSpace(s)) return false;
        String^ t = s->Trim()->ToLowerInvariant();
        if (t == "stable") { out = UpdateChannel::Stable; return true; }
        if (t == "beta") { out = UpdateChannel::Beta; return true; }
        if (t == "prerelease" || t == "pre-release" || t == "pre_release" || t == "prerelease") { out = UpdateChannel::PreRelease; return true; }
        return false;
    }

    bool ChannelHelper::IsIncludedBySelection(UpdateChannel selected, UpdateChannel candidate)
    {
        if (candidate == UpdateChannel::Unknown) return false;
        switch (selected) {
        case UpdateChannel::Stable:
            return candidate == UpdateChannel::Stable;
        case UpdateChannel::Beta:
            return candidate == UpdateChannel::Stable || candidate == UpdateChannel::Beta;
        case UpdateChannel::PreRelease:
            return candidate == UpdateChannel::Stable || candidate == UpdateChannel::Beta || candidate == UpdateChannel::PreRelease;
        default:
            return candidate == UpdateChannel::Stable;
        }
    }

    array<UpdateChannel>^ ChannelHelper::AllChannels()
    {
        return gcnew array<UpdateChannel>{ UpdateChannel::Stable, UpdateChannel::Beta, UpdateChannel::PreRelease };
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
