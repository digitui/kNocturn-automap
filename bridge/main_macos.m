/*
 * NightCapBridge - native Apple Silicon / Intel driver for the Novation Nocturn controller.
 *
 * Replaces the 32-bit the original 32-bit server USB layer. Talks to the NightCap with
 * user-space IOKit (no kext needed) and exposes it as a CoreMIDI device:
 *   "NightCap"          (source + destination) - for your DAW / the Live remote script
 *   "NightCap"  (source + destination) - for the NightCap wrapper plug-ins
 * Only one port pair owns the hardware at a time; switch with SysEx F0 7D 4B 4E 43 01 <0|1> F7.
 *
 * Also shows the NightCap HUD: a floating panel with the names/values of the 8 knobs, fed by the
 * Live remote script over SysEx (F0 7D 4B 4E 43 10 <chunk> <total> <ascii json...> F7).
 *
 * Build:  sh build.sh      (needs Xcode Command Line Tools only)
 */
#import <Cocoa/Cocoa.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOMessage.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "nightcap_core.h"

#define LOG(...) do { fprintf(stderr, "[nightcap] " __VA_ARGS__); fputc('\n', stderr); } while (0)

typedef IOUSBDeviceInterface650    **DevIf;
typedef IOUSBInterfaceInterface650 **IntfIf;

static struct {
    nc_state core;
    pthread_mutex_t lock;          /* guards core + USB writes */
    DevIf dev;
    IntfIf intf;
    UInt8 pipe_in, pipe_out;
    io_object_t dev_service;
    io_object_t term_notification;
    pthread_t reader;
    volatile int connected;
    int channel;                   /* 0..15 */
    int verbose;
    MIDIClientRef client;
    MIDIEndpointRef src[NC_NUM_PORTS], dst[NC_NUM_PORTS];
    uint8_t sx[NC_NUM_PORTS][512];  /* sysex assembly per destination */
    size_t sxlen[NC_NUM_PORTS];
    IONotificationPortRef notify;
    int hud_disabled;
    char hud_buf[16384];           /* HUD JSON reassembly (MIDI thread) */
    size_t hud_len;
} G;

static void hud_deliver(NSData *json);   /* main-thread HUD update, defined below */

/* HUD SysEx: F0 7D 4B 4E 43 10 <chunk index> <chunk count> <data...> F7 */
static int handle_hud_sysex(const uint8_t *m, size_t n) {
    if (n < 9 || m[1] != 0x7D || m[2] != 0x4B || m[3] != 0x4E || m[4] != 0x43 || m[5] != 0x10) return 0;
    int idx = m[6], total = m[7];
    const uint8_t *data = m + 8;
    size_t dlen = n - 9;                                   /* minus trailing F7 */
    static int hud_seen = 0;
    if (!hud_seen) { hud_seen = 1; LOG("HUD: receiving HUD data from Live"); }
    if (idx == 0) G.hud_len = 0;
    if (G.hud_len + dlen < sizeof G.hud_buf) { memcpy(G.hud_buf + G.hud_len, data, dlen); G.hud_len += dlen; }
    if (idx == total - 1 && !G.hud_disabled) {
        NSData *json = [NSData dataWithBytes:G.hud_buf length:G.hud_len];
        dispatch_async(dispatch_get_main_queue(), ^{ hud_deliver(json); });
    }
    return 1;
}

static uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}

/* ---------- core callbacks ---------- */
static void usb_write_cb(void *ctx, const uint8_t *data, size_t len) {
    (void)ctx;
    if (!G.intf || !G.connected) return;
    IOReturn r = (*G.intf)->WritePipe(G.intf, G.pipe_out, (void *)data, (UInt32)len);
    if (G.verbose) {
        fprintf(stderr, "[nightcap] usb out:");
        for (size_t i = 0; i < len; i++) fprintf(stderr, " %02X", data[i]);
        fprintf(stderr, r ? "  (err 0x%08x)\n" : "\n", r);
    }
}

