#include "AudioCapture.h"

namespace moontune
{

AudioCapture::AudioCapture() : juce::Thread ("MoonTune capture")
{
    prepare (44100.0, 512);
    startThread (juce::Thread::Priority::normal);
}

AudioCapture::~AudioCapture()
{
    stop();
}

void AudioCapture::stop()
{
    signalThreadShouldExit();
    dataEvent.signal();
    stopThread (2000);
}

void AudioCapture::prepare (double sr, int maximumBlockSize)
{
    const std::lock_guard<std::mutex> drainLock (drainMutex);

    sampleRate = sr;

    const int fifoSize = juce::jmax (maximumBlockSize * 4, (int) (sr * fifoSeconds));
    sampleRing.assign ((size_t) fifoSize, 0.0f);
    headerRing.assign (4096, {});
    sampleFifo.setTotalSize (fifoSize);
    headerFifo.setTotalSize ((int) headerRing.size());
    sampleFifo.reset();
    headerFifo.reset();
    monoScratch.assign ((size_t) juce::jmax (1, maximumBlockSize), 0.0f);
    drainScratch.assign ((size_t) fifoSize, 0.0f);
}

void AudioCapture::setArmed (bool shouldBeArmed) noexcept
{
    armed.store (shouldBeArmed);
    dataEvent.signal();   // let the thread notice a disarm promptly
}

void AudioCapture::pushBlock (const juce::AudioBuffer<float>& buffer, int numInputChannels,
                              std::optional<juce::int64> timelineSample, bool hostIsPlaying) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const int channels = juce::jmin (numInputChannels, buffer.getNumChannels());

    if (! armed.load (std::memory_order_relaxed) || ! hostIsPlaying || numSamples <= 0 || channels <= 0)
    {
        if (! timelineSample.has_value() && hostIsPlaying)
            freeRunningPosition += numSamples;
        return;
    }

    const juce::int64 position = timelineSample.has_value() ? *timelineSample : freeRunningPosition;
    freeRunningPosition = position + numSamples;

    // Blocks larger than prepared are pushed in pieces.
    for (int offset = 0; offset < numSamples;)
    {
        const int n = juce::jmin ((int) monoScratch.size(), numSamples - offset);
        const float gain = 1.0f / (float) channels;

        for (int i = 0; i < n; ++i)
        {
            float sum = 0.0f;
            for (int ch = 0; ch < channels; ++ch)
                sum += buffer.getSample (ch, offset + i);
            monoScratch[(size_t) i] = sum * gain;
        }

        if (sampleFifo.getFreeSpace() < n || headerFifo.getFreeSpace() < 1)
        {
            droppedBlocks.fetch_add (1, std::memory_order_relaxed);
        }
        else
        {
            {
                const auto w = sampleFifo.write (n);
                if (w.blockSize1 > 0) std::copy_n (monoScratch.data(), w.blockSize1, sampleRing.data() + w.startIndex1);
                if (w.blockSize2 > 0) std::copy_n (monoScratch.data() + w.blockSize1, w.blockSize2, sampleRing.data() + w.startIndex2);
            }
            {
                const auto w = headerFifo.write (1);
                const Header h { position + offset, n };
                if (w.blockSize1 > 0) headerRing[(size_t) w.startIndex1] = h;
                else if (w.blockSize2 > 0) headerRing[(size_t) w.startIndex2] = h;
            }
        }
        offset += n;
    }

    dataEvent.signal();
}

std::shared_ptr<TakeAudio> AudioCapture::getTake() const
{
    const std::lock_guard<std::mutex> lock (takeMutex);
    return take;
}

void AudioCapture::setTake (std::shared_ptr<TakeAudio> t)
{
    const std::lock_guard<std::mutex> lock (takeMutex);
    take = std::move (t);
}

void AudioCapture::clearTake()
{
    const std::lock_guard<std::mutex> lock (takeMutex);
    take.reset();
    sessionActive = false;
}

bool AudioCapture::drainOnce()
{
    const std::lock_guard<std::mutex> drainLock (drainMutex);
    bool any = false;

    while (headerFifo.getNumReady() > 0)
    {
        Header h {};
        {
            const auto r = headerFifo.read (1);
            h = r.blockSize1 > 0 ? headerRing[(size_t) r.startIndex1] : headerRing[(size_t) r.startIndex2];
        }

        const int n = h.numSamples;
        {
            const auto r = sampleFifo.read (n);
            if (r.blockSize1 > 0) std::copy_n (sampleRing.data() + r.startIndex1, r.blockSize1, drainScratch.data());
            if (r.blockSize2 > 0) std::copy_n (sampleRing.data() + r.startIndex2, r.blockSize2, drainScratch.data() + r.blockSize1);
        }

        std::shared_ptr<TakeAudio> target;
        {
            const std::lock_guard<std::mutex> lock (takeMutex);
            // A take holds one sample rate; after a host rate change a new take is started.
            if (take == nullptr || std::abs (take->getSampleRate() - sampleRate) > 0.5)
                take = std::make_shared<TakeAudio> (sampleRate);
            target = take;

            if (! sessionActive)
            {
                sessionActive = true;
                sessionRange = { h.timelineStart, h.timelineStart + n };
            }
            sessionRange.start = std::min (sessionRange.start, h.timelineStart);
            sessionRange.end = std::max (sessionRange.end, h.timelineStart + n);
            sessionSeconds.store ((double) sessionRange.length() / sampleRate);
        }

        target->write (h.timelineStart, drainScratch.data(), n);
        any = true;
    }

    if (any)
    {
        lastDataTime = juce::Time::getMillisecondCounter();
        receiving.store (true);
    }
    return any;
}

void AudioCapture::finishSession()
{
    TakeAudio::Range range;
    {
        const std::lock_guard<std::mutex> lock (takeMutex);
        if (! sessionActive)
            return;
        sessionActive = false;
        range = sessionRange;
    }

    receiving.store (false);
    sessionSeconds.store (0.0);
    if (onSessionFinished)
        onSessionFinished (range);
}

void AudioCapture::run()
{
    while (! threadShouldExit())
    {
        dataEvent.wait (50);
        drainOnce();

        bool active;
        {
            const std::lock_guard<std::mutex> lock (takeMutex);
            active = sessionActive;
        }

        if (active)
        {
            const auto idle = juce::Time::getMillisecondCounter() - lastDataTime;
            if (! armed.load() || idle > (juce::uint32) idleMsToFinish)
            {
                drainOnce();   // anything that arrived in the meantime
                finishSession();
            }
        }
    }
}

void AudioCapture::flushForTesting()
{
    drainOnce();
    finishSession();
}

} // namespace moontune
