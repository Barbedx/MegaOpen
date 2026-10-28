#include <unity.h>
#include <string.h>
#include <stdio.h>
#include <new>
#include <initializer_list>
#include "proto/canbox/Protocol.h"
#include "proto/canbox/FieldView.h"
#include "proto/canbox/Rav4Emitter.h"
#include "proto/canbox/RaiseFramer.h"
#include "cfg/SettingsRegistry.h"
#include "sinks/CanboxUartSink.h"
#include "vehicle/VehicleState.h"

void setUp() {}
void tearDown() {}

// The firmware knows a head-unit protocol only through Canbox::Protocol and
// Canbox::Emitter (docs/design/rules.md, slice 0): a second protocol is a
// module with its own fields, messages, settings and frame, and the field
// map, FieldView, HuConfig and the settings registry work on it unchanged.
// The stub has a one-byte frame and one message.
using namespace Vehicle;
using namespace Canbox;

namespace
{
    struct StubTuning { uint8_t speedDiv = 100; };

    struct StubEmitter final : Emitter {
        WriteFn w = nullptr;
        void* ctx = nullptr;
        HuConfig cfg;
        uint32_t sent = 0;
        void begin(WriteFn fn, void* c) override { w = fn; ctx = c; }
        void configure(const HuConfig& c) override { cfg = c; }
        void tick(const FieldView&, uint32_t) override {}
        bool preview(uint8_t msg, const FieldView& s, uint8_t* f, uint8_t& len) const override
        {
            if (msg != 0 || !s.has(SIG_SPEED)) return false;
            f[0] = (uint8_t)(s.get(SIG_SPEED) / cfg.tuningAs<StubTuning>().speedDiv);
            len = 1;
            return true;
        }
        bool sendNow(uint8_t msg, const FieldView& s) override
        {
            uint8_t f[4], len = 0;
            if (!preview(msg, s, f, len)) return false;
            w(f, len, ctx);
            ++sent;
            return true;
        }
        bool sendByCmd(uint8_t cmd, const FieldView& s) override { return cmd == 0x55 && sendNow(0, s); }
        uint32_t framesSent() const override { return sent; }
    };

    // One byte per frame, no ack: the byte is the command.
    struct StubDecoder final : Decoder {
        HuInbound in;
        uint32_t n = 0;
        void begin(const HuInbound& i) override { in = i; }
        void configure(const HuConfig&) override {}
        void forgetText() override {}
        TextChannels textChannels() const override { return {}; }
        void feed(uint8_t b) override { ++n; if (in.frame) in.frame(b, nullptr, 0, 0, in.ctx); }
        bool midFrame() const override { return false; }
        void reset() override {}
        DecoderStats stats() const override { DecoderStats s; s.frames = n; return s; }
        const char* dialect() const override { return "stub"; }
        bool wrongDialect() const override { return false; }
    };

    struct Stub final : Protocol {
        const char* id() const override { return "stub"; }
        uint8_t fieldCount() const override { return 2; }
        TargetView field(uint8_t i) const override
        {
            TargetView v{};
            v.id  = i ? "eng_temp" : "speed";
            v.sig = i ? SIG_ENG_TEMP : SIG_SPEED;
            v.fw  = true;
            v.msg = i ? NO_MSG : 0;
            return v;
        }
        const FieldMap& defaults() const override
        {
            static const FieldMap m = [] {
                FieldMap d;
                FieldRule r;
                r.field = SIG_ENG_TEMP;
                r.kind = FK_OFF;
                d.set(r);
                return d;
            }();
            return m;
        }
        uint8_t     messageCount() const override { return 1; }
        MessageView message(uint8_t) const override { return {"speed", 0x55, 100, "low", ""}; }
        size_t   emitterSize() const override { return sizeof(StubEmitter); }
        Emitter* makeEmitter(void* mem) const override { return new (mem) StubEmitter(); }
        // A key is [0xE0 | state][code]; mute is a code of its own (0x11).
        uint8_t keyFrame(uint8_t code, uint8_t state, uint8_t* out) const override
        {
            out[0] = (uint8_t)(0xE0 | state);
            out[1] = code;
            return 2;
        }
        uint8_t keyCodeFor(Keys::Action a) const override
        {
            return a == Keys::Action::VolUp ? 0x10 : a == Keys::Action::Mute ? 0x11 : 0;
        }
        bool    isKnobCode(uint8_t) const override { return false; }
        bool    muteIsLong() const override { return false; }
        uint8_t keyCodeCount() const override { return 2; }
        KeyCodeView keyCode(uint8_t i) const override { return i ? KeyCodeView{0x11, "mute", "low", ""} : KeyCodeView{0x10, "vol+", "low", ""}; }

