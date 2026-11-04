#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "DSP/Resampler.h"

namespace moontune
{

MoonTuneAudioProcessor::MoonTuneAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::mono(), true)
                          .withOutput ("Output", juce::AudioChannelSet::mono(), true)),
      parameters (*this, nullptr, "MoonTuneParameters", createParameterLayout())
{
    bypassValue     = parameters.getRawParameterValue (ParamIDs::bypass.getParamID());
    outputGainValue = parameters.getRawParameterValue (ParamIDs::outputGain.getParamID());
    keyRootValue      = parameters.getRawParameterValue (ParamIDs::keyRoot.getParamID());
    scaleTypeValue    = parameters.getRawParameterValue (ParamIDs::scaleType.getParamID());
    snapToScaleValue  = parameters.getRawParameterValue (ParamIDs::snapToScale.getParamID());
    snapStrengthValue = parameters.getRawParameterValue (ParamIDs::snapStrength.getParamID());
    jassert (bypassValue != nullptr && outputGainValue != nullptr && keyRootValue != nullptr
             && scaleTypeValue != nullptr && snapToScaleValue != nullptr && snapStrengthValue != nullptr);

    const juce::ParameterID* procIds[] = { &ParamIDs::procPitch, &ParamIDs::procTime, &ParamIDs::procFormant,
                                           &ParamIDs::procEq, &ParamIDs::procProtect, &ParamIDs::procCorrection };
    for (size_t i = 0; i < procValues.size(); ++i)
    {
        procValues[i] = parameters.getRawParameterValue (procIds[i]->getParamID());
        jassert (procValues[i] != nullptr);
    }
    engineValue = parameters.getRawParameterValue (ParamIDs::engine.getParamID());
    scaleEditsValue = parameters.getRawParameterValue (ParamIDs::scaleEdits.getParamID());
    jassert (engineValue != nullptr && scaleEditsValue != nullptr);
    renderer.optionsProvider = [this] { return getRenderOptions(); };

    // Capture thread -> analysis thread. Neither runs on the audio thread.
    capture.onSessionFinished = [this] (TakeAudio::Range range)
    {
        analysis.analyseCapture (capture.getTake(), range);
    };
    analysis.onTakeLoaded = [this] (std::shared_ptr<TakeAudio> take) { capture.setTake (std::move (take)); };

   #if JucePlugin_Enable_ARA
    startTimerHz (5);   // watches the host's ARA playback regions
   #endif
}

MoonTuneAudioProcessor::~MoonTuneAudioProcessor()
{
    stopTimer();
    araPool.reset();   // waits for a running import
    // Stop worker threads first: their callbacks reference each other.
    renderer.stop();
    capture.stop();
    analysis.stop();
}

void MoonTuneAudioProcessor::clearCapturedVocal()
{
    capture.setArmed (false);
    if (auto take = capture.getTake())
        analysis.getTakeFile (take->getId()).deleteFile();
    capture.clearTake();
    analysis.clear();
    noteModel.clear();
    history.clear();
}

