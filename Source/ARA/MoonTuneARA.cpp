#include "MoonTuneARA.h"

#if JucePlugin_Enable_ARA
const ARA::ARAFactory* JUCE_CALLTYPE createARAFactory()
{
    return juce::ARADocumentControllerSpecialisation::createARAFactory<moontune::MoonTuneDocumentController>();
}
#endif