static void send_to_port(int port, const Byte *msg, UInt16 n) {
    Byte buf[128];
    MIDIPacketList *pl = (MIDIPacketList *)buf;
    MIDIPacket *pk = MIDIPacketListInit(pl);
    MIDIPacketListAdd(pl, sizeof buf, pk, 0, n, msg);
    MIDIReceived(G.src[port], pl);
}

static void midi_out_cb(void *ctx, int port, uint8_t cc, uint8_t value) {
    (void)ctx;
    Byte msg[3] = { (Byte)(0xB0 | G.channel), cc, value };
    send_to_port(port, msg, 3);
    if (G.verbose) LOG("midi out [%s]: CC %d = %d", port ? "nightcap" : "main", cc, value);
}

static void mode_changed_cb(void *ctx, int port) {
    (void)ctx;
    Byte msg[8] = { 0xF0, 0x7D, 0x4B, 0x4E, 0x43, NC_SYSEX_MODE_IS, (Byte)port, 0xF7 };
    for (int p = 0; p < NC_NUM_PORTS; p++) send_to_port(p, msg, 8);
    LOG("hardware now controls: %s", port ? "NightCap wrapper" : "DAW / Live");
}

/* ---------- MIDI from DAW -> LEDs ---------- */
static void midi_in_proc(const MIDIPacketList *pl, void *refCon, void *srcRef) {
    (void)srcRef;
    int port = (int)(intptr_t)refCon;
    const MIDIPacket *pk = &pl->packet[0];
    pthread_mutex_lock(&G.lock);
    for (UInt32 i = 0; i < pl->numPackets; i++, pk = MIDIPacketNext(pk)) {
        UInt16 j = 0, len = pk->length;
        while (j < len) {
            Byte st = pk->data[j];
            if (G.sxlen[port]) {                           /* inside sysex (may span packets) */
                if (st >= 0xF8) { j++; continue; }
                if (G.sxlen[port] < sizeof G.sx[port]) G.sx[port][G.sxlen[port]++] = st;
                j++;
                if (st == 0xF7) {
                    if (!handle_hud_sysex(G.sx[port], G.sxlen[port]))
                        nc_on_host_sysex(&G.core, port, G.sx[port], G.sxlen[port]);
                    G.sxlen[port] = 0;
                }
                continue;
            }
            if (st == 0xF0) { G.sx[port][0] = 0xF0; G.sxlen[port] = 1; j++; continue; }
            if (!(st & 0x80) || st >= 0xF0) { j++; continue; }
            Byte hi = st & 0xF0;
            UInt16 size = (hi == 0xC0 || hi == 0xD0) ? 2 : 3;
            if (j + size > len) break;
            if (hi == 0xB0 && (st & 0x0F) == G.channel && G.connected)
                nc_on_host_cc(&G.core, port, pk->data[j + 1] & 0x7F, pk->data[j + 2] & 0x7F);
            j += size;
        }
    }
    pthread_mutex_unlock(&G.lock);
}

/* ---------- USB reader thread ---------- */
static void *reader_main(void *arg) {
    (void)arg;
    uint8_t buf[64];
    while (G.connected) {
        UInt32 n = sizeof buf;
        IOReturn r = (*G.intf)->ReadPipe(G.intf, G.pipe_in, buf, &n);
        if (r != kIOReturnSuccess) {
            if (!G.connected || r == kIOReturnAborted || r == kIOReturnNoDevice || r == kIOReturnNotOpen) break;
            (*G.intf)->ClearPipeStallBothEnds(G.intf, G.pipe_in);
            usleep(10000);
            continue;
        }
        if (G.verbose) {
            fprintf(stderr, "[nightcap] usb in :");
            for (UInt32 i = 0; i < n; i++) fprintf(stderr, " %02X", buf[i]);
            fputc('\n', stderr);
        }
        pthread_mutex_lock(&G.lock);
        nc_on_usb_data(&G.core, buf, n, now_ns());
        pthread_mutex_unlock(&G.lock);
    }
    return NULL;
}