juce::AudioProcessorValueTreeState::ParameterLayout MoonTuneAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterBool> (
        ParamIDs::bypass, "Bypass", false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::outputGain, "Output Gain",
        juce::NormalisableRange<float> (-24.0f, 12.0f, 0.01f),
        0.0f,
        juce::AudioParameterFloatAttributes().withLabel ("dB")));

    // Key and scale are project settings, not performance controls: not automatable.
    const auto notAutomatable = juce::AudioParameterChoiceAttributes().withAutomatable (false);

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        ParamIDs::keyRoot, "Key", KeyScale::allPitchClassNames(), 0, notAutomatable));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        ParamIDs::scaleType, "Scale", KeyScale::allScaleTypeNames(),
        (int) ScaleType::Chromatic, notAutomatable));   // default: no restriction

    layout.add (std::make_unique<juce::AudioParameterBool> (
        ParamIDs::snapToScale, "Snap to Scale", false,
        juce::AudioParameterBoolAttributes().withAutomatable (false)));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::snapStrength, "Snap Strength",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 100.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%").withAutomatable (false)));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        ParamIDs::scaleHighlight, "Scale Highlight",
        juce::NormalisableRange<float> (0.0f, 100.0f, 1.0f), 50.0f,
        juce::AudioParameterFloatAttributes().withLabel ("%").withAutomatable (false)));

    auto stage = [&layout] (const juce::ParameterID& id, const juce::String& name)
    {
        layout.add (std::make_unique<juce::AudioParameterBool> (
            id, name, true, juce::AudioParameterBoolAttributes().withAutomatable (false)));
    };
    stage (ParamIDs::procPitch, "Pitch Processing");
    stage (ParamIDs::procTime, "Time Processing");
    stage (ParamIDs::procFormant, "Formant Preservation");
    stage (ParamIDs::procEq, "Note EQ");
    stage (ParamIDs::procProtect, "Consonant Protection");
    stage (ParamIDs::procCorrection, "Scale Correction");

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        ParamIDs::engine, "Engine", juce::StringArray { "Harmony Clean", "True Voice", "Hybrid" }, 0, notAutomatable));
    layout.add (std::make_unique<juce::AudioParameterInt> (
        ParamIDs::scaleEdits, "Custom Scale", 0, 4095, 0, juce::AudioParameterIntAttributes().withAutomatable (false)));
    layout.add (std::make_unique<juce::AudioParameterChoice> (
        ParamIDs::gridDivision, "Grid", juce::StringArray { "Off", "1/4", "1/8", "1/16", "1/32", "1/64" }, 0, notAutomatable));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        ParamIDs::gridTriplet, "Grid Triplets", false, juce::AudioParameterBoolAttributes().withAutomatable (false)));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        ParamIDs::followPlayhead, "Follow Playhead", true, juce::AudioParameterBoolAttributes().withAutomatable (false)));
    layout.add (std::make_unique<juce::AudioParameterBool> (
        ParamIDs::audition, "Audition Notes", true, juce::AudioParameterBoolAttributes().withAutomatable (false)));

    return layout;
}

RenderOptions MoonTuneAudioProcessor::getRenderOptions() const noexcept
{
    auto on = [this] (size_t i) { return procValues[i]->load() >= 0.5f; };
    RenderOptions o;
    o.pitch = on (0);
    o.time = on (1);
    o.preserveFormants = on (2);
    o.eq = on (3);
    o.protectConsonants = on (4);
    o.scaleCorrection = on (5);
    o.engine = juce::jlimit (0, 2, juce::roundToInt (engineValue->load()));
    return o;
}

KeyScale MoonTuneAudioProcessor::getKeyScale() const noexcept
{
    return KeyScale (juce::roundToInt (keyRootValue->load()), (ScaleType) juce::roundToInt (scaleTypeValue->load()))
               .withPitchClassesToggled ((juce::uint16) juce::roundToInt (scaleEditsValue->load()));
}

namespace
{
    float paramValue (const juce::AudioProcessorValueTreeState& s, const juce::ParameterID& id)
    {
        return s.getRawParameterValue (id.getParamID())->load();
    }
}

HostTempo MoonTuneAudioProcessor::getHostTempo() const noexcept
{
    HostTempo t;
    t.bpm = tempoBpm.load();
    t.ppqAtZero = tempoPpqAtZero.load();
    t.barOriginPpq = tempoBarOrigin.load();
    t.numerator = timeSigNum.load();
    t.denominator = timeSigDen.load();
    return t;
}

int MoonTuneAudioProcessor::getGridDivision() const noexcept
{
    static constexpr int divs[] = { 0, 4, 8, 16, 32, 64 };
    return divs[juce::jlimit (0, 5, juce::roundToInt (paramValue (parameters, ParamIDs::gridDivision)))];
}

bool MoonTuneAudioProcessor::isGridTriplet() const noexcept { return paramValue (parameters, ParamIDs::gridTriplet) >= 0.5f; }
bool MoonTuneAudioProcessor::isFollowPlayhead() const noexcept { return paramValue (parameters, ParamIDs::followPlayhead) >= 0.5f; }
bool MoonTuneAudioProcessor::isAuditionEnabled() const noexcept { return paramValue (parameters, ParamIDs::audition) >= 0.5f; }

