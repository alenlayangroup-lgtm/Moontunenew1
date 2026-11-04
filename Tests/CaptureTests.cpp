// End-to-end: a simulated DAW plays a vocal through the plugin while REC is armed;
// MoonTune captures it, analyses it in the background and fills the note model.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Music/KeyDetector.h"
#include "TestSignals.h"

#include "TestHelpers.h"
using namespace moontune;
using namespace moontune::testhelpers;
using namespace moontune::testsignals;

namespace
{
    struct FakePlayHead final : juce::AudioPlayHead
    {
        juce::int64 timeInSamples = 0;
        bool playing = false;
        bool provideTime = true;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo info;
            info.setIsPlaying (playing);
            if (provideTime)
                info.setTimeInSamples (timeInSamples);
            return info;
        }
    };

    // Plays `audio` (mono) through the processor from timeline position `startSample`.
    void play (MoonTuneAudioProcessor& p, FakePlayHead& head, const std::vector<float>& audio,
               juce::int64 startSample, int channels, int blockSize)
    {
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> block (channels, blockSize);
        head.playing = true;
        for (juce::int64 pos = 0; pos < (juce::int64) audio.size(); pos += blockSize)
        {
            const int n = (int) std::min<juce::int64> (blockSize, (juce::int64) audio.size() - pos);
            block.setSize (channels, n, false, false, true);
            for (int ch = 0; ch < channels; ++ch)
                block.copyFrom (ch, 0, audio.data() + pos, n);
            head.timeInSamples = startSample + pos;
            p.processBlock (block, midi);
            // Real-time pacing is not needed: the FIFO holds 4 s; let the capture thread drain.
            if ((pos / blockSize) % 64 == 0)
                juce::Thread::sleep (1);
        }
        head.playing = false;
    }

    bool waitForNotes (MoonTuneAudioProcessor& p, int minNotes, int timeoutMs)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < end)
        {
            if (p.getNoteModel().size() >= minNotes
                && p.getAnalysis().getStatus() == AnalysisManager::Status::ready
                && p.getAnalysis().waitUntilIdle (10))
                return true;
            juce::Thread::sleep (20);
        }
        return false;
    }

    juce::File tempDir()
    {
        auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("MoonTuneTests-" + juce::Uuid().toDashedString());
        d.createDirectory();
        return d;
    }

    const std::vector<MelodyNote> melody {
        { 0.30, 0.80, 60 }, { 0.80, 1.30, 62 }, { 1.30, 1.80, 64 }, { 1.80, 2.60, 65, 5.5, 70 },
        { 2.60, 3.10, 64 }, { 3.10, 3.60, 62 }, { 3.60, 4.40, 60 } };
}

class CaptureTests final : public juce::UnitTest
{
public:
    CaptureTests() : juce::UnitTest ("Capture -> analysis -> notes (end to end)", "MoonTune") {}

