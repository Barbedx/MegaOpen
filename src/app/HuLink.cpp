#include "HuLink.h"
#include "../Features.h"
#include "HuTask.h"
#include "Media.h"
#include "../board/BoardProfile.h"
#include "../cfg/CarStore.h"
#include "../media/HuMediaSource.h"
#include "../sinks/UartTx.h"
#include "../utils/JsonEscape.h"
#include "../utils/Log.h"
#include "../utils/LogBus.h"
#include <esp_heap_caps.h>
#if FEATURE_DISPLAY_CAN
#include "UiTask.h"
#endif
#if FEATURE_CAPTURE
#include "../capture/CanRecorder.h"
#endif

namespace App
{
    namespace HuLink
    {
        namespace
        {
            Cfg::Config* s_cfg = nullptr;
            UartTx       s_tx;
            // The head-unit protocol's live emitter (Canbox::Protocol::makeEmitter), made
            // once at boot in internal RAM; the hu task's from App::Hu::begin on.
            Canbox::Emitter* s_emitter = nullptr;
            // The head-unit protocol's decoder (inbound), made at boot like the emitter.
            Canbox::Decoder* s_huRx = nullptr;

            // A ring of recent head-unit frames, so traffic is kept without a page open
            // (track-change text, 0x90 "send me command X" requests). 24 entries.
            struct HuEntry {
                char     dir;            // 'T' ours, 'R' a framed reply, 'J' junk
                uint8_t  cmd;
                uint8_t  len;            // bytes stored, not bytes seen
                uint16_t total;          // bytes the frame declared, so truncation shows
                uint32_t atMs;
                // 64: the head unit's now-playing text is UTF-16LE (2 bytes a char).
                // `total` keeps the declared length, so truncation stays visible.
                uint8_t  data[64];
            };
            constexpr uint8_t kHuRingLen = 24;
            HuEntry  g_huRing[kHuRingLen] = {};
            uint8_t  g_huHead = 0;
            uint32_t g_huCount = 0;
            // The head unit's own frames (R, J) again, apart: ours go out every 200 ms
            // and push its rare ones (track text, 0x90, the clock) out of the shared
            // ring within seconds. The web's exchange log reads this one.
            constexpr uint8_t kHuInLen = 12;
            HuEntry  g_huIn[kHuInLen] = {};
            uint8_t  g_huInHead = 0;
            // Two writers: the keys task (its key frames, through uartTap) and the hu
            // task (everything else). An entry is ~80 bytes; the copy under the
            // spinlock is a few microseconds. Readers copy an entry out the same way.
            portMUX_TYPE g_huMux = portMUX_INITIALIZER_UNLOCKED;

            uint32_t g_huPrinted = 0;

            void huRecord(char dir, uint8_t cmd, const uint8_t* p, uint8_t len, uint16_t total)
            {
                const uint8_t n = len > sizeof(HuEntry::data) ? (uint8_t)sizeof(HuEntry::data) : len;
                const uint32_t now = millis();
                portENTER_CRITICAL(&g_huMux);
                HuEntry& e = g_huRing[g_huHead];
                e.dir = dir;
                e.cmd = cmd;
                e.total = total;
                e.atMs = now;
                e.len = n;
                memcpy(e.data, p, n);
                g_huHead = (uint8_t)((g_huHead + 1) % kHuRingLen);
                ++g_huCount;
                if (dir == 'R' || dir == 'J') {
                    g_huIn[g_huInHead] = e;
                    g_huInHead = (uint8_t)((g_huInHead + 1) % kHuInLen);
                }
                portEXIT_CRITICAL(&g_huMux);
#if FEATURE_CAPTURE
                // Every frame both ways into the capture, bus 2 (docs/design/logging.md):
                // records of 8 bytes, id = cmd | dir << 8 | piece << 11 | len << 15
                // (dir 0 T, 1 V, 2 R, 3 J). Capture::push never waits.
                const uint8_t d = dir == 'T' ? 0 : dir == 'V' ? 1 : dir == 'R' ? 2 : 3;
                for (uint8_t k = 0; k == 0 || k * 8 < n; ++k) {
                    Frame f;
                    const uint8_t at = (uint8_t)(k * 8);
                    f.id = (uint32_t)cmd | ((uint32_t)d << 8) | ((uint32_t)k << 11) | ((uint32_t)n << 15);
                    f.len = (uint8_t)(n - at < 8 ? n - at : 8);
                    if (f.len) memcpy(f.data, p + at, f.len);
                    Capture::push(f, Capture::HU, now);
                }
#endif
            }

