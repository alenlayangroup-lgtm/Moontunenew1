#include "Music/KeyScale.h"
#include "UI/NoteGridView.h"
#include "UI/MoonLookAndFeel.h"
#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>
#include <limits>

#include "TestHelpers.h"
using namespace moontune;
using namespace moontune::testhelpers;

namespace
{
    juce::uint16 maskOf (std::initializer_list<int> pcs)
    {
        juce::uint16 m = 0;
        for (int pc : pcs) m = (juce::uint16) (m | (1u << pc));
        return m;
    }

    juce::uint16 rotate (juce::uint16 m, int by)
    {
        juce::uint16 r = 0;
        for (int pc = 0; pc < 12; ++pc)
            if ((m >> pc) & 1u) r = (juce::uint16) (r | (1u << ((pc + by) % 12)));
        return r;
    }

    void setParam (MoonTuneAudioProcessor& p, const juce::ParameterID& id, float plainValue)
    {
        auto* param = p.getParameterState().getParameter (id.getParamID());
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }
}

class KeyScaleTests final : public juce::UnitTest
{
public:
    KeyScaleTests() : juce::UnitTest ("Key / scale logic", "MoonTune") {}

    void runTest() override
    {
        beginTest ("Scale definitions (root C)");
        {
            //                                         C  D  E  F  G  A  B
            expectEquals ((int) KeyScale (0, ScaleType::Major).getPitchClassMask(),         (int) maskOf ({ 0, 2, 4, 5, 7, 9, 11 }));
            expectEquals ((int) KeyScale (0, ScaleType::NaturalMinor).getPitchClassMask(),  (int) maskOf ({ 0, 2, 3, 5, 7, 8, 10 }));
            expectEquals ((int) KeyScale (0, ScaleType::HarmonicMinor).getPitchClassMask(), (int) maskOf ({ 0, 2, 3, 5, 7, 8, 11 }));
            expectEquals ((int) KeyScale (0, ScaleType::MelodicMinor).getPitchClassMask(),  (int) maskOf ({ 0, 2, 3, 5, 7, 9, 11 }));
            expectEquals ((int) KeyScale (0, ScaleType::Dorian).getPitchClassMask(),        (int) maskOf ({ 0, 2, 3, 5, 7, 9, 10 }));
            expectEquals ((int) KeyScale (0, ScaleType::Phrygian).getPitchClassMask(),      (int) maskOf ({ 0, 1, 3, 5, 7, 8, 10 }));
            expectEquals ((int) KeyScale (0, ScaleType::Lydian).getPitchClassMask(),        (int) maskOf ({ 0, 2, 4, 6, 7, 9, 11 }));
            expectEquals ((int) KeyScale (0, ScaleType::Mixolydian).getPitchClassMask(),    (int) maskOf ({ 0, 2, 4, 5, 7, 9, 10 }));
            expectEquals ((int) KeyScale (0, ScaleType::Locrian).getPitchClassMask(),       (int) maskOf ({ 0, 1, 3, 5, 6, 8, 10 }));
            expectEquals ((int) KeyScale (5, ScaleType::Chromatic).getPitchClassMask(), 0x0fff);
        }

        beginTest ("Church modes of C major share its notes; harmonic minor has a raised 7th");
        {
            const auto cMajor = KeyScale (0, ScaleType::Major).getPitchClassMask();
            expectEquals ((int) KeyScale (2,  ScaleType::Dorian).getPitchClassMask(),       (int) cMajor);
            expectEquals ((int) KeyScale (4,  ScaleType::Phrygian).getPitchClassMask(),     (int) cMajor);
            expectEquals ((int) KeyScale (5,  ScaleType::Lydian).getPitchClassMask(),       (int) cMajor);
            expectEquals ((int) KeyScale (7,  ScaleType::Mixolydian).getPitchClassMask(),   (int) cMajor);
            expectEquals ((int) KeyScale (9,  ScaleType::NaturalMinor).getPitchClassMask(), (int) cMajor);
            expectEquals ((int) KeyScale (11, ScaleType::Locrian).getPitchClassMask(),      (int) cMajor);

            const KeyScale aHarm (9, ScaleType::HarmonicMinor);
            expect (aHarm.containsPitchClass (8));    // G#
            expect (! aHarm.containsPitchClass (7));  // G
        }

        beginTest ("All 12 roots are correct transpositions; root wraps");
        {
            for (int t = 0; t < (int) ScaleType::numTypes; ++t)
                for (int root = 0; root < 12; ++root)
                {
                    const auto type = (ScaleType) t;
                    expectEquals ((int) KeyScale (root, type).getPitchClassMask(),
                                  (int) rotate (KeyScale (0, type).getPitchClassMask(), root),
                                  KeyScale (root, type).getName());
                    expect (KeyScale (root, type).containsPitchClass (root));   // root always in scale
                }

            expectEquals (KeyScale (14, ScaleType::Major).getRoot(), 2);
            expectEquals (KeyScale (-1, ScaleType::Major).getRoot(), 11);
            expect (KeyScale (0, (ScaleType) 99).isChromatic());   // invalid type -> safe default
            expect (KeyScale (0, ScaleType::Major).containsMidi (-12));
        }

        beginTest ("Nearest scale note, ties resolved by drag direction");
        {
            const KeyScale c (0, ScaleType::Major);
            expectEquals (c.nearestScaleNote (61.0f), 60);       // C#: tie C/D -> lower
            expectEquals (c.nearestScaleNote (61.0f, +1), 62);   // dragging up -> D
            expectEquals (c.nearestScaleNote (61.0f, -1), 60);
            expectEquals (c.nearestScaleNote (61.2f), 62);
            expectEquals (c.nearestScaleNote (60.8f), 60);
            expectEquals (c.nearestScaleNote (64.4f), 64);       // E
            expectEquals (c.nearestScaleNote (64.6f), 65);       // F (E-F is a semitone)
            expectEquals (c.nearestScaleNote (65.6f), 65);       // F# region -> F closer than G

            const KeyScale chrom;
            expectEquals (chrom.nearestScaleNote (61.4f), 61);
            expectEquals (chrom.nearestScaleNote (61.6f), 62);
            expectEquals (chrom.nearestScaleNote (std::nanf ("")), 60);

            // Exhaustive: result is always in scale and never further than 2 semitones.
            for (int t = 0; t < (int) ScaleType::numTypes; ++t)
                for (int root = 0; root < 12; ++root)
                {
                    const KeyScale ks (root, (ScaleType) t);
                    for (float p = 20.0f; p < 110.0f; p += 0.07f)
                    {
                        const int n = ks.nearestScaleNote (p);
                        if (! ks.containsMidi (n) || std::abs ((float) n - p) > 2.0f)
                        {
                            expect (false, ks.getName() + " p=" + juce::String (p));
                            break;
                        }
                    }
                }
        }

        beginTest ("Snap strength, semitones and cents");
        {
            const KeyScale c (0, ScaleType::Major);

            auto r = c.snap (61.3f, 1.0f);
            expectEquals (r.targetNote, 62);
            expectEquals (r.pitch, 62.0f);
            expectWithinAbsoluteError (r.correctionCents, 70.0f, 0.01f);

            r = c.snap (61.3f, 0.0f);                       // strength 0: untouched
            expectEquals (r.pitch, 61.3f);
            expectEquals (r.correctionCents, 0.0f);

            r = c.snap (61.3f, 0.5f);                       // proportional pull
            expectWithinAbsoluteError (r.pitch, 61.65f, 1.0e-4f);
            expectWithinAbsoluteError (r.correctionCents, 35.0f, 0.01f);

            r = c.snap (64.0f, 1.0f);                       // already in scale: no change
            expectEquals (r.correctionCents, 0.0f);

            r = c.snap (59.0f, 1.0f);                       // B is in C major
            expectEquals (r.pitch, 59.0f);

            r = c.snap (61.3f, 7.0f);                       // strength clamped
            expectEquals (r.pitch, 62.0f);
            r = c.snap (61.3f, std::nanf (""));
            expectEquals (r.pitch, 61.3f);
            r = c.snap (-std::numeric_limits<float>::infinity(), 1.0f);
            expect (std::isfinite (r.pitch));

            r = c.snap (62.4f, 1.0f);                       // below a scale note: negative cents
            expectWithinAbsoluteError (r.correctionCents, -40.0f, 0.01f);
        }

        beginTest ("Names match parameter choices");
        {
            expectEquals (KeyScale (2, ScaleType::Dorian).getName(), juce::String ("D Dorian"));
            expectEquals (KeyScale (6, ScaleType::HarmonicMinor).getName(), juce::String ("F# Harmonic Minor"));
            expectEquals (KeyScale (3, ScaleType::Chromatic).getName(), juce::String ("Chromatic"));

            MoonTuneAudioProcessor p;
            auto* scaleParam = dynamic_cast<juce::AudioParameterChoice*> (
                p.getParameterState().getParameter (ParamIDs::scaleType.getParamID()));
            auto* keyParam = dynamic_cast<juce::AudioParameterChoice*> (
                p.getParameterState().getParameter (ParamIDs::keyRoot.getParamID()));
            expect (scaleParam != nullptr && keyParam != nullptr);
            expectEquals (scaleParam->choices.size(), (int) ScaleType::numTypes);
            expectEquals (keyParam->choices.size(), 12);
            expect (! scaleParam->isAutomatable());
        }

        beginTest ("Processor defaults and key/scale persistence");
        {
            MoonTuneAudioProcessor a;
            expect (a.getKeyScale().isChromatic());          // default: no restriction
            expect (! a.isSnapToScaleEnabled());             // default: no forced correction

            setParam (a, ParamIDs::keyRoot, 9.0f);
            setParam (a, ParamIDs::scaleType, (float) ScaleType::HarmonicMinor);
            setParam (a, ParamIDs::snapToScale, 1.0f);
            setParam (a, ParamIDs::snapStrength, 60.0f);
            setParam (a, ParamIDs::scaleHighlight, 20.0f);
            expect (a.getKeyScale() == KeyScale (9, ScaleType::HarmonicMinor));
            expectWithinAbsoluteError (a.getSnapStrength(), 0.6f, 1.0e-4f);

            juce::MemoryBlock state;
            a.getStateInformation (state);
            MoonTuneAudioProcessor b;
            b.setStateInformation (state.getData(), (int) state.getSize());
            expect (b.getKeyScale() == KeyScale (9, ScaleType::HarmonicMinor));
            expect (b.isSnapToScaleEnabled());
            expectWithinAbsoluteError (b.getSnapStrength(), 0.6f, 1.0e-4f);
        }

        beginTest ("Key/scale settings never change audio");
        {
            MoonTuneAudioProcessor p;
            setParam (p, ParamIDs::keyRoot, 4.0f);
            setParam (p, ParamIDs::scaleType, (float) ScaleType::Phrygian);
            setParam (p, ParamIDs::snapToScale, 1.0f);
            p.prepareToPlay (48000.0, 256);
            juce::AudioBuffer<float> b (1, 256), ref (1, 256);
            juce::Random rng (7);
            juce::MidiBuffer midi;
            for (int k = 0; k < 50; ++k)
            {
                for (int i = 0; i < 256; ++i) b.setSample (0, i, rng.nextFloat() * 2.0f - 1.0f);
                ref.makeCopyOf (b);
                if (k == 20) setParam (p, ParamIDs::scaleType, (float) ScaleType::Major);
                p.processBlock (b, midi);
                for (int i = 0; i < 256; ++i)
                    if (! juce::exactlyEqual (b.getSample (0, i), ref.getSample (0, i)))
                    {
                        expect (false, "audio changed");
                        return;
                    }
            }
        }

        beginTest ("Grid highlighting: display only, configurable, out-of-scale visible");
        {
            NoteModel model;
            model.replaceAll ({ makeNote (1, 0.1, 0.6, 60.0f, 0.0f, 0.0),     // C  - in C major
                                makeNote (2, 0.7, 1.2, 61.0f, 0.0f, 0.0) });  // C# - out of scale
            NoteGridView grid (model);
            grid.setBounds (0, 0, 800, 500);

            const auto pianoWhite = grid.laneColour (62), pianoBlack = grid.laneColour (61);
            grid.setScaleDisplay (KeyScale (0, ScaleType::Major), 1.0f);
            expect (grid.scaleHighlightActive());
            expect (grid.laneColour (62) != grid.laneColour (61));       // in vs out differ
            expect (grid.laneColour (60) != grid.laneColour (62));       // root marked
            expect (grid.laneColour (61).getBrightness() > 0.0f);        // out-of-scale not hidden

            grid.setScaleDisplay (KeyScale (0, ScaleType::Major), 0.0f); // highlight off
            expect (! grid.scaleHighlightActive());
            expect (grid.laneColour (62) == pianoWhite && grid.laneColour (61) == pianoBlack);

            grid.setScaleDisplay (KeyScale (0, ScaleType::Chromatic), 1.0f);
            expect (! grid.scaleHighlightActive());

            // Changing the key does not touch the notes.
            const auto before = model.getSnapshot();
            grid.setScaleDisplay (KeyScale (7, ScaleType::Mixolydian), 0.7f);
            expect (model.getSnapshot() == before);

            juce::Image img (juce::Image::ARGB, 800, 500, true);
            juce::Graphics g (img);
            grid.paintEntireComponent (g, false);   // exercise in-/out-of-scale note drawing
        }

        beginTest ("Editor follows key/scale parameters");
        {
            MoonTuneAudioProcessor p;
            MoonTuneAudioProcessorEditor ed (p);
            expect (ed.getGridView().getScale().isChromatic());
            setParam (p, ParamIDs::keyRoot, 2.0f);
            setParam (p, ParamIDs::scaleType, (float) ScaleType::Dorian);
            // ParameterAttachment delivers synchronously on the message thread.
            expect (ed.getGridView().getScale() == KeyScale (2, ScaleType::Dorian));

            const auto snapshot = juce::SystemStats::getEnvironmentVariable ("MOONTUNE_SNAPSHOT_SCALE", {});
            if (snapshot.isNotEmpty())
            {
                p.getNoteModel().replaceAll ({ makeNote (1, 0.3, 0.9, 62.0f, 0.0f, 0.0),
                                               makeNote (2, 1.0, 1.6, 64.0f, 0.0f, 0.0),
                                               makeNote (3, 1.7, 2.1, 66.0f, 0.0f, 0.0),     // F#: out of D Dorian
                                               makeNote (4, 2.2, 3.2, 65.0f, 0.0f, 0.0),
                                               makeNote (5, 3.3, 4.4, 69.0f, 0.0f, 0.0) });
                ed.getGridView().refreshNotes();
                NoteGridView::View v; v.topPitch = 76.5f; v.pixelsPerSemitone = 16.0f; v.pixelsPerSecond = 160.0;
                ed.getGridView().setView (v);
                juce::Image img (juce::Image::ARGB, ed.getWidth(), ed.getHeight(), true);
                juce::Graphics g (img);
                ed.paintEntireComponent (g, false);
                juce::File f (snapshot);
                f.deleteFile();
                juce::FileOutputStream os (f);
                juce::PNGImageFormat().writeImageToStream (img, os);
            }
            p.editorBeingDeleted (&ed);
        }
    }
};

static KeyScaleTests keyScaleTests;
