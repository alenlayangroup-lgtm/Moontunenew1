#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>

#include "DSP/PassthroughEngine.h"
#include "Model/NoteModel.h"
#include "State/ProjectState.h"
#include "Music/KeyScale.h"
#include "Capture/AudioCapture.h"
#include "Analysis/AnalysisManager.h"
#include "DSP/RenderManager.h"
#include "Model/EditHistory.h"
#include "ARA/MoonTuneARA.h"

namespace moontune
{

namespace ParamIDs
{
    inline const juce::ParameterID bypass     { "bypass", 1 };
    inline const juce::ParameterID outputGain { "outputGain", 1 };

    // Key / scale (spec addendum). Highlighting and snapping only - they never change audio.
    inline const juce::ParameterID keyRoot        { "keyRoot", 1 };        // C..B
    inline const juce::ParameterID scaleType      { "scaleType", 1 };      // moontune::ScaleType
    inline const juce::ParameterID snapToScale    { "snapToScale", 1 };    // on/off, default off
    inline const juce::ParameterID snapStrength   { "snapStrength", 1 };   // 0..100 %
    inline const juce::ParameterID scaleHighlight { "scaleHighlight", 1 }; // 0..100 %, 0 = no highlight

    // Processing stages (each can be switched off to compare / bypass one stage).
    inline const juce::ParameterID procPitch      { "procPitch", 1 };
    inline const juce::ParameterID procTime       { "procTime", 1 };
    inline const juce::ParameterID procFormant    { "procFormant", 1 };     // formant preservation
    inline const juce::ParameterID procEq         { "procEq", 1 };
    inline const juce::ParameterID procProtect    { "procProtect", 1 };     // consonant / breath protection
    inline const juce::ParameterID procCorrection { "procCorrection", 1 };  // scale-correction layer

