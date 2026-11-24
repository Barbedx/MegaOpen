#include "LogSetup.h"
#include "../Features.h"
#include "../utils/Log.h"
#include "../utils/LogBus.h"
#include "../net/LogPush.h"
#include <esp_system.h>
#include <esp_log.h>
#if FEATURE_LOGFS
#include "../utils/LogFile.h"
#endif
#if FEATURE_CAPTURE
#include "../capture/CanRecorder.h"
#endif

namespace App
{
    namespace LogSetup
    {
        namespace
        {
            bool s_logfs = false;

            // The LogBus sinks. They run on the LogBus task, so blocking here is allowed.
            //
            // Every line also stays in the LogBus ring, which is the web log
            // (GET /api/logs?since=); these sinks are Serial, the flash log and the push.
            // Each takes a line by its level alone (Log::sinkTakes, the settings logSerial /
            // logFlash / logPush): no sink looks at what a line says (docs/design/logging.md).
            // The head unit's frames are in the capture (bus 2), not text, below TRC.
            // IDF/library lines from the esp_log tap (logTapVprintf). Where the IDF
            // console is Serial itself (UART0: the DevKit) they are already on it, so the
            // serial sink skips them; on a USB-CDC build the console is not Serial, and
            // this is the only way they reach the USB port.
            static constexpr char IDF[] = "I [idf] ";   // the first byte becomes the line's level
            static bool isIdfLine(const char* line) { return !strncmp(line + 1, IDF + 1, sizeof(IDF) - 2); }

            static void serialSink(const char* line, int len, uint32_t)
            {
                if (!Log::sinkTakes(Log::SINK_SERIAL, line, len)) return;
#if !ARDUINO_USB_CDC_ON_BOOT
                if (isIdfLine(line)) return;
#endif
#if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
                // TinyUSB CDC (t2can_usbhid): its write() spins, with no timeout, while
                // the host holds DTR and does not read. Whole line or nothing.
                if (Serial.availableForWrite() < len + 1) return;
#endif
                Serial.write((const uint8_t*)line, len);
                Serial.write('\n');
            }
#if FEATURE_LOG_PUSH
            static void pushSink(const char* line, int len, uint32_t ms)
            {
                if (Log::sinkTakes(Log::SINK_PUSH, line, len)) LogPush::sink(line, len, ms);
            }
#endif
#if FEATURE_LOGFS
            // The flash log never takes TRC (Log::setSinkLevel caps it at DBG): a line per
            // frame would erase sectors with both cores stopped.
            static void fileSink(const char* line, int len, uint32_t ms)
            {
                if (Log::sinkTakes(Log::SINK_FLASH, line, len)) LogFile::sink(line, len, ms);
            }

            // Every software reboot (ESP.restart, esp_restart, the OTA and rollback
            // reboots): what is still in the LogBus ring and the flash log's RAM buffer
            // goes to /log/0.log first. Not run on a panic or a power cut.
            static void logShutdown()
            {
                LOGI("log", "rebooting: log flushed");
#if FEATURE_CAPTURE
                // The capture's last chunk first: it is the one a drive was recorded for.
                Capture::flush(500);
#endif
                LogBus::flushAndWait(500);
            }
#endif

            // esp_log tap: IDF/library log lines into the LogBus as "[idf] ...", so they
            // reach the flash log and the web (and USB serial where the IDF console is not
            // Serial already; see serialSink). The console write itself is unchanged.
            static int (*s_idfVprintf)(const char*, va_list) = nullptr;

            // Bytes of stack left below this frame on the calling task.
            static size_t stackLeft()
            {
                const uint8_t* base = pxTaskGetStackStart(nullptr);
                const uint8_t* sp   = (const uint8_t*)__builtin_frame_address(0);
                return (base && sp > base) ? (size_t)(sp - base) : 0;
            }

