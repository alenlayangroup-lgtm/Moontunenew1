#include "PluginProcessor.h"
#include "PluginEditor.h"

#include "TestHelpers.h"
using namespace moontune;
using namespace moontune::testhelpers;

namespace
{
    float plain (MoonTuneAudioProcessor& p, const juce::ParameterID& id)
    {
        return p.getParameterState().getRawParameterValue (id.getParamID())->load();
    }

    void setParam (MoonTuneAudioProcessor& p, const juce::ParameterID& id, float plainValue)
    {
        auto* param = p.getParameterState().getParameter (id.getParamID());
        param->setValueNotifyingHost (param->convertTo0to1 (plainValue));
    }

    juce::MemoryBlock xmlState (const juce::String& xmlText)
    {
        juce::MemoryBlock mb;
        if (auto xml = juce::parseXML (xmlText))
            juce::AudioProcessor::copyXmlToBinary (*xml, mb);
        return mb;
    }
}

class StateTests final : public juce::UnitTest
{
public:
    StateTests() : juce::UnitTest ("Project state save / restore", "MoonTune") {}

    void runTest() override
    {
        beginTest ("Round trip: parameters, notes and view state");
        {
            MoonTuneAudioProcessor a;
            setParam (a, ParamIDs::outputGain, -7.5f);
            setParam (a, ParamIDs::bypass, 1.0f);
            a.getNoteModel().replaceAll ({ makeNote (1, 0.5, 1.25, 57.3f, 35.0f, 0.01),
                                           makeNote (2, 1.30, 2.0, 59.9f, -12.0f, 0.0) });
            a.setUiProperty ("viewPixelsPerSecond", 321.0);
            a.setUiProperty ("editorWidth", 1234);

            juce::MemoryBlock state;
            a.getStateInformation (state);
            expect (state.getSize() > 0);

            MoonTuneAudioProcessor b;
            b.setStateInformation (state.getData(), (int) state.getSize());
            expectWithinAbsoluteError (plain (b, ParamIDs::outputGain), -7.5f, 0.005f);
            expectEquals (plain (b, ParamIDs::bypass), 1.0f);
            expect (b.getNoteModel().getSnapshot() == a.getNoteModel().getSnapshot());
            expectEquals ((double) b.getUiProperty ("viewPixelsPerSecond", 0.0), 321.0);
            expectEquals ((int) b.getUiProperty ("editorWidth", 0), 1234);

            // Second round trip produces identical bytes (state is stable).
            juce::MemoryBlock state2;
            b.getStateInformation (state2);
            expect (state == state2);
        }

        beginTest ("Garbage / empty / foreign data leaves current state untouched");
        {
            MoonTuneAudioProcessor p;
            setParam (p, ParamIDs::outputGain, 4.0f);
            p.getNoteModel().replaceAll ({ makeNote (7, 0.0, 1.0, 60.0f, 0.0f, 0.0) });

            const char garbage[] = "this is not a plugin state \x01\x02\x03";
            p.setStateInformation (garbage, (int) sizeof (garbage));
            p.setStateInformation (nullptr, 0);
            p.setStateInformation (garbage, 0);
            const auto foreign = xmlState ("<SomeOtherPlugin version=\"3\"><PARAM id=\"outputGain\" value=\"-20\"/></SomeOtherPlugin>");
            p.setStateInformation (foreign.getData(), (int) foreign.getSize());
            const auto noVersion = xmlState ("<MoonTuneState><Parameters/></MoonTuneState>");
            p.setStateInformation (noVersion.getData(), (int) noVersion.getSize());

            // Truncated real state
            juce::MemoryBlock good;
            p.getStateInformation (good);
            p.setStateInformation (good.getData(), (int) good.getSize() / 2);

            expectWithinAbsoluteError (plain (p, ParamIDs::outputGain), 4.0f, 0.005f);
            expectEquals (p.getNoteModel().size(), 1);
            expect (p.getLastStateMessage().isNotEmpty());
        }

        beginTest ("Missing parameters fall back to defaults, out-of-range values are clamped");
        {
            MoonTuneAudioProcessor p;
            setParam (p, ParamIDs::bypass, 1.0f);
            setParam (p, ParamIDs::outputGain, 5.0f);

            const auto s = xmlState (R"(<MoonTuneState version="1">
                <Parameters><PARAM id="outputGain" value="999"/></Parameters>
              </MoonTuneState>)");
            p.setStateInformation (s.getData(), (int) s.getSize());

            expectWithinAbsoluteError (plain (p, ParamIDs::outputGain), 12.0f, 0.005f);  // clamped to max
            expectEquals (plain (p, ParamIDs::bypass), 0.0f);                              // missing -> default
            // every parameter except outputGain was missing
            const int expectedMissing = p.getParameters().size() - 1;
            expect (p.getLastStateMessage().contains (juce::String (expectedMissing) + " parameter"),
                    p.getLastStateMessage());

            const auto nan = xmlState (R"(<MoonTuneState version="1"><Parameters>
                <PARAM id="outputGain" value="nan"/></Parameters></MoonTuneState>)");
            p.setStateInformation (nan.getData(), (int) nan.getSize());
            expect (std::isfinite (plain (p, ParamIDs::outputGain)));
        }

        beginTest ("State from a newer version: known parts load, unknown parts ignored");
        {
            MoonTuneAudioProcessor p;
            const auto s = xmlState (R"(<MoonTuneState version="99" appVersion="9.0.0">
                <Parameters>
                  <PARAM id="outputGain" value="-3"/>
                  <PARAM id="someFutureParam" value="0.7"/>
                </Parameters>
                <Notes>
                  <Note id="5" start="1.0" end="2.0" pitch="62" pitchShiftCents="10" timeShift="0" futureField="x"/>
                  <FutureThing/>
                </Notes>
                <FutureSection a="b"/>
              </MoonTuneState>)");
            p.setStateInformation (s.getData(), (int) s.getSize());
            expectWithinAbsoluteError (plain (p, ParamIDs::outputGain), -3.0f, 0.005f);
            expectEquals (p.getNoteModel().size(), 1);
            expect (p.getLastStateMessage().contains ("newer"));
        }

        beginTest ("Invalid notes are skipped, valid ones kept");
        {
            MoonTuneAudioProcessor p;
            const auto s = xmlState (R"(<MoonTuneState version="1"><Notes>
                <Note id="1" start="0" end="1" pitch="60"/>
                <Note id="2" start="2" end="1" pitch="60"/>
                <Note id="3" start="0" end="1" pitch="300"/>
                <Note id="1" start="3" end="4" pitch="60"/>
                <Note id="0" start="3" end="4" pitch="60"/>
                <Note id="4" start="5" end="6" pitch="61" pitchShiftCents="99999"/>
                <Note id="6" start="7" end="8" pitch="64"/>
              </Notes></MoonTuneState>)");
            p.setStateInformation (s.getData(), (int) s.getSize());
            expectEquals (p.getNoteModel().size(), 2);
            expect (p.getLastStateMessage().contains ("5 invalid"));
        }

        beginTest ("Editor restores saved size and view, clamps invalid view values");
        {
            MoonTuneAudioProcessor p;
            p.setUiProperty ("editorWidth", 1300);
            p.setUiProperty ("editorHeight", 600);
            p.setUiProperty ("viewPixelsPerSecond", 250.0);
            p.setUiProperty ("viewPixelsPerSemitone", 1.0e9);   // absurd -> clamped
            {
                MoonTuneAudioProcessorEditor ed (p);
                expectEquals (ed.getWidth(), 1300);
                expectEquals (ed.getHeight(), 600);
                expectEquals (ed.getGridView().getView().pixelsPerSecond, 250.0);
                expectEquals (ed.getGridView().getView().pixelsPerSemitone, NoteGridView::maxPixelsPerSemitone);
                p.editorBeingDeleted (&ed);
            }

            p.setUiProperty ("editorWidth", 5);   // below minimum
            MoonTuneAudioProcessorEditor ed2 (p);
            expectEquals (ed2.getWidth(), MoonTuneAudioProcessorEditor::minWidth);
            p.editorBeingDeleted (&ed2);
        }
    }
};

static StateTests stateTests;
