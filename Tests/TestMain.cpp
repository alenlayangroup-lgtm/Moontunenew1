// MoonTune test runner: runs every juce::UnitTest in the "MoonTune" category.
// Exit code 0 = all passed, 1 = at least one failure.

#include <juce_core/juce_core.h>
#include <cstdlib>
#include <iostream>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

class ConsoleRunner final : public juce::UnitTestRunner
{
    void logMessage (const juce::String& message) override
    {
        std::cout << message << std::endl;
    }
};

int main()
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // message manager for editor / parameter tests

    ConsoleRunner runner;
    runner.setAssertOnFailure (false);
    // MOONTUNE_TEST_FILTER=<part of a test name> runs only the matching test groups.
    if (const char* filter = std::getenv ("MOONTUNE_TEST_FILTER"))
    {
        juce::Array<juce::UnitTest*> chosen;
        for (auto* t : juce::UnitTest::getTestsInCategory ("MoonTune"))
            if (t->getName().containsIgnoreCase (filter))
                chosen.add (t);
        runner.runTests (chosen);
    }
    else
        runner.runTestsInCategory ("MoonTune");

    int failures = 0, passes = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        failures += runner.getResult (i)->failures;
        passes   += runner.getResult (i)->passes;
    }

    std::cout << "\n==== MoonTune tests: " << passes << " checks passed, "
              << failures << " failed ====" << std::endl;
    return failures == 0 ? 0 : 1;
}