/* ---------- device open/close ---------- */
static void close_device(void) {
    if (!G.dev && !G.intf) return;
    G.connected = 0;
    if (G.intf) (*G.intf)->AbortPipe(G.intf, G.pipe_in);
    if (G.reader) { pthread_join(G.reader, NULL); G.reader = 0; }
    if (G.intf) { (*G.intf)->USBInterfaceClose(G.intf); (*G.intf)->Release(G.intf); G.intf = NULL; }
    if (G.dev)  { (*G.dev)->USBDeviceClose(G.dev);     (*G.dev)->Release(G.dev);   G.dev = NULL; }
    LOG("Controller disconnected");
}

static int open_device(io_service_t service) {
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    if (IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID,
            kIOCFPlugInInterfaceID, &plug, &score) || !plug) { LOG("cannot create device plug-in"); return -1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID650), (LPVOID *)&G.dev);
    IODestroyPlugInInterface(plug);
    if (!G.dev) { LOG("cannot get device interface"); return -1; }

    IOReturn r = (*G.dev)->USBDeviceOpen(G.dev);
    if (r == kIOReturnExclusiveAccess) {
        LOG("device is held by another process (old Novation software/driver?) - seizing");
        r = (*G.dev)->USBDeviceOpenSeize(G.dev);
    }
    if (r) { LOG("USBDeviceOpen failed 0x%08x", r); (*G.dev)->Release(G.dev); G.dev = NULL; return -1; }

    /* Same as the original driver: select configuration 1 (first descriptor) */
    IOUSBConfigurationDescriptorPtr cd;
    if ((*G.dev)->GetConfigurationDescriptorPtr(G.dev, 0, &cd) == kIOReturnSuccess)
        (*G.dev)->SetConfiguration(G.dev, cd->bConfigurationValue);

    IOUSBFindInterfaceRequest req = { kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare,
                                      kIOUSBFindInterfaceDontCare, kIOUSBFindInterfaceDontCare };
    io_iterator_t it;
    (*G.dev)->CreateInterfaceIterator(G.dev, &req, &it);
    io_service_t is = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!is) { LOG("no USB interface found"); close_device(); return -1; }
    plug = NULL;
    IOCreatePlugInInterfaceForService(is, kIOUSBInterfaceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(is);
    if (!plug) { LOG("cannot create interface plug-in"); close_device(); return -1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID650), (LPVOID *)&G.intf);
    IODestroyPlugInInterface(plug);
    if (!G.intf || (*G.intf)->USBInterfaceOpen(G.intf)) { LOG("cannot open interface"); close_device(); return -1; }

    /* Original uses pipe 1 = IN, pipe 2 = OUT; verify by direction */
    UInt8 n = 0; G.pipe_in = 1; G.pipe_out = 2;
    (*G.intf)->GetNumEndpoints(G.intf, &n);
    int fin = 0, fout = 0;
    for (UInt8 p = 1; p <= n; p++) {
        UInt8 dir, num, type, interval; UInt16 maxp;
        if ((*G.intf)->GetPipeProperties(G.intf, p, &dir, &num, &type, &maxp, &interval)) continue;
        if (type != kUSBInterrupt && type != kUSBBulk) continue;
        if (dir == kUSBIn  && !fin)  { G.pipe_in = p;  fin = 1; }
        if (dir == kUSBOut && !fout) { G.pipe_out = p; fout = 1; }
        if (G.verbose) LOG("pipe %d: ep %d dir %s type %d maxpacket %d", p, num, dir == kUSBIn ? "in" : "out", type, maxp);
    }

    G.connected = 1;
    pthread_mutex_lock(&G.lock);
    nc_send_init(&G.core);
    nc_clear_leds(&G.core);
    if (G.core.cfg.absolute_encoders && G.core.cfg.local_feedback)
        for (int i = 0; i < 8; i++) G.core.shadow[G.core.mode][NC_LED_RING_FIRST + i] = G.core.enc_value[i];
    nc_set_mode(&G.core, G.core.mode);              /* repaint LEDs of the active port */
    pthread_mutex_unlock(&G.lock);
    pthread_create(&G.reader, NULL, reader_main, NULL);
    LOG("Controller connected (in pipe %d, out pipe %d)", G.pipe_in, G.pipe_out);
    return 0;
}