void MoonTuneAudioProcessor::startAudition (double startSeconds, double endSeconds, bool loop)
{
    if (endSeconds <= startSeconds)
        return;
    previewMode.store (false);
    auditionStart.store ((juce::int64) (startSeconds * currentSampleRate));
    auditionEnd.store ((juce::int64) (endSeconds * currentSampleRate));
    auditionLoop.store (loop);
    auditionActive.store (true);
    auditionRequest.fetch_add (1);
}

void MoonTuneAudioProcessor::updateAuditionRange (double startSeconds, double endSeconds)
{
    if (endSeconds <= startSeconds)
        return;
    auditionStart.store ((juce::int64) (startSeconds * currentSampleRate));
    auditionEnd.store ((juce::int64) (endSeconds * currentSampleRate));
}

#if JucePlugin_Enable_ARA
namespace
{
    ARA::PlugIn::HostPlaybackController* hostController (juce::AudioProcessorARAExtension& ext)
    {
        if (auto* r = ext.getPlaybackRenderer())
            if (auto* dc = r->getDocumentController())
                return dc->getHostPlaybackController();
        return nullptr;
    }
}
#endif

bool MoonTuneAudioProcessor::canControlHost() const
{
   #if JucePlugin_Enable_ARA
    return hostController (const_cast<MoonTuneAudioProcessor&> (*this)) != nullptr;
   #else
    return false;
   #endif
}

bool MoonTuneAudioProcessor::requestHostLocate (double seconds)
{
   #if JucePlugin_Enable_ARA
    if (auto* pc = hostController (*this)) { pc->requestSetPlaybackPosition (juce::jmax (0.0, seconds)); return true; }
   #else
    juce::ignoreUnused (seconds);
   #endif
    return false;
}

bool MoonTuneAudioProcessor::requestHostPlay()
{
   #if JucePlugin_Enable_ARA
    if (auto* pc = hostController (*this)) { pc->requestStartPlayback(); return true; }
   #endif
    return false;
}

bool MoonTuneAudioProcessor::requestHostStop()
{
   #if JucePlugin_Enable_ARA
    if (auto* pc = hostController (*this)) { pc->requestStopPlayback(); return true; }
   #endif
    return false;
}

void MoonTuneAudioProcessor::startPreview (double seconds)
{
    auto take = capture.getTake();
    if (take == nullptr || take->isEmpty())
        return;
    const double end = (double) take->getEndSample() / take->getSampleRate();
    if (seconds >= end - 0.01)
        return;
    startAudition (std::max (0.0, seconds), end, false);
    previewMode.store (true);
}

juce::String MoonTuneAudioProcessor::getAraStatus() const { return araStatus; }

// --- ARA import --------------------------------------------------------------------------
// The vocal of every playback region given to this instance is read from the host and laid out
// on the song timeline (a "take" like a REC pass). When the host moves or trims an event, the
// take is rebuilt and the notes inside it move with it, so edits are kept.

#if JucePlugin_Enable_ARA
class MoonTuneAudioProcessor::AraImportJob final : public juce::ThreadPoolJob
{
public:
    struct Item { std::unique_ptr<juce::ARAAudioSourceReader> reader; AraRegion region; int channels = 1; };

    AraImportJob (MoonTuneAudioProcessor& p, std::vector<Item> it, std::vector<AraRegion> layout, double rate)
        : juce::ThreadPoolJob ("MoonTune ARA import"), safe (&p), items (std::move (it)), regions (std::move (layout)), hostRate (rate) {}

