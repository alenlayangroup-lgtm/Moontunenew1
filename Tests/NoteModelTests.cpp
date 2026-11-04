#include "Model/NoteModel.h"
#include "UI/NoteGridView.h"

#include "TestHelpers.h"
using namespace moontune;
using namespace moontune::testhelpers;

class NoteModelTests final : public juce::UnitTest
{
public:
    NoteModelTests() : juce::UnitTest ("Note model and grid coordinates", "MoonTune") {}

    void runTest() override
    {
        beginTest ("Notes are kept sorted by start time");
        {
            NoteModel m;
            m.replaceAll ({ makeNote (3, 5.0, 6.0, 60.0f, 0.0f, 0.0),
                            makeNote (1, 1.0, 2.0, 62.0f, 0.0f, 0.0),
                            makeNote (2, 3.0, 4.0, 64.0f, 0.0f, 0.0) });
            const auto s = m.getSnapshot();
            expectEquals ((int) s.size(), 3);
            expect (s[0].id == 1 && s[1].id == 2 && s[2].id == 3);
        }

        beginTest ("Large ids survive serialisation (int64)");
        {
            NoteModel a, b;
            const juce::int64 bigId = (juce::int64) 1 << 52;
            a.replaceAll ({ makeNote (bigId + 1, 0.0, 1.0, 60.0f) });
            expectEquals (b.fromValueTree (a.toValueTree()), 0);
            expect (b.getSnapshot().front().id == bigId + 1);
        }

        beginTest ("Validation");
        {
            expect (NoteModel::isValid (makeNote (1, 0.0, 0.1, 60.0f, 0.0f, 0.0)));
            expect (! NoteModel::isValid (makeNote (1, 0.0, 0.0, 60.0f, 0.0f, 0.0)));
            expect (! NoteModel::isValid (makeNote (1, -1.0, 0.1, 60.0f)));
            expect (! NoteModel::isValid (makeNote (1, 0.0, 0.1, std::nanf (""))));
            expect (! NoteModel::isValid (makeNote (1, 0.0, 0.1, 60.0f, 2401.0f, 0.0)));
        }

        beginTest ("Grid coordinate mapping is invertible");
        {
            NoteModel m;
            NoteGridView grid (m);
            grid.setBounds (0, 0, 800, 600);
            NoteGridView::View v;
            v.startSeconds = 12.5; v.pixelsPerSecond = 200.0; v.topPitch = 80.0f; v.pixelsPerSemitone = 20.0f;
            grid.setView (v);
            for (double t : { 12.5, 13.0, 15.75 })
                expectWithinAbsoluteError (grid.xToSeconds (grid.secondsToX (t)), t, 1.0e-6);
            for (float p : { 80.0f, 70.25f, 60.0f })
                expectWithinAbsoluteError (grid.yToPitch (grid.pitchToY (p)), p, 1.0e-4f);
            expectWithinAbsoluteError (grid.pitchToY (80.0f), (float) NoteGridView::timeRulerHeight, 1.0e-4f);
        }

        beginTest ("Grid view is clamped to the supported pitch range");
        {
            NoteModel m;
            NoteGridView grid (m);
            grid.setBounds (0, 0, 800, 600);
            NoteGridView::View v;
            v.topPitch = 500.0f;
            v.startSeconds = -10.0;
            grid.setView (v);
            expect (grid.getView().topPitch <= NoteGridView::highestPitch + 1.0f);
            expectEquals (grid.getView().startSeconds, 0.0);
        }
    }
};

static NoteModelTests noteModelTests;
