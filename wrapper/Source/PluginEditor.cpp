#include "PluginEditor.h"

using namespace juce;

namespace Colours_
{
    const Colour bg      { 0xff1d1f24 };
    const Colour panel   { 0xff2a2d34 };
    const Colour accent  { 0xffff7a1a };   // Novation orange
    const Colour learn   { 0xffe0405a };
    const Colour text    { 0xffe8e8ea };
    const Colour dim     { 0xff8b8f99 };
}

//============================================================================== scanner
void PluginScanner::start()
{
    if (scanning) return;
    scanning = true;
    formatIndex = -1;
    scanner.reset();
    status = "Scanning...";
    startTimer (1);
}

void PluginScanner::timerCallback()
{
    for (int burst = 0; burst < 3; ++burst)
    {
        if (scanner == nullptr)
        {
            if (++formatIndex >= catalog.formats.getNumFormats())
            {
                stopTimer();
                scanning = false;
                catalog.save();
                status = String (catalog.list.getNumTypes()) + " plug-ins found";
                if (onFinished) onFinished();
                return;
            }
            auto* f = catalog.formats.getFormat (formatIndex);
            scanner = std::make_unique<PluginDirectoryScanner> (catalog.list, *f, f->getDefaultLocationsToSearch(),
                                                                true, PluginCatalog::deadMansPedal(), false);
        }
        String name;
        const bool more = scanner->scanNextFile (true, name);
        status = "Scanning " + catalog.formats.getFormat (formatIndex)->getName() + ": "
                 + name.fromLastOccurrenceOf ("/", false, false);
        if (! more) scanner.reset();
    }
    if (onProgress) onProgress();
}

//============================================================================== slot display
void SlotDisplay::set (const NocturnAutomapProcessor::SlotInfo& i, int number, bool t, bool l)
{
    info = i;
    caption = number > 0 ? String (number) : String ("X-fade");
    touched = t;
    learnTarget = l;
    repaint();
}

void SlotDisplay::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    g.setColour (learnTarget ? Colours_::learn.withAlpha (0.35f) : Colours_::panel);
    g.fillRoundedRectangle (r, 5.0f);
    if (touched || learnTarget)
    {
        g.setColour (learnTarget ? Colours_::learn : Colours_::accent);
        g.drawRoundedRectangle (r, 5.0f, 1.5f);
    }

    // ring-style value bar
    auto bar = r.removeFromBottom (5.0f).reduced (6.0f, 0.0f);
    g.setColour (Colours_::bg);
    g.fillRoundedRectangle (bar, 2.0f);
    if (info.mapped)
    {
        g.setColour (Colours_::accent);
        g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * jlimit (0.0f, 1.0f, info.norm)), 2.0f);
    }

    r.reduce (6.0f, 3.0f);
    g.setFont (FontOptions (10.0f));
    g.setColour (Colours_::dim);
    g.drawText (caption, r.removeFromTop (12.0f), Justification::topLeft);
    g.setFont (FontOptions (12.0f, Font::bold));
    g.setColour (info.mapped ? Colours_::text : Colours_::dim);
    g.drawFittedText (info.mapped ? info.name : String ("-"), r.removeFromTop (16.0f).toNearestInt(), Justification::centredLeft, 1);
    g.setFont (FontOptions (11.0f));
    g.setColour (Colours_::dim);
    g.drawFittedText (info.value, r.toNearestInt(), Justification::centredLeft, 1);
}

