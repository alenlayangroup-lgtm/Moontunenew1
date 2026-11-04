#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#if JucePlugin_Enable_ARA

namespace moontune
{

// ARA 2 document controller. MoonTune reads the vocal straight from the host's audio events
// (no REC pass) and plays the edited result in their place; see MoonTuneAudioProcessor::pollAra().
// The notes and edits are stored with the plugin instance's state, so nothing extra is archived
// in the ARA document.
class MoonTuneDocumentController final : public juce::ARADocumentControllerSpecialisation
{
public:
    using ARADocumentControllerSpecialisation::ARADocumentControllerSpecialisation;

protected:
    bool doRestoreObjectsFromStream (juce::ARAInputStream& input, const juce::ARARestoreObjectsFilter*) noexcept override
    {
        return ! input.failed();
    }

    bool doStoreObjectsToStream (juce::ARAOutputStream&, const juce::ARAStoreObjectsFilter*) noexcept override
    {
        return true;
    }
};

} // namespace moontune

#endif
