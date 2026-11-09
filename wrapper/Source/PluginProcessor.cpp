#include "PluginProcessor.h"
#include "PluginEditor.h"

using namespace juce;

//============================================================================== proxies
float ProxyParameter::getValue() const
{
    if (auto* p = owner.getHostedParam (owner.proxyTarget (index))) return p->getValue();
    return cached;
}
void ProxyParameter::setValue (float v)
{
    cached = v;
    if (auto* p = owner.getHostedParam (owner.proxyTarget (index))) p->setValue (v);
}
float ProxyParameter::getDefaultValue() const
{
    if (auto* p = owner.getHostedParam (owner.proxyTarget (index))) return p->getDefaultValue();
    return 0.0f;
}
String ProxyParameter::getName (int maxLen) const
{
    if (auto* p = owner.getHostedParam (owner.proxyTarget (index))) return p->getName (maxLen);
    return ("Param " + String (index + 1)).substring (0, maxLen);
}
String ProxyParameter::getLabel() const
{
    if (auto* p = owner.getHostedParam (owner.proxyTarget (index))) return p->getLabel();
    return {};
}
String ProxyParameter::getText (float v, int maxLen) const
{
    if (auto* p = owner.getHostedParam (owner.proxyTarget (index))) return p->getText (v, maxLen);
    return String (v, 2);
}
float ProxyParameter::getValueForText (const String& t) const
{
    if (auto* p = owner.getHostedParam (owner.proxyTarget (index))) return p->getValueForText (t);
    return t.getFloatValue();
}

//============================================================================== processor
AudioProcessor::BusesProperties NightCapProcessor::makeBuses()
{
   #if JucePlugin_IsSynth
    return AudioProcessor::BusesProperties().withOutput ("Output", AudioChannelSet::stereo(), true);
   #else
    return AudioProcessor::BusesProperties().withInput ("Input", AudioChannelSet::stereo(), true)
                                            .withOutput ("Output", AudioChannelSet::stereo(), true);
   #endif
}

NightCapProcessor::NightCapProcessor() : AudioProcessor (makeBuses())
{
    std::fill (std::begin (ledCache), std::end (ledCache), -1);
    for (auto& t : proxyTargets) t = -1;
    addParameter (new FocusParameter (focusRequested));
    for (int i = 0; i < numProxies; ++i)
    {
        auto* p = new ProxyParameter (*this, i);
        proxies.add (p);
        addParameter (p);
    }
    link->addClient (this);
    startTimerHz (30);
}

NightCapProcessor::~NightCapProcessor()
{
    alive->store (false);
    stopTimer();
    for (int slot = 0; slot <= slotsPerPage; ++slot) endSlotGesture (slot);
    link->removeClient (this);
    if (auto* inst = current.exchange (nullptr))
        inst->removeListener (this);
    owned.reset();
    graveyard.clear();
}

bool NightCapProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    const auto out = l.getMainOutputChannelSet();
    if (out != AudioChannelSet::mono() && out != AudioChannelSet::stereo()) return false;
   #if ! JucePlugin_IsSynth
    if (l.getMainInputChannelSet() != out) return false;
   #endif
    return true;
}

void NightCapProcessor::configureBuses (AudioPluginInstance& inst)
{
    auto l = inst.getBusesLayout();
    const auto want = getMainBusNumOutputChannels() == 1 ? AudioChannelSet::mono() : AudioChannelSet::stereo();
    for (int i = 0; i < l.inputBuses.size(); ++i)
        l.inputBuses.getReference (i) = i == 0 ? want : AudioChannelSet::disabled();
    for (int i = 0; i < l.outputBuses.size(); ++i)
        l.outputBuses.getReference (i) = i == 0 ? want : AudioChannelSet::disabled();
    if (inst.checkBusesLayoutSupported (l))
        inst.setBusesLayout (l);
}