        size_t   decoderSize() const override { return sizeof(StubDecoder); }
        Decoder* makeDecoder(void* mem) const override { return new (mem) StubDecoder(); }
        int16_t  ackByte() const override { return -1; }
        bool unframe(const uint8_t* f, uint8_t len, uint8_t& cmd, const uint8_t*& p, uint8_t& pl, uint16_t& t) const override
        {
            if (len != 1) return false;
            cmd = f[0]; p = f + 1; pl = 0; t = 0;
            return true;
        }
        uint8_t frame(uint8_t cmd, const uint8_t*, uint8_t len, uint8_t* out) const override
        {
            if (len) return 0;
            out[0] = cmd;
            return 1;
        }
        void        initTuning(void* t) const override { new (t) StubTuning(); }
        const char* tuningKey() const override { return "stubtune"; }
        uint8_t     tuningBlobSize() const override { return 1; }
        void tuningToBlob(const void* t, uint8_t* out) const override { out[0] = static_cast<const StubTuning*>(t)->speedDiv; }
        bool tuningFromBlob(void* t, const uint8_t* in, uint8_t len) const override
        {
            if (len != 1 || !in[0]) return false;
            static_cast<StubTuning*>(t)->speedDiv = in[0];
            return true;
        }
        size_t settingCount() const override { return 0; }
        const Cfg::SEntry& setting(size_t) const override { return Cfg::Registry::at(0); }
    };

    uint8_t g_frame[8], g_len;
    void capture(const uint8_t* f, uint8_t n, void*) { memcpy(g_frame, f, n); g_len = n; }

    // The emitter a protocol makes, in 8-aligned storage, as the hu task holds it.
    struct Made {
        alignas(8) uint8_t mem[4096];
        Emitter* e;
        explicit Made(const Protocol& p) { TEST_ASSERT_TRUE(p.emitterSize() <= sizeof(mem)); e = p.makeEmitter(mem); }
        ~Made() { e->~Emitter(); }
    };
}

void test_a_second_protocol_runs_the_map_unchanged()
{
    const Stub p;
    TEST_ASSERT_EQUAL_INT(1, p.findField("eng_temp"));
    TEST_ASSERT_EQUAL_INT(-1, p.findField("rpm"));
    State s;
    s.set(SIG_SPEED, 4700, 1);
    s.set(SIG_ENG_TEMP, 90, 1);
    FieldMap own, eff;
    // Its default: the coolant field starts off; the speed is its own signal.
    p.withDefaults(own, eff);
    const FieldView v(s, &eff);
    TEST_ASSERT_TRUE(v.has(SIG_SPEED));
    TEST_ASSERT_FALSE(v.has(SIG_ENG_TEMP));
    TEST_ASSERT_EQUAL_UINT8(FK_OFF, p.ruleFor(own, SIG_ENG_TEMP)->kind);
    TEST_ASSERT_NULL(p.ruleFor(own, SIG_SPEED));
    // The owner's rule goes over the default: a constant.
    FieldRule k;
    k.field = SIG_ENG_TEMP;
    k.kind = FK_CONST;
    k.add = 70;
    own.set(k);
    p.withDefaults(own, eff);
    const FieldView w(s, &eff);
    TEST_ASSERT_TRUE(w.has(SIG_ENG_TEMP));
    TEST_ASSERT_EQUAL_INT32(70, w.get(SIG_ENG_TEMP));
    TEST_ASSERT_EQUAL_UINT8(FK_CONST, p.ruleFor(own, SIG_ENG_TEMP)->kind);
}