    JobStatus runJob() override
    {
        auto take = std::make_shared<TakeAudio> (hostRate, "ara-" + juce::Uuid().toString());
        juce::String error;
        for (auto& item : items)
        {
            if (shouldExit()) return jobHasFinished;
            const auto& r = item.region;
            const double ratio = r.sourceRate / hostRate;                 // source samples per host sample
            const bool sameRate = std::abs (ratio - 1.0) < 1.0e-9;
            const auto outLen = (int) std::min<juce::int64> (r.songEnd - r.songStart, (juce::int64) (TakeAudio::maxSeconds * hostRate));
            if (outLen <= 0) continue;
            // Source span to read (with the resampler's kernel around it when the rates differ).
            const int pad = sameRate ? 0 : SincResampler::zeroCrossings * 2 + 4;
            const juce::int64 readStart = sameRate ? (juce::int64) std::llround (r.modStartPos) : (juce::int64) std::floor (r.modStartPos) - pad;
            const auto srcLen = (int) std::ceil (outLen * ratio) + 2 * pad + 4;
            const int nCh = juce::jmax (1, item.channels);
            juce::AudioBuffer<float> src (nCh, srcLen);
            src.clear();
            const juce::int64 firstValid = std::max<juce::int64> (0, readStart);
            if (! item.reader->read (&src, (int) (firstValid - readStart), srcLen - (int) (firstValid - readStart), firstValid, true, true))
            {
                error = "Could not read an audio event from the host.";
                continue;
            }
            // Every channel at the project rate (exact copy when the rates match), then the mono
            // mix that is analysed and edited.
            std::vector<std::vector<float>> chOut ((size_t) nCh, std::vector<float> ((size_t) outLen));
            for (int ch = 0; ch < nCh; ++ch)
            {
                if (sameRate)
                    std::copy (src.getReadPointer (ch), src.getReadPointer (ch) + outLen, chOut[(size_t) ch].begin());
                else
                    resampler.process (src.getReadPointer (ch), srcLen, r.modStartPos - (double) readStart, ratio,
                                       chOut[(size_t) ch].data(), outLen);
            }
            std::vector<float> mono ((size_t) outLen, 0.0f);
            for (int ch = 0; ch < nCh; ++ch)
                juce::FloatVectorOperations::addWithMultiply (mono.data(), chOut[(size_t) ch].data(), 1.0f / (float) nCh, outLen);
            take->write (r.songStart, mono.data(), outLen);

            // Keep the original channels when they differ (stereo vocal): untouched audio then
            // plays in its own channels, bit for bit.
            bool differs = false;
            for (int ch = 1; ch < nCh && ! differs; ++ch)
                for (int i = 0; i < outLen && ! differs; ++i)
                    differs = chOut[(size_t) ch][(size_t) i] != chOut[0][(size_t) i];
            if (differs)
            {
                if (channels.size() < (size_t) nCh) channels.resize ((size_t) nCh);
                const auto end = (size_t) (r.songStart + outLen);
                for (int ch = 0; ch < nCh; ++ch)
                {
                    auto& c = channels[(size_t) ch];
                    if (c.size() < end) c.resize (end, 0.0f);
                    std::copy (chOut[(size_t) ch].begin(), chOut[(size_t) ch].end(), c.begin() + (long) r.songStart);
                }
                stereoRanges.push_back ({ r.songStart, r.songStart + outLen });
            }
        }
        if (! channels.empty())
        {
            // Mono events next to stereo ones: their mono signal in every channel.
            const auto end = (size_t) take->getEndSample();
            for (auto& c : channels) if (c.size() < end) c.resize (end, 0.0f);
            for (const auto& rg : take->getRanges())
                for (auto pos = rg.start; pos < rg.end; ++pos)
                {
                    bool st = false;
                    for (const auto& s : stereoRanges) st = st || (pos >= s.start && pos < s.end);
                    if (st) continue;
                    float m = 0.0f;
                    take->read (pos, &m, 1);
                    for (auto& c : channels) c[(size_t) pos] = m;
                }
            take->setOriginalChannels (std::make_shared<std::vector<std::vector<float>>> (std::move (channels)));
        }
        if (! shouldExit())
        {
            auto layout = std::move (regions);
            juce::MessageManager::callAsync ([s = safe, take, layout, error]() mutable
            {
                if (auto* p = s.get())
                    p->araImportFinished (take, std::move (layout), error);
            });
        }
        return jobHasFinished;
    }

private:
    struct Safe
    {
        explicit Safe (MoonTuneAudioProcessor* p) : ref (p->araAlive) {}
        MoonTuneAudioProcessor* get() const { auto a = ref.lock(); return a != nullptr ? *a : nullptr; }
        std::weak_ptr<MoonTuneAudioProcessor*> ref;
    } safe;
    std::vector<Item> items;
    std::vector<AraRegion> regions;
    double hostRate;
    SincResampler resampler;
    std::vector<std::vector<float>> channels;
    std::vector<TakeAudio::Range> stereoRanges;
};
#endif