//============================================================================== editor
NocturnAutomapEditor::NocturnAutomapEditor (NocturnAutomapProcessor& p)
    : AudioProcessorEditor (p), proc (p), scanner (p.getCatalog())
{
    for (auto* b : { &loadButton, &learnButton, &prevPage, &nextPage, &focusButton, &menuButton })
    {
        addAndMakeVisible (b);
        b->setColour (TextButton::buttonColourId, Colours_::panel);
        b->setColour (TextButton::textColourOffId, Colours_::text);
    }
    learnButton.setClickingTogglesState (false);
    learnButton.setColour (TextButton::buttonOnColourId, Colours_::learn);

    for (auto* l : { &pluginName, &pageLabel, &statusLabel })
    {
        addAndMakeVisible (l);
        l->setColour (Label::textColourId, Colours_::text);
    }
    pluginName.setFont (FontOptions (16.0f, Font::bold));
    pageLabel.setJustificationType (Justification::centred);
    statusLabel.setFont (FontOptions (11.0f));
    statusLabel.setColour (Label::textColourId, Colours_::dim);
    statusLabel.setJustificationType (Justification::centredRight);

    for (int i = 0; i <= NocturnAutomapProcessor::slotsPerPage; ++i)
    {
        addAndMakeVisible (slots[i]);
        slots[i].onClick = [this, i]
        {
            // clicking a slot while learning picks it as the learn target
            if (proc.isLearnArmed()) proc.setLearnSlot (i);
        };
    }

    loadButton.onClick  = [this] { showPluginMenu(); };
    learnButton.onClick = [this] { proc.setLearnArmed (! proc.isLearnArmed()); };
    prevPage.onClick    = [this] { proc.setPage (proc.getPage() - 1); };
    nextPage.onClick    = [this] { proc.setPage (proc.getPage() + 1); };
    focusButton.onClick = [this] { proc.claimNocturn(); };
    menuButton.onClick  = [this]
    {
        PopupMenu m;
        m.addItem (1, "Reset mapping to all parameters", proc.getHosted() != nullptr);
        m.addItem (2, "Rescan plug-ins");
        m.addItem (3, "Unload plug-in", proc.getHosted() != nullptr);
        m.addItem (4, "Give Nocturn back to Live / DAW");
        m.showMenuAsync (PopupMenu::Options().withTargetComponent (menuButton), [this] (int r)
        {
            if (r == 1) proc.resetMapping();
            if (r == 2) { scanner.start(); refresh(); }
            if (r == 3) proc.unloadPlugin();
            if (r == 4) proc.releaseNocturn();
        });
    };

    scanner.onProgress = [this] { refresh(); };
    scanner.onFinished = [this] { refresh(); showPluginMenu(); };

    proc.hostedChanged.addChangeListener (this);
    proc.displayChanged.addChangeListener (this);
    proc.getLink().statusChanged.addChangeListener (this);

    rebuildHostedEditor();
    refresh();
    startTimerHz (15);

    // Opening the wrapper's window gives it the Nocturn, like Automap did.
    proc.claimNocturn();
}

NocturnAutomapEditor::~NocturnAutomapEditor()
{
    proc.hostedChanged.removeChangeListener (this);
    proc.displayChanged.removeChangeListener (this);
    proc.getLink().statusChanged.removeChangeListener (this);
    if (hostedEditor != nullptr) hostedEditor->removeComponentListener (this);
    hostedEditor.reset();
}

void NocturnAutomapEditor::rebuildHostedEditor()
{
    if (hostedEditor != nullptr)
    {
        hostedEditor->removeComponentListener (this);
        removeChildComponent (hostedEditor.get());
        hostedEditor.reset();
    }
    if (auto* inst = proc.getHosted())
    {
        AudioProcessorEditor* ed = inst->hasEditor() ? inst->createEditorIfNeeded() : nullptr;
        if (ed == nullptr) ed = new GenericAudioProcessorEditor (*inst);
        hostedEditor.reset (ed);
        addAndMakeVisible (*hostedEditor);
        hostedEditor->addComponentListener (this);
    }
    updateSize();
}

void NocturnAutomapEditor::updateSize()
{
    const int w = jmax (720, hostedEditor != nullptr ? hostedEditor->getWidth() : 0);
    const int h = headerHeight + (hostedEditor != nullptr ? hostedEditor->getHeight() : 160);
    if (getWidth() != w || getHeight() != h) setSize (w, h);
    else resized();
}

void NocturnAutomapEditor::componentMovedOrResized (Component& c, bool, bool wasResized)
{
    if (wasResized && &c == hostedEditor.get())
        updateSize();
}

void NocturnAutomapEditor::changeListenerCallback (ChangeBroadcaster* b)
{
    if (b == &proc.hostedChanged) rebuildHostedEditor();
    refresh();
}

void NocturnAutomapEditor::timerCallback() { refresh(); }

