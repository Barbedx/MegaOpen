#include "BootLog.h"
#include "../Features.h"
#include "../Version.h"
#include "../board/BoardProfile.h"
#include "../utils/Log.h"
#include <Preferences.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <esp_core_dump.h>
#include <esp_flash.h>
#if FEATURE_LOGFS
#include "../utils/LogFile.h"
#include <LittleFS.h>
#endif

namespace App
{
    namespace Boot
    {
        // Free heap at each setup() stage, with the delta since the last mark.
        void memMark(const char* what)
        {
            static uint32_t s_last = 0;
            const uint32_t now = ESP.getFreeHeap();
            LOGI("mem", "%-12s free=%6u big=%6u %+d", what, (unsigned)now,
                 (unsigned)ESP.getMaxAllocHeap(), s_last ? (int)now - (int)s_last : 0);
            s_last = now;
        }

        namespace
        {
            // The last crash, from the coredump partition (the prebuilt SDK writes an ELF
            // coredump there on every panic): which task, where, why, and the backtrace,
            // as one warning line, so the flash log and the log push carry it home. Then
            // the image is erased: the next crash writes a new one, and this line is never
            // repeated for an old one. Decode with the ELF of that build (`elf=`):
            //   xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf <pc> <bt...>
            // The whole image is kept too, as /crash/<boot>.cdmp on littlefs, for the log
            // push to send home (boot-<boot>/coredump.bin, the boot that crashed): every
            // task's stack, which the summary's corrupted backtrace cannot give.
            //   esp-coredump info_corefile -t raw -c coredump.bin firmware.elf
            static void keepCrash(uint32_t boot)
            {
#if FEATURE_LOGFS
                size_t addr = 0, size = 0;
                if (!LogFile::fsMounted() || esp_core_dump_image_get(&addr, &size) != ESP_OK || !size || size > 64 * 1024) return;
                auto* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!buf) return;
                if (esp_flash_read(esp_flash_default_chip, buf, addr, size) == ESP_OK) {
                    if (!LittleFS.exists("/crash")) LittleFS.mkdir("/crash");
                    char path[24];
                    snprintf(path, sizeof(path), "/crash/%05u.cdmp", (unsigned)boot);
                    File f = LittleFS.open(path, "w");
                    const size_t put = f ? f.write(buf, size) : 0;
                    if (f) f.close();
                    LOGI("crash", "coredump kept: %s, %u bytes%s", path, (unsigned)size, put == size ? "" : " (write failed)");
                }
                heap_caps_free(buf);
#else
                (void)boot;
#endif
            }

            static void logCrash(uint32_t boot)
            {
                if (esp_core_dump_image_check() != ESP_OK) return;
                keepCrash(boot);
                auto* s = (esp_core_dump_summary_t*)heap_caps_malloc(sizeof(esp_core_dump_summary_t),
                                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                if (!s) return;
                if (esp_core_dump_get_summary(s) == ESP_OK) {
                    char bt[16 * 11 + 1];
                    size_t n = 0;
                    const uint32_t depth = s->exc_bt_info.depth < 16 ? s->exc_bt_info.depth : 16;
                    bt[0] = 0;
                    for (uint32_t i = 0; i < depth && n < sizeof(bt) - 12; ++i)
                        n += snprintf(bt + n, sizeof(bt) - n, " 0x%08x", (unsigned)s->exc_bt_info.bt[i]);
                    s->exc_task[sizeof(s->exc_task) - 1] = 0;
                    LOGW("crash", "task=%s pc=0x%08x cause=%u vaddr=0x%08x elf=%.16s bt%s:%s", s->exc_task,
                         (unsigned)s->exc_pc, (unsigned)s->ex_info.exc_cause, (unsigned)s->ex_info.exc_vaddr,
                         (const char*)s->app_elf_sha256, s->exc_bt_info.corrupted ? "(corrupted)" : "", bt);
                } else {
                    LOGW("crash", "a coredump is on flash but its summary did not read");
                }
                heap_caps_free(s);
                esp_core_dump_image_erase();
            }
        }

        void logReason()
        {
            const esp_reset_reason_t r = esp_reset_reason();
            // One token each, so the [boot] line splits on spaces (and a grep for
            // reason=task_wdt finds a watchdog reboot).
            const char* why;
            switch (r) {
                case ESP_RST_POWERON:   why = "poweron";   break;
                case ESP_RST_EXT:       why = "ext";       break;   // the EN/reset pin
                case ESP_RST_SW:        why = "sw";        break;   // esp_restart(), OTA
                case ESP_RST_PANIC:     why = "panic";     break;   // a crash
                case ESP_RST_INT_WDT:   why = "int_wdt";   break;
                case ESP_RST_TASK_WDT:  why = "task_wdt";  break;
                case ESP_RST_WDT:       why = "wdt";       break;   // any other watchdog
                case ESP_RST_DEEPSLEEP: why = "deepsleep"; break;
                case ESP_RST_BROWNOUT:  why = "brownout";  break;   // the supply sagged
                case ESP_RST_SDIO:      why = "sdio";      break;
                default:                why = "unknown";   break;
            }
            Preferences p;
            p.begin("sys", false);
            const uint32_t boots = p.getUInt("boots", 0) + 1;
            p.putUInt("boots", boots);
            MegaOpen::setBootNo(boots);
            // Abnormal resets counted separately: a number that should stay at zero.
            const bool bad = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                             r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
            uint32_t crashes = p.getUInt("crashes", 0);
            if (bad) p.putUInt("crashes", ++crashes);
            p.end();
            // key=value, one line, the build first: the flash log keeps a file per
            // boot, and this is the line that says which image wrote it and why the
            // board came up. psram=0 on a board that should have it is a wrong image.
            LOGI("boot", "build=%s fw=" MEGAOPEN_VERSION " board=%s reason=%s boots=%u crashes=%u psram=%u",
                 MegaOpen::buildId(), Board::NAME, why, (unsigned)boots, (unsigned)crashes,
                 (unsigned)ESP.getPsramSize());
            logCrash(boots - 1);
        }
    }
}