void NightCapProcessor::prepareHosted (AudioPluginInstance& inst)
{
    inst.setRateAndBufferSizeDetails (sampleRate, blockSize);
    inst.setNonRealtime (isNonRealtime());
    inst.prepareToPlay (sampleRate, blockSize);
    const int ch = std::max ({ 2, inst.getTotalNumInputChannels(), inst.getTotalNumOutputChannels(),
                               getTotalNumInputChannels(), getTotalNumOutputChannels() });
    work.setSize (ch, jmax (blockSize, 32), false, true, false);
}

void NightCapProcessor::prepareToPlay (double sr, int bs)
{
    sampleRate = sr;
    blockSize = bs;
    prepared = true;
    if (auto* inst = current.load())
    {
        prepareHosted (*inst);
        setLatencySamples (inst->getLatencySamples());
    }
    else
        work.setSize (2, jmax (bs, 32), false, true, false);
}

void NightCapProcessor::releaseResources()
{
    prepared = false;
    if (auto* inst = current.load()) inst->releaseResources();
}

double NightCapProcessor::getTailLengthSeconds() const
{
    if (auto* inst = current.load()) return inst->getTailLengthSeconds();
    return 0.0;
}

void NightCapProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    auto* inst = current.load();
    const int n = buffer.getNumSamples();

    if (inst == nullptr || bypassHosted.load())
    {
       #if JucePlugin_IsSynth
        buffer.clear();
       #endif
        return;   // effect: dry pass-through
    }

    const int need = jmax (inst->getTotalNumInputChannels(), inst->getTotalNumOutputChannels());
    const int ourIn = getTotalNumInputChannels(), ourOut = getTotalNumOutputChannels();

    if (need <= buffer.getNumChannels())
    {
        for (int c = ourIn; c < need; ++c) buffer.clear (c, 0, n);
        AudioBuffer<float> view (buffer.getArrayOfWritePointers(), need, n);
        inst->processBlock (view, midi);
        return;
    }

    if (work.getNumChannels() < need || work.getNumSamples() < n)
        work.setSize (need, n, false, true, true);   // rare: host exceeded its block size
    for (int c = 0; c < need; ++c)
    {
        if (c < ourIn) work.copyFrom (c, 0, buffer, c, 0, n);
        else           work.clear (c, 0, n);
    }
    AudioBuffer<float> view (work.getArrayOfWritePointers(), need, n);
    inst->processBlock (view, midi);
    for (int c = 0; c < ourOut; ++c)
        buffer.copyFrom (c, 0, work, jmin (c, inst->getTotalNumOutputChannels() - 1), 0, n);
}

//============================================================================== hosting
AudioProcessorParameter* NightCapProcessor::getHostedParam (int index) const
{
    if (auto* inst = current.load())
    {
        auto& ps = inst->getParameters();
        if (isPositiveAndBelow (index, ps.size())) return ps.getUnchecked (index);
    }
    return nullptr;
}

String NightCapProcessor::getHostedName() const
{
    return current.load() != nullptr ? hostedDesc.name : String();
}

bool NightCapProcessor::loadPlugin (const PluginDescription& desc, String& error)
{
    jassert (MessageManager::existsAndIsCurrentThread());
    auto inst = catalog->formats.createPluginInstance (desc, sampleRate, blockSize, error);
    if (inst == nullptr) return false;

    configureBuses (*inst);
    if (prepared) prepareHosted (*inst);

    unloadPlugin();
    hostedDesc = desc;
    buildProxyMap (*inst);
    owned = std::move (inst);
    owned->addListener (this);
    current.store (owned.get());
    setLatencySamples (owned->getLatencySamples());

    page = 0;
    learnArmed = false;
    learnSlot = touchedSlot = -1;
    if (! loadMappingFile()) loadDefaultMapping();

    updateHostDisplay (AudioProcessorListener::ChangeDetails().withParameterInfoChanged (true).withLatencyChanged (true));
    hostedChanged.sendSynchronousChangeMessage();
    displayChanged.sendChangeMessage();
    hardwareRepaintLeds();
    return true;
}