static void device_removed(void *refCon, io_service_t service, natural_t type, void *arg) {
    (void)refCon; (void)service; (void)arg;
    if (type == kIOMessageServiceIsTerminated) {
        pthread_mutex_lock(&G.lock); G.connected = 0; pthread_mutex_unlock(&G.lock);
        close_device();
        if (G.term_notification) { IOObjectRelease(G.term_notification); G.term_notification = 0; }
    }
}

static void device_added(void *refCon, io_iterator_t it) {
    (void)refCon;
    io_service_t s;
    while ((s = IOIteratorNext(it))) {
        if (!G.dev) {
            usleep(200000); /* let the device settle */
            if (open_device(s) == 0)
                IOServiceAddInterestNotification(G.notify, s, kIOGeneralInterest, device_removed, NULL, &G.term_notification);
        }
        IOObjectRelease(s);
    }
}

static void on_signal(int sig) {
    (void)sig;
    if (G.connected) {
        pthread_mutex_lock(&G.lock); nc_clear_leds(&G.core); pthread_mutex_unlock(&G.lock);
    }
    close_device();
    _exit(0);
}


/* ===================================================================== HUD */
#define HUD_W 860.0
#define HUD_H 150.0
#define HUD_PAD 14.0

static NSColor *hudOrange(void) { return [NSColor colorWithSRGBRed:1.0 green:0.48 blue:0.10 alpha:1.0]; }
static NSColor *hudRed(void)    { return [NSColor colorWithSRGBRed:0.95 green:0.27 blue:0.36 alpha:1.0]; }

@interface HudView : NSView
@property (nonatomic, strong) NSDictionary *state;
@end

@implementation HudView
- (BOOL)isFlipped { return YES; }

static void drawText(NSString *t, NSRect r, CGFloat size, NSFontWeight w, NSColor *c, NSTextAlignment align) {
    if (!t.length) return;
    NSMutableParagraphStyle *ps = [NSMutableParagraphStyle new];
    ps.lineBreakMode = NSLineBreakByTruncatingTail;
    ps.alignment = align;
    [t drawWithRect:r options:NSStringDrawingUsesLineFragmentOrigin | NSStringDrawingTruncatesLastVisibleLine
         attributes:@{ NSFontAttributeName: [NSFont systemFontOfSize:size weight:w],
                       NSForegroundColorAttributeName: c, NSParagraphStyleAttributeName: ps }
            context:nil];
}

static NSString *str(id v) { return [v isKindOfClass:[NSString class]] ? v : @""; }

