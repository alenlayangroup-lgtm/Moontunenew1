#include "TakeAudio.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cstring>

namespace moontune
{

TakeAudio::TakeAudio (double sr, juce::String takeId)
    : sampleRate (sr),
      id (takeId.isNotEmpty() ? takeId : juce::Uuid().toDashedString()),
      maxSamples ((juce::int64) (sr * maxSeconds))
{
    jassert (sr > 0.0);
}

float* TakeAudio::chunkForWrite (juce::int64 chunkIndex)
{
    if ((size_t) chunkIndex >= chunks.size())
        chunks.resize ((size_t) chunkIndex + 1);

    auto& c = chunks[(size_t) chunkIndex];
    if (c == nullptr)
    {
        c.reset (new float[chunkSize]);
        std::fill (c.get(), c.get() + chunkSize, 0.0f);
    }
    return c.get();
}

void TakeAudio::addRange (std::vector<Range>& rs, Range r)
{
    if (r.end <= r.start)
        return;

    rs.push_back (r);
    std::sort (rs.begin(), rs.end(), [] (const Range& a, const Range& b) { return a.start < b.start; });

    std::vector<Range> merged;
    for (const auto& x : rs)
    {
        if (! merged.empty() && x.start <= merged.back().end)
            merged.back().end = std::max (merged.back().end, x.end);
        else
            merged.push_back (x);
    }
    rs = std::move (merged);
}

int TakeAudio::write (juce::int64 startSample, const float* data, int numSamples)
{
    if (numSamples <= 0 || data == nullptr)
        return 0;

    juce::int64 start = startSample, end = startSample + numSamples;
    const juce::int64 skip = start < 0 ? -start : 0;
    start = std::max<juce::int64> (0, start);
    end = std::min (end, maxSamples);
    if (end <= start)
        return 0;

    const std::lock_guard<std::mutex> lock (mutex);

    const float* src = data + skip;
    for (juce::int64 pos = start; pos < end;)
    {
        const auto chunkIndex = pos / chunkSize;
        const auto offset = (int) (pos % chunkSize);
        const int n = (int) std::min<juce::int64> (chunkSize - offset, end - pos);
        std::memcpy (chunkForWrite (chunkIndex) + offset, src, sizeof (float) * (size_t) n);
        src += n;
        pos += n;
    }

    addRange (ranges, { start, end });
    return (int) (end - start);
}

void TakeAudio::read (juce::int64 startSample, float* dest, int numSamples) const
{
    const std::lock_guard<std::mutex> lock (mutex);

    for (int i = 0; i < numSamples;)
    {
        const juce::int64 pos = startSample + i;
        if (pos < 0)
        {
            dest[i++] = 0.0f;
            continue;
        }

        const auto chunkIndex = pos / chunkSize;
        const auto offset = (int) (pos % chunkSize);
        const int n = (int) std::min<juce::int64> (chunkSize - offset, numSamples - i);

        if ((size_t) chunkIndex < chunks.size() && chunks[(size_t) chunkIndex] != nullptr)
            std::memcpy (dest + i, chunks[(size_t) chunkIndex].get() + offset, sizeof (float) * (size_t) n);
        else
            std::fill (dest + i, dest + i + n, 0.0f);
        i += n;
    }
}

std::vector<float> TakeAudio::readRange (Range r) const
{
    std::vector<float> out ((size_t) std::max<juce::int64> (0, r.length()));
    if (! out.empty())
        read (r.start, out.data(), (int) out.size());
    return out;
}

std::vector<TakeAudio::Range> TakeAudio::getRanges() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return ranges;
}

juce::int64 TakeAudio::getEndSample() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return ranges.empty() ? 0 : ranges.back().end;
}

double TakeAudio::getCapturedSeconds() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    juce::int64 total = 0;
    for (const auto& r : ranges)
        total += r.length();
    return (double) total / sampleRate;
}

bool TakeAudio::isEmpty() const
{
    const std::lock_guard<std::mutex> lock (mutex);
    return ranges.empty();
}

void TakeAudio::setRanges (std::vector<Range> rs)
{
    const std::lock_guard<std::mutex> lock (mutex);
    ranges.clear();
    for (const auto& r : rs)
        addRange (ranges, { std::max<juce::int64> (0, r.start), std::min (r.end, maxSamples) });
}

juce::uint64 TakeAudio::computeHash() const
{
    // FNV-1a over range bounds and sample bits.
    juce::uint64 h = 1469598103934665603ull;
    auto mix = [&h] (const void* p, size_t n)
    {
        auto* b = static_cast<const juce::uint8*> (p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    };

    std::vector<float> buf (8192);
    for (const auto& r : getRanges())
    {
        mix (&r.start, sizeof (r.start));
        mix (&r.end, sizeof (r.end));
        for (juce::int64 pos = r.start; pos < r.end; pos += (juce::int64) buf.size())
        {
            const int n = (int) std::min<juce::int64> ((juce::int64) buf.size(), r.end - pos);
            read (pos, buf.data(), n);
            mix (buf.data(), sizeof (float) * (size_t) n);
        }
    }
    return h;
}

bool TakeAudio::saveFlac (const juce::File& file) const
{
    const auto end = getEndSample();
    if (end <= 0)
        return false;

    file.getParentDirectory().createDirectory();
    const auto temp = file.getSiblingFile (file.getFileName() + ".tmp");
    temp.deleteFile();

    {
        std::unique_ptr<juce::OutputStream> stream (temp.createOutputStream());
        if (stream == nullptr)
            return false;

        juce::FlacAudioFormat flac;
        const auto options = juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate)
                                                             .withNumChannels (1)
                                                             .withBitsPerSample (24)
                                                             .withQualityOptionIndex (5);
        auto writer = flac.createWriterFor (stream, options);   // takes ownership of the stream on success
        if (writer == nullptr)
            return false;

        juce::AudioBuffer<float> block (1, 32768);
        for (juce::int64 pos = 0; pos < end; pos += block.getNumSamples())
        {
            const int n = (int) std::min<juce::int64> (block.getNumSamples(), end - pos);
            read (pos, block.getWritePointer (0), n);
            for (int i = 0; i < n; ++i)   // clamp to the 24-bit range
                block.setSample (0, i, juce::jlimit (-1.0f, 1.0f, block.getSample (0, i)));
            if (! writer->writeFromAudioSampleBuffer (block, 0, n))
                return false;
        }
    }

    return temp.moveFileTo (file);
}

std::shared_ptr<TakeAudio> TakeAudio::loadFlac (const juce::File& file, const juce::String& takeId,
                                                std::vector<Range> rs)
{
    juce::FlacAudioFormat flac;
    std::unique_ptr<juce::AudioFormatReader> reader (flac.createReaderFor (file.createInputStream().release(), true));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return nullptr;

    auto take = std::make_shared<TakeAudio> (reader->sampleRate, takeId);
    const auto length = reader->lengthInSamples;

    juce::AudioBuffer<float> block (1, 32768);
    for (juce::int64 pos = 0; pos < length; pos += block.getNumSamples())
    {
        const int n = (int) std::min<juce::int64> (block.getNumSamples(), length - pos);
        if (! reader->read (&block, 0, n, pos, true, false))
            return nullptr;
        take->write (pos, block.getReadPointer (0), n);
    }

    // Keep only the ranges that were really captured (the file contains silence between them).
    for (auto& r : rs)
        r.end = std::min (r.end, length);
    take->setRanges (std::move (rs));
    return take;
}

} // namespace moontune