            // One entry, copied whole: a writer on the other task cannot tear it.
            void huEntryAt(uint8_t idx, HuEntry& out)
            {
                portENTER_CRITICAL(&g_huMux);
                out = g_huRing[idx % kHuRingLen];
                portEXIT_CRITICAL(&g_huMux);
            }
            // i-th newest of the head unit's own frames.
            void huInNewest(uint8_t i, HuEntry& out)
            {
                portENTER_CRITICAL(&g_huMux);
                out = g_huIn[(uint8_t)((g_huInHead + kHuInLen - 1 - i) % kHuInLen)];
                portEXIT_CRITICAL(&g_huMux);
            }

            // The readable column; decodes ASCII-range UTF-16LE (the head unit's track text)
            // when one byte parity is all zero, trying header offsets 0-3. Returns true when
            // it decoded wide characters.
            static bool huTextColumn(const uint8_t* d, uint8_t len, char* out, size_t cap)
            {
                size_t n = 0;
                auto emit = [&](uint8_t c) {
                    if (n + 1 < cap) out[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                };

                // Strip trailing NUL padding first: the 0x99 text field is a fixed 32 bytes,
                // and zeroes satisfy both parities.
                uint8_t end = len;
                while (end && !d[end - 1]) --end;

                // A header shifts the parity: 0x99 carries `83 <field>` before the text.
                for (uint8_t hdr = 0; hdr <= 3 && (uint8_t)(hdr + 4) <= end; ++hdr) {
                    bool zeros = true, anyText = false;
                    // NUL half all zero; the other half has at least one printable char.
                    for (uint8_t i = (uint8_t)(hdr + 1); i < end; i = (uint8_t)(i + 2))
                        if (d[i]) { zeros = false; break; }
                    for (uint8_t i = hdr; i < end; i = (uint8_t)(i + 2))
                        if (d[i] >= 0x20 && d[i] < 0x7F) { anyText = true; break; }
                    if (!zeros || !anyText) continue;
                    for (uint8_t i = hdr; i < end; i = (uint8_t)(i + 2)) emit(d[i]);
                    out[n] = '\0';
                    return true;
                }

                for (uint8_t i = 0; i < len; ++i) emit(d[i]);
                out[n] = '\0';
                return false;
            }

            // Print what was recorded since the last pass, at TRC ("hu": every frame; the
            // capture keeps them all anyway, bus 2). The hu task only. Each line is
            // `T [hu] <dir> ...`; R is left to onHuFrame's decoded line (DBG). Prints the
            // canonical frame `2E <cmd> <len> ... <cs>` (checksum recomputed) to match the
            // head unit's CANBUS Info screen; a truncated entry prints its payload and says
            // so. At most HU_ECHO_PER_PASS lines a pass: a burst of head-unit traffic is
            // caught up over the next passes (or counted as lost), never formatted all at
            // once in front of an ack.
            static constexpr uint8_t HU_ECHO_PER_PASS = 4;
            static void huDrainSerial()
            {
                const uint32_t count = g_huCount;
                static int8_t s_huTag = -1;
                if (!Log::on(LogLevel::TRC, "hu", s_huTag)) { g_huPrinted = count; return; }
                // Report frames lost when the console falls behind the ring.
                if (count - g_huPrinted > kHuRingLen) {
                    LOGI("hu", "... %u frames lost (console behind the ring)",
                                   (unsigned)(count - g_huPrinted - kHuRingLen));
                    g_huPrinted = count - kHuRingLen;
                }
                for (uint8_t lines = 0; g_huPrinted < count && lines < HU_ECHO_PER_PASS; ++lines) {
                    // Static: the hu task's stack is 3 kB, and this is its only caller.
                    static HuEntry e;
                    huEntryAt((uint8_t)(g_huPrinted % kHuRingLen), e);
                    ++g_huPrinted;
                    if (!e.dir || e.dir == 'R') continue;

                    // Sized for 64 payload bytes (192 hex chars) plus the text column.
                    static char b[400];
                    int n = snprintf(b, sizeof(b), "T [hu] %c ", e.dir);
                    if (e.dir == 'J') {
                        // Junk: bytes outside any framing, printed raw.
                        n += snprintf(b + n, sizeof(b) - n, "junk  ");
                        for (uint8_t k = 0; k < e.len && n < (int)sizeof(b) - 4; ++k)
                            n += snprintf(b + n, sizeof(b) - n, "%02X ", e.data[k]);
                    } else if (e.len == e.total) {
                        uint8_t sum = (uint8_t)(e.cmd + e.total);
                        for (uint8_t k = 0; k < e.len; ++k) sum = (uint8_t)(sum + e.data[k]);
                        n += snprintf(b + n, sizeof(b) - n, "2E %02X %02X ", e.cmd, e.total);
                        for (uint8_t k = 0; k < e.len && n < (int)sizeof(b) - 8; ++k)
                            n += snprintf(b + n, sizeof(b) - n, "%02X ", e.data[k]);
                        n += snprintf(b + n, sizeof(b) - n, "%02X", (uint8_t)(sum ^ 0xFF));
                    } else {
                        n += snprintf(b + n, sizeof(b) - n, "cmd=%02X len=%u (stored %u) ",
                                      e.cmd, (unsigned)e.total, (unsigned)e.len);
                        for (uint8_t k = 0; k < e.len && n < (int)sizeof(b) - 4; ++k)
                            n += snprintf(b + n, sizeof(b) - n, "%02X ", e.data[k]);
                    }

                    // The text column.
                    if (n < (int)sizeof(b) - 80 && e.len) {
                        char txt[72];
                        const bool wide = huTextColumn(e.data, e.len, txt, sizeof(txt));
                        // "u16" marks a UTF-16LE decode.
                        snprintf(b + n, sizeof(b) - n, "  %s|%s|", wide ? "u16" : "", txt);
                    }
                    LogBus::line(b);
                }
            }

            // Newest first, same as the CAN ring. `inOnly`: the head unit's frames alone.
            // Streamed into the reply (ChunkOut): 24 rows of ~300 characters used to be
            // one String grown by += on httpd, in internal heap, under the settings lock.
            void lastFramesJsonImpl(bool inOnly, Catalog::Out& out)
            {
                char h[64];
                out.put(h, (size_t)snprintf(h, sizeof(h), "{\"total\":%lu,\"baud\":%lu,\"frames\":[",
                                            (unsigned long)g_huCount, (unsigned long)s_cfg->s().uartBaud));
                const uint8_t head = g_huHead;
                const uint32_t now = millis();
                const uint8_t rows = inOnly ? kHuInLen : kHuRingLen;
                bool first = true;
                for (uint8_t i = 0; i < rows; ++i) {
                    HuEntry e;
                    if (inOnly) huInNewest(i, e);
                    else        huEntryAt((uint8_t)((head + kHuRingLen - 1 - i) % kHuRingLen), e);
                    if (!e.dir) continue;
                    char b[420];
                    int n = snprintf(b, sizeof(b),
                                     "%s{\"dir\":\"%c\",\"cmd\":\"%02X\",\"len\":%u,\"total\":%u,\"ageMs\":%u,\"data\":\"",
                                     first ? "" : ",", e.dir, (unsigned)e.cmd,
                                     (unsigned)e.len, (unsigned)e.total,
                                     (unsigned)(now - e.atMs));
                    first = false;
                    for (uint8_t k = 0; k < e.len; ++k)
                        n += snprintf(b + n, sizeof(b) - n, "%02X", e.data[k]);
                    // The text column, decoded here so the browser need not.

                    n += snprintf(b + n, sizeof(b) - n, "\",\"ascii\":\"");
                    {
                        char txt[72];
                        const bool wide = huTextColumn(e.data, e.len, txt, sizeof(txt));
                        for (const char* s = txt; *s && n < (int)sizeof(b) - 12; ++s) {
                            // JSON-escape " and \.
                            if (*s == '"' || *s == '\\') { b[n++] = '\\'; b[n++] = *s; }
                            else b[n++] = *s;
                        }
                        b[n] = '\0';
                        snprintf(b + n, sizeof(b) - n, "\",\"u16\":%s", wide ? "true" : "false");
                    }
                    out.put(b, strlen(b));
                    // The track text as the protocol reads it, UTF-8 (Cyrillic and all).
                    if (e.dir == 'R') {
                        char t[HuMediaSource::TEXT_MAX];
                        uint8_t field = 0;
                        if (Canbox::activeProtocol().frameText(e.data, e.len, t, sizeof(t), field)) {
                            out.put(h, (size_t)snprintf(h, sizeof(h), ",\"field\":%u,\"text\":\"", (unsigned)field));
                            Json::escape(t, strlen(t), [](const char* s, size_t k, void* c) { ((Catalog::Out*)c)->put(s, k); }, &out);
                            out.put("\"", 1);
                        }
                    }
                    out.put("}", 1);
                }
                out.put("]}", 2);
            }

            // Every frame UartTx wrote, on the task that wrote it (keys or hu): into the
            // ring, and nothing more. The log sees it as the hu task's [hu] echo.
            static void uartTap(char dir, const uint8_t* frame, uint8_t len, void*)
            {
                // The payload unwrapped (Protocol::unframe), like the inbound rows.
                uint8_t cmd = 0, plen = 0;
                const uint8_t* p = nullptr;
                uint16_t total = 0;
                if (Canbox::activeProtocol().unframe(frame, len, cmd, p, plen, total)) huRecord(dir, cmd, p, plen, total);
                else                                                                    huRecord(dir, 0, frame, len, len);
            }

            // The head unit's clock (HuInbound::clock; Raise's 0x9D, once a minute,
            // unprompted). On the hu task: the clock and the media header are App::Ui's,
            // so this only asks (postHuClock), and the ui task posts it and retries until
            // delivered.
            static void onHuClock(uint8_t h, uint8_t m, void*)
            {
                // Once a minute is the unit's own pace; logging each is cheap.
                static uint16_t s_last = 0xFFFF;
                const uint16_t hm = (uint16_t)((h << 8) | m);
                if (hm == s_last) return;                        // same minute, nothing to redraw
                s_last = hm;
#if FEATURE_DISPLAY_CAN
                App::Ui::postHuClock(hm);
#endif
                LOGI("hu", "clock from head unit: %02u:%02u", (unsigned)h, (unsigned)m);
            }

            // The head unit asked for one of our messages (HuInbound::request; Raise's
            // 0x90). The answer goes in the reply lane, ahead of queued telemetry.
            static void onHuRequest(uint8_t cmd, void*)
            {
                const bool sent = App::Hu::sendByCmd(cmd);
                LOGI("hu", "head unit asked for %02X -> %s", cmd, sent ? "sent" : "nothing to say");
            }

            static void onHuAckIn(void*) { App::Hu::noteAckIn(); }

            // Now-playing text, as the protocol found it in the head unit's frames.
            static void onHuText(Canbox::HuText f, const char* t, void*)
            {
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
                App::Media::hu().onText(f, t, millis());
#else
                (void)f; (void)t;
#endif
            }

            // Every head-unit frame, on the hu task, after its ack went out.
            static void onHuFrame(uint8_t cmd, const uint8_t* payload, uint8_t len, uint16_t total, void*)
            {
                // Decoded, at DBG ("hu"); the frame itself is in the capture (bus 2).
                // Static: the hu task is the only caller, and its stack is 3 kB.
                static int8_t s_hurTag = -1;
                const bool say = Log::on(LogLevel::DBG, "hu", s_hurTag);
                static char b[LogBus::MAX_LINE];
                int n = say ? snprintf(b, sizeof(b), "D [hu] R cmd=%02X len=%u", cmd, (unsigned)total) : 0;
                if (say) {
                    if (total > len) n += snprintf(b + n, sizeof(b) - n, " (+%u trunc)", (unsigned)(total - len));

                    for (uint8_t i = 0; i < len && n < (int)sizeof(b) - 8; ++i)
                        n += snprintf(b + n, sizeof(b) - n, " %02X", payload[i]);

                    uint8_t printable = 0;
                    for (uint8_t i = 0; i < len; ++i)
                        if (payload[i] >= 0x20 && payload[i] < 0x7F) ++printable;

                    // Text if at least half the payload is printable.
                    const bool looksLikeText = (len >= 4 && printable * 2 >= len);
                    if (looksLikeText && n < (int)sizeof(b) - (int)len - 8) {
                        n += snprintf(b + n, sizeof(b) - n, "  |");
                        for (uint8_t i = 0; i < len && n < (int)sizeof(b) - 4; ++i) {
                            const uint8_t c = payload[i];
                            b[n++] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
                        }
                        b[n++] = '|';
                        b[n] = '\0';
                    }
                    LogBus::line(b);
                }
                huRecord('R', cmd, payload, len, total);

#if FEATURE_DISPLAY_CAN || FEATURE_BLE
                // Every frame: it is also the head unit's liveness. HuMediaSource decides
                // what is text; `looksLikeText` is only for the log column.
                App::Media::hu().onHeard(millis());
#endif
            }

            // Bytes outside any frame, batched so they log as lines, not single bytes.

            static uint8_t  g_junk[32];
            static uint8_t  g_junkLen = 0;
            static uint32_t g_junkAtMs = 0;

            // The hu task. Junk goes in the ring (its shape identifies a foreign dialect),
            // and the ring's echo shows it on Serial and the web.
            static void onHuJunk(uint8_t b, void*)
            {
                g_junk[g_junkLen++] = b;
                g_junkAtMs = millis();
                if (g_junkLen >= sizeof(g_junk)) {
                    huRecord('J', 0, g_junk, g_junkLen, g_junkLen);
                    g_junkLen = 0;
                }
            }

            // The hu task's per-pass housekeeping (HuTask Setup::wake): a junk run that
            // has gone quiet is flushed as one entry, then the ring's echo catches up.
            static void huWake(uint32_t now)
            {
                if (g_junkLen && (uint32_t)(now - g_junkAtMs) > 50) {
                    huRecord('J', 0, g_junk, g_junkLen, g_junkLen);
                    g_junkLen = 0;
                }
#if FEATURE_DISPLAY_CAN || FEATURE_BLE
                // Text frames a busy media mutex held back (HuMediaSource::onFrame).
                App::Media::hu().service();
#endif
                huDrainSerial();
            }
        }