- (void)drawRect:(NSRect)dirty {
    NSDictionary *s = self.state;
    if (!s) return;
    CGFloat w = self.bounds.size.width;
    BOOL learn = [s[@"learn"] boolValue];

    drawText(str(s[@"t"]), NSMakeRect(HUD_PAD, 10, w - 260, 20), 15, NSFontWeightBold, NSColor.whiteColor, NSTextAlignmentLeft);
    drawText(str(s[@"p"]), NSMakeRect(w - 240 - HUD_PAD, 12, 240, 18), 12, NSFontWeightMedium,
             [NSColor colorWithWhite:1 alpha:0.6], NSTextAlignmentRight);
    NSString *sub = learn ? @"LEARN  —  touch a knob (or move the crossfader), then move a control in the plug-in" : str(s[@"sub"]);
    drawText(sub, NSMakeRect(HUD_PAD, 31, w - 2 * HUD_PAD, 16), 11, learn ? NSFontWeightSemibold : NSFontWeightRegular,
             learn ? hudRed() : [NSColor colorWithWhite:1 alpha:0.5], NSTextAlignmentLeft);

    NSArray *slots = [s[@"s"] isKindOfClass:[NSArray class]] ? s[@"s"] : @[];
    id xf = s[@"x"];
    BOOL hasX = [xf isKindOfClass:[NSArray class]];
    NSInteger n = 8 + (hasX ? 1 : 0);
    NSInteger touch = [s[@"touch"] integerValue], lt = learn ? [s[@"lt"] integerValue] : -9;
    CGFloat gap = 6, top = 54, ch = self.bounds.size.height - top - HUD_PAD;
    CGFloat cw = (w - 2 * HUD_PAD - gap * (n - 1)) / n;

    for (NSInteger i = 0; i < n; i++) {
        NSArray *slot = (i < 8) ? (i < (NSInteger)slots.count && [slots[i] isKindOfClass:[NSArray class]] ? slots[i] : nil) : xf;
        NSRect r = NSMakeRect(HUD_PAD + i * (cw + gap), top, cw, ch);
        BOOL touched = (i == touch), target = (i == lt);
        NSBezierPath *bg = [NSBezierPath bezierPathWithRoundedRect:r xRadius:7 yRadius:7];
        [(touched ? [hudOrange() colorWithAlphaComponent:0.22] : [NSColor colorWithWhite:1 alpha:0.07]) setFill];
        [bg fill];
        if (touched || target) {
            [(target ? hudRed() : hudOrange()) setStroke];
            bg.lineWidth = 1.5;
            [bg stroke];
        }
        NSRect in = NSInsetRect(r, 8, 6);
        drawText(i < 8 ? [NSString stringWithFormat:@"%ld", (long)i + 1] : @"X-FADE",
                 NSMakeRect(in.origin.x, in.origin.y, in.size.width, 13), 9.5, NSFontWeightMedium,
                 [NSColor colorWithWhite:1 alpha:0.45], NSTextAlignmentLeft);
        NSString *name = slot.count > 0 ? str(slot[0]) : @"";
        drawText(name.length ? name : @"—", NSMakeRect(in.origin.x, in.origin.y + 14, in.size.width, 32), 12.5,
                 NSFontWeightSemibold, name.length ? NSColor.whiteColor : [NSColor colorWithWhite:1 alpha:0.3], NSTextAlignmentLeft);
        NSString *val = slot.count > 1 ? str(slot[1]) : @"";
        drawText(val, NSMakeRect(in.origin.x, NSMaxY(in) - 26, in.size.width, 15), 11.5, NSFontWeightRegular,
                 [NSColor colorWithWhite:1 alpha:0.8], NSTextAlignmentLeft);
        NSRect bar = NSMakeRect(in.origin.x, NSMaxY(in) - 6, in.size.width, 4);
        [[NSColor colorWithWhite:0 alpha:0.35] setFill];
        [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:2 yRadius:2] fill];
        if (slot.count > 2 && name.length) {
            CGFloat v = MAX(0, MIN(1, [slot[2] doubleValue]));
            bar.size.width *= v;
            [hudOrange() setFill];
            [[NSBezierPath bezierPathWithRoundedRect:bar xRadius:2 yRadius:2] fill];
        }
    }
}
@end

typedef NS_ENUM(NSInteger, HudMode) { HudAuto = 0, HudAlways = 1, HudOff = 2 };

@interface HudController : NSObject <NSWindowDelegate, NSMenuDelegate>
@property (nonatomic, strong) NSPanel *panel;
@property (nonatomic, strong) HudView *view;
@property (nonatomic, strong) NSStatusItem *statusItem;
@property (nonatomic, strong) NSTimer *hideTimer;
@property (nonatomic) HudMode mode;
@property (nonatomic) BOOL hiddenByHost;
@end

