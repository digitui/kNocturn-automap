#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include "NocturnLink.h"
#include "PluginCatalog.h"

class NocturnAutomapProcessor;

/** Exposes one parameter of the hosted plug-in to the DAW (automation, Live device view). */
class ProxyParameter : public juce::AudioProcessorParameter
{
public:
    ProxyParameter (NocturnAutomapProcessor& o, int i) : owner (o), index (i) {}
    float getValue() const override;
    void setValue (float) override;
    float getDefaultValue() const override;
    juce::String getName (int maxLen) const override;
    juce::String getLabel() const override;
    juce::String getText (float, int) const override;
    float getValueForText (const juce::String&) const override;
    bool isAutomatable() const override { return true; }
private:
    NocturnAutomapProcessor& owner;
    const int index;
    float cached = 0.0f;
};

/** Set by the Live remote script on the selected wrapper to give it the Nocturn. */
class FocusParameter : public juce::AudioProcessorParameter
{
public:
    explicit FocusParameter (std::atomic<bool>& f) : flag (f) {}
    float getValue() const override { return value; }
    void setValue (float v) override { const bool on = v > 0.5f; if (on && value <= 0.5f) flag = true; value = v; }
    float getDefaultValue() const override { return 0.0f; }
    juce::String getName (int maxLen) const override { return juce::String ("Automap Focus").substring (0, maxLen); }
    juce::String getLabel() const override { return {}; }
    juce::String getText (float v, int) const override { return v > 0.5f ? "On" : "Off"; }
    float getValueForText (const juce::String& t) const override { return t.equalsIgnoreCase ("On") ? 1.0f : 0.0f; }
    int getNumSteps() const override { return 2; }
    bool isDiscrete() const override { return true; }
    bool isBoolean() const override { return true; }
private:
    std::atomic<bool>& flag;
    float value = 0.0f;
};

class NocturnAutomapProcessor : public juce::AudioProcessor,
                                public NocturnClient,
                                private juce::AudioProcessorListener,
                                private juce::Timer
{
public:
    static constexpr int numProxies = 127;   // + focus = 128 parameters (Live shows them all)
    static constexpr int slotsPerPage = 8;
    static constexpr int xfaderSlot = slotsPerPage;

    NocturnAutomapProcessor();
    ~NocturnAutomapProcessor() override;

    // AudioProcessor
    void prepareToPlay (double, int) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    double getTailLengthSeconds() const override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // hosting
    bool loadPlugin (const juce::PluginDescription&, juce::String& error);
    void unloadPlugin();
    juce::AudioPluginInstance* getHosted() const noexcept { return current.load(); }
    juce::AudioProcessorParameter* getHostedParam (int index) const;
    /** Proxy i (DAW-visible) -> hosted parameter index (the i-th automatable one), or -1 */
    int proxyTarget (int proxy) const noexcept { return proxyTargets[(size_t) proxy].load(); }
    juce::String getHostedName() const;
    juce::ChangeBroadcaster hostedChanged;   // editor rebuilds hosted UI
    juce::ChangeBroadcaster displayChanged;  // page/learn/touch changed

    // Automap model
    struct SlotInfo { bool mapped = false; juce::String name, value; float norm = 0.0f; };
    SlotInfo getSlotInfo (int slot) const;      // slot 0..7, or xfaderSlot
    int getPage() const noexcept { return page; }
    int getNumPages() const;
    void setPage (int);
    bool isLearnArmed() const noexcept { return learnArmed; }
    void setLearnArmed (bool);
    int getLearnSlot() const noexcept { return learnSlot; }
    void setLearnSlot (int s) { learnSlot = s; displayChanged.sendChangeMessage(); }
    int getTouchedSlot() const noexcept { return touchedSlot; }
    void resetMapping();                         // back to "all parameters in order"
    void claimNocturn()                          { link->setFocus (this, true); }
    void releaseNocturn()                        { link->releaseToDaw(); }
    bool hasNocturnFocus() const                 { return link->hasFocus (this); }
    NocturnLink& getLink()                       { return *link; }
    PluginCatalog& getCatalog()                  { return *catalog; }

    // NocturnClient
    void nocturnControl (int cc, int value) override;
    void nocturnFocusChanged (bool) override;
    void nocturnRepaintLeds() override;
    juce::String nocturnClientName() const override { return getHostedName(); }

private:
    // AudioProcessorListener (hosted plug-in)
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override;
    void audioProcessorChanged (juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails&) override;
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int) override;
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int) override;
    void timerCallback() override;

    static BusesProperties makeBuses();
    int paramForSlot (int slot) const;
    void nudgeSlot (int slot, double delta);
    void beginSlotGesture (int slot);
    void endSlotGesture (int slot);
    void setHostedParamFromNocturn (int paramIndex, float v);
    void prepareHosted (juce::AudioPluginInstance&);
    void configureBuses (juce::AudioPluginInstance&);
    void loadDefaultMapping();
    void saveMappingFile() const;
    bool loadMappingFile();
    juce::File mappingFile() const;
    void sendLeds (bool force);
    void handleProgramStep (int step);

    juce::SharedResourcePointer<NocturnLink> link;
    juce::SharedResourcePointer<PluginCatalog> catalog;

    std::unique_ptr<juce::AudioPluginInstance> owned;
    std::atomic<juce::AudioPluginInstance*> current { nullptr };
    std::vector<std::pair<juce::uint32, std::unique_ptr<juce::AudioPluginInstance>>> graveyard;
    juce::PluginDescription hostedDesc;
    juce::AudioBuffer<float> work;
    double sampleRate = 44100.0;
    int blockSize = 512;
    bool prepared = false;
    std::atomic<bool> bypassHosted { false };

    juce::Array<ProxyParameter*> proxies;
    std::array<std::atomic<int>, numProxies> proxyTargets;
    std::vector<int> hostToProxy;
    void buildProxyMap (juce::AudioPluginInstance&);
    std::atomic<bool> focusRequested { false };

    // mapping: slot index (page*8+slot) -> hosted parameter index (-1 = empty)
    std::vector<int> mapping;
    bool customMapping = false;
    int xfaderParam = -1;
    int page = 0;
    bool learnArmed = false;
    int learnSlot = -1;
    int touchedSlot = -1;
    int lastSlot = 0;
    std::atomic<int> lastUiParam { -1 };
    bool writingFromNocturn = false;
    double accum[slotsPerPage + 1] {};
    juce::uint32 lastMove[slotsPerPage + 1] {};
    bool gestureOpen[slotsPerPage + 1] {};
    bool touchHeld[slotsPerPage + 1] {};
    int ledCache[128];
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NocturnAutomapProcessor)
};
