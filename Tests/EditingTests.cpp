// Editing workflow: undo/redo, mouse and keyboard editing, inspector, correction macro,
// and the full loop capture -> edit -> the DAW plays the corrected vocal.

#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Music/ScaleCorrector.h"
#include "Analysis/VocalAnalyzer.h"
#include "TestSignals.h"
#include "TestHelpers.h"

using namespace moontune;
using namespace moontune::testsignals;
using namespace moontune::testhelpers;

namespace
{
    struct Head final : juce::AudioPlayHead
    {
        juce::int64 time = 0;
        bool playing = false;
        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo i;
            i.setIsPlaying (playing);
            i.setTimeInSamples (time);
            return i;
        }
    };

    // Plays mono audio through the processor; returns the (first channel of the) output.
    std::vector<float> play (MoonTuneAudioProcessor& p, Head& head, const std::vector<float>& audio, int block = 512)
    {
        std::vector<float> out (audio.size(), 0.0f);
        juce::MidiBuffer midi;
        juce::AudioBuffer<float> b (1, block);
        head.playing = true;
        for (size_t pos = 0; pos < audio.size(); pos += (size_t) block)
        {
            const int n = (int) std::min<size_t> ((size_t) block, audio.size() - pos);
            b.setSize (1, n, false, false, true);
            b.copyFrom (0, 0, audio.data() + pos, n);
            head.time = (juce::int64) pos;
            p.processBlock (b, midi);
            std::copy_n (b.getReadPointer (0), n, out.data() + pos);
            if ((pos / (size_t) block) % 64 == 0) juce::Thread::sleep (1);
        }
        head.playing = false;
        p.processBlock (b, midi);   // a stopped block (resets playback state)
        return out;
    }

    float medianPitch (const AnalysisResult& r, double t0, double t1)
    {
        std::vector<float> v;
        for (int i = r.frameIndexAt (t0); i <= r.frameIndexAt (t1) && i < (int) r.frames.size(); ++i)
            if (r.frames[(size_t) i].cls == FrameClass::Voiced) v.push_back (r.frames[(size_t) i].pitch);
        if (v.empty()) return 0.0f;
        std::sort (v.begin(), v.end());
        return v[v.size() / 2];
    }

    juce::MouseEvent mouse (juce::Component& c, juce::Point<float> pos, juce::Point<float> down,
                            juce::ModifierKeys mods = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier),
                            int clicks = 1)
    {
        auto source = juce::Desktop::getInstance().getMainMouseSource();
        return juce::MouseEvent (source, pos, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                                 juce::Time::getCurrentTime(), down, juce::Time::getCurrentTime(), clicks, pos != down);
    }

    void setParam (MoonTuneAudioProcessor& p, const juce::ParameterID& id, float plain)
    {
        auto* param = p.getParameterState().getParameter (id.getParamID());
        param->setValueNotifyingHost (param->convertTo0to1 (plain));
    }

    juce::File tempDir()
    {
        auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("MoonTuneEdit-" + juce::Uuid().toDashedString());
        d.createDirectory();
        return d;
    }
}

class EditingTests final : public juce::UnitTest
{
public:
    EditingTests() : juce::UnitTest ("Editing workflow", "MoonTune") {}