@implementation HudController
- (instancetype)init {
    if (!(self = [super init])) return nil;
    NSUserDefaults *d = NSUserDefaults.standardUserDefaults;
    _mode = (HudMode)[d integerForKey:@"HudMode"];

    NSRect frame = NSMakeRect(0, 0, HUD_W, HUD_H);
    _panel = [[NSPanel alloc] initWithContentRect:frame
                                        styleMask:NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel
                                          backing:NSBackingStoreBuffered defer:NO];
    _panel.level = NSStatusWindowLevel;
    _panel.opaque = NO;
    _panel.backgroundColor = NSColor.clearColor;
    _panel.hasShadow = YES;
    _panel.movableByWindowBackground = YES;
    _panel.hidesOnDeactivate = NO;
    _panel.floatingPanel = YES;
    _panel.becomesKeyOnlyIfNeeded = YES;
    _panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorFullScreenAuxiliary |
                                NSWindowCollectionBehaviorStationary | NSWindowCollectionBehaviorIgnoresCycle;
    _panel.delegate = self;

    NSVisualEffectView *fx = [[NSVisualEffectView alloc] initWithFrame:frame];
    fx.material = NSVisualEffectMaterialHUDWindow;
    fx.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    fx.state = NSVisualEffectStateActive;
    fx.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    fx.wantsLayer = YES;
    fx.layer.cornerRadius = 12;
    fx.layer.masksToBounds = YES;
    _view = [[HudView alloc] initWithFrame:frame];
    _view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [fx addSubview:_view];
    _panel.contentView = fx;
    [self restorePosition];

    _statusItem = [NSStatusBar.systemStatusBar statusItemWithLength:NSVariableStatusItemLength];
    NSImage *img = nil;
    if (@available(macOS 11.0, *)) img = [NSImage imageWithSystemSymbolName:@"dial.medium" accessibilityDescription:@"NightCap"];
    if (img) { img.template = YES; _statusItem.button.image = img; } else _statusItem.button.title = @"N";
    _statusItem.button.toolTip = @"NightCapBridge";
    [self rebuildMenu];
    return self;
}

- (void)rebuildMenu {
    NSMenu *m = [NSMenu new];
    [m addItemWithTitle:G.connected ? @"Controller connected" : @"Controller not connected" action:nil keyEquivalent:@""];
    [m addItem:NSMenuItem.separatorItem];
    NSArray *titles = @[ @"HUD: show when touched", @"HUD: always visible", @"HUD: off" ];
    for (NSInteger i = 0; i < 3; i++) {
        NSMenuItem *it = [m addItemWithTitle:titles[i] action:@selector(setModeFromMenu:) keyEquivalent:@""];
        it.target = self; it.tag = i; it.state = (i == self.mode) ? NSControlStateValueOn : NSControlStateValueOff;
    }
    NSMenuItem *show = [m addItemWithTitle:@"Show HUD now" action:@selector(showNow:) keyEquivalent:@""];
    show.target = self;
    NSMenuItem *reset = [m addItemWithTitle:@"Reset HUD position" action:@selector(resetPosition:) keyEquivalent:@""];
    reset.target = self;
    [m addItem:NSMenuItem.separatorItem];
    [m addItemWithTitle:@"Quit NightCapBridge" action:@selector(terminate:) keyEquivalent:@"q"];
    m.delegate = self;
    self.statusItem.menu = m;
}
- (void)menuWillOpen:(NSMenu *)menu { [self rebuildMenu]; }

- (void)setModeFromMenu:(NSMenuItem *)it {
    self.mode = (HudMode)it.tag;
    [NSUserDefaults.standardUserDefaults setInteger:self.mode forKey:@"HudMode"];
    if (self.mode == HudOff) [self hideNow];
    else if (self.mode == HudAlways && !self.hiddenByHost) [self showFor:0];
    [self rebuildMenu];
}
- (void)showNow:(id)sender { [self showFor:4.0]; }

