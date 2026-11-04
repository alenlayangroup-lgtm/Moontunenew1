#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "UI/MoonLookAndFeel.h"
#include "UI/NoteGridView.h"
#include "UI/NoteInspector.h"
#include "UI/LevelMeter.h"
#include "UI/ScaleToolbar.h"
#include "UI/OverviewBar.h"
#include "Music/ScaleCorrector.h"

namespace moontune
{

class MoonTuneAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                           private juce::Timer
{
public:
    explicit MoonTuneAudioProcessorEditor (MoonTuneAudioProcessor&);
    ~MoonTuneAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int defaultWidth = 1280, defaultHeight = 760;
    static constexpr int minWidth = 1200, minHeight = 560, maxWidth = 3840, maxHeight = 2160;

    NoteGridView& getGridView() noexcept { return grid; }
    NoteInspector& getInspector() noexcept { return inspector; }
    juce::String getStatusText() const { return statusLabel.getText(); }

    // Exposed for tests: one UI refresh tick (normally run by the timer).
    void refreshFromProcessor() { timerCallback(); }
    void applyCorrectionForTesting (const ScaleCorrector::Settings&, bool selectedOnly);

    // Live Correct Pitch session (also used by tests): begin, preview any number of times, end.
    void beginCorrectionPreview();
    void previewCorrection (const ScaleCorrector::Settings&, bool selectedOnly);
    void endCorrectionPreview (bool keep);
    OverviewBar& getOverview() noexcept { return overview; }

private:
    void timerCallback() override;
    void restoreView();
    void storeView (const NoteGridView::View&);
    void analyseKey();
    void showCorrectPitch();
    void applyCorrection (const ScaleCorrector::Settings&, bool selectedOnly);
    void removeCorrection (bool selectedOnly);
    void updateUndoButtons();
    void updateFollow();
    void updateEngineButtons();

    MoonTuneAudioProcessor& processor;
    MoonLookAndFeel lookAndFeel;

    juce::TextButton recButton { "REC" }, clearButton { "CLEAR" }, undoButton { "UNDO" }, redoButton { "REDO" },
                     keyButton { "ANALYZE KEY" }, correctButton { "CORRECT PITCH" };
    juce::ToggleButton bypassButton { "A/B" };
    juce::Slider outputGainSlider;
    juce::Label outputGainLabel;
    LevelMeter meter;
    juce::Label statusLabel;
    ScaleToolbar scaleToolbar;
    NoteGridView grid;
    NoteInspector inspector;

    // Navigation / engine bar
    juce::Label engineLabel, gridLabel;
    juce::TextButton cleanButton { "HARMONY CLEAN" }, trueVoiceButton { "TRUE VOICE" }, hybridButton { "HYBRID" };
    std::unique_ptr<juce::ParameterAttachment> engineAttachment;
    juce::ComboBox gridBox;
    juce::ToggleButton tripletButton { "3" }, followButton { "FOLLOW" }, auditionButton { "AUDITION" };
    juce::TextButton showAllButton { "SHOW ALL" };
    juce::TextButton editToolButton { "EDIT" }, splitToolButton { "SPLIT" }, gainToolButton { "GAIN" };
    juce::ToggleButton waveButton { "WAVE" };
    juce::TextButton playButton { "PLAY" }, breathButton { "BREATH" };
    double locator = -1.0;          // play-from marker set by a ruler click (-1 = play from the DAW cursor)
    double locatorHostPos = -1.0;   // DAW cursor when the marker was set
    double lastPlayheadForLocator = -1.0;
    void playFrom (double seconds);
    void togglePlay();
    void locate (double seconds);
    void setTool (NoteGridView::Tool);
    void updateWaveform();
    const void* waveTake = nullptr;
    juce::int64 waveEnd = -1;
    double waveBuiltAt = 0.0;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> gridAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> tripletAttachment, followAttachment, auditionAttachment;
    OverviewBar overview;
    static constexpr int navHeight = 30;

    // Playhead following
    bool followSuspended = false, wasPlaying = false, initialJumpDone = false;
    double lastPlayhead = -1.0;

    // Live correction session
    bool correctionLive = false;
    std::vector<Note> correctionOriginal;

    // Processing stages (each can be switched off to compare).
    struct StageButton { std::unique_ptr<juce::ToggleButton> button;
                         std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment; };
    std::vector<StageButton> stages;
    juce::Label stagesLabel;

    juce::AudioProcessorValueTreeState::ButtonAttachment bypassAttachment;
    juce::AudioProcessorValueTreeState::SliderAttachment outputGainAttachment;

    juce::uint32 seenNoteVersion = 0, seenResultVersion = 0;
    bool hadNotes = false;

    static constexpr int headerHeight = 48;
    bool constructed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MoonTuneAudioProcessorEditor)
};

} // namespace moontune
