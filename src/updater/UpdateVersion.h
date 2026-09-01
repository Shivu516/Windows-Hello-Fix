#pragma once

#include <windows.h>

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    /// <summary>
    /// Semantic version helper for HelloFix releases.
    /// Single authority for tag parsing, ordering, and updater capability.
    /// Tags expected: vMAJOR.MINOR.PATCH[-PRERELEASE] e.g. v2.1.0, v2.2.0-beta.1, v2.2.0-rc.1
    /// Malformed tags are considered Unknown and sorted last.
    /// </summary>
    public ref class UpdateVersion sealed : System::IComparable<UpdateVersion^>
    {
    private:
        int major;
        int minor;
        int patch;
        System::String^ prereleaseLabel; // null for stable, e.g. "beta.1", "rc.1"
        int prereleaseNumber; // numeric suffix, 0 if none
        System::String^ rawTag;
        System::String^ normalizedLabelLower; // lower-cased label for rank
        bool isValid;

        static int RankForLabel(System::String^ labelLower);

    public:
        UpdateVersion(int major, int minor, int patch, System::String^ prereleaseLabel, int prereleaseNumber, System::String^ rawTag, bool isValid);

        property int Major { int get() { return major; } }
        property int Minor { int get() { return minor; } }
        property int Patch { int get() { return patch; } }
        property System::String^ PrereleaseLabel { System::String^ get() { return prereleaseLabel; } }
        property System::String^ RawTag { System::String^ get() { return rawTag; } }
        property bool IsValid { bool get() { return isValid; } }
        property bool IsStable { bool get() { return isValid && System::String::IsNullOrEmpty(prereleaseLabel); } }
        property bool IsPrerelease { bool get() { return isValid && !System::String::IsNullOrEmpty(prereleaseLabel); } }

        /// <summary>True if this version includes the in-app updater (v2.1.0+) per capability rule.</summary>
        bool IsUpdaterSupported();

        /// <summary>Display form e.g. v2.1.0 or v2.2.0-beta.1</summary>
        System::String^ ToDisplayString();

        virtual System::String^ ToString() override { return ToDisplayString(); }

        virtual int CompareTo(UpdateVersion^ other);

        static UpdateVersion^ Parse(System::String^ tag);
        static bool TryParse(System::String^ tag, UpdateVersion^% outVersion);

        /// <summary>Current installed version. Reads app.manifest assemblyIdentity at runtime or falls back to compiled constant.</summary>
        static UpdateVersion^ GetCurrentVersion();

        /// <summary>Compiled fallback — bump when releasing.</summary>
        static property UpdateVersion^ Current { UpdateVersion^ get(); }

        static System::String^ CurrentDisplayString();
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