- (void)restorePosition {
    NSString *saved = [NSUserDefaults.standardUserDefaults stringForKey:@"HudOrigin"];
    NSScreen *screen = NSScreen.mainScreen;
    NSPoint origin;
    if (saved) origin = NSPointFromString(saved);
    else {
        NSRect vf = screen.visibleFrame;
        origin = NSMakePoint(NSMidX(vf) - HUD_W / 2, NSMinY(vf) + 70);
    }
    [self.panel setFrameOrigin:origin];
    BOOL onScreen = NO;
    for (NSScreen *sc in NSScreen.screens) if (NSIntersectsRect(sc.frame, self.panel.frame)) onScreen = YES;
    if (!onScreen) { [NSUserDefaults.standardUserDefaults removeObjectForKey:@"HudOrigin"]; if (saved) [self restorePosition]; }
}
- (void)resetPosition:(id)sender {
    [NSUserDefaults.standardUserDefaults removeObjectForKey:@"HudOrigin"];
    [self restorePosition];
    [self showFor:3.0];
}
- (void)windowDidMove:(NSNotification *)n {
    [NSUserDefaults.standardUserDefaults setObject:NSStringFromPoint(self.panel.frame.origin) forKey:@"HudOrigin"];
}

- (void)showFor:(NSTimeInterval)seconds {
    if (self.mode == HudOff) { LOG("HUD: not shown because it is set to Off (menu-bar dial icon)"); return; }
    static BOOL logged = NO;
    if (!logged) {
        logged = YES;
        NSRect f = self.panel.frame, sf = (self.panel.screen ?: NSScreen.mainScreen).frame;
        LOG("HUD: panel shown at x=%.0f y=%.0f (screen %.0fx%.0f)", f.origin.x, f.origin.y, sf.size.width, sf.size.height);
    }
    [self.hideTimer invalidate];
    self.hideTimer = nil;
    if (!self.panel.isVisible || self.panel.alphaValue < 1.0) {
        self.panel.alphaValue = 1.0;
        [self.panel orderFrontRegardless];
    }
    if (self.mode == HudAuto && seconds > 0)
        self.hideTimer = [NSTimer scheduledTimerWithTimeInterval:seconds target:self selector:@selector(fadeOut:)
                                                        userInfo:nil repeats:NO];
}
- (void)fadeOut:(NSTimer *)t {
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) {
        ctx.duration = 0.35;
        self.panel.animator.alphaValue = 0.0;
    } completionHandler:^{
        if (self.panel.alphaValue == 0.0) [self.panel orderOut:nil];
    }];
}
- (void)hideNow { [self.hideTimer invalidate]; self.hideTimer = nil; [self.panel orderOut:nil]; }

- (void)update:(NSDictionary *)state {
    if ([state[@"hide"] boolValue]) { self.hiddenByHost = YES; [self hideNow]; return; }
    self.hiddenByHost = NO;
    self.view.state = state;
    self.view.needsDisplay = YES;
    BOOL pop = [state[@"pop"] boolValue];
    BOOL touching = [state[@"touch"] integerValue] >= 0 || [state[@"learn"] boolValue];
    if (self.mode == HudAlways) { if (!self.panel.isVisible) [self showFor:0]; return; }
    if (touching) [self showFor:0];                 /* stay up while a knob is touched / learning */
    else if (pop || self.panel.isVisible) {
        if (pop || self.hideTimer == nil) [self showFor:2.5];
    }
}
@end

static HudController *gHud;

static void hud_deliver(NSData *json) {
    if (!gHud) return;
    NSError *err = nil;
    id obj = [NSJSONSerialization JSONObjectWithData:json options:0 error:&err];
    if ([obj isKindOfClass:[NSDictionary class]]) [gHud update:obj];
    else LOG("HUD: could not read HUD data (%s)", err.localizedDescription.UTF8String);
}

static void usage(const char *a) {
    printf("NightCapBridge - driver for the Novation Nocturn controller on Apple Silicon\n\n"
           "usage: %s [options]\n"
           "  -c N      MIDI channel 1-16 (default 1)\n"
           "  -n NAME   MIDI port base name (default \"NightCap\")\n"
           "  -A        plain MIDI-learn mode: absolute encoders 0-127 with local LED rings\n"
           "            (default is relative encoders + DAW-driven LEDs, for the Live script / NightCap)\n"
           "  -t        buttons toggle on/off instead of momentary\n"
           "  -a        disable encoder acceleration\n"
           "  -H        no HUD / menu-bar icon (headless)\n"
           "  -v        verbose: log all USB and MIDI traffic\n", a);
}