void NightCapProcessor::unloadPlugin()
{
    auto* inst = current.exchange (nullptr);
    if (inst == nullptr) return;
    hostedChanged.sendSynchronousChangeMessage();   // editor drops the hosted UI first
    inst->removeListener (this);
    for (auto& t : proxyTargets) t = -1;
    // keep the instance alive briefly in case the audio thread is still inside it
    graveyard.emplace_back (Time::getMillisecondCounter(), std::move (owned));
    mapping.clear();
    xfaderParam = -1;
    customMapping = false;
    setLatencySamples (0);
    updateHostDisplay (AudioProcessorListener::ChangeDetails().withParameterInfoChanged (true));
    displayChanged.sendChangeMessage();
    hardwareRepaintLeds();
}

void NightCapProcessor::buildProxyMap (AudioPluginInstance& inst)
{
    auto& ps = inst.getParameters();
    hostToProxy.assign ((size_t) ps.size(), -1);
    int next = 0;
    for (int i = 0; i < ps.size() && next < numProxies; ++i)
        if (ps.getUnchecked (i)->isAutomatable())
        {
            hostToProxy[(size_t) i] = next;
            proxyTargets[(size_t) next++] = i;
        }
    while (next < numProxies) proxyTargets[(size_t) next++] = -1;
}

void NightCapProcessor::audioProcessorChanged (AudioProcessor* p, const AudioProcessorListener::ChangeDetails& d)
{
    if (d.latencyChanged)
    {
        const int lat = p->getLatencySamples();
        MessageManager::callAsync ([this, flag = alive, lat]
        {
            if (flag->load()) setLatencySamples (lat);
        });
    }
}

void NightCapProcessor::audioProcessorParameterChanged (AudioProcessor*, int index, float v)
{
    if (isPositiveAndBelow (index, (int) hostToProxy.size()) && hostToProxy[(size_t) index] >= 0)
        proxies.getUnchecked (hostToProxy[(size_t) index])->sendValueChangedMessageToListeners (v);
    if (! writingFromHardware)
        lastUiParam = index;   // for learn (picked up on the message thread)
}

void NightCapProcessor::audioProcessorParameterChangeGestureBegin (AudioProcessor*, int index)
{
    if (isPositiveAndBelow (index, (int) hostToProxy.size()) && hostToProxy[(size_t) index] >= 0)
        proxies.getUnchecked (hostToProxy[(size_t) index])->beginChangeGesture();
}

void NightCapProcessor::audioProcessorParameterChangeGestureEnd (AudioProcessor*, int index)
{
    if (isPositiveAndBelow (index, (int) hostToProxy.size()) && hostToProxy[(size_t) index] >= 0)
        proxies.getUnchecked (hostToProxy[(size_t) index])->endChangeGesture();
}

//============================================================================== mapping
void NightCapProcessor::loadDefaultMapping()
{
    mapping.clear();
    customMapping = false;
    xfaderParam = -1;
    if (auto* inst = current.load())
    {
        auto& ps = inst->getParameters();
        for (int i = 0; i < ps.size(); ++i)
            if (ps.getUnchecked (i)->isAutomatable())
                mapping.push_back (i);
    }
}

void NightCapProcessor::resetMapping()
{
    loadDefaultMapping();
    mappingFile().deleteFile();
    page = 0;
    displayChanged.sendChangeMessage();
    hardwareRepaintLeds();
}

File NightCapProcessor::mappingFile() const
{
    auto name = File::createLegalFileName (hostedDesc.manufacturerName + " - " + hostedDesc.name
                                           + " (" + hostedDesc.pluginFormatName + ")");
    return PluginCatalog::mappingDir().getChildFile (name + ".nightcap.xml");
}

void NightCapProcessor::saveMappingFile() const
{
    if (! customMapping || current.load() == nullptr) return;
    XmlElement xml ("NIGHTCAP_MAPPING");
    xml.setAttribute ("plugin", hostedDesc.name);
    xml.setAttribute ("crossfader", xfaderParam);
    StringArray ids;
    for (int m : mapping) ids.add (String (m));
    xml.setAttribute ("slots", ids.joinIntoString (","));
    PluginCatalog::mappingDir().createDirectory();
    xml.writeTo (mappingFile());
}