void MoonTuneAudioProcessor::timerCallback()
{
    pollAra();
}

void MoonTuneAudioProcessor::pollAra()
{
   #if JucePlugin_Enable_ARA
    auto* pr = getPlaybackRenderer();
    araActive.store (pr != nullptr);
    if (pr == nullptr)
        return;

    std::vector<AraRegion> now;
    for (auto* region : pr->getPlaybackRegions())
    {
        auto* mod = region->getAudioModification();
        auto* source = mod != nullptr ? mod->getAudioSource() : nullptr;
        if (source == nullptr) continue;
        AraRegion r;
        r.key = region;
        r.source = source;
        const auto song = region->getSampleRange (currentSampleRate, juce::ARAPlaybackRegion::IncludeHeadAndTail::no);
        r.songStart = std::max<juce::int64> (0, song.getStart());
        r.songEnd = song.getEnd();
        r.sourceRate = source->getSampleRate();
        r.modStartPos = (double) region->getStartInAudioModificationSamples()
                      + (double) (r.songStart - song.getStart()) * r.sourceRate / currentSampleRate;
        r.modStart = (juce::int64) std::llround (r.modStartPos);
        r.modEnd = region->getEndInAudioModificationSamples();
        r.sourceRate = source->getSampleRate();
        r.accessible = source->isSampleAccessEnabled();
        now.push_back (r);
    }
    std::sort (now.begin(), now.end(), [] (const AraRegion& a, const AraRegion& b) { return a.songStart < b.songStart; });

    const double t = juce::Time::getMillisecondCounterHiRes();
    if (now != araSeen) { araSeen = now; araChangedAt = t; }
    if (araImporting || (araImportedOnce && araSeen == araImported) || t - araChangedAt < 300.0)
        return;   // wait until the host has stopped changing things for a moment

    std::vector<AraImportJob::Item> items;
    for (auto* region : pr->getPlaybackRegions())
    {
        for (const auto& r : araSeen)
        {
            if (r.key != region || ! r.accessible) continue;
            auto* source = region->getAudioModification()->getAudioSource();
            AraImportJob::Item item;
            item.reader = std::make_unique<juce::ARAAudioSourceReader> (source);
            item.region = r;
            item.channels = (int) source->getChannelCount();
            items.push_back (std::move (item));
        }
    }
    if (items.empty())
    {
        araStatus = araSeen.empty() ? "ARA: no audio event assigned to MoonTune yet."
                                    : "ARA: waiting for the host to give access to the audio...";
        if (araSeen.empty()) { araImported.clear(); araImportedOnce = true; }
        return;
    }
    if (araPool == nullptr)
        araPool = std::make_unique<juce::ThreadPool> (1);
    araImporting = true;
    araStatus = "ARA: reading the vocal from the host...";
    araPool->addJob (new AraImportJob (*this, std::move (items), araSeen, currentSampleRate), true);
   #endif
}