    void runTest() override
    {
        beginTest ("Edit history: perform, undo, redo, gestures, external changes");
        {
            NoteModel m;
            m.replaceAll ({ makeNote (1, 0.0, 1.0, 60.0f), makeNote (2, 1.0, 2.0, 62.0f) });
            EditHistory h (m);
            expect (! h.canUndo());

            auto a = m.getSnapshot();
            a[0].pitchShiftCents = 100.0f;
            h.perform ("Move", a);
            expectEquals (m.getSnapshot()[0].pitchShiftCents, 100.0f);
            expect (h.canUndo() && h.getUndoName() == "Move");

            h.perform ("Nothing", m.getSnapshot());   // no change -> no step
            expect (h.getUndoName() == "Move");

            expect (h.undo());
            expectEquals (m.getSnapshot()[0].pitchShiftCents, 0.0f);
            expect (h.redo());
            expectEquals (m.getSnapshot()[0].pitchShiftCents, 100.0f);

            h.beginGesture();
            for (float c : { 120.0f, 150.0f, 180.0f })
            {
                auto g = m.getSnapshot();
                g[1].pitchShiftCents = c;
                h.updateGesture (g);
            }
            h.endGesture ("Drag");
            expectEquals (m.getSnapshot()[1].pitchShiftCents, 180.0f);
            h.undo();   // the whole drag is one step
            expectEquals (m.getSnapshot()[1].pitchShiftCents, 0.0f);

            // A new recording replaced the notes: old steps must not come back.
            m.replaceAll ({ makeNote (5, 0.0, 1.0, 50.0f) });
            expect (! h.canUndo());
            expect (! h.undo());
            expectEquals (m.size(), 1);
        }

        beginTest ("Pitch drag snapping: semitone grid, scale, free");
        {
            NoteModel m;
            NoteGridView grid (m);
            const auto n = makeNote (1, 0.0, 1.0, 60.3f);   // 30 cents sharp C4
            expectWithinAbsoluteError (grid.draggedShift (n, 1.0f, false), 70.0f, 0.01f);    // 61.3 -> C#4 (61)
            expectWithinAbsoluteError (grid.draggedShift (n, 0.1f, false), -30.0f, 0.01f);   // back onto C4
            expectWithinAbsoluteError (grid.draggedShift (n, 0.37f, true), 37.0f, 0.01f);    // free
            grid.setScaleDisplay (KeyScale (0, ScaleType::Major), 0.5f);
            grid.setSnap (true, 1.0f);
            expectWithinAbsoluteError (grid.draggedShift (n, 1.0f, false), 170.0f, 0.01f);   // 61.3 in C major -> D4 (62)
            grid.setSnap (true, 0.5f);   // half-strength pull
            expectWithinAbsoluteError (grid.draggedShift (n, 0.0f, false), -15.0f, 0.01f);
        }

        beginTest ("Mouse: drag pitch, drag time, stretch edge, lasso, double-click, keys; all undoable");
        {
            NoteModel m;
            m.replaceAll ({ makeNote (1, 0.5, 1.0, 60.0f), makeNote (2, 1.2, 1.8, 64.0f), makeNote (3, 2.0, 2.5, 67.0f) });
            EditHistory h (m);
            NoteGridView grid (m);
            grid.setHistory (&h);
            grid.setBounds (0, 0, 900, 500);
            NoteGridView::View v; v.startSeconds = 0.0; v.pixelsPerSecond = 200.0; v.topPitch = 72.5f; v.pixelsPerSemitone = 16.0f;
            grid.setView (v);

            auto centreOf = [&] (juce::int64 id)
            {
                for (const auto& n : m.getSnapshot()) if (n.id == id) return grid.noteBounds (n).getCentre();
                return juce::Point<float>();
            };
            auto noteById = [&] (juce::int64 id)
            {
                for (const auto& n : m.getSnapshot()) if (n.id == id) return n;
                return Note();
            };

            // Pitch: drag note 2 up by two semitones.
            auto c = centreOf (2);
            grid.mouseDown (mouse (grid, c, c));
            grid.mouseDrag (mouse (grid, c.translated (0, -16.0f * 2.0f), c));
            grid.mouseUp (mouse (grid, c.translated (0, -16.0f * 2.0f), c));
            expectWithinAbsoluteError (noteById (2).pitchShiftCents, 200.0f, 0.01f);
            expect (h.getUndoName() == "Move pitch");

            // Time: drag note 3 right by 0.1 s (direction lock: horizontal).
            c = centreOf (3);
            grid.mouseDown (mouse (grid, c, c));
            grid.mouseDrag (mouse (grid, c.translated (20.0f, 1.0f), c));
            grid.mouseUp (mouse (grid, c.translated (20.0f, 1.0f), c));
            expectWithinAbsoluteError (noteById (3).timeShiftSeconds, 0.1, 1.0e-6);
            expectWithinAbsoluteError (noteById (3).pitchShiftCents, 0.0f, 0.01f);

            // Edge: stretch note 1's end by 0.05 s (stays before note 2).
            const auto box = grid.noteBounds (noteById (1));
            const juce::Point<float> edge (box.getRight() - 2.0f, box.getCentreY());
            grid.mouseDown (mouse (grid, edge, edge));
            grid.mouseDrag (mouse (grid, edge.translated (10.0f, 0), edge));
            grid.mouseUp (mouse (grid, edge.translated (10.0f, 0), edge));
            expectWithinAbsoluteError (noteById (1).endEditSeconds, 0.05, 1.0e-6);

            // Time drag is limited by neighbours: note 1 cannot be pushed through note 2.
            c = centreOf (1);
            grid.mouseDown (mouse (grid, c, c));
            grid.mouseDrag (mouse (grid, c.translated (400.0f, 0), c));
            grid.mouseUp (mouse (grid, c.translated (400.0f, 0), c));
            expectLessThan (noteById (1).outEnd(), noteById (2).outEnd());

            // Undo everything step by step.
            int steps = 0;
            while (h.undo()) ++steps;
            expectEquals (steps, 4);
            for (const auto& n : m.getSnapshot())
                expect (! n.hasSoundEdit(), "all edits undone");

            // Lasso selects notes 2 and 3.
            const auto p2 = centreOf (2), p3 = centreOf (3);
            const juce::Point<float> start (p2.x - 30.0f, 30.0f);
            grid.mouseDown (mouse (grid, start, start));
            grid.mouseDrag (mouse (grid, juce::Point<float> (p3.x + 10.0f, 480.0f), start));
            grid.mouseUp (mouse (grid, juce::Point<float> (p3.x + 10.0f, 480.0f), start));
            juce::String sel; for (auto id : grid.getSelection()) sel << (int) id << " ";
            expect (grid.getSelection() == std::set<juce::int64> ({ 2, 3 }), "selection: " + sel);

            // Keys: Up moves both a semitone, Shift+Down 10 cents down.
            grid.keyPressed (juce::KeyPress (juce::KeyPress::upKey));
            grid.keyPressed (juce::KeyPress (juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0));
            expectWithinAbsoluteError (noteById (2).pitchShiftCents, 90.0f, 0.01f);
            expectWithinAbsoluteError (noteById (3).pitchShiftCents, 90.0f, 0.01f);

            // Double-click: onto the nearest note.
            c = centreOf (2);
            grid.mouseDoubleClick (mouse (grid, c, c, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 2));
            expectWithinAbsoluteError (noteById (2).pitchShiftCents, 100.0f, 0.01f);

            grid.keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
            expectWithinAbsoluteError (noteById (2).pitchShiftCents, 90.0f, 0.01f);
            grid.keyPressed (juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0));
            expectWithinAbsoluteError (noteById (2).pitchShiftCents, 100.0f, 0.01f);
        }