bool NightCapProcessor::loadMappingFile()
{
    auto xml = XmlDocument::parse (mappingFile());
    if (xml == nullptr || ! xml->hasTagName ("NIGHTCAP_MAPPING")) return false;
    mapping.clear();
    for (auto& s : StringArray::fromTokens (xml->getStringAttribute ("slots"), ",", {}))
        mapping.push_back (s.getIntValue());
    xfaderParam = xml->getIntAttribute ("crossfader", -1);
    customMapping = true;
    return true;
}

int NightCapProcessor::paramForSlot (int slot) const
{
    if (slot == xfaderSlot) return xfaderParam;
    const size_t i = (size_t) (page * slotsPerPage + slot);
    return i < mapping.size() ? mapping[i] : -1;
}

int NightCapProcessor::getNumPages() const
{
    return jmax (1, ((int) mapping.size() + slotsPerPage - 1) / slotsPerPage);
}

void NightCapProcessor::setPage (int p)
{
    // allow one empty page past the end so you can learn new slots there
    const int maxPage = customMapping || mapping.empty() ? getNumPages() : getNumPages() - 1;
    p = jlimit (0, jmax (0, maxPage), p);
    for (int s = 0; s < slotsPerPage; ++s) endSlotGesture (s);
    page = p;
    displayChanged.sendChangeMessage();
    sendLeds (false);
}

void NightCapProcessor::setLearnArmed (bool on)
{
    learnArmed = on;
    learnSlot = on ? lastSlot : -1;
    lastUiParam = -1;
    displayChanged.sendChangeMessage();
    sendLeds (false);
}

NightCapProcessor::SlotInfo NightCapProcessor::getSlotInfo (int slot) const
{
    SlotInfo info;
    if (auto* p = getHostedParam (paramForSlot (slot)))
    {
        info.mapped = true;
        info.name = p->getName (32);
        info.norm = p->getValue();
        info.value = p->getCurrentValueAsText();
        auto label = p->getLabel();
        if (label.isNotEmpty() && ! info.value.endsWith (label)) info.value << " " << label;
    }
    return info;
}

//============================================================================== kNocTurn input
void NightCapProcessor::setHostedParamFromHardware (int paramIndex, float v)
{
    if (auto* p = getHostedParam (paramIndex))
    {
        const ScopedValueSetter<bool> svs (writingFromHardware, true);
        p->setValueNotifyingHost (jlimit (0.0f, 1.0f, v));
    }
}

void NightCapProcessor::beginSlotGesture (int slot)
{
    if (gestureOpen[slot]) return;
    if (auto* p = getHostedParam (paramForSlot (slot)))
    {
        p->beginChangeGesture();
        gestureOpen[slot] = true;
    }
}

void NightCapProcessor::endSlotGesture (int slot)
{
    if (! gestureOpen[slot]) return;
    gestureOpen[slot] = false;
    if (auto* p = getHostedParam (paramForSlot (slot)))
        p->endChangeGesture();
}

void NightCapProcessor::nudgeSlot (int slot, double delta)
{
    auto* p = getHostedParam (paramForSlot (slot));
    if (p == nullptr) return;

    const int steps = p->getNumSteps();
    const bool coarse = p->isDiscrete() && steps > 1 && steps <= 32;
    double change;
    if (coarse)
    {
        // a few clicks per step so switches don't fly past their positions
        accum[slot] += delta;
        const int clicks = (int) (accum[slot] / 3.0);
        if (clicks == 0) return;
        accum[slot] -= clicks * 3.0;
        change = clicks / (double) (steps - 1);
    }
    else
        change = delta / 127.0;

    beginSlotGesture (slot);
    lastMove[slot] = Time::getMillisecondCounter();
    setHostedParamFromHardware (paramForSlot (slot), (float) (p->getValue() + change));
}