// The hu task's whole use of a protocol: a config with its tuning, an
// emitter made and configured, a send, the head unit's request, a raw frame.
void test_a_second_protocol_sends_through_the_interface()
{
    const Stub p;
    HuConfig c;
    p.initConfig(c);
    TEST_ASSERT_EQUAL_UINT8(100, c.tuningAs<StubTuning>().speedDiv);
    c.tuningAs<StubTuning>().speedDiv = 10;
    Made m(p);
    m.e->begin(capture, nullptr);
    m.e->configure(c);
    State s;
    s.set(SIG_SPEED, 470, 1);
    g_len = 0;
    TEST_ASSERT_TRUE(m.e->sendNow(0, s));
    TEST_ASSERT_EQUAL_UINT8(1, g_len);
    TEST_ASSERT_EQUAL_UINT8(47, g_frame[0]);
    TEST_ASSERT_TRUE(m.e->sendByCmd(0x55, s));
    TEST_ASSERT_FALSE(m.e->sendByCmd(0x56, s));
    TEST_ASSERT_EQUAL_UINT32(2, m.e->framesSent());
    uint8_t f[4];
    TEST_ASSERT_EQUAL_UINT8(1, p.frame(0x42, nullptr, 0, f));
    TEST_ASSERT_EQUAL_HEX8(0x42, f[0]);
    // Its tuning stored and read back; a bad blob leaves it as it was.
    uint8_t b[1];
    p.tuningToBlob(c.tuning, b);
    HuConfig d;
    p.initConfig(d);
    TEST_ASSERT_TRUE(p.tuningFromBlob(d.tuning, b, 1));
    TEST_ASSERT_EQUAL_UINT8(10, d.tuningAs<StubTuning>().speedDiv);
    const uint8_t zero = 0;
    TEST_ASSERT_FALSE(p.tuningFromBlob(d.tuning, &zero, 1));
    TEST_ASSERT_EQUAL_UINT8(10, d.tuningAs<StubTuning>().speedDiv);
}

// The key sink's timing is every protocol's; what goes on the wire is the
// protocol's: its frame, its codes, and mute as a code of its own here.
namespace
{
    uint8_t g_keys[8][4];
    uint8_t g_nKeys = 0;
    void keyOut(const uint8_t* f, uint8_t n, void*) { if (g_nKeys < 8 && n <= 4) memcpy(g_keys[g_nKeys++], f, n); }
}

void test_a_second_protocol_takes_the_keys()
{
    const Stub p;
    TEST_ASSERT_EQUAL_STRING("mute", p.keyCodeName(0x11));
    TEST_ASSERT_EQUAL_STRING("?", p.keyCodeName(0x55));
    Sinks::CanboxUartSink k;
    k.setProtocol(p);
    k.begin(keyOut, nullptr);
    g_nKeys = 0;
    Keys::Event e{};
    e.action = Keys::Action::Mute;
    e.state = Keys::State::Press;
    k.onKey(e);
    TEST_ASSERT_EQUAL_UINT8(1, g_nKeys);
    TEST_ASSERT_EQUAL_HEX8(0xE0 | KEY_PRESS, g_keys[0][0]);   // a plain press: no long mute here
    TEST_ASSERT_EQUAL_HEX8(0x11, g_keys[0][1]);
    TEST_ASSERT_TRUE(k.sendKey(0x10, KEY_RELEASE));
    TEST_ASSERT_EQUAL_HEX8(0xE0, g_keys[1][0]);
    TEST_ASSERT_EQUAL_HEX8(0x10, g_keys[1][1]);
}

void test_the_active_protocol_is_raise_112()
{
    const Protocol& p = activeProtocol();
    TEST_ASSERT_EQUAL_STRING("raise112", p.id());
    TEST_ASSERT_EQUAL_UINT8(1, Protocols::count());
    TEST_ASSERT_EQUAL_INT(0, Protocols::find("raise112"));
    TEST_ASSERT_EQUAL_INT(-1, Protocols::find("nope"));
    Protocols::select(7);                               // out of range: the first
    TEST_ASSERT_EQUAL_STRING("raise112", activeProtocol().id());
    TEST_ASSERT_TRUE(p.fieldCount() > 50);
    const int i = p.findField("speed");
    TEST_ASSERT_TRUE(i >= 0);
    TEST_ASSERT_EQUAL_UINT8(SIG_SPEED, p.field((uint8_t)i).sig);
    // The fields that start off (Rav4Emitter::defaults).
    FieldMap none;
    for (const uint8_t f : {SIG_ENG_TEMP, SIG_PRND, SIG_FUEL_PCT})
        TEST_ASSERT_EQUAL_UINT8(FK_OFF, p.ruleFor(none, f)->kind);
    TEST_ASSERT_EQUAL_UINT8(Rav4Emitter::MSG_COUNT, p.messageCount());
    TEST_ASSERT_EQUAL_STRING("speed 0x7D03", p.message(Rav4Emitter::MSG_SPEED).name);
    TEST_ASSERT_EQUAL_UINT8(0x7D, p.message(Rav4Emitter::MSG_SPEED).cmd);
    // Keys: SOURCE held is mute, the wheel is a rotary code.
    TEST_ASSERT_TRUE(p.muteIsLong());
    TEST_ASSERT_EQUAL_HEX8(0x07, p.keyCodeFor(Keys::Action::Mute));
    TEST_ASSERT_TRUE(p.isKnobCode(p.keyCodeFor(Keys::Action::EncCw)));
    uint8_t kf[8];
    TEST_ASSERT_EQUAL_UINT8(6, p.keyFrame(0x01, KEY_PRESS, kf));
    const uint8_t volUp[] = {0x2E, 0x20, 0x02, 0x01, 0x01};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(volUp, kf, sizeof(volUp));
}

