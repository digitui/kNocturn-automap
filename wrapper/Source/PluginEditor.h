#pragma once
#include "PluginProcessor.h"
#include <juce_gui_basics/juce_gui_basics.h>

/** Scans installed AU / VST3 plug-ins a few files per tick so the UI stays alive. */
class PluginScanner : private juce::Timer
{
public:
    explicit PluginScanner (PluginCatalog& c) : catalog (c) {}
    void start();
    bool isScanning() const noexcept { return scanning; }
    juce::String getStatus() const { return status; }
    std::function<void()> onFinished, onProgress;
private:
    void timerCallback() override;
    PluginCatalog& catalog;
    std::unique_ptr<juce::PluginDirectoryScanner> scanner;
    int formatIndex = 0;
    bool scanning = false;
    juce::String status;
};

class SlotDisplay : public juce::Component
{
public:
    void set (const NocturnAutomapProcessor::SlotInfo& i, int number, bool touched, bool learnTarget);
    void paint (juce::Graphics&) override;
    std::function<void()> onClick;
    void mouseDown (const juce::MouseEvent&) override { if (onClick) onClick(); }
private:
    NocturnAutomapProcessor::SlotInfo info;
    juce::String caption;
    bool touched = false, learnTarget = false;
};

class NocturnAutomapEditor : public juce::AudioProcessorEditor,
                             private juce::ChangeListener,
                             private juce::ComponentListener,
                             private juce::Timer
{
public:
    explicit NocturnAutomapEditor (NocturnAutomapProcessor&);
    ~NocturnAutomapEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    static constexpr int headerHeight = 124;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override;
    void timerCallback() override;
    void rebuildHostedEditor();
    void refresh();
    void showPluginMenu();
    void updateSize();

    NocturnAutomapProcessor& proc;
    PluginScanner scanner;
    std::unique_ptr<juce::AudioProcessorEditor> hostedEditor;

    juce::TextButton loadButton { "Load plug-in..." }, learnButton { "Learn" },
                     prevPage { "<" }, nextPage { ">" }, focusButton { "Take Nocturn" },
                     menuButton { "..." };
    juce::Label pluginName, pageLabel, statusLabel;
    SlotDisplay slots[NocturnAutomapProcessor::slotsPerPage + 1];

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NocturnAutomapEditor)
};