    void runTest() override
    {
        const auto storage = tempDir();

        beginTest ("REC + play: notes appear at the right times and pitches; audio unchanged");
        for (double sr : { 44100.0, 48000.0, 96000.0 })
        {
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            FakePlayHead head;
            p.setPlayHead (&head);
            p.prepareToPlay (sr, 512);

            const juce::int64 offset = (juce::int64) (12.0 * sr);   // vocal starts at 0:12 on the timeline
            auto audio = renderMelody (melody, sr, 4.8);
            p.setCaptureArmed (true);
            play (p, head, audio, offset, 1, 512);

            const bool ok = waitForNotes (p, (int) melody.size(), 20000);
            expect (ok, "analysis finished @" + juce::String (sr));
            const auto notes = p.getNoteModel().getSnapshot();
            expectEquals ((int) notes.size(), (int) melody.size(), "note count @" + juce::String (sr));
            if (notes.size() == melody.size())
                for (size_t i = 0; i < notes.size(); ++i)
                {
                    expectWithinAbsoluteError (notes[i].detectedPitch, (float) melody[i].pitch, 0.15f);
                    expectWithinAbsoluteError (notes[i].startSeconds, 12.0 + melody[i].start, 0.05);
                }
            expectEquals (p.getCapture().getDroppedBlocks(), 0);

            auto result = p.getAnalysis().getResult();
            expect (result != nullptr && ! result->frames.empty());
            p.setPlayHead (nullptr);
        }

        beginTest ("Stereo track, odd block sizes, host without timeline position");
        {
            const double sr = 48000.0;
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            p.setBusesLayout ({ { juce::AudioChannelSet::stereo() }, { juce::AudioChannelSet::stereo() } });
            FakePlayHead head;
            head.provideTime = false;
            p.setPlayHead (&head);
            p.prepareToPlay (sr, 333);
            p.setCaptureArmed (true);
            play (p, head, renderMelody (melody, sr, 4.8), 0, 2, 333);
            expect (waitForNotes (p, (int) melody.size(), 20000));
            expectEquals (p.getNoteModel().size(), (int) melody.size());
            p.setPlayHead (nullptr);
        }

        beginTest ("Not armed or not playing: nothing is captured");
        {
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            FakePlayHead head;
            p.setPlayHead (&head);
            p.prepareToPlay (48000.0, 512);
            play (p, head, renderMelody (melody, 48000.0, 4.8), 0, 1, 512);   // REC off
            juce::Thread::sleep (600);
            expect (p.getCapture().getTake() == nullptr);
            expectEquals (p.getNoteModel().size(), 0);
            p.setPlayHead (nullptr);
        }

        beginTest ("Re-recording a part replaces only the notes in that part");
        {
            const double sr = 48000.0;
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            FakePlayHead head;
            p.setPlayHead (&head);
            p.prepareToPlay (sr, 512);
            p.setCaptureArmed (true);
            play (p, head, renderMelody (melody, sr, 4.8), 0, 1, 512);
            expect (waitForNotes (p, (int) melody.size(), 20000));

            // Sing the first two notes a fourth higher and re-record just that part (0 .. 1.3 s).
            auto partial = renderMelody ({ { 0.30, 0.80, 65 }, { 0.80, 1.30, 67 } }, sr, 1.3);
            juce::Thread::sleep (500);   // previous session finished
            play (p, head, partial, 0, 1, 512);
            juce::Thread::sleep (500);
            expect (p.getAnalysis().waitUntilIdle (20000));
            const auto notes = p.getNoteModel().getSnapshot();
            expectEquals ((int) notes.size(), (int) melody.size());
            if (notes.size() == melody.size())
            {
                expectWithinAbsoluteError (notes[0].detectedPitch, 65.0f, 0.15f);
                expectWithinAbsoluteError (notes[1].detectedPitch, 67.0f, 0.15f);
                expectWithinAbsoluteError (notes[2].detectedPitch, 64.0f, 0.15f);   // untouched
            }
            p.setPlayHead (nullptr);
        }

        beginTest ("Project save / reopen: notes restored, captured audio reloaded from disk");
        {
            const double sr = 44100.0;
            juce::MemoryBlock state;
            std::vector<Note> before;
            {
                MoonTuneAudioProcessor p;
                p.getAnalysis().setStorageDirectory (storage);
                FakePlayHead head;
                p.setPlayHead (&head);
                p.prepareToPlay (sr, 256);
                p.setCaptureArmed (true);
                play (p, head, renderMelody (melody, sr, 4.8), (juce::int64) (3 * sr), 1, 256);
                expect (waitForNotes (p, (int) melody.size(), 20000));
                before = p.getNoteModel().getSnapshot();
                p.getStateInformation (state);
                expect (p.getAnalysis().getTakeFile (p.getCapture().getTake()->getId()).existsAsFile());
                p.setPlayHead (nullptr);
            }
            {
                MoonTuneAudioProcessor p;
                p.getAnalysis().setStorageDirectory (storage);
                p.setStateInformation (state.getData(), (int) state.getSize());
                expect (p.getNoteModel().getSnapshot() == before);   // notes come back immediately
                expect (p.getAnalysis().waitUntilIdle (20000));
                auto r = p.getAnalysis().getResult();
                expect (r != nullptr, "pitch curves available after reopening");
                expect (p.getCapture().getTake() != nullptr);
                expect (p.getNoteModel().getSnapshot() == before);   // analysis did not overwrite them
            }
        }

        beginTest ("Reopen when the audio file is missing: notes kept, clear message");
        {
            juce::MemoryBlock state;
            {
                MoonTuneAudioProcessor p;
                p.getAnalysis().setStorageDirectory (storage);
                FakePlayHead head;
                p.setPlayHead (&head);
                p.prepareToPlay (48000.0, 512);
                p.setCaptureArmed (true);
                play (p, head, renderMelody (melody, 48000.0, 4.8), 0, 1, 512);
                expect (waitForNotes (p, (int) melody.size(), 20000));
                p.getStateInformation (state);
                p.getAnalysis().getTakeFile (p.getCapture().getTake()->getId()).deleteFile();
                p.setPlayHead (nullptr);
            }
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            p.setStateInformation (state.getData(), (int) state.getSize());
            expect (p.getAnalysis().waitUntilIdle (10000));
            expectEquals (p.getNoteModel().size(), (int) melody.size());
            expect (p.getAnalysis().getStatus() == AnalysisManager::Status::failed);
            expect (p.getAnalysis().getMessage().contains ("not found"));
        }

        beginTest ("Clear removes notes, analysis and the stored file");
        {
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            FakePlayHead head;
            p.setPlayHead (&head);
            p.prepareToPlay (48000.0, 512);
            p.setCaptureArmed (true);
            play (p, head, renderMelody (melody, 48000.0, 4.8), 0, 1, 512);
            expect (waitForNotes (p, (int) melody.size(), 20000));
            const auto file = p.getAnalysis().getTakeFile (p.getCapture().getTake()->getId());
            p.clearCapturedVocal();
            expectEquals (p.getNoteModel().size(), 0);
            expect (p.getAnalysis().getResult() == nullptr);
            expect (! file.existsAsFile());
            expect (! p.isCaptureArmed());
            p.setPlayHead (nullptr);
        }

        beginTest ("Analyze Key: clear melody, honest about ambiguity and short material");
        {
            auto mk = [] (std::vector<int> pitches, double dur)
            {
                std::vector<Note> v;
                double t = 0.0;
                juce::int64 id = 1;
                for (int pch : pitches) { v.push_back (makeNote (id++, t, t + dur, (float) pch, 0.0f, 0.0)); t += dur; }
                return v;
            };

            // G major melody with tonic emphasis, long enough.
            std::vector<int> gMaj;
            for (int rep = 0; rep < 6; ++rep)
                for (int pch : { 67, 71, 74, 72, 71, 69, 66, 67, 62, 67 }) gMaj.push_back (pch);
            const auto r1 = KeyDetector::detect (mk (gMaj, 0.4));
            expect (r1.enoughData);
            expect (r1.hasSuggestion() && r1.candidates[0].key == KeyScale (7, ScaleType::Major), r1.summary);

            // Very short: suggestion but flagged as uncertain.
            const auto r2 = KeyDetector::detect (mk ({ 60, 62, 64 }, 0.3));
            expect (! r2.enoughData);
            expect (r2.ambiguous);

            const auto r3 = KeyDetector::detect ({});
            expect (! r3.hasSuggestion());
            expect (r3.summary.isNotEmpty());
        }

        beginTest ("Editor shows captured notes with pitch curves (snapshot)");
        {
            const double sr = 48000.0;
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            FakePlayHead head;
            p.setPlayHead (&head);
            p.prepareToPlay (sr, 512);

            std::vector<MelodyNote> song {
                { 0.40, 0.95, 64 }, { 0.95, 1.45, 67 }, { 1.45, 2.70, 69, 5.3, 90 }, { 2.70, 3.15, 67 },
                { 3.15, 3.60, 64 }, { 3.60, 4.80, 62, 5.0, 60 } };
            auto audio = renderMelody (song, sr, 6.6);
            auto more = renderMelody ({ { 5.4, 5.9, 60 }, { 5.9, 6.4, 62 } }, sr, 6.6);
            for (size_t i = 0; i < audio.size(); ++i) audio[i] += more[i];
            addNoise (audio, sr, 5.0, 5.3, 0.04f, false, 7);   // breath

            auto* editor = dynamic_cast<MoonTuneAudioProcessorEditor*> (p.createEditorIfNeeded());
            expect (editor != nullptr);
            editor->setSize (1200, 680);
            p.setCaptureArmed (true);
            play (p, head, audio, 0, 1, 512);
            expect (waitForNotes (p, 6, 20000));
            juce::Thread::sleep (100);
            editor->refreshFromProcessor();   // pick up notes and analysis as the 30 Hz timer would

            juce::Image img (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
            juce::Graphics g (img);
            editor->paintEntireComponent (g, false);

            const auto out = juce::SystemStats::getEnvironmentVariable ("MOONTUNE_SNAPSHOT_CAPTURE", {});
            if (out.isNotEmpty())
            {
                juce::File f (out);
                f.deleteFile();
                juce::FileOutputStream os (f);
                juce::PNGImageFormat().writeImageToStream (img, os);
            }
            p.editorBeingDeleted (editor);
            delete editor;
            p.setPlayHead (nullptr);
        }

        storage.deleteRecursively();
    }
};

static CaptureTests captureTests;
