// Developer check (not part of the normal run): renders a real recording with given pitch edits
// through both engines. Enabled with MOONTUNE_REAL_WAV=<mono wav> MOONTUNE_REAL_EDITS=<"t cents" per line>
// MOONTUNE_REAL_OUT=<output prefix>. Each analysed note gets the median edit over its length.
#include <juce_audio_formats/juce_audio_formats.h>
#include "Analysis/VocalAnalyzer.h"
#include "DSP/VocalRenderer.h"
#include "Capture/TakeAudio.h"
#include "DSP/HeldTone.h"
#include <cstdlib>

using namespace moontune;

class RealFileCheck final : public juce::UnitTest
{
public:
    RealFileCheck() : juce::UnitTest ("Real file check", "MoonTune") {}

    void runTest() override
    {
        beginTest ("Real recording through both engines (developer check)");
        const char* wav = std::getenv ("MOONTUNE_REAL_WAV");
        const char* edits = std::getenv ("MOONTUNE_REAL_EDITS");
        const char* outPrefix = std::getenv ("MOONTUNE_REAL_OUT");
        if (wav == nullptr || edits == nullptr || outPrefix == nullptr) { expect (true); return; }

        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (juce::File (wav)));
        expect (reader != nullptr);
        if (reader == nullptr) return;
        juce::AudioBuffer<float> buf (1, (int) reader->lengthInSamples);
        reader->read (&buf, 0, buf.getNumSamples(), 0, true, false);
        const double sr = reader->sampleRate;

        TakeAudio take (sr);
        take.write (0, buf.getReadPointer (0), buf.getNumSamples());
        const auto analysis = VocalAnalyzer::analyse (take);

        // Held-note demo (MOONTUNE_HOLD_OUT=<wav>): the three longest notes, each held 2.5 s the way
        // the audition holds them (onset once, then the frozen moment through the same HeldTone).
        if (const char* holdOut = std::getenv ("MOONTUNE_HOLD_OUT"))
        {
            auto sorted = analysis.notes;
            std::sort (sorted.begin(), sorted.end(), [] (const auto& a, const auto& b)
                       { return a.endSeconds - a.startSeconds > b.endSeconds - b.startSeconds; });
            std::vector<float> out;
            const float* x = buf.getReadPointer (0);
            for (size_t ni = 0; ni < std::min<size_t> (3, sorted.size()); ++ni)
            {
                const auto& a = sorted[ni];
                const double f0 = 440.0 * std::pow (2.0, (a.pitch - 69.0) / 12.0);
                const auto start = (juce::int64) (a.startSeconds * sr), end = (juce::int64) (a.endSeconds * sr);
                const auto window = (juce::int64) std::llround (4.0 * sr / f0);
                const auto holdAt = start + std::max<juce::int64> (window + (juce::int64) (0.015 * sr), (juce::int64) (0.4 * (double) (end - start)));
                const auto xfade = std::min<juce::int64> ((juce::int64) (0.030 * sr), end - holdAt);
                HeldTone tone;
                const bool ok = tone.analyse (x + holdAt - window, (int) window, f0, sr);
                logMessage (juce::String::formatted ("  hold note %.3f-%.3f pitch %.2f: %s", a.startSeconds, a.endSeconds, a.pitch, ok ? "held" : "not periodic"));
                const auto total = (juce::int64) (2.5 * sr);
                std::vector<float> t ((size_t) total);
                if (ok) tone.render (t.data(), (int) (total - (holdAt - start)));
                for (juce::int64 i = 0; i < total; ++i)
                {
                    const auto pos = start + i;
                    const float real = pos < end ? x[pos] : 0.0f;
                    float w = 0.0f;
                    if (ok && pos >= holdAt) w = pos >= holdAt + xfade ? 1.0f : (float) std::pow (std::sin ((double) (pos - holdAt) / (double) xfade * 1.5707963), 2.0);
                    const float tv = (ok && pos >= holdAt) ? t[(size_t) (pos - holdAt)] : 0.0f;
                    float g = i < (juce::int64) (0.015 * sr) ? (float) i / (float) (0.015 * sr) : 1.0f;
                    if (total - i < (juce::int64) (0.04 * sr)) g *= (float) (total - i) / (float) (0.04 * sr);
                    if (! ok && pos >= end) g = 0.0f;
                    out.push_back ((real * (1.0f - w) + tv * w) * g);
                }
                out.insert (out.end(), (size_t) (0.4 * sr), 0.0f);
            }
            juce::AudioBuffer<float> ob (1, (int) out.size());
            for (size_t i = 0; i < out.size(); ++i) ob.setSample (0, (int) i, out[i]);
            juce::File f (holdOut); f.deleteFile();
            juce::WavAudioFormat wavFmt;
            std::unique_ptr<juce::AudioFormatWriter> wr (wavFmt.createWriterFor (new juce::FileOutputStream (f), sr, 1, 32, {}, 0));
            if (wr != nullptr) wr->writeFromAudioSampleBuffer (ob, 0, ob.getNumSamples());
        }

