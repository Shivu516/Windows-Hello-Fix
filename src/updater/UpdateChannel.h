#pragma once

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    public enum class UpdateChannel
    {
        Stable = 0,
        Beta = 1,
        PreRelease = 2,
        Unknown = 99
    };

    public ref class ChannelHelper sealed
    {
    public:
        static UpdateChannel FromRelease(bool isPrerelease, System::String^ tag, System::String^ name);
        static System::String^ ToDisplayString(UpdateChannel channel);
        static System::String^ ToPersistedString(UpdateChannel channel);
        static bool TryParsePersisted(System::String^ s, UpdateChannel% out);

        // Returns true if candidate channel is included when user selected 'selected'.
        // Stable includes only Stable; Beta includes Stable+Beta; PreRelease includes all.
        static bool IsIncludedBySelection(UpdateChannel selected, UpdateChannel candidate);

        static array<UpdateChannel>^ AllChannels();
    };

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0
