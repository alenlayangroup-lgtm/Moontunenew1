#include "LevelMeter.h"
#include "MoonLookAndFeel.h"

namespace moontune
{

namespace
{
    constexpr float minDb = -60.0f, maxDb = 6.0f;
    constexpr int refreshHz = 30;
    constexpr float fallDbPerFrame = 24.0f / refreshHz;   // 24 dB/s release
    constexpr int holdFramesTotal = refreshHz * 1;         // 1 s peak hold
}

LevelMeter::LevelMeter (std::function<float()> peakSource) : source (std::move (peakSource))
{
    setOpaque (true);
    startTimerHz (refreshHz);
}

LevelMeter::~LevelMeter() { stopTimer(); }

void LevelMeter::timerCallback()
{
    const float peak = source ? source() : 0.0f;
    const float db = juce::Decibels::gainToDecibels (peak, -100.0f);

    displayDb = db > displayDb ? db : juce::jmax (db, displayDb - fallDbPerFrame);

    if (db >= holdDb)       { holdDb = db; holdFrames = holdFramesTotal; }
    else if (--holdFrames <= 0) holdDb = juce::jmax (displayDb, holdDb - fallDbPerFrame);

    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.fillAll (Colours::panelLight);

    auto toX = [&] (float db)
    {
        return bounds.getX() + bounds.getWidth() * juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
    };

    const float x = toX (displayDb);
    const float zeroX = toX (0.0f);

    g.setColour (Colours::meterGreen);
    g.fillRect (bounds.withRight (juce::jmin (x, zeroX)));
    if (x > zeroX)
    {
        g.setColour (Colours::meterRed);
        g.fillRect (juce::Rectangle<float> (zeroX, bounds.getY(), x - zeroX, bounds.getHeight()));
    }

    if (holdDb > minDb)
    {
        g.setColour (holdDb > 0.0f ? Colours::meterRed : Colours::text);
        g.fillRect (juce::Rectangle<float> (toX (holdDb) - 1.0f, bounds.getY(), 2.0f, bounds.getHeight()));
    }

    g.setColour (Colours::background.withAlpha (0.6f));
    g.fillRect (juce::Rectangle<float> (zeroX, bounds.getY(), 1.0f, bounds.getHeight()));
}

} // namespace moontune
