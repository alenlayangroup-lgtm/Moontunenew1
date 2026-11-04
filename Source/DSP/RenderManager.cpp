#include "RenderManager.h"

#include <algorithm>

namespace moontune
{

RenderManager::RenderManager (NoteModel& n, AnalysisManager& a, AudioCapture& c)
    : juce::Thread ("MoonTune render"), notes (n), analysis (a), capture (c)
{
    startThread (juce::Thread::Priority::low);
}

RenderManager::~RenderManager()
{
    stop();
}

void RenderManager::stop()
{
    cancel.store (true);
    signalThreadShouldExit();
    stopThread (10000);
}

RenderManager::Inputs RenderManager::currentInputs() const
{
    Inputs in;
    in.notesVersion = notes.getVersion();
    in.resultVersion = analysis.getResultVersion();
    in.take = capture.getTake().get();
    in.options = optionsProvider ? optionsProvider() : RenderOptions{};
    return in;
}

void RenderManager::publish (std::shared_ptr<RenderedAudio> audio)
{
    const auto generation = publishedGeneration.load() + 1;
    if (audio != nullptr)
        audio->generation = generation;
    {
        const std::lock_guard<std::mutex> lock (ownedMutex);
        if (audio != nullptr)
            owned.push_back (audio);
    }
    publishedIsEdit.store (audio != nullptr && ! audio->plain);
    published.store (audio.get());
    publishedGeneration.store (generation);
    retireUnused();
}

void RenderManager::retireUnused()
{
    const auto inUse = audioInUse.load();
    const auto* live = published.load();
    const std::lock_guard<std::mutex> lock (ownedMutex);
    owned.erase (std::remove_if (owned.begin(), owned.end(),
                                 [&] (const std::shared_ptr<RenderedAudio>& r)
                                 { return r.get() != live && r->generation < inUse; }),
                 owned.end());
}

void RenderManager::run()
{
    while (! threadShouldExit())
    {
        const auto in = currentInputs();
        if (renderedOnce && in == renderedInputs)
        {
            upToDate.store (true);
            retireUnused();
            juce::Thread::sleep (15);
            continue;
        }

        upToDate.store (false);
        // Debounce: wait until the inputs stop changing for a moment (drags, typing).
        juce::Thread::sleep (debounceMs);
        if (threadShouldExit())
            break;
        const auto settled = currentInputs();
        if (! (settled == in) && ! (renderedOnce && settled == renderedInputs))
        {
            // Still changing (a drag in progress). Wait, but render at least every ~0.2 s
            // so the sound follows the mouse while dragging.
            if (++deferred < 8)
                continue;
        }
        deferred = 0;

        rendering.store (true);
        cancel.store (false);
        std::shared_ptr<RenderedAudio> audio;
        try
        {
            const auto take = capture.getTake();
            const auto result = analysis.getResult();
            const auto snapshot = notes.getSnapshot();
            if (take.get() != cachedTake || settled.resultVersion != cachedResultVersion)
            {
                voiceCache.clear();   // the voice analysis depends only on the take and its analysis
                cachedTake = take.get();
                cachedResultVersion = settled.resultVersion;
            }
            if (take != nullptr && result != nullptr && ! take->isEmpty())
                audio = VocalRenderer::render (*take, *result, snapshot, settled.options, &cancel, &voiceCache);
            if (audio == nullptr && take != nullptr && ! take->isEmpty() && ! cancel.load())
                audio = makePlain (take);
            else if (audio != nullptr)
            {
                ensurePlainSamples (take);
                audio->original = plainSamples;
                audio->channels = take->getOriginalChannels();
            }   // nothing edited: the take itself (audition / ARA playback)
        }
        catch (...)
        {
            audio.reset();   // never let a render error reach the host; the input plays instead
        }
        rendering.store (false);

        if (threadShouldExit())
            break;

        publish (std::move (audio));
        renderedInputs = settled;
        renderedOnce = true;
    }
}

void RenderManager::ensurePlainSamples (const std::shared_ptr<TakeAudio>& take)
{
    if (plainSamples == nullptr || plainTake != take.get() || plainVersion != cachedResultVersion)
    {
        auto v = std::make_shared<std::vector<float>> ((size_t) take->getEndSample(), 0.0f);
        for (const auto& r : take->getRanges())
            take->read (r.start, v->data() + r.start, (int) r.length());
        plainSamples = std::move (v);
        plainTake = take.get();
        plainVersion = cachedResultVersion;
    }
}

std::shared_ptr<RenderedAudio> RenderManager::makePlain (const std::shared_ptr<TakeAudio>& take)
{
    ensurePlainSamples (take);
    auto a = std::make_shared<RenderedAudio>();
    a->sampleRate = take->getSampleRate();
    a->ranges = take->getRanges();
    a->plain = true;
    a->sharedSamples = plainSamples;
    a->original = plainSamples;
    a->channels = take->getOriginalChannels();
    return a;
}

bool RenderManager::waitUntilRendered (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
    juce::Thread::sleep (debounceMs + 20);
    while (juce::Time::getMillisecondCounter() < end)
    {
        if (upToDate.load() && ! rendering.load() && currentInputs() == renderedInputs)
            return true;
        juce::Thread::sleep (5);
    }
    return false;
}

std::shared_ptr<const RenderedAudio> RenderManager::getRenderedForTesting() const
{
    const auto* live = published.load();
    if (live != nullptr && live->plain)
        return nullptr;
    const std::lock_guard<std::mutex> lock (ownedMutex);
    for (const auto& r : owned)
        if (r.get() == live)
            return r;
    return nullptr;
}

// --- Audio thread -------------------------------------------------------------------

void RenderManager::resetPlayback() noexcept
{
    fadeRemaining = 0;
    previous = nullptr;
}

bool RenderManager::process (juce::AudioBuffer<float>& buffer, int numChannels, juce::int64 timelineSample,
                             double hostSampleRate) noexcept
{
    return processImpl (buffer, numChannels, timelineSample, hostSampleRate, false);
}

bool RenderManager::processAudition (juce::AudioBuffer<float>& buffer, int numChannels, juce::int64 timelineSample,
                                     double hostSampleRate) noexcept
{
    return processImpl (buffer, numChannels, timelineSample, hostSampleRate, true);
}

bool RenderManager::processOriginal (juce::AudioBuffer<float>& buffer, int numChannels, juce::int64 timelineSample,
                                     double hostSampleRate) noexcept
{
    // Uses whatever process()/processAudition() last took (`current`): same hazard protection.
    const RenderedAudio* r = current != nullptr ? current : published.load();
    if (r == nullptr || std::abs (r->sampleRate - hostSampleRate) >= 0.5)
        return false;
    if (r != current)
    {
        audioInUse.store (std::min (publishedGeneration.load(), r->generation));
        if (published.load() != r) return false;
    }
    bool used = false;
    const int channels = std::min (numChannels, buffer.getNumChannels());
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        float v;
        if (! r->originalAt (timelineSample + i, v)) continue;
        used = true;
        for (int ch = 0; ch < channels; ++ch)
            buffer.setSample (ch, i, r->originalChannel (timelineSample + i, ch, v));
    }
    return used;
}

