/*
 * kNocTurnLink - one per process, shared by every kNocTurn NightCap wrapper instance.
 * Talks to kNocTurnBridge's "kNocTurn NightCap" MIDI ports and routes the hardware to
 * whichever wrapper instance currently has focus (like NightCap Server did).
 */
#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>

namespace kNocTurnCC
{
    constexpr int encoderFirst   = 64;   // 64..71 relative
    constexpr int crossfader     = 72;   // absolute 0..127
    constexpr int speedDial      = 74;   // relative
    constexpr int speedTouch     = 81;
    constexpr int xfaderTouch    = 83;
    constexpr int touchFirst     = 96;   // 96..103
    constexpr int buttonFirst    = 112;  // 112..119 top row
    constexpr int learn = 120, view = 121, pageDown = 122, pageUp = 123,
                  user = 124, fx = 125, inst = 126, mixer = 127;
    constexpr int ringFirst      = 64;   // LED ring outputs 64..71

    inline int relative (int v) noexcept { return v < 64 ? v : v - 128; }
}

class kNocTurnClient
{
public:
    virtual ~kNocTurnClient() = default;
    virtual void hardwareControl (int cc, int value) = 0;       // message thread
    virtual void hardwareFocusChanged (bool hasFocus) = 0;      // message thread
    virtual void hardwareRepaintLeds() = 0;                     // message thread
    virtual juce::String hardwareClientName() const = 0;
};

class kNocTurnLink : private juce::MidiInputCallback,
                    private juce::Timer
{
public:
    static constexpr const char* portName = "kNocTurn NightCap";

    kNocTurnLink();
    ~kNocTurnLink() override;

    void addClient (kNocTurnClient*);
    void removeClient (kNocTurnClient*);

    /** Give the hardware to this client. claimHardware also tells kNocTurnBridge to
        route the kNocTurn to NightCap (instead of Live / the DAW). */
    void setFocus (kNocTurnClient*, bool claimHardware);
    kNocTurnClient* getFocus() const noexcept { return focused; }
    bool hasFocus (const kNocTurnClient* c) const noexcept { return c != nullptr && c == focused; }

    /** Hand the hardware back to the DAW (Live remote script). */
    void releaseToDaw();

    void sendCC (const kNocTurnClient* from, int cc, int value);

    bool isConnected() const noexcept { return input != nullptr && output != nullptr; }
    bool nightcapOwnsHardware() const noexcept { return bridgeMode == 1; }

    juce::ChangeBroadcaster statusChanged;

private:
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;
    void timerCallback() override;
    void tryConnect();
    void sendModeSysex (int mode);

    std::unique_ptr<juce::MidiInput> input;
    std::unique_ptr<juce::MidiOutput> output;
    juce::Array<kNocTurnClient*> clients;
    kNocTurnClient* focused = nullptr;
    int bridgeMode = 0;
    int connectCountdown = 0;

    juce::CriticalSection queueLock;
    std::vector<juce::MidiMessage> queue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (kNocTurnLink)
};