void MoonTuneAudioProcessor::araImportFinished (std::shared_ptr<TakeAudio> take, std::vector<AraRegion> layout, juce::String error)
{
    araImporting = false;
    const double sr = currentSampleRate;

    // Move the existing notes with their events; collect what has to be analysed afresh.
    std::vector<std::pair<double, double>> replace;
    if (araImportedOnce)
    {
        auto notes = noteModel.getSnapshot();
        std::vector<Note> kept;
        for (const auto& n : notes)
        {
            const double mid = 0.5 * (n.startSeconds + n.endSeconds) * sr;
            const AraRegion* oldR = nullptr;
            for (const auto& r : araImported) if (mid >= (double) r.songStart && mid < (double) r.songEnd) oldR = &r;
            if (oldR == nullptr) continue;   // was not in any event: drop
            const AraRegion* newR = nullptr;
            for (const auto& r : layout) if (r.key == oldR->key) newR = &r;
            if (newR == nullptr) continue;   // its event is gone
            // Same source position before and after: shift = (song shift) - (source shift).
            const double srcPos = mid - (double) oldR->songStart + (double) oldR->modStart * sr / oldR->sourceRate;
            const double newMid = srcPos - (double) newR->modStart * sr / newR->sourceRate + (double) newR->songStart;
            if (newMid < (double) newR->songStart || newMid >= (double) newR->songEnd) continue;   // trimmed away
            const double shift = (newMid - mid) / sr;
            Note m = n;
            m.startSeconds += shift;
            m.endSeconds += shift;
            kept.push_back (m);
        }
        for (const auto& r : layout)
        {
            bool known = false;
            for (const auto& o : araImported) known = known || (o.key == r.key && o.source == r.source);
            if (! known) replace.emplace_back ((double) r.songStart / sr, (double) r.songEnd / sr);
        }
        if (kept != notes)
        {
            noteModel.replaceAll (std::move (kept));
            history.clear();
        }
    }

    araImported = layout;
    const bool first = ! araImportedOnce;
    araImportedOnce = true;
    capture.setTake (take);
    analysis.analyseImported (take, std::move (replace), first);
    araStatus = error.isNotEmpty() ? "ARA: " + error : juce::String();
}

void MoonTuneAudioProcessor::stopAudition()
{
    auditionLoop.store (false);
    auditionRequest.fetch_add (1);
    auditionActive.store (false);
}

void MoonTuneAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    engine.prepare (sampleRate, samplesPerBlock, juce::jmax (getTotalNumInputChannels(),
                                                             getTotalNumOutputChannels()));
    engine.setBypassed (bypassValue->load() >= 0.5f);
    engine.setOutputGainDecibels (outputGainValue->load());
    engine.reset();   // start at the target values, no ramp from silence
    capture.prepare (sampleRate, samplesPerBlock);
    dryBuffer.setSize (juce::jmax (2, getTotalNumInputChannels(), getTotalNumOutputChannels()),
                       juce::jmax (1, samplesPerBlock), false, true, false);
    renderer.resetPlayback();
    setLatencySamples (0);
   #if JucePlugin_Enable_ARA
    prepareToPlayForARA (sampleRate, samplesPerBlock, getMainBusNumOutputChannels(), getProcessingPrecision());
   #endif
}

void MoonTuneAudioProcessor::releaseResources()
{
   #if JucePlugin_Enable_ARA
    releaseResourcesForARA();
   #endif
}

bool MoonTuneAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // Mono vocal is the primary use; stereo is accepted so the plugin can sit on stereo tracks.
    const auto in  = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (in != out)
        return false;

    return out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo();
}

void MoonTuneAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    midi.clear();

    const int numIn  = getTotalNumInputChannels();
    const int numOut = getTotalNumOutputChannels();
    for (int ch = numIn; ch < numOut; ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    // Host transport: timeline position for capture and the editor's playhead.
    std::optional<juce::int64> timeline;
    bool playing = false;
    if (auto* hostPlayHead = getPlayHead())
        if (auto position = hostPlayHead->getPosition())
        {
            playing = position->getIsPlaying();
            if (auto samples = position->getTimeInSamples())
                timeline = *samples;
            const auto bpm = position->getBpm();
            const auto ppq = position->getPpqPosition();
            if (bpm.hasValue() && *bpm > 1.0 && ppq.hasValue())
            {
                const double secs = position->getTimeInSeconds().hasValue() ? *position->getTimeInSeconds()
                                  : (timeline.has_value() ? (double) *timeline / currentSampleRate : 0.0);
                tempoBpm.store (*bpm, std::memory_order_relaxed);
                tempoPpqAtZero.store (*ppq - secs * *bpm / 60.0, std::memory_order_relaxed);
                if (auto bar = position->getPpqPositionOfLastBarStart())
                    tempoBarOrigin.store (*bar, std::memory_order_relaxed);
                if (auto sig = position->getTimeSignature())
                {
                    timeSigNum.store (juce::jmax (1, sig->numerator), std::memory_order_relaxed);
                    timeSigDen.store (juce::jmax (1, sig->denominator), std::memory_order_relaxed);
                }
            }
        }
    hostPlaying.store (playing, std::memory_order_relaxed);
    if (timeline.has_value())
    {
        playheadSeconds.store ((double) *timeline / currentSampleRate, std::memory_order_relaxed);
        seenHostPosition.store (true, std::memory_order_relaxed);
    }

   #if JucePlugin_Enable_ARA
    const bool ara = isBoundToARA();
   #else
    const bool ara = false;
   #endif
    if (! ara)
        capture.pushBlock (buffer, numIn, timeline, playing);   // dry input, before any processing

    engine.setBypassed (bypassValue->load (std::memory_order_relaxed) >= 0.5f);
    engine.setOutputGainDecibels (outputGainValue->load (std::memory_order_relaxed));

    // ARA: the vocal comes from the host's events (imported take); MoonTune plays it, edited.
    bool edited = false;
    if (ara)
    {
        // Where the imported take covers the timeline MoonTune plays it (edited); elsewhere (or
        // before the import has finished) whatever the host sends passes through unchanged.
        const int n = buffer.getNumSamples();
        if (playing && timeline.has_value() && n <= dryBuffer.getNumSamples() && numOut <= dryBuffer.getNumChannels())
        {
            juce::AudioBuffer<float> dryView (dryBuffer.getArrayOfWritePointers(), numOut, n);
            for (int ch = 0; ch < numOut; ++ch)
                dryView.copyFrom (ch, 0, buffer, ch, 0, n);
            edited = renderer.processAudition (buffer, numOut, *timeline, currentSampleRate);
            renderer.processOriginal (dryView, numOut, *timeline, currentSampleRate);
        }
        else if (! playing)
            renderer.resetPlayback();
    }
    else if (playing && timeline.has_value() && ! capture.isArmed()
        && buffer.getNumSamples() <= dryBuffer.getNumSamples() && numOut <= dryBuffer.getNumChannels())
    {
        for (int ch = 0; ch < numOut; ++ch)
            dryBuffer.copyFrom (ch, 0, buffer, ch, 0, buffer.getNumSamples());
        edited = renderer.process (buffer, numOut, *timeline, currentSampleRate);
    }
    else if (! playing)
    {
        renderer.resetPlayback();
    }

    // Note audition while the host is stopped: the edited vocal of one note, looped while held.
    if (! playing)
    {
        const auto request = auditionRequest.load();
        if (request != auditionSeen)
        {
            auditionSeen = request;
            if (auditionActive.load()) { auditionPos = auditionStart.load(); auditionStopping = false; auditionFade = 0; }
            else if (auditionPos > 0) { auditionStopping = true; auditionFade = (int) (0.015 * currentSampleRate); }
        }
        if (auditionActive.load() || auditionStopping)
        {
            const int n = buffer.getNumSamples();
            const auto end = auditionEnd.load(), start = auditionStart.load();
            for (int ch = 0; ch < numOut; ++ch)
                buffer.clear (ch, 0, n);
            if (renderer.processAudition (buffer, numOut, auditionPos, currentSampleRate))
            {
                const int fadeLen = juce::jmax (1, (int) (0.015 * currentSampleRate));
                for (int i = 0; i < n; ++i)
                {
                    const auto pos = auditionPos + i;
                    float g = 1.0f;
                    if (pos - start < fadeLen) g = (float) (pos - start) / (float) fadeLen;           // fade in
                    if (end - pos < fadeLen) g = juce::jmin (g, (float) std::max<juce::int64> (0, end - pos) / (float) fadeLen);
                    if (auditionStopping) g *= juce::jmax (0.0f, (float) (auditionFade - i) / (float) fadeLen);
                    for (int ch = 0; ch < numOut; ++ch)
                        buffer.setSample (ch, i, buffer.getSample (ch, i) * g);
                }
            }
            auditionPos += n;
            auditionSeconds.store ((double) auditionPos / currentSampleRate, std::memory_order_relaxed);
            if (auditionStopping)
            {
                auditionFade -= n;
                if (auditionFade <= 0) { auditionStopping = false; auditionPos = 0; }
            }
            else if (auditionPos >= end)
            {
                if (auditionLoop.load()) auditionPos = start;
                else { auditionActive.store (false); auditionPos = 0; }
            }
        }
    }
    else if (auditionActive.load())
    {
        auditionActive.store (false);   // the host took over
        auditionPos = 0;
    }

    if (edited)
    {
        juce::AudioBuffer<float> dryView (dryBuffer.getArrayOfWritePointers(), numOut, buffer.getNumSamples());
        engine.process (buffer, &dryView);
    }
    else
    {
        engine.process (buffer);
    }

    float peak = 0.0f;
    for (int ch = 0; ch < numOut; ++ch)
        peak = juce::jmax (peak, buffer.getMagnitude (ch, 0, buffer.getNumSamples()));

    // Keep the highest peak until the UI collects it.
    float previous = outputPeak.load (std::memory_order_relaxed);
    while (peak > previous && ! outputPeak.compare_exchange_weak (previous, peak, std::memory_order_relaxed)) {}
}