// The interface changes no byte: the emitter made through Raise 112 and
// configured with a HuConfig sends what a Rav4Emitter set up by hand does.
void test_raise_112_through_the_interface_sends_the_same_bytes()
{
    const Protocol& p = activeProtocol();
    HuConfig c;
    p.initConfig(c);
    c.enabled = true;
    Tuning& t = c.tuningAs<Tuning>();
    TEST_ASSERT_EQUAL_UINT8(10, t.steerDiv);            // Tuning's defaults
    t.cmd28 = 1;
    t.histLen = 3;
    Made m(p);
    Rav4Emitter direct;
    Tuning dt;
    dt.cmd28 = 1;
    dt.histLen = 3;
    direct.setTuning(dt);
    State s;
    s.set(SIG_SPEED, 4700, 1);
    s.set(SIG_STEERING, -265, 1);
    s.set(SIG_FUEL_INST, 72, 1);
    s.set(SIG_AC_ON, 1, 1);
    m.e->configure(c);
    for (uint8_t i = 0; i < p.messageCount(); ++i) {
        uint8_t a[64], b[64], la = 0, lb = 0;
        const bool oa = m.e->preview(i, s, a, la), ob = direct.preview(i, s, b, lb);
        TEST_ASSERT_EQUAL_MESSAGE(ob, oa, p.message(i).name);
        if (oa) {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(lb, la, p.message(i).name);
            TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(b, a, la, p.message(i).name);
        }
    }
    // The raw frame is Raise's: 2E cmd len payload checksum.
    uint8_t f[8];
    const uint8_t pl[] = {0x01, 0x02};
    TEST_ASSERT_EQUAL_UINT8(6, p.frame(0x20, pl, 2, f));
    TEST_ASSERT_EQUAL_HEX8(0x2E, f[0]);
    // The tuning blob round-trips through the protocol (NVS "rav4tune").
    TEST_ASSERT_EQUAL_STRING("rav4tune", p.tuningKey());
    uint8_t blob[64];
    p.tuningToBlob(c.tuning, blob);
    HuConfig d;
    p.initConfig(d);
    TEST_ASSERT_TRUE(p.tuningFromBlob(d.tuning, blob, p.tuningBlobSize()));
    TEST_ASSERT_EQUAL_UINT8(1, d.tuningAs<Tuning>().cmd28);
    TEST_ASSERT_EQUAL_UINT8(3, d.tuningAs<Tuning>().histLen);
    // A layout-1 blob (before the text channels) still reads, channels 0.
    uint8_t v1[Tuning::BLOB_SIZE_V1];
    memcpy(v1, blob, sizeof(v1));
    v1[0] = 1;
    d.tuningAs<Tuning>().titleCmd = 7;
    TEST_ASSERT_TRUE(p.tuningFromBlob(d.tuning, v1, sizeof(v1)));
    TEST_ASSERT_EQUAL_UINT8(3, d.tuningAs<Tuning>().histLen);
    TEST_ASSERT_EQUAL_UINT8(0, d.tuningAs<Tuning>().titleCmd);
    TEST_ASSERT_FALSE(p.tuningFromBlob(d.tuning, blob, sizeof(v1)));   // version 2 with layout 1's length
    // 0x28 carries one form: the other one's fields are blocked, with why.
    TEST_ASSERT_EQUAL_STRING("form_ac", p.messageBlocked(Rav4Emitter::MSG_TEMP, c));
    TEST_ASSERT_NULL(p.messageBlocked(Rav4Emitter::MSG_AC, c));
    t.cmd28 = 0;
    TEST_ASSERT_EQUAL_STRING("form_temp", p.messageBlocked(Rav4Emitter::MSG_AC, c));
    TEST_ASSERT_NULL(p.messageBlocked(Rav4Emitter::MSG_SPEED, c));
}