bool RenderManager::processImpl (juce::AudioBuffer<float>& buffer, int numChannels, juce::int64 timelineSample,
                                 double hostSampleRate, bool includePlain) noexcept
{
    // Hazard protocol: announce the oldest generation we may still touch *before*
    // loading the newest pointer, so the render thread never frees it under us.
    const auto generation = publishedGeneration.load();
    juce::uint64 oldest = generation;
    if (current != nullptr) oldest = std::min (oldest, current->generation);
    if (previous != nullptr) oldest = std::min (oldest, previous->generation);
    audioInUse.store (oldest);

    const RenderedAudio* latest = published.load();
    const int fadeLength = std::max (1, (int) (0.020 * hostSampleRate));

    if (latest != current || ! haveCurrentFlag)
    {
        if (haveCurrentFlag)
        {
            previous = current;
            fadeRemaining = fadeLength;
        }
        current = latest;
        haveCurrentFlag = true;
    }

    auto usable = [hostSampleRate, includePlain] (const RenderedAudio* r)
    {
        return r != nullptr && (includePlain || ! r->plain) && std::abs (r->sampleRate - hostSampleRate) < 0.5;
    };
    const bool curOk = usable (current), prevOk = usable (previous) && fadeRemaining > 0;
    if (! curOk && ! prevOk)
    {
        fadeRemaining = 0;
        previous = nullptr;
        return false;
    }

    const int numSamples = buffer.getNumSamples();
    const int channels = std::min (numChannels, buffer.getNumChannels());
    bool used = false;

    for (int i = 0; i < numSamples; ++i)
    {
        const juce::int64 pos = timelineSample + i;
        float rCur = 0.0f, rPrev = 0.0f;
        const bool hasCur = curOk && current->sampleAt (pos, rCur);
        const bool hasPrev = prevOk && fadeRemaining > 0 && previous->sampleAt (pos, rPrev);

        float mixToCur = 1.0f;
        if (fadeRemaining > 0)
        {
            mixToCur = 1.0f - (float) fadeRemaining / (float) fadeLength;
            --fadeRemaining;
            if (fadeRemaining == 0)
                previous = nullptr;
        }

        if (! hasCur && ! hasPrev)
            continue;

        used = true;
        // Each channel keeps its own original and only receives the edit (rendered - original
        // mono): untouched audio is the original in every channel, stereo included. The base is
        // the host's input (insert mode) or the stored original channels (ARA / audition).
        auto value = [&] (const RenderedAudio* r, float rendered, int ch, float in) -> float
        {
            float o;
            if (! r->originalAt (pos, o))
                return rendered;
            const float base = includePlain ? r->originalChannel (pos, ch, o) : in;
            return base + (rendered - o);
        };
        for (int ch = 0; ch < channels; ++ch)
        {
            const float in = buffer.getSample (ch, i);
            const float a = hasPrev ? value (previous != nullptr ? previous : current, rPrev, ch, in) : in;
            const float b = hasCur ? value (current, rCur, ch, in) : in;
            buffer.setSample (ch, i, a + (b - a) * mixToCur);
        }
    }

    return used;
}

} // namespace moontune