juce::AudioProcessorParameter* MoonTuneAudioProcessor::getBypassParameter() const
{
    return parameters.getParameter (ParamIDs::bypass.getParamID());
}

juce::AudioProcessorEditor* MoonTuneAudioProcessor::createEditor()
{
    return new MoonTuneAudioProcessorEditor (*this);
}

// --- State -----------------------------------------------------------------

void MoonTuneAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree uiCopy;
    {
        const juce::ScopedLock sl (uiLock);
        uiCopy = uiState.createCopy();
    }
    juce::ValueTree takeState (ProjectState::ids::take);
    if (auto take = capture.getTake(); take != nullptr && ! take->isEmpty() && ! take->getId().startsWith ("ara-"))
    {
        juce::StringArray ranges;
        for (const auto& r : take->getRanges())
            ranges.add (juce::String (r.start) + ":" + juce::String (r.end));
        takeState.setProperty ("id", take->getId(), nullptr);
        takeState.setProperty ("sampleRate", take->getSampleRate(), nullptr);
        takeState.setProperty ("ranges", ranges.joinIntoString (";"), nullptr);
    }

    ProjectState::save (parameters, noteModel, uiCopy, destData, takeState);
}

void MoonTuneAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::ValueTree uiCopy (ProjectState::ids::ui);
    juce::ValueTree takeState;
    const auto result = ProjectState::restore (data, sizeInBytes, parameters, noteModel, uiCopy, &takeState);

    // Re-open the captured vocal (loaded and analysed in the background).
    const auto takeId = result.loaded ? takeState.getProperty ("id").toString() : juce::String();
    if (takeId.isNotEmpty() && ! araActive.load())
    {
        auto current = capture.getTake();
        if (current == nullptr || current->getId() != takeId)
        {
            std::vector<TakeAudio::Range> ranges;
            for (const auto& part : juce::StringArray::fromTokens (takeState.getProperty ("ranges").toString(), ";", ""))
            {
                const auto a = part.upToFirstOccurrenceOf (":", false, false).getLargeIntValue();
                const auto b = part.fromFirstOccurrenceOf (":", false, false).getLargeIntValue();
                if (b > a && a >= 0)
                    ranges.push_back ({ a, b });
            }
            analysis.loadTake (takeId, std::move (ranges));
        }
    }

    const juce::ScopedLock sl (uiLock);
    if (result.loaded)
        uiState = uiCopy;
    lastStateMessage = result.message;
    DBG ("MoonTune: " << result.message);
}

juce::String MoonTuneAudioProcessor::getLastStateMessage() const
{
    const juce::ScopedLock sl (uiLock);
    return lastStateMessage;
}

juce::var MoonTuneAudioProcessor::getUiProperty (const juce::Identifier& name, const juce::var& defaultValue) const
{
    const juce::ScopedLock sl (uiLock);
    return uiState.getProperty (name, defaultValue);
}

void MoonTuneAudioProcessor::setUiProperty (const juce::Identifier& name, const juce::var& value)
{
    const juce::ScopedLock sl (uiLock);
    uiState.setProperty (name, value, nullptr);
}

} // namespace moontune

// Plugin entry point required by JUCE.
#if ! MOONTUNE_TEST_BUILD
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new moontune::MoonTuneAudioProcessor();
}
#endif