// What the head unit says, through Raise 112's decoder: the ack owed at the
// end of every frame, the clock, a request, the unit's own ack of ours, its
// NACK, and a unit set to the VW profile.
namespace
{
    struct Heard {
        int acksDue = 0, frames = 0, junk = 0, acksIn = 0, h = -1, m = -1, req = -1;
        uint16_t total = 0;
    };
    Heard g_h;
    HuInbound heard()
    {
        HuInbound in;
        in.ackDue  = [](void*) { ++g_h.acksDue; };
        in.frame   = [](uint8_t, const uint8_t*, uint8_t, uint16_t t, void*) { ++g_h.frames; g_h.total = t; };
        in.junk    = [](uint8_t, void*) { ++g_h.junk; };
        in.ackIn   = [](void*) { ++g_h.acksIn; };
        in.clock   = [](uint8_t h, uint8_t m, void*) { g_h.h = h; g_h.m = m; };
        in.request = [](uint8_t c, void*) { g_h.req = c; };
        return in;
    }
    void say(Decoder& d, uint8_t cmd, std::initializer_list<uint8_t> p)
    {
        uint8_t pl[16], f[24], n = 0;
        for (uint8_t b : p) pl[n++] = b;
        const uint8_t len = buildFrame(cmd, pl, n, f);
        for (uint8_t i = 0; i < len; ++i) d.feed(f[i]);
    }
    struct MadeDecoder {
        alignas(8) uint8_t mem[1024];
        Decoder* d;
        explicit MadeDecoder(const Protocol& p) { TEST_ASSERT_TRUE(p.decoderSize() <= sizeof(mem)); d = p.makeDecoder(mem); }
        ~MadeDecoder() { d->~Decoder(); }
    };
}

void test_raise_112_hears_the_head_unit_through_the_interface()
{
    const Protocol& p = activeProtocol();
    TEST_ASSERT_EQUAL_INT16(0xFF, p.ackByte());
    MadeDecoder md(p);
    Decoder& d = *md.d;
    g_h = Heard{};
    d.begin(heard());
    say(d, 0x9D, {12, 34, 0});                        // the clock
    TEST_ASSERT_EQUAL_INT(12, g_h.h);
    TEST_ASSERT_EQUAL_INT(34, g_h.m);
    TEST_ASSERT_EQUAL_INT(1, g_h.acksDue);
    TEST_ASSERT_EQUAL_UINT16(3, g_h.total);
    say(d, 0x9D, {25, 0, 0});                         // out of range: no clock
    TEST_ASSERT_EQUAL_INT(12, g_h.h);
    say(d, 0x90, {0x24});                             // a request, DUDU's len-1 form
    TEST_ASSERT_EQUAL_INT(0x24, g_h.req);
    TEST_ASSERT_EQUAL_INT(3, g_h.frames);
    TEST_ASSERT_EQUAL_INT(3, g_h.acksDue);
    d.feed(0xFF);                                     // its ack of ours: not junk
    d.feed(0xF3);                                     // its NACK: junk, and counted
    TEST_ASSERT_EQUAL_INT(1, g_h.acksIn);
    TEST_ASSERT_EQUAL_INT(1, g_h.junk);
    DecoderStats st = d.stats();
    TEST_ASSERT_EQUAL_UINT32(3, st.frames);
    TEST_ASSERT_EQUAL_UINT32(1, st.notSupported);
    TEST_ASSERT_EQUAL_STRING("toyota", d.dialect());
    TEST_ASSERT_FALSE(d.wrongDialect());
    // Mid-frame, then abandoned: a desync, and the lanes may open again.
    d.feed(SOF);
    d.feed(0x9D);
    TEST_ASSERT_TRUE(d.midFrame());
    d.reset();
    TEST_ASSERT_FALSE(d.midFrame());
    TEST_ASSERT_EQUAL_UINT32(1, d.stats().desyncs);
    // A unit on the VW profile: wired right, set wrong.
    MadeDecoder vw(p);
    vw.d->begin(heard());
    for (int i = 0; i < 3; ++i) say(*vw.d, 0xC1, {1});
    TEST_ASSERT_EQUAL_STRING("vw", vw.d->dialect());
    TEST_ASSERT_TRUE(vw.d->wrongDialect());
    // Track text: DUDU's structured 0x99 field becomes a Title, and the ring
    // can show it.
    {
        static char got[32];
        got[0] = 0;
        HuInbound in = heard();
        in.text = [](HuText f, const char* t, void*) { if (f == HuText::Title) snprintf(got, sizeof(got), "%s", t); };
        MadeDecoder td(p);
        td.d->begin(in);
        uint8_t pl[12] = {0x83, 0x02};
        const char* s = "Song";
        for (uint8_t i = 0; s[i]; ++i) pl[2 + 2 * i] = (uint8_t)s[i];
        uint8_t fr[24];
        const uint8_t fl = buildFrame(0x99, pl, sizeof(pl), fr);
        for (uint8_t i = 0; i < fl; ++i) td.d->feed(fr[i]);
        TEST_ASSERT_EQUAL_STRING("Song", got);
        TEST_ASSERT_EQUAL_UINT32(1, td.d->stats().textFrames);
        char t[16];
        uint8_t field = 0;
        TEST_ASSERT_TRUE(p.frameText(pl, sizeof(pl), t, sizeof(t), field));
        TEST_ASSERT_EQUAL_UINT8(2, field);
        TEST_ASSERT_EQUAL_STRING("Song", t);
        // Pinning a channel clears what was shown, at once.
        static int clears = 0;
        in.text = [](HuText f, const char*, void*) { clears += f == HuText::Clear; };
        td.d->begin(in);
        HuConfig c;
        p.initConfig(c);
        c.tuningAs<Tuning>().titleCmd = 0xC0;
        td.d->configure(c);
        TEST_ASSERT_EQUAL_INT(1, clears);
        TEST_ASSERT_EQUAL_UINT8(0xC0, td.d->textChannels().titleCmd);
        td.d->configure(c);                               // unchanged: no clear
        TEST_ASSERT_EQUAL_INT(1, clears);
    }
    // A frame of ours, unwrapped for the ring.
    uint8_t f[16], cmd = 0, plen = 0;
    const uint8_t pl[] = {0x07, 0x01};
    const uint8_t n = p.frame(0x20, pl, 2, f);
    const uint8_t* pp = nullptr;
    uint16_t total = 0;
    TEST_ASSERT_TRUE(p.unframe(f, n, cmd, pp, plen, total));
    TEST_ASSERT_EQUAL_HEX8(0x20, cmd);
    TEST_ASSERT_EQUAL_UINT8(2, plen);
    TEST_ASSERT_EQUAL_HEX8(0x07, pp[0]);
    TEST_ASSERT_FALSE(p.unframe(pl, 2, cmd, pp, plen, total));
}