        beginTest ("Inspector: relative pitch for several notes, absolute values, EQ auto-enable, undo");
        {
            NoteModel m;
            m.replaceAll ({ makeNote (1, 0.0, 0.5, 60.0f, 50.0f), makeNote (2, 0.6, 1.0, 62.0f, -20.0f) });
            EditHistory h (m);
            NoteInspector insp (m, h);
            insp.setBounds (0, 0, 1200, NoteInspector::preferredHeight);
            insp.setSelection ({ 1, 2 });

            insp.setValueForTesting ("pitch", 150.0);   // first note goes 50 -> 150: +100 for both
            auto s = m.getSnapshot();
            expectWithinAbsoluteError (s[0].pitchShiftCents, 150.0f, 0.01f);
            expectWithinAbsoluteError (s[1].pitchShiftCents, 80.0f, 0.01f);

            insp.setValueForTesting ("modulation", 40.0);
            insp.setValueForTesting ("formant", -200.0);
            insp.setValueForTesting ("gain", -3.0);
            s = m.getSnapshot();
            for (const auto& n : s)
            {
                expectWithinAbsoluteError (n.pitchModulation, 0.4f, 1.0e-4f);
                expectWithinAbsoluteError (n.formantCents, -200.0f, 0.01f);
                expectWithinAbsoluteError (n.gainDb, -3.0f, 0.01f);
                expect (! n.eq.enabled, "EQ stays off until used");
            }

            insp.setValueForTesting ("eq2gain", 6.0);
            s = m.getSnapshot();
            expect (s[0].eq.enabled && s[0].eq.bands[2].enabled);
            expectWithinAbsoluteError (s[1].eq.bands[2].gainDb, 6.0f, 0.01f);
            expect (! s[0].eq.bands[0].enabled);

            h.undo();
            expect (! m.getSnapshot()[0].eq.enabled);

            juce::Image img (juce::Image::ARGB, 1200, NoteInspector::preferredHeight, true);
            juce::Graphics g (img);
            insp.paintEntireComponent (g, false);
        }

        beginTest ("Correct Pitch macro: strength, tolerance, short notes, selection, remove");
        {
            std::vector<Note> notes { makeNote (1, 0.0, 0.5, 60.3f), makeNote (2, 0.6, 1.0, 61.1f),
                                      makeNote (3, 1.1, 1.15, 63.4f), makeNote (4, 1.2, 1.8, 64.04f) };
            const KeyScale cMaj (0, ScaleType::Major);
            ScaleCorrector::Settings s;
            s.strength = 1.0f; s.tolerance = 5.0f; s.minNoteSeconds = 0.08; s.speedMs = 50.0f; s.driftStrength = 0.5f;
            auto r = ScaleCorrector::apply (notes, {}, cMaj, s);
            expectWithinAbsoluteError (r[0].correctionCents, -30.0f, 0.01f);   // C4 +30 -> C4
            expectWithinAbsoluteError (r[1].correctionCents, 90.0f, 0.01f);    // C#4 +10 -> D4
            expectEquals (r[2].correctionCents, 0.0f);                          // too short: untouched
            expectEquals (r[3].correctionCents, 0.0f);                          // within tolerance
            expectEquals (r[0].correctionSpeedMs, 50.0f);
            expectWithinAbsoluteError (r[0].correctionDrift, 0.5f, 1.0e-6f);
            expectEquals (r[0].pitchShiftCents, 0.0f);                          // manual layer untouched

            s.strength = 0.5f;
            r = ScaleCorrector::apply (notes, { 2 }, cMaj, s);
            expectEquals (r[0].correctionCents, 0.0f);
            expectWithinAbsoluteError (r[1].correctionCents, 45.0f, 0.01f);

            r = ScaleCorrector::remove (r, {});
            for (const auto& n : r) expect (! n.hasCorrection());
        }

        beginTest ("New note fields survive project save / load");
        {
            MoonTuneAudioProcessor a;
            auto n = makeNote (1, 0.5, 1.5, 62.0f, 123.0f, 0.02);
            n.pitchModulation = 0.3f; n.pitchDrift = 1.4f; n.startEditSeconds = -0.01; n.endEditSeconds = 0.07;
            n.formantCents = -150.0f; n.gainDb = -2.5f; n.correctionCents = -12.0f; n.correctionDrift = 0.4f;
            n.correctionSpeedMs = 80.0f; n.eq.enabled = true;
            n.eq.bands[1] = { true, EqBand::peak, 3150.0f, -4.5f, 2.2f };
            n.eq.bands[3] = { true, EqBand::lowCut, 90.0f, 0.0f, 0.7f };
            a.getNoteModel().replaceAll ({ n });
            juce::MemoryBlock state;
            a.getStateInformation (state);
            MoonTuneAudioProcessor b;
            b.setStateInformation (state.getData(), (int) state.getSize());
            expect (b.getNoteModel().getSnapshot().front() == n);
        }