void NocturnAutomapEditor::refresh()
{
    const bool loaded = proc.getHosted() != nullptr;
    pluginName.setText (loaded ? proc.getHostedName() : String ("No plug-in loaded"), dontSendNotification);
    pageLabel.setText ("Page " + String (proc.getPage() + 1) + " / " + String (proc.getNumPages()), dontSendNotification);
    learnButton.setToggleState (proc.isLearnArmed(), dontSendNotification);

    String status;
    auto& link = proc.getLink();
    if (scanner.isScanning())         status = scanner.getStatus();
    else if (! link.isConnected())    status = "NocturnBridge not running";
    else if (! proc.hasNocturnFocus()) status = "Nocturn: another Automap instance";
    else if (! link.automapOwnsHardware()) status = "Nocturn: controlling Live";
    else                              status = "Nocturn: in control";
    if (proc.isLearnArmed())
        status = "LEARN: touch a Nocturn knob (or click a slot), then move a control in the plug-in";
    statusLabel.setText (status, dontSendNotification);
    focusButton.setEnabled (! (proc.hasNocturnFocus() && link.automapOwnsHardware()));

    const int touched = proc.getTouchedSlot(), learnSlot = proc.isLearnArmed() ? proc.getLearnSlot() : -2;
    for (int i = 0; i <= NocturnAutomapProcessor::slotsPerPage; ++i)
        slots[i].set (proc.getSlotInfo (i), i < NocturnAutomapProcessor::slotsPerPage ? i + 1 : 0,
                      i == touched, i == learnSlot);
}

void NocturnAutomapEditor::showPluginMenu()
{
    auto& cat = proc.getCatalog();
    if (cat.list.getNumTypes() == 0 && ! scanner.isScanning())
    {
        scanner.start();
        refresh();
        return;
    }
    auto types = cat.list.getTypes();
    PopupMenu m;
    KnownPluginList::addToMenu (m, types, KnownPluginList::sortByManufacturer);
    m.addSeparator();
    m.addItem (-2, "Rescan plug-ins");
    m.showMenuAsync (PopupMenu::Options().withTargetComponent (loadButton), [this, types] (int r)
    {
        if (r == -2) { scanner.start(); refresh(); return; }
        const int idx = KnownPluginList::getIndexChosenByMenu (types, r);
        if (! isPositiveAndBelow (idx, types.size())) return;
        String err;
        if (! proc.loadPlugin (types.getReference (idx), err))
            AlertWindow::showMessageBoxAsync (MessageBoxIconType::WarningIcon, "Nocturn Automap",
                                              "Could not load " + types.getReference (idx).name + "\n" + err);
    });
}

void NocturnAutomapEditor::mouseDown (const MouseEvent&)
{
    if (! proc.hasNocturnFocus()) proc.claimNocturn();
}

void NocturnAutomapEditor::paint (Graphics& g)
{
    g.fillAll (Colours_::bg);
    g.setColour (Colours_::accent);
    g.fillRect (0, headerHeight - 2, getWidth(), 2);
    if (proc.getHosted() == nullptr)
    {
        g.setColour (Colours_::dim);
        g.setFont (FontOptions (14.0f));
        g.drawText ("Click \"Load plug-in...\" to wrap an instrument or effect.",
                    getLocalBounds().withTrimmedTop (headerHeight), Justification::centred);
    }
}

void NocturnAutomapEditor::resized()
{
    auto r = getLocalBounds();
    auto header = r.removeFromTop (headerHeight).reduced (8, 6);

    auto row1 = header.removeFromTop (28);
    loadButton.setBounds (row1.removeFromLeft (120));
    row1.removeFromLeft (8);
    menuButton.setBounds (row1.removeFromRight (32));
    row1.removeFromRight (6);
    focusButton.setBounds (row1.removeFromRight (110));
    row1.removeFromRight (6);
    learnButton.setBounds (row1.removeFromRight (64));
    row1.removeFromRight (10);
    nextPage.setBounds (row1.removeFromRight (28));
    pageLabel.setBounds (row1.removeFromRight (90));
    prevPage.setBounds (row1.removeFromRight (28));
    row1.removeFromRight (10);
    pluginName.setBounds (row1);

    statusLabel.setBounds (header.removeFromTop (18));
    header.removeFromTop (4);

    const int n = NocturnAutomapProcessor::slotsPerPage + 1;
    const int w = header.getWidth() / n;
    for (int i = 0; i < n; ++i)
        slots[i].setBounds (header.removeFromLeft (i == n - 1 ? header.getWidth() : w));

    if (hostedEditor != nullptr)
        hostedEditor->setTopLeftPosition (0, headerHeight);
}