            static int logTapVprintf(const char* fmt, va_list ap)
            {
                // Lines logged by the LogBus task itself (a sink: LittleFS) are not fed
                // back into the LogBus: the console only. That is the
                // whole re-entry guard: LogBus::line never logs and never blocks, so no
                // other task can loop through here. (The shared s_tapOwner it replaces was
                // one global for every task, and any logging task cleared another's claim.)
                // Also skipped on a task without the room for the buffer plus vsnprintf
                // (NimBLE's host task runs this too).
                const TaskHandle_t self = xTaskGetCurrentTaskHandle();
                if (self != LogBus::task() && stackLeft() > 1536) {
                    char b[160];
                    memcpy(b, IDF, sizeof(IDF) - 1);
                    constexpr int P = (int)sizeof(IDF) - 1;
                    va_list copy;
                    va_copy(copy, ap);
                    int n = vsnprintf(b + P, sizeof(b) - P, fmt, copy);
                    va_end(copy);
                    if (n > (int)sizeof(b) - P - 1) n = (int)sizeof(b) - P - 1;
                    // Strip the IDF's trailing newline.
                    while (n > 0 && (b[P + n - 1] == '\n' || b[P + n - 1] == '\r')) b[P + --n] = '\0';
                    // The IDF's own level letter (E W I D V) is the line's ("idf").
                    const char c = n > 0 ? b[P] : 'I';
                    const LogLevel lvl = c == 'E' ? LogLevel::ERR : c == 'W' ? LogLevel::WRN : c == 'D' ? LogLevel::DBG
                                       : c == 'V' ? LogLevel::TRC : LogLevel::INF;
                    static int8_t s_idfTag = -1;
                    b[0] = "EWIDT"[(uint8_t)lvl];
                    if (n > 0 && Log::on(lvl, "idf", s_idfTag)) LogBus::line(b);
                }
                return s_idfVprintf ? s_idfVprintf(fmt, ap) : vprintf(fmt, ap);
            }
        }

        void sinks()
        {
            LogBus::addSink(serialSink);
#if FEATURE_LOGFS
            s_logfs = LogFile::begin();
            if (s_logfs) {
                LogBus::addSink(fileSink);
                // Run on the LogBus task by flushAndWait(): the flash log's RAM buffer
                // to /log/0.log, synced.
                LogBus::onFlush(LogFile::flushNow);
            }
#endif
        }

        bool flashOn() { return s_logfs; }

#if FEATURE_LOG_PUSH
        void push(size_t (*snapshot)(char* out, size_t cap))
        {
            const bool push = LogPush::begin();
            if (push) LogBus::addSink(pushSink);
            LogPush::setSnapshot(snapshot);
            LogPush::View v;
            LogPush::view(v);
            if (push)
                LOGI("push", "spool %u kB in PSRAM; %s, repo %s, device %s", (unsigned)(v.spoolBytes / 1024),
                     v.on ? (v.hasToken ? "on" : "on, no token") : "off", v.repo, v.device);
            else
                LOGI("push", "no memory for the spool: log push off this boot");
        }
#endif

        void onShutdown()
        {
#if FEATURE_LOGFS
            if (s_logfs) esp_register_shutdown_handler(logShutdown);
#endif
        }

        void report(bool ringGrown)
        {
            LOGI("log", "ring %d B in %s%s", LogBus::ringSize(), LogBus::ringInPsram() ? "PSRAM" : "internal RAM",
                 ringGrown ? "" : " (the boot ring: no memory for a bigger one)");
#if FEATURE_LOGFS
            if (s_logfs)
                LOGI("log", "flash log /log/0.log (previous boot: /log/1.log), %u/%u kB used; "
                     "written every %u kB, on warnings and before a reboot",
                     (unsigned)(LogFile::usedBytes() / 1024), (unsigned)(LogFile::totalBytes() / 1024),
                     (unsigned)(LogFile::FLUSH_BYTES / 1024));
            else
                LOGI("log", "flash log FAILED: %s; RAM and serial only", LogFile::mountError());
#else
            (void)s_logfs;
#endif
        }

        void tapIdf() { s_idfVprintf = esp_log_set_vprintf(logTapVprintf); }

        void apply(const Cfg::Settings& s)
        {
            Log::setLevel((LogLevel)(s.logLevel < Log::LEVELS ? s.logLevel : 2));
            Log::setTagSpec(s.logTags.c_str());
            Log::setSinkLevel(Log::SINK_WEB, (LogLevel)s.logWeb);
            Log::setSinkLevel(Log::SINK_FLASH, (LogLevel)s.logFlash);
            Log::setSinkLevel(Log::SINK_PUSH, (LogLevel)s.logPush);
            Log::setSinkLevel(Log::SINK_SERIAL, (LogLevel)s.logSerial);
            static const uint32_t STATUS_MS[3] = {10000, 60000, 300000};
            Log::setStatusInterval(STATUS_MS[s.logStatus < 3 ? s.logStatus : 1]);
            if (s.logLevel != 2 || s.logTags.length())
                LOGI("log", "level %s%s%s", Log::levelName(Log::level()), s.logTags.length() ? ", per tag: " : "",
                     s.logTags.c_str());
        }
    }
}