        beginTest ("Full loop: record, edit a note, the DAW plays the corrected vocal; A/B and stage bypass");
        {
            const double sr = 48000.0;
            const auto storage = tempDir();
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            Head head;
            p.setPlayHead (&head);
            p.prepareToPlay (sr, 512);

            const std::vector<MelodyNote> mel { { 0.3, 0.9, 60 }, { 0.9, 1.6, 62 }, { 1.6, 2.3, 64 } };
            const auto audio = renderMelody (mel, sr, 2.6);

            p.setCaptureArmed (true);
            play (p, head, audio);
            const auto deadline = juce::Time::getMillisecondCounter() + 20000;
            while ((p.getNoteModel().size() < 3 || p.getAnalysis().getStatus() != AnalysisManager::Status::ready)
                   && juce::Time::getMillisecondCounter() < deadline)
                juce::Thread::sleep (20);
            expectEquals (p.getNoteModel().size(), 3);
            p.setCaptureArmed (false);

            // Unedited: playback is the input, bit for bit.
            expect (p.getRenderer().waitUntilRendered (20000));
            auto out = play (p, head, audio);
            float diff = 0.0f;
            for (size_t i = 0; i < audio.size(); ++i) diff = std::max (diff, std::abs (out[i] - audio[i]));
            expectEquals (diff, 0.0f);

            // Edit: middle note up 3 semitones.
            auto notes = p.getNoteModel().getSnapshot();
            notes[1].pitchShiftCents = 300.0f;
            p.getHistory().perform ("Move pitch", notes);
            expect (p.getRenderer().waitUntilRendered (20000));
            expect (p.getRenderer().hasRenderedAudio());

            out = play (p, head, audio);
            auto r = VocalAnalyzer::analyseBuffer (out.data(), (int) out.size(), sr);
            expectWithinAbsoluteError (medianPitch (r, 1.05, 1.45), 65.0f, 0.1f);
            expectWithinAbsoluteError (medianPitch (r, 1.75, 2.15), 64.0f, 0.1f);
            expectWithinAbsoluteError (medianPitch (r, 0.4, 0.75), 60.0f, 0.1f);

            // The other engine (True Voice) plays the same edit.
            setParam (p, ParamIDs::engine, 1.0f);
            expect (p.getRenderOptions().engine == Engine::trueVoice);
            expect (p.getRenderer().waitUntilRendered (20000));
            out = play (p, head, audio);
            r = VocalAnalyzer::analyseBuffer (out.data(), (int) out.size(), sr);
            expectWithinAbsoluteError (medianPitch (r, 1.05, 1.45), 65.0f, 0.1f);
            setParam (p, ParamIDs::engine, 0.0f);
            expect (p.getRenderer().waitUntilRendered (20000));

            // Audition with the DAW stopped: the edited note sounds, then stops.
            {
                const auto n1 = p.getNoteModel().getSnapshot()[1];
                p.startAudition (n1.outStart(), n1.outEnd(), false);
                std::vector<float> heard;
                juce::AudioBuffer<float> b (1, 512);
                juce::MidiBuffer midi;
                head.playing = false;
                for (int k = 0; k < 120; ++k)
                {
                    b.clear();
                    p.processBlock (b, midi);
                    heard.insert (heard.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
                }
                expect (! p.isAuditioning());   // finished by itself (no loop)
                auto ra = VocalAnalyzer::analyseBuffer (heard.data(), (int) heard.size(), sr);
                const float dur = (float) (n1.outEnd() - n1.outStart());
                expectWithinAbsoluteError (medianPitch (ra, 0.1, dur - 0.1), 65.0f, 0.15f);
                float tailPeak = 0.0f;
                for (size_t i = (size_t) ((dur + 0.05) * sr); i < heard.size(); ++i) tailPeak = std::max (tailPeak, std::abs (heard[i]));
                expectEquals (tailPeak, 0.0f);
            }

            // Held note (sustain, like Melodyne): one continuous vowel at the edited pitch for as long
            // as it is held, with no "la-la-la" level dips, then a short release.
            {
                const auto n1 = p.getNoteModel().getSnapshot()[1];
                p.startAudition (n1.outStart(), n1.outEnd(), true, 65.0f);
                std::vector<float> heard;
                juce::AudioBuffer<float> b (1, 512);
                juce::MidiBuffer midi;
                head.playing = false;
                const int holdBlocks = (int) (3.0 * sr / 512.0);   // 3 s, several times the note length
                for (int k = 0; k < holdBlocks; ++k)
                {
                    b.clear();
                    p.processBlock (b, midi);
                    heard.insert (heard.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
                }
                expect (p.isAuditioning());
                auto ra = VocalAnalyzer::analyseBuffer (heard.data(), (int) heard.size(), sr);
                expectWithinAbsoluteError (medianPitch (ra, 0.2, 2.9), 65.0f, 0.15f);
                // Level in 20 ms windows after the onset: no dips (a re-attacked loop would fall towards 0).
                const int w = (int) (0.02 * sr);
                std::vector<float> rms;
                for (size_t i = (size_t) (0.3 * sr); i + (size_t) w <= heard.size(); i += (size_t) w)
                {
                    double e = 0.0;
                    for (int j = 0; j < w; ++j) e += (double) heard[i + (size_t) j] * heard[i + (size_t) j];
                    rms.push_back ((float) std::sqrt (e / w));
                }
                const float maxR = *std::max_element (rms.begin(), rms.end());
                const float minR = *std::min_element (rms.begin(), rms.end());
                logMessage ("  held note level: min/max over 20 ms windows = " + juce::String (minR / maxR, 3));
                expectGreaterThan (minR / maxR, 0.6f);
                // Sample-to-sample jumps stay in the range of the signal itself (no clicks at the loop joint).
                float maxStep = 0.0f, peak = 0.0f;
                for (size_t i = (size_t) (0.3 * sr) + 1; i < heard.size(); ++i)
                {
                    maxStep = std::max (maxStep, std::abs (heard[i] - heard[i - 1]));
                    peak = std::max (peak, std::abs (heard[i]));
                }
                expectLessThan (maxStep, peak * 0.5f);

                p.stopAudition();
                std::vector<float> tail;
                for (int k = 0; k < 20; ++k)
                {
                    b.clear();
                    p.processBlock (b, midi);
                    tail.insert (tail.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
                }
                float tailPeak = 0.0f;
                for (size_t i = (size_t) (0.06 * sr); i < tail.size(); ++i) tailPeak = std::max (tailPeak, std::abs (tail[i]));
                expectEquals (tailPeak, 0.0f);   // released within ~40 ms
                expect (! p.isAuditioning());
            }

            // Preview inside MoonTune (no ARA): plays the edited vocal from 1.0 s while the DAW is stopped.
            {
                p.startPreview (1.0);
                expect (p.isPreviewing());
                std::vector<float> heard;
                juce::AudioBuffer<float> b (1, 512);
                juce::MidiBuffer midi;
                head.playing = false;
                for (int k = 0; k < 60; ++k)
                {
                    b.clear();
                    p.processBlock (b, midi);
                    heard.insert (heard.end(), b.getReadPointer (0), b.getReadPointer (0) + 512);
                }
                auto rp = VocalAnalyzer::analyseBuffer (heard.data(), (int) heard.size(), sr);
                expectWithinAbsoluteError (medianPitch (rp, 0.1, 0.4), 65.0f, 0.15f);   // the edited note
                expectGreaterThan (p.getAuditionSeconds(), 1.5);
                p.stopAudition();
                expect (! p.isPreviewing());
            }

            // A/B (bypass): the original.
            setParam (p, ParamIDs::bypass, 1.0f);
            play (p, head, std::vector<float> (2048, 0.0f));   // let the 20 ms crossfade settle
            out = play (p, head, audio);
            r = VocalAnalyzer::analyseBuffer (out.data(), (int) out.size(), sr);
            expectWithinAbsoluteError (medianPitch (r, 1.05, 1.45), 62.0f, 0.1f);
            setParam (p, ParamIDs::bypass, 0.0f);

            // Pitch stage switched off: original pitch, nothing rendered.
            setParam (p, ParamIDs::procPitch, 0.0f);
            expect (p.getRenderer().waitUntilRendered (20000));
            expect (! p.getRenderer().hasRenderedAudio());
            setParam (p, ParamIDs::procPitch, 1.0f);
            expect (p.getRenderer().waitUntilRendered (20000));
            expect (p.getRenderer().hasRenderedAudio());

            // While REC is armed the input plays (re-recording), not the edit.
            p.setCaptureArmed (true);
            out = play (p, head, audio);
            p.setCaptureArmed (false);
            r = VocalAnalyzer::analyseBuffer (out.data(), (int) out.size(), sr);
            expectWithinAbsoluteError (medianPitch (r, 1.05, 1.45), 62.0f, 0.1f);

            // Undo: back to the original sound.
            juce::Thread::sleep (600);
            p.getHistory().undo();
            expect (p.getRenderer().waitUntilRendered (20000));

            // Edits during playback switch over without clicks (crossfade).
            notes = p.getNoteModel().getSnapshot();
            notes[1].pitchShiftCents = 200.0f;
            p.getHistory().perform ("Move", notes);
            expect (p.getRenderer().waitUntilRendered (20000));
            out = play (p, head, audio);
            float step = 0.0f, srcStep = 0.0f;
            for (size_t i = 1; i < out.size(); ++i)
            {
                step = std::max (step, std::abs (out[i] - out[i - 1]));
                srcStep = std::max (srcStep, std::abs (audio[i] - audio[i - 1]));
            }
            expectLessThan (step, srcStep * 1.6f + 0.002f);

            p.setPlayHead (nullptr);
            storage.deleteRecursively();
        }

        beginTest ("Musical grid snapping, vibrato / drift handles, custom scale");
        {
            NoteModel m;
            m.replaceAll ({ makeNote (1, 0.52, 1.0, 60.0f), makeNote (2, 1.6, 2.2, 64.0f) });
            EditHistory h (m);
            NoteGridView grid (m);
            grid.setHistory (&h);
            grid.setBounds (0, 0, 900, 500);
            NoteGridView::View v; v.startSeconds = 0.0; v.pixelsPerSecond = 200.0; v.topPitch = 72.5f; v.pixelsPerSemitone = 16.0f;
            grid.setView (v);
            grid.setMusicalGrid (120.0, 0.0, 4, 4, 16, false);   // 1/16 at 120 bpm = 0.125 s
            expectWithinAbsoluteError (grid.snapTime (0.52), 0.5, 1.0e-9);
            expectWithinAbsoluteError (grid.snapTime (0.57), 0.625, 1.0e-9);

            auto boundsOf = [&] (juce::int64 id) { for (const auto& n : m.getSnapshot()) if (n.id == id) return grid.noteBounds (n); return juce::Rectangle<float>(); };
            auto noteById = [&] (juce::int64 id) { for (const auto& n : m.getSnapshot()) if (n.id == id) return n; return Note(); };

            auto c = boundsOf (1).getCentre();
            grid.mouseDown (mouse (grid, c, c));
            grid.mouseDrag (mouse (grid, c.translated (21.0f, 0.0f), c));   // +0.105 s -> start snaps to 0.625
            grid.mouseUp (mouse (grid, c.translated (21.0f, 0.0f), c));
            expectWithinAbsoluteError (noteById (1).outStart(), 0.625, 1.0e-6);

            // Handles of the selected note: up = more vibrato, down = less drift.
            const auto b = boundsOf (1);
            const juce::Point<float> top (b.getCentreX(), b.getY() - 3.0f), bottom (b.getCentreX(), b.getBottom() + 3.0f);
            grid.mouseDown (mouse (grid, top, top));
            grid.mouseDrag (mouse (grid, top.translated (0, -40.0f), top));
            grid.mouseUp (mouse (grid, top.translated (0, -40.0f), top));
            expectWithinAbsoluteError (noteById (1).pitchModulation, 1.5f, 0.01f);
            grid.mouseDown (mouse (grid, bottom, bottom));
            grid.mouseDrag (mouse (grid, bottom.translated (0, 80.0f), bottom));
            grid.mouseUp (mouse (grid, bottom.translated (0, 80.0f), bottom));
            expectWithinAbsoluteError (noteById (1).pitchDrift, 0.0f, 0.01f);
            expect (h.canUndo());
            h.undo();
            expectWithinAbsoluteError (noteById (1).pitchDrift, 1.0f, 0.01f);

            // Scissors: a click cuts the note in two at that point; Gain tool: drag up = louder.
            grid.setTool (NoteGridView::Tool::split);
            {
                const auto b2 = boundsOf (2);
                const juce::Point<float> cut (b2.getX() + b2.getWidth() * 0.5f, b2.getCentreY());
                grid.mouseDown (mouse (grid, cut, cut));
                grid.mouseUp (mouse (grid, cut, cut));
                expectEquals ((int) m.size(), 3);
                expect (h.canUndo());
            }
            grid.setTool (NoteGridView::Tool::gain);
            {
                const auto c1 = boundsOf (1).getCentre();
                grid.mouseDown (mouse (grid, c1, c1));
                grid.mouseDrag (mouse (grid, c1.translated (0, -24.0f), c1));   // 4 px per dB
                grid.mouseUp (mouse (grid, c1.translated (0, -24.0f), c1));
                expectWithinAbsoluteError (noteById (1).gainDb, 6.0f, 0.01f);
            }
            grid.setTool (NoteGridView::Tool::edit);

            // Custom scale: A minor + G#.
            const auto aMinor = KeyScale (9, ScaleType::NaturalMinor);
            expect (! aMinor.containsPitchClass (8));
            const auto custom = aMinor.withPitchClassesToggled (1 << 8);
            expect (custom.containsPitchClass (8) && custom.isCustomised());
            expectEquals (custom.nearestScaleNote (68.2f), 68);                 // G#4 now allowed
            expect (custom.getName().contains ("G#"));
            expect (aMinor.withPitchClassesToggled (1 << 9).containsPitchClass (9));   // the root cannot be removed
            expect (! aMinor.withPitchClassesToggled (1 << 7).containsPitchClass (7));  // G removed

            MoonTuneAudioProcessor p;
            setParam (p, ParamIDs::keyRoot, 9.0f);
            setParam (p, ParamIDs::scaleType, (float) ScaleType::NaturalMinor);
            setParam (p, ParamIDs::scaleEdits, (float) (1 << 8));
            expect (p.getKeyScale().containsPitchClass (8));
            juce::MemoryBlock state;
            p.getStateInformation (state);
            MoonTuneAudioProcessor q;
            q.setStateInformation (state.getData(), (int) state.getSize());
            expect (q.getKeyScale().containsPitchClass (8));   // saved with the project
        }

        beginTest ("Double-click on a selection snaps all of it; BREATH mode; consonants move with their note");
        {
            NoteModel m;
            Note br; br.id = 10; br.kind = Note::breath; br.startSeconds = 2.25; br.endSeconds = 2.45; br.detectedPitch = 60.0f;
            Note k;  k.id = 11; k.kind = Note::plosive; k.startSeconds = 1.17; k.endSeconds = 1.2; k.detectedPitch = 64.0f;
            m.replaceAll ({ makeNote (1, 0.5, 1.0, 60.3f), makeNote (2, 1.2, 1.8, 63.6f), makeNote (3, 1.9, 2.2, 67.2f), br, k });
            expectEquals (m.size(), 3);
            expectEquals (m.sizeAll(), 5);
            EditHistory h (m);
            NoteGridView grid (m);
            grid.setHistory (&h);
            grid.setBounds (0, 0, 900, 500);
            NoteGridView::View v; v.startSeconds = 0.0; v.pixelsPerSecond = 200.0; v.topPitch = 72.5f; v.pixelsPerSemitone = 16.0f;
            grid.setView (v);
            auto noteById = [&] (juce::int64 id) { for (const auto& n : m.getSnapshot()) if (n.id == id) return n; return Note(); };

            grid.setSelection ({ 1, 2, 3 });
            const auto c = grid.noteBounds (noteById (2)).getCentre();
            grid.mouseDoubleClick (mouse (grid, c, c, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier), 2));
            expectWithinAbsoluteError (noteById (1).editedPitch(), 60.0f, 0.001f);
            expectWithinAbsoluteError (noteById (2).editedPitch(), 64.0f, 0.001f);
            expectWithinAbsoluteError (noteById (3).editedPitch(), 67.0f, 0.001f);
            h.undo();   // one step for all
            expectWithinAbsoluteError (noteById (1).editedPitch(), 60.3f, 0.001f);
            expectWithinAbsoluteError (noteById (3).editedPitch(), 67.2f, 0.001f);

            // Moving note 2 in time takes the "k" right before it along.
            grid.setSelection ({ 2 });
            const auto c2 = grid.noteBounds (noteById (2)).getCentre();
            grid.mouseDown (mouse (grid, c2, c2));
            grid.mouseDrag (mouse (grid, c2.translated (-10.0f, 0.0f), c2, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::ctrlModifier)));
            grid.mouseUp (mouse (grid, c2.translated (-10.0f, 0.0f), c2));
            expectWithinAbsoluteError (noteById (2).timeShiftSeconds, -0.05, 1.0e-6);
            expectWithinAbsoluteError (noteById (11).timeShiftSeconds, -0.05, 1.0e-6);
            expectEquals (noteById (10).timeShiftSeconds, 0.0);

            // BREATH mode: every breath selected, notes cannot be picked.
            grid.setBreathMode (true);
            expect (grid.getSelection() == std::set<juce::int64> { 10 });
            expectEquals (grid.noteIndexAt (grid.noteBounds (noteById (1)).getCentre()), -1);
            grid.setBreathMode (false);

            // Breaths keep their pitch when notes are snapped / nudged.
            grid.setSelection ({ 1, 10 });
            grid.nudgePitch (100.0f);
            expectEquals (noteById (10).pitchShiftCents, 0.0f);
        }

        beginTest ("Live Correct Pitch preview: shown at once, Cancel restores, Apply is one undo step; follow playhead");
        {
            const auto storage = tempDir();
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            p.getNoteModel().replaceAll ({ makeNote (1, 0.5, 1.0, 60.3f), makeNote (2, 1.2, 1.8, 63.6f) });
            setParam (p, ParamIDs::keyRoot, 0.0f);
            setParam (p, ParamIDs::scaleType, (float) ScaleType::Major);
            std::unique_ptr<juce::AudioProcessorEditor> ed (p.createEditor());
            auto* editor = dynamic_cast<MoonTuneAudioProcessorEditor*> (ed.get());
            expect (editor != nullptr);
            if (editor != nullptr)
            {
                const auto before = p.getNoteModel().getSnapshot();
                ScaleCorrector::Settings st;
                editor->beginCorrectionPreview();
                editor->previewCorrection (st, false);
                expect (p.getNoteModel().getSnapshot() != before);
                st.strength = 0.5f;
                editor->previewCorrection (st, false);
                expect (p.getNoteModel().getSnapshot()[0].hasCorrection());
                editor->endCorrectionPreview (false);   // Cancel
                expect (p.getNoteModel().getSnapshot() == before);
                expect (! p.getHistory().canUndo());

                editor->beginCorrectionPreview();
                editor->previewCorrection (st, false);
                editor->previewCorrection ({}, false);
                editor->endCorrectionPreview (true);    // Apply
                expect (p.getHistory().canUndo());
                p.getHistory().undo();
                expect (p.getNoteModel().getSnapshot() == before);

                // The editor shows the DAW playhead (here: stopped at 30 s).
                Head head;
                p.setPlayHead (&head);
                p.prepareToPlay (48000.0, 512);
                head.time = (juce::int64) (30.0 * 48000.0);
                juce::AudioBuffer<float> b (1, 512);
                juce::MidiBuffer midi;
                b.clear();
                p.processBlock (b, midi);
                editor->refreshFromProcessor();
                expect (editor->getGridView().isTimeVisible (30.0, 0.02));
                p.setPlayHead (nullptr);

                // Without ARA there is no host control: Play previews inside MoonTune (no take -> nothing).
                expect (! p.canControlHost());
            }
            ed.reset();
            storage.deleteRecursively();
        }

        beginTest ("Stereo track: untouched audio stays stereo bit for bit, the edit reaches both channels");
        {
            const double sr = 48000.0;
            const auto storage = tempDir();
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            juce::AudioProcessor::BusesLayout stereo;
            stereo.inputBuses.add (juce::AudioChannelSet::stereo());
            stereo.outputBuses.add (juce::AudioChannelSet::stereo());
            expect (p.setBusesLayout (stereo));
            Head head;
            p.setPlayHead (&head);
            p.prepareToPlay (sr, 512);
            const auto mono = renderMelody ({ { 0.3, 0.9, 60 }, { 0.9, 1.6, 62 }, { 1.6, 2.3, 64 } }, sr, 2.6);
            std::vector<float> left (mono), right (mono.size());
            for (size_t i = 0; i < mono.size(); ++i) right[i] = 0.7f * mono[i] + (i > 4 ? 0.2f * mono[i - 5] : 0.0f);   // different channel

            auto playStereo = [&] ()
            {
                std::vector<float> outL (mono.size()), outR (mono.size());
                juce::MidiBuffer midi;
                juce::AudioBuffer<float> b (2, 512);
                head.playing = true;
                for (size_t pos = 0; pos < mono.size(); pos += 512)
                {
                    const int n = (int) std::min<size_t> (512, mono.size() - pos);
                    b.setSize (2, n, false, false, true);
                    b.copyFrom (0, 0, left.data() + pos, n);
                    b.copyFrom (1, 0, right.data() + pos, n);
                    head.time = (juce::int64) pos;
                    p.processBlock (b, midi);
                    std::copy_n (b.getReadPointer (0), n, outL.data() + pos);
                    std::copy_n (b.getReadPointer (1), n, outR.data() + pos);
                }
                head.playing = false;
                p.processBlock (b, midi);
                return std::pair { outL, outR };
            };

            p.setCaptureArmed (true);
            playStereo();
            const auto deadline = juce::Time::getMillisecondCounter() + 20000;
            while ((p.getNoteModel().size() < 3 || p.getAnalysis().getStatus() != AnalysisManager::Status::ready)
                   && juce::Time::getMillisecondCounter() < deadline)
                juce::Thread::sleep (20);
            p.setCaptureArmed (false);
            auto notes = p.getNoteModel().getSnapshot();
            for (auto& n : notes) if (n.isNote() && n.startSeconds > 0.85 && n.startSeconds < 1.2) n.pitchShiftCents = 300.0f;
            p.getHistory().perform ("Move", notes);
            expect (p.getRenderer().waitUntilRendered (20000));
            const auto [outL, outR] = playStereo();
            float dL = 0.0f, dR = 0.0f;
            for (auto i = (size_t) (0.3 * sr); i < (size_t) (0.7 * sr); ++i)
            {
                dL = std::max (dL, std::abs (outL[i] - left[i]));
                dR = std::max (dR, std::abs (outR[i] - right[i]));
            }
            expectEquals (dL, 0.0f);
            expectEquals (dR, 0.0f);
            auto rl = VocalAnalyzer::analyseBuffer (outL.data(), (int) outL.size(), sr);
            auto rr = VocalAnalyzer::analyseBuffer (outR.data(), (int) outR.size(), sr);
            expectWithinAbsoluteError (medianPitch (rl, 1.05, 1.45), 65.0f, 0.15f);
            expectWithinAbsoluteError (medianPitch (rr, 1.05, 1.45), 65.0f, 0.3f);
            p.setPlayHead (nullptr);
            storage.deleteRecursively();
        }

        beginTest ("Editor with edits renders (snapshot)");
        {
            const double sr = 48000.0;
            const auto storage = tempDir();
            MoonTuneAudioProcessor p;
            p.getAnalysis().setStorageDirectory (storage);
            Head head;
            p.setPlayHead (&head);
            p.prepareToPlay (sr, 512);
            setParam (p, ParamIDs::keyRoot, 2.0f);
            setParam (p, ParamIDs::scaleType, (float) ScaleType::Major);

            const std::vector<MelodyNote> song {
                { 0.40, 0.95, 62.3 }, { 0.95, 1.45, 66.2 }, { 1.45, 2.70, 69, 5.3, 90 }, { 2.70, 3.15, 66.7 },
                { 3.15, 3.60, 64 }, { 3.60, 4.80, 62, 5.0, 60 } };
            auto audio = renderMelody (song, sr, 5.2);
            addNoise (audio, sr, 4.85, 5.1, 0.04f, false, 7);

            p.setCaptureArmed (true);
            play (p, head, audio);
            const auto deadline = juce::Time::getMillisecondCounter() + 20000;
            while ((p.getNoteModel().size() < 6 || p.getAnalysis().getStatus() != AnalysisManager::Status::ready)
                   && juce::Time::getMillisecondCounter() < deadline)
                juce::Thread::sleep (20);
            p.setCaptureArmed (false);

            auto* editor = dynamic_cast<MoonTuneAudioProcessorEditor*> (p.createEditorIfNeeded());
            expect (editor != nullptr);
            editor->setSize (1280, 760);
            editor->refreshFromProcessor();

            ScaleCorrector::Settings cs; cs.strength = 1.0f; cs.speedMs = 40.0f;
            editor->applyCorrectionForTesting (cs, false);
            auto notes = p.getNoteModel().getSnapshot();
            if (notes.size() >= 6)
            {
                notes[4].pitchShiftCents = 200.0f;     // moved by hand
                notes[4].correctionCents = 0.0f;
                notes[2].pitchModulation = 0.5f;       // calmer vibrato
                notes[5].timeShiftSeconds = 0.08;
                notes[5].eq.enabled = true;
                notes[5].eq.bands[2] = { true, EqBand::peak, 2500.0f, 5.0f, 1.2f };
                p.getHistory().perform ("Edits", notes);
                editor->getGridView().setSelection ({ notes[5].id });
            }
            setParam (p, ParamIDs::scaleEdits, (float) (1 << 10));   // custom scale: C# allowed... (A#) shown
            setParam (p, ParamIDs::gridDivision, 3.0f);              // 1/16
            {
                // A stopped host with tempo, so the musical grid and bar numbers show.
                struct TempoHead final : juce::AudioPlayHead
                {
                    juce::Optional<PositionInfo> getPosition() const override
                    {
                        PositionInfo i;
                        i.setIsPlaying (false); i.setTimeInSamples (48000); i.setTimeInSeconds (1.0);
                        i.setBpm (100.0); i.setPpqPosition (100.0 / 60.0); i.setPpqPositionOfLastBarStart (0.0);
                        i.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
                        return i;
                    }
                } tempoHead;
                p.setPlayHead (&tempoHead);
                juce::AudioBuffer<float> b (1, 512);
                juce::MidiBuffer midi;
                b.clear();
                p.processBlock (b, midi);
                p.setPlayHead (&head);
            }
            editor->refreshFromProcessor();
            expect (p.getRenderer().waitUntilRendered (20000));
            editor->refreshFromProcessor();

            juce::Image img (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
            juce::Graphics g (img);
            editor->paintEntireComponent (g, false);
            const auto outFile = juce::SystemStats::getEnvironmentVariable ("MOONTUNE_SNAPSHOT_EDIT", {});
            if (outFile.isNotEmpty())
            {
                juce::File f (outFile);
                f.deleteFile();
                juce::FileOutputStream os (f);
                juce::PNGImageFormat().writeImageToStream (img, os);
            }
            p.editorBeingDeleted (editor);
            delete editor;
            p.setPlayHead (nullptr);
            storage.deleteRecursively();
        }
    }
};

static EditingTests editingTests;