void NightCapProcessor::hardwareControl (int cc, int value)
{
    using namespace kNocTurnCC;
    const bool pressed = value > 0;

    if (cc >= encoderFirst && cc < encoderFirst + slotsPerPage)
    {
        const int slot = cc - encoderFirst;
        lastSlot = slot;
        if (learnArmed) { learnSlot = slot; displayChanged.sendChangeMessage(); }
        nudgeSlot (slot, relative (value));
        return;
    }
    if (cc >= touchFirst && cc < touchFirst + slotsPerPage)
    {
        const int slot = cc - touchFirst;
        touchHeld[slot] = pressed;
        if (pressed)
        {
            touchedSlot = lastSlot = slot;
            if (learnArmed) learnSlot = slot;
        }
        else
        {
            if (touchedSlot == slot) touchedSlot = -1;
            endSlotGesture (slot);
        }
        displayChanged.sendChangeMessage();
        return;
    }
    if (cc == speedDial)
    {
        // fine adjust of the last used encoder (1/10 resolution), like NightCap's speed dial
        nudgeSlot (lastSlot, relative (value) / 10.0);
        return;
    }
    if (cc == crossfader)
    {
        if (learnArmed) { learnSlot = xfaderSlot; displayChanged.sendChangeMessage(); return; }
        if (xfaderParam >= 0)
        {
            beginSlotGesture (xfaderSlot);
            lastMove[xfaderSlot] = Time::getMillisecondCounter();
            setHostedParamFromHardware (xfaderParam, value / 127.0f);
        }
        return;
    }
    if (cc == xfaderTouch && ! pressed) { endSlotGesture (xfaderSlot); return; }

    if (! pressed) return;   // buttons below act on press

    if (cc >= buttonFirst && cc < buttonFirst + 8) { setPage (cc - buttonFirst); return; }
    switch (cc)
    {
        case learn:    setLearnArmed (! learnArmed); break;
        case view:     if (auto* p = getHostedParam (paramForSlot (lastSlot)))
                           setHostedParamFromHardware (paramForSlot (lastSlot), p->getDefaultValue());
                       break;
        case pageDown: setPage (page - 1); break;
        case pageUp:   setPage (page + 1); break;
        case user:     handleProgramStep (-1); break;
        case fx:       handleProgramStep (1); break;
        case inst:     bypassHosted = ! bypassHosted.load(); sendLeds (false); displayChanged.sendChangeMessage(); break;
        case mixer:    link->releaseToDaw(); break;
        default: break;
    }
}

void NightCapProcessor::handleProgramStep (int step)
{
    if (auto* inst = current.load())
    {
        const int n = inst->getNumPrograms();
        if (n > 1)
        {
            inst->setCurrentProgram ((inst->getCurrentProgram() + step + n) % n);
            displayChanged.sendChangeMessage();
        }
    }
}

void NightCapProcessor::hardwareFocusChanged (bool)
{
    for (int s = 0; s <= slotsPerPage; ++s) endSlotGesture (s);
    displayChanged.sendChangeMessage();
}

void NightCapProcessor::hardwareRepaintLeds()
{
    std::fill (std::begin (ledCache), std::end (ledCache), -1);
    sendLeds (true);
}

void NightCapProcessor::sendLeds (bool force)
{
    if (! link->hasFocus (this)) return;
    auto send = [&] (int cc, int v)
    {
        if (force || ledCache[cc] != v) { ledCache[cc] = v; link->sendCC (this, cc, v); }
    };
    using namespace kNocTurnCC;
    for (int s = 0; s < slotsPerPage; ++s)
    {
        auto* p = getHostedParam (paramForSlot (s));
        send (ringFirst + s, p != nullptr ? roundToInt (p->getValue() * 127.0f) : 0);
    }
    for (int b = 0; b < 8; ++b)
        send (buttonFirst + b, b == page ? 127 : 0);
    send (learn, learnArmed ? 127 : 0);
    send (view, 0);
    send (pageDown, page > 0 ? 127 : 0);
    send (pageUp, page < getNumPages() - 1 ? 127 : 0);
    send (user, 0);
    send (fx, 0);
    send (inst, bypassHosted ? 127 : 0);
    send (mixer, 0);
}