    inline const juce::ParameterID engine         { "engine", 1 };          // 0 Harmony Clean, 1 True Voice
    inline const juce::ParameterID scaleEdits     { "scaleEdits", 1 };      // custom scale: toggled pitch classes (bits)
    inline const juce::ParameterID gridDivision   { "gridDivision", 1 };    // Off, 1/4 .. 1/64
    inline const juce::ParameterID gridTriplet    { "gridTriplet", 1 };
    inline const juce::ParameterID followPlayhead { "followPlayhead", 1 };
    inline const juce::ParameterID audition       { "audition", 1 };        // play notes while editing
}

// Host tempo map as last seen by the audio thread (constant-tempo approximation).
struct HostTempo
{
    double bpm = 0.0;            // 0 = unknown
    double ppqAtZero = 0.0;      // quarter-note position at timeline second 0
    double barOriginPpq = 0.0;   // a bar line, in ppq
    int numerator = 4, denominator = 4;
    double secondsAt (double ppq) const noexcept { return (ppq - ppqAtZero) * 60.0 / bpm; }
    double ppqAt (double seconds) const noexcept { return ppqAtZero + seconds * bpm / 60.0; }
};

class MoonTuneAudioProcessor final : public juce::AudioProcessor,
                                    #if JucePlugin_Enable_ARA
                                     public juce::AudioProcessorARAExtension,
                                    #endif
                                     private juce::Timer
{
public:
    MoonTuneAudioProcessor();
    ~MoonTuneAudioProcessor() override;

    // --- AudioProcessor ---
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorParameter* getBypassParameter() const override;

    // --- MoonTune ---
    juce::AudioProcessorValueTreeState& getParameterState() noexcept { return parameters; }
    NoteModel& getNoteModel() noexcept { return noteModel; }

    // Current key/scale from the parameters (lock-free, callable from any thread).
    KeyScale getKeyScale() const noexcept;
    bool isSnapToScaleEnabled() const noexcept { return snapToScaleValue->load() >= 0.5f; }
    float getSnapStrength() const noexcept { return snapStrengthValue->load() / 100.0f; }   // 0..1

    // Editor view state (size, zoom, scroll). Plain property storage guarded by a lock,
    // because hosts may call get/setStateInformation off the message thread.
    juce::var getUiProperty (const juce::Identifier& name, const juce::var& defaultValue) const;
    void setUiProperty (const juce::Identifier& name, const juce::var& value);

    // --- Vocal capture & analysis ---
    void setCaptureArmed (bool shouldBeArmed) noexcept { capture.setArmed (shouldBeArmed); }
    bool isCaptureArmed() const noexcept { return capture.isArmed(); }
    void clearCapturedVocal();   // message thread
    AudioCapture& getCapture() noexcept { return capture; }
    AnalysisManager& getAnalysis() noexcept { return analysis; }
    RenderManager& getRenderer() noexcept { return renderer; }
    EditHistory& getHistory() noexcept { return history; }
    RenderOptions getRenderOptions() const noexcept;

    HostTempo getHostTempo() const noexcept;
    int getGridDivision() const noexcept;     // 0 = off, else 4, 8, 16, 32, 64
    bool isGridTriplet() const noexcept;
    bool isFollowPlayhead() const noexcept;
    bool isAuditionEnabled() const noexcept;

    // Note audition (message thread): plays [startSeconds, endSeconds) of the edited vocal while the
    // host is stopped, looping while `loop` is set, until stopAudition().
    void startAudition (double startSeconds, double endSeconds, bool loop);
    void stopAudition();
    void updateAuditionRange (double startSeconds, double endSeconds);   // keeps playing

    // Host transport through ARA (false if this instance has no ARA playback control).
    bool requestHostLocate (double seconds);   // move the playhead
    bool requestHostPlay();
    bool requestHostStop();
    bool canControlHost() const;

    // Preview without ARA: MoonTune plays the (edited) vocal itself from `seconds` to the end
    // while the host is stopped.
    void startPreview (double seconds);
    bool isPreviewing() const noexcept { return auditionActive.load() && previewMode.load(); }
    double getAuditionSeconds() const noexcept { return auditionSeconds.load (std::memory_order_relaxed); }
    bool hasSeenHostPosition() const noexcept { return seenHostPosition.load (std::memory_order_relaxed); }

    // ARA: true while the host drives this instance through ARA (audio comes from the host's
    // events, no REC needed). The editor shows that instead of the REC workflow.
    bool isAraMode() const noexcept { return araActive.load(); }
    juce::String getAraStatus() const;
    bool isAuditioning() const noexcept { return auditionActive.load(); }

    double getPlayheadSeconds() const noexcept { return playheadSeconds.load (std::memory_order_relaxed); }
    bool isHostPlaying() const noexcept { return hostPlaying.load (std::memory_order_relaxed); }

    // Peak of the output since the last call (lock-free, for the level meter).
    float popOutputPeak() noexcept { return outputPeak.exchange (0.0f, std::memory_order_relaxed); }

    // Result of the last setStateInformation() call (message thread only).
    juce::String getLastStateMessage() const;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

private:
    juce::AudioProcessorValueTreeState parameters;
    NoteModel noteModel;
    AnalysisManager analysis { noteModel };   // declared before capture: capture feeds it
    AudioCapture capture;
    RenderManager renderer { noteModel, analysis, capture };
    EditHistory history { noteModel };
    PassthroughEngine engine;
    juce::AudioBuffer<float> dryBuffer;   // original input while the edited vocal plays
    std::atomic<double> playheadSeconds { 0.0 };
    double currentSampleRate = 44100.0;   // from prepareToPlay (hosts differ in when getSampleRate() is valid)
    std::atomic<bool> hostPlaying { false };
    std::atomic<bool> seenHostPosition { false };

    // --- ARA (message thread) ---
    void timerCallback() override;
    void pollAra();
    std::atomic<bool> araActive { false };
    struct AraRegion
    {
        const void* key = nullptr;          // playback region
        const void* source = nullptr;       // audio source
        juce::int64 songStart = 0, songEnd = 0;   // host samples
        juce::int64 modStart = 0, modEnd = 0;     // source samples
        double modStartPos = 0.0;                 // exact source position of songStart
        double sourceRate = 0.0;
        bool accessible = false;
        bool operator== (const AraRegion& o) const noexcept
        {
            return key == o.key && source == o.source && songStart == o.songStart && songEnd == o.songEnd
                && modStart == o.modStart && modEnd == o.modEnd && juce::exactlyEqual (sourceRate, o.sourceRate)
                && accessible == o.accessible;
        }
    };
    std::vector<AraRegion> araSeen, araImported;
    double araChangedAt = 0.0;
    bool araImporting = false, araImportedOnce = false;
    juce::String araStatus;
    class AraImportJob;
    std::unique_ptr<juce::ThreadPool> araPool;
    void araImportFinished (std::shared_ptr<TakeAudio>, std::vector<AraRegion>, juce::String error);
    std::shared_ptr<MoonTuneAudioProcessor*> araAlive = std::make_shared<MoonTuneAudioProcessor*> (this);

    std::atomic<float>* bypassValue = nullptr;
    std::atomic<float>* outputGainValue = nullptr;
    std::atomic<float>* keyRootValue = nullptr;
    std::atomic<float>* scaleTypeValue = nullptr;
    std::atomic<float>* snapToScaleValue = nullptr;
    std::atomic<float>* snapStrengthValue = nullptr;
    std::array<std::atomic<float>*, 6> procValues {};
    std::atomic<float> outputPeak { 0.0f };
    std::atomic<float>* engineValue = nullptr;
    std::atomic<float>* scaleEditsValue = nullptr;

    // Host tempo (written by the audio thread, read by the UI)
    std::atomic<double> tempoBpm { 0.0 }, tempoPpqAtZero { 0.0 }, tempoBarOrigin { 0.0 };
    std::atomic<int> timeSigNum { 4 }, timeSigDen { 4 };

    // Audition
    std::atomic<bool> auditionActive { false }, auditionLoop { false }, previewMode { false };
    std::atomic<double> auditionSeconds { 0.0 };
    std::atomic<juce::int64> auditionStart { 0 }, auditionEnd { 0 };
    std::atomic<juce::uint32> auditionRequest { 0 };
    juce::uint32 auditionSeen = 0;        // audio thread
    juce::int64 auditionPos = 0;          // audio thread
    int auditionFade = 0;                 // audio thread: samples of fade-out left (stop)
    bool auditionStopping = false;        // audio thread

    mutable juce::CriticalSection uiLock;
    juce::ValueTree uiState { ProjectState::ids::ui };
    juce::String lastStateMessage;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MoonTuneAudioProcessor)
};

} // namespace moontune