int main(int argc, char **argv) {
    nc_config cfg = { .absolute_encoders = 0, .toggle_buttons = 0, .local_feedback = 0, .acceleration = 1 };
    const char *name = "NightCap";
    int opt;
    G.channel = 0;
    while ((opt = getopt(argc, argv, "c:n:AtaHvh")) != -1) {
        switch (opt) {
        case 'c': G.channel = (atoi(optarg) - 1) & 0x0F; break;
        case 'n': name = optarg; break;
        case 'A': cfg.absolute_encoders = 1; cfg.local_feedback = 1; break;
        case 't': cfg.toggle_buttons = 1; cfg.local_feedback = 1; break;
        case 'a': cfg.acceleration = 0; break;
        case 'H': G.hud_disabled = 1; break;
        case 'v': G.verbose = 1; break;
        default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];   /* no Dock icon */
    if (!G.hud_disabled) {
        gHud = [HudController new];
        /* startup self-test: show the HUD for a few seconds so it's obvious it works */
        dispatch_async(dispatch_get_main_queue(), ^{
            [gHud update:@{ @"t": @"NightCapBridge HUD ready", @"p": @"",
                            @"sub": @"Touch a NightCap knob in Ableton Live to see its parameters here. Drag this panel to move it.",
                            @"s": @[], @"touch": @(-1), @"pop": @NO }];
            [gHud showFor:5.0];
        });
    }
    pthread_mutex_init(&G.lock, NULL);
    nc_init(&G.core, &cfg, usb_write_cb, midi_out_cb, NULL);
    G.core.mode_changed = mode_changed_cb;

    OSStatus st = MIDIClientCreate(CFSTR("NightCapBridge"), NULL, NULL, &G.client);
    for (int p = 0; p < NC_NUM_PORTS && !st; p++) {
        char full[128];
        snprintf(full, sizeof full, p ? "%s Wrapper" : "%s", name);
        CFStringRef cname = CFStringCreateWithCString(NULL, full, kCFStringEncodingUTF8);
        st = MIDISourceCreate(G.client, cname, &G.src[p]);
        if (!st) st = MIDIDestinationCreate(G.client, cname, midi_in_proc, (void *)(intptr_t)p, &G.dst[p]);
        CFRelease(cname);
        if (st) break;
        /* stable unique IDs so DAWs remember the ports across launches */
        MIDIObjectSetIntegerProperty(G.src[p], kMIDIPropertyUniqueID, 0x4B4E5431 + 2 * p);
        MIDIObjectSetIntegerProperty(G.dst[p], kMIDIPropertyUniqueID, 0x4B4E5432 + 2 * p);
        MIDIObjectSetStringProperty(G.src[p], kMIDIPropertyManufacturer, CFSTR("digitui"));
        MIDIObjectSetStringProperty(G.dst[p], kMIDIPropertyManufacturer, CFSTR("digitui"));
    }
    if (st) { LOG("CoreMIDI setup failed (%d)", (int)st); return 1; }

    signal(SIGINT, on_signal); signal(SIGTERM, on_signal);

    G.notify = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(CFRunLoopGetMain(), IONotificationPortGetRunLoopSource(G.notify), kCFRunLoopCommonModes);
    CFMutableDictionaryRef match = IOServiceMatching(kIOUSBDeviceClassName);
    int vid = NC_DEVICE_VID, pid = NC_DEVICE_PID;
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &vid), p = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFDictionarySetValue(match, CFSTR(kUSBVendorID), v);
    CFDictionarySetValue(match, CFSTR(kUSBProductID), p);
    CFRelease(v); CFRelease(p);
    io_iterator_t added;
    IOServiceAddMatchingNotification(G.notify, kIOFirstMatchNotification, match, device_added, NULL, &added);
    LOG("MIDI ports \"%s\" and \"%s Wrapper\" ready on channel %d - waiting for the controller...", name, name, G.channel + 1);
    device_added(NULL, added);   /* handle already-plugged device */

    [NSApp run];
    }
    return 0;
}