        std::vector<std::pair<double, double>> curve;
        for (auto line : juce::StringArray::fromLines (juce::File (edits).loadFileAsString()))
        {
            auto parts = juce::StringArray::fromTokens (line, " ", "");
            if (parts.size() >= 2) curve.emplace_back (parts[0].getDoubleValue(), parts[1].getDoubleValue());
        }
        std::vector<Note> notes;
        juce::int64 id = 1;
        for (const auto& a : analysis.notes)
        {
            Note n; n.id = id++; n.startSeconds = a.startSeconds; n.endSeconds = a.endSeconds; n.detectedPitch = a.pitch;
            std::vector<double> v;
            for (auto& [t, c] : curve) if (t >= a.startSeconds && t < a.endSeconds) v.push_back (c);
            if (! v.empty()) { std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end()); n.pitchShiftCents = (float) v[v.size() / 2]; }
            if (std::abs (n.pitchShiftCents) < 6.0f) n.pitchShiftCents = 0.0f;
            logMessage (juce::String::formatted ("  note %.3f-%.3f  pitch %.2f  shift %+.0f c", a.startSeconds, a.endSeconds, a.pitch, n.pitchShiftCents));
            notes.push_back (n);
        }

        for (const auto& nz : analysis.noise)
            logMessage (juce::String::formatted ("  noise %.3f-%.3f  %s", nz.startSeconds, nz.endSeconds,
                                                 nz.kind == NoiseRegion::breath ? "breath" : nz.kind == NoiseRegion::sibilant ? "sibilant" : "plosive"));

        for (int engine : { Engine::harmonyClean, Engine::trueVoice, Engine::hybrid })
        {
            RenderOptions o; o.engine = engine;
            auto r = VocalRenderer::render (take, analysis, notes, o);
            expect (r != nullptr);
            if (r == nullptr) continue;
            juce::AudioBuffer<float> outBuf (1, buf.getNumSamples());
            for (int i = 0; i < buf.getNumSamples(); ++i)
            {
                float y = buf.getSample (0, i);
                r->sampleAt (i, y);
                outBuf.setSample (0, i, y);
            }
            juce::File f (juce::String (outPrefix) + (engine == Engine::harmonyClean ? "_clean.wav" : engine == Engine::trueVoice ? "_truevoice.wav" : "_hybrid.wav"));
            f.deleteFile();
            juce::WavAudioFormat wavFmt;
            std::unique_ptr<juce::AudioFormatWriter> w (wavFmt.createWriterFor (new juce::FileOutputStream (f), sr, 1, 32, {}, 0));
            if (w != nullptr) w->writeFromAudioSampleBuffer (outBuf, 0, outBuf.getNumSamples());
        }
    }
};

static RealFileCheck realFileCheck;