//============================================================================== timer
void NightCapProcessor::timerCallback()
{
    if (focusRequested.exchange (false))
        link->setFocus (this, true);

    // learn: a parameter was moved in the plug-in's own UI
    const int ui = lastUiParam.exchange (-1);
    if (learnArmed && ui >= 0 && learnSlot >= 0)
    {
        if (learnSlot == xfaderSlot)
            xfaderParam = ui;
        else
        {
            const size_t i = (size_t) (page * slotsPerPage + learnSlot);
            if (mapping.size() <= i) mapping.resize (i + 1, -1);
            mapping[i] = ui;
        }
        customMapping = true;
        saveMappingFile();
        learnArmed = false;
        learnSlot = -1;
        displayChanged.sendChangeMessage();
    }

    // close gestures after the encoder has been idle for a moment
    const auto now = Time::getMillisecondCounter();
    for (int s = 0; s <= slotsPerPage; ++s)
        if (gestureOpen[s] && ! touchHeld[s] && now - lastMove[s] > 400)
            endSlotGesture (s);

    sendLeds (false);

    graveyard.erase (std::remove_if (graveyard.begin(), graveyard.end(),
                                     [now] (auto& g) { return now - g.first > 3000; }),
                     graveyard.end());
}

//============================================================================== state
void NightCapProcessor::getStateInformation (MemoryBlock& dest)
{
    XmlElement xml ("NIGHTCAP");
    xml.setAttribute ("version", 1);
    if (auto* inst = current.load())
    {
        xml.addChildElement (hostedDesc.createXml().release());
        MemoryBlock state;
        inst->getStateInformation (state);
        xml.createNewChildElement ("STATE")->addTextElement (state.toBase64Encoding());
        StringArray ids;
        for (int m : mapping) ids.add (String (m));
        auto* map = xml.createNewChildElement ("MAPPING");
        map->setAttribute ("slots", ids.joinIntoString (","));
        map->setAttribute ("crossfader", xfaderParam);
        map->setAttribute ("custom", customMapping);
        map->setAttribute ("page", page);
        xml.setAttribute ("bypass", bypassHosted.load());
    }
    copyXmlToBinary (xml, dest);
}

void NightCapProcessor::setStateInformation (const void* data, int size)
{
    std::shared_ptr<XmlElement> xml (getXmlFromBinary (data, size).release());
    if (xml == nullptr || ! xml->hasTagName ("NIGHTCAP")) return;

    auto apply = [this, xml]
    {
        unloadPlugin();
        auto* descXml = xml->getChildByName ("PLUGIN");
        if (descXml == nullptr) return;
        PluginDescription desc;
        if (! desc.loadFromXml (*descXml)) return;
        String err;
        if (! loadPlugin (desc, err)) { DBG ("kNocTurn NightCap: could not reload " << desc.name << ": " << err); return; }
        if (auto* st = xml->getChildByName ("STATE"))
        {
            MemoryBlock mb;
            if (mb.fromBase64Encoding (st->getAllSubText().trim()))
                current.load()->setStateInformation (mb.getData(), (int) mb.getSize());
        }
        if (auto* map = xml->getChildByName ("MAPPING"))
        {
            if (map->getBoolAttribute ("custom"))
            {
                mapping.clear();
                for (auto& s : StringArray::fromTokens (map->getStringAttribute ("slots"), ",", {}))
                    mapping.push_back (s.getIntValue());
                xfaderParam = map->getIntAttribute ("crossfader", -1);
                customMapping = true;
            }
            page = map->getIntAttribute ("page", 0);
        }
        bypassHosted = xml->getBoolAttribute ("bypass", false);
        displayChanged.sendChangeMessage();
        hardwareRepaintLeds();
    };

    if (MessageManager::existsAndIsCurrentThread()) apply();
    else MessageManager::callAsync ([flag = alive, apply] { if (flag->load()) apply(); });
}

AudioProcessorEditor* NightCapProcessor::createEditor()
{
    return new NightCapEditor (*this);
}

AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NightCapProcessor();
}