        void beginUart(Cfg::Config& cfg)
        {
            s_cfg = &cfg;
            // The hu task drains up to 512 B a pass; the 256 B default ring would
            // overflow first. Only takes effect before begin().
            Serial1.setRxBufferSize(1024);
            Serial1.begin(cfg.s().uartBaud, SERIAL_8N1, Board::PIN_HU_RX, Board::PIN_HU_TX);
            s_tx.begin(&Serial1);
            s_tx.onTap(uartTap, nullptr);
        }

        void begin(Cfg::Config& cfg, Vehicle::State& state)
        {
            // The head-unit link: the emitter is the hu task's from here on. Tuning
            // from NVS (every ignition is a reboot); the emitter sends only when the
            // owner has switched it on (remembered) and the UART is on.
            {
                const Canbox::Protocol& proto = Canbox::activeProtocol();
                static Canbox::HuConfig ec;   // ~450 B, once: not on setup()'s stack
                proto.initConfig(ec);
                cfg.loadTuning(proto, ec.tuning);
                ec.enabled = cfg.loadHuSend() && cfg.s().uartEnabled;
                CarStore::loadFields(proto, ec.fields);
                if (ec.fields.count())
                    LOGI("hu", "field map: %u rule(s) from the car file", (unsigned)ec.fields.count());
                // Emitter and decoder, once, in internal RAM: the hu task's 2 ms pass.
                void* mem = heap_caps_aligned_alloc(8, proto.emitterSize(), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (mem) s_emitter = proto.makeEmitter(mem);
                else     LOGE("hu", "no memory for the %s emitter: the head unit gets no car data", proto.id());
                void* dmem = heap_caps_aligned_alloc(8, proto.decoderSize(), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (dmem) s_huRx = proto.makeDecoder(dmem);
                else      LOGE("hu", "no memory for the %s decoder: the head unit is not heard", proto.id());
                App::Hu::Setup hs;
                hs.port    = &Serial1;
                hs.tx      = &s_tx;
                hs.rx      = s_huRx;
                hs.proto   = &proto;
                hs.emitter = s_emitter;
                hs.state   = &state;
                hs.wake    = huWake;
                // What the head unit's frames mean (the protocol decides which);
                // the ack is the hu task's own, written the moment a frame ends.
                Canbox::HuInbound in;
                in.frame   = onHuFrame;
                in.junk    = onHuJunk;
                in.ackIn   = onHuAckIn;
                in.clock   = onHuClock;
                in.request = onHuRequest;
                in.text    = onHuText;
                App::Hu::begin(hs, ec, in);
            }
        }

        UartTx&          tx()      { return s_tx; }
        Canbox::Emitter* emitter() { return s_emitter; }
        Canbox::Decoder* decoder() { return s_huRx; }
        void lastFramesJson(bool inOnly, Catalog::Out& out) { lastFramesJsonImpl(inOnly, out); }
    }
}
