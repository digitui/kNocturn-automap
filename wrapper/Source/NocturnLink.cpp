#include "NocturnLink.h"

static const juce::uint8 sysexHeader[] = { 0x00, 0x20, 0x29, 0x7F };

NocturnLink::NocturnLink()
{
    tryConnect();
    startTimer (10);
}

NocturnLink::~NocturnLink()
{
    stopTimer();
    if (input != nullptr) input->stop();
}

void NocturnLink::tryConnect()
{
    if (input == nullptr)
        for (auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name == portName)
            {
                input = juce::MidiInput::openDevice (d.identifier, this);
                if (input != nullptr) input->start();
                break;
            }

    if (output == nullptr)
        for (auto& d : juce::MidiOutput::getAvailableDevices())
            if (d.name == portName)
            {
                output = juce::MidiOutput::openDevice (d.identifier);
                break;
            }

    if (isConnected() && focused != nullptr)
        focused->nocturnRepaintLeds();

    statusChanged.sendChangeMessage();
}

void NocturnLink::addClient (NocturnClient* c)
{
    clients.addIfNotAlreadyThere (c);
    if (focused == nullptr)
        setFocus (c, false);
}

void NocturnLink::removeClient (NocturnClient* c)
{
    clients.removeFirstMatchingValue (c);
    if (focused == c)
    {
        focused = nullptr;
        if (! clients.isEmpty())
            setFocus (clients.getLast(), false);
    }
    statusChanged.sendChangeMessage();
}

void NocturnLink::setFocus (NocturnClient* c, bool claimHardware)
{
    if (c != focused)
    {
        auto* old = focused;
        focused = c;
        if (old != nullptr) old->nocturnFocusChanged (false);
        if (focused != nullptr) focused->nocturnFocusChanged (true);
    }
    if (claimHardware)
        sendModeSysex (1);
    if (focused != nullptr)
        focused->nocturnRepaintLeds();
    statusChanged.sendChangeMessage();
}

void NocturnLink::releaseToDaw()
{
    sendModeSysex (0);
}

void NocturnLink::sendModeSysex (int mode)
{
    if (output == nullptr) return;
    const juce::uint8 data[] = { 0x00, 0x20, 0x29, 0x7F, 0x01, (juce::uint8) (mode ? 1 : 0) };
    output->sendMessageNow (juce::MidiMessage::createSysExMessage (data, (int) sizeof (data)));
}

void NocturnLink::sendCC (const NocturnClient* from, int cc, int value)
{
    if (output == nullptr || from != focused) return;
    output->sendMessageNow (juce::MidiMessage::controllerEvent (1, cc, juce::jlimit (0, 127, value)));
}

void NocturnLink::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    const juce::ScopedLock sl (queueLock);
    queue.push_back (m);
}

void NocturnLink::timerCallback()
{
    std::vector<juce::MidiMessage> pending;
    {
        const juce::ScopedLock sl (queueLock);
        pending.swap (queue);
    }

    for (auto& m : pending)
    {
        if (m.isSysEx())
        {
            auto* d = m.getSysExData();
            if (m.getSysExDataSize() == 6 && std::memcmp (d, sysexHeader, 4) == 0 && d[4] == 0x02)
            {
                bridgeMode = d[5];
                if (bridgeMode == 1 && focused != nullptr) focused->nocturnRepaintLeds();
                statusChanged.sendChangeMessage();
            }
        }
        else if (m.isController() && m.getChannel() == 1 && focused != nullptr)
        {
            focused->nocturnControl (m.getControllerNumber(), m.getControllerValue());
        }
    }

    // Reconnect every ~2 s if NocturnBridge was started after the plug-in (or restarted)
    if (++connectCountdown >= 200)
    {
        connectCountdown = 0;
        bool inGone = input != nullptr, outGone = output != nullptr;
        for (auto& d : juce::MidiInput::getAvailableDevices())  if (d.name == portName) inGone = false;
        for (auto& d : juce::MidiOutput::getAvailableDevices()) if (d.name == portName) outGone = false;
        if (inGone)  { input->stop(); input.reset(); }
        if (outGone) output.reset();
        if (! isConnected()) tryConnect();
    }
}