// The registry carries the protocol's rows after its own, its group first.
void test_the_registry_takes_the_protocol_settings()
{
    const Protocol& p = activeProtocol();
    TEST_ASSERT_TRUE(p.settingCount() > 5);
    TEST_ASSERT_EQUAL_STRING("raise", p.setting(0).id);
    const size_t n = Cfg::Registry::count();
    TEST_ASSERT_EQUAL_STRING(p.setting(p.settingCount() - 1).id, Cfg::Registry::at(n - 1).id);
    const Cfg::SEntry* e = Cfg::Registry::find("raise.cmd28");
    TEST_ASSERT_NOT_NULL(e);
    // Its get/put edit HuConfig::tuning through Model::hu.
    HuConfig c;
    p.initConfig(c);
    Cfg::Settings s;
    Cfg::Model m{&s, nullptr, nullptr, nullptr, c.tuning, c.msgOn};
    TEST_ASSERT_TRUE(Cfg::Registry::available(m, *e));
    TEST_ASSERT_TRUE(Cfg::Registry::set(m, *e, 1));
    TEST_ASSERT_EQUAL_UINT8(1, c.tuningAs<Tuning>().cmd28);
    Cfg::Model none{&s, nullptr};
    TEST_ASSERT_FALSE(Cfg::Registry::available(none, *e));
    // The protocol is a setting, read at boot.
    const Cfg::SEntry* hp = Cfg::Registry::find("huProtocol");
    TEST_ASSERT_NOT_NULL(hp);
    TEST_ASSERT_TRUE(hp->apply & Cfg::AP_REBOOT);
    TEST_ASSERT_EQUAL_INT(0, Cfg::Registry::get(m, *hp));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_second_protocol_runs_the_map_unchanged);
    RUN_TEST(test_a_second_protocol_sends_through_the_interface);
    RUN_TEST(test_a_second_protocol_takes_the_keys);
    RUN_TEST(test_the_active_protocol_is_raise_112);
    RUN_TEST(test_raise_112_through_the_interface_sends_the_same_bytes);
    RUN_TEST(test_raise_112_hears_the_head_unit_through_the_interface);
    RUN_TEST(test_the_registry_takes_the_protocol_settings);
    return UNITY_END();
}
