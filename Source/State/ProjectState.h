#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Model/NoteModel.h"

namespace moontune
{

// Serialises everything MoonTune stores in the DAW project:
//   - plugin parameters (AudioProcessorValueTreeState)
//   - the note list (NoteModel)
//   - editor view state (size, zoom, scroll)
//
// Loading is defensive: corrupt data is rejected without touching the current
// state; missing parameters fall back to defaults; out-of-range values are
// clamped; unknown parameters and unknown children are ignored; states saved by
// a newer MoonTune version are loaded as far as they are understood.
namespace ProjectState
{
    // Version 1 = MoonTune 0.1.x. Increment when the layout changes and add a migration in restore().
    constexpr int currentVersion = 1;

    namespace ids
    {
        inline const juce::Identifier root       { "MoonTuneState" };
        inline const juce::Identifier version    { "version" };
        inline const juce::Identifier appVersion { "appVersion" };
        inline const juce::Identifier params     { "Parameters" };
        inline const juce::Identifier param      { "PARAM" };
        inline const juce::Identifier paramId    { "id" };
        inline const juce::Identifier value      { "value" };
        inline const juce::Identifier ui         { "UI" };
        inline const juce::Identifier take       { "Take" };   // captured vocal reference (id, ranges)
    }

    struct LoadResult
    {
        bool loaded = false;           // false: data was unusable, current state untouched
        int  version = 0;              // version found in the data
        int  rejectedNotes = 0;        // invalid note entries that were skipped
        int  missingParameters = 0;    // parameters that fell back to defaults
        juce::String message;          // human-readable summary for logs / UI
    };

    // takeState: properties describing the captured vocal (may be invalid = none).
    juce::ValueTree createTree (juce::AudioProcessorValueTreeState& parameters,
                                const NoteModel& notes,
                                const juce::ValueTree& uiState,
                                const juce::ValueTree& takeState = {});

    void save (juce::AudioProcessorValueTreeState& parameters,
               const NoteModel& notes,
               const juce::ValueTree& uiState,
               juce::MemoryBlock& destination,
               const juce::ValueTree& takeState = {});

    // On success takeState receives the saved take properties (empty if none were saved).
    LoadResult restore (const void* data, int sizeInBytes,
                        juce::AudioProcessorValueTreeState& parameters,
                        NoteModel& notes,
                        juce::ValueTree& uiState,
                        juce::ValueTree* takeState = nullptr);

    LoadResult restoreFromTree (const juce::ValueTree& tree,
                                juce::AudioProcessorValueTreeState& parameters,
                                NoteModel& notes,
                                juce::ValueTree& uiState,
                                juce::ValueTree* takeState = nullptr);
}

} // namespace moontune
