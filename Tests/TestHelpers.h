#pragma once

#include "Model/NoteModel.h"

namespace moontune::testhelpers
{
    // Named construction of a note (the Note struct has many edit fields).
    inline Note makeNote (juce::int64 id, double start, double end, float pitch,
                          float pitchShiftCents = 0.0f, double timeShiftSeconds = 0.0)
    {
        Note n;
        n.id = id;
        n.startSeconds = start;
        n.endSeconds = end;
        n.detectedPitch = pitch;
        n.pitchShiftCents = pitchShiftCents;
        n.timeShiftSeconds = timeShiftSeconds;
        return n;
    }
}
