// Developer check (not part of the normal run): renders a real recording with given pitch edits
// through both engines. Enabled with MOONTUNE_REAL_WAV=<mono wav> MOONTUNE_REAL_EDITS=<"t cents" per line>
// MOONTUNE_REAL_OUT=<output prefix>. Each analysed note gets the median edit over its length.
#include <juce_audio_formats/juce_audio_formats.h>
#include "Analysis/VocalAnalyzer.h"
#include "DSP/VocalRenderer.h"
#include "Capture/TakeAudio.h"
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
