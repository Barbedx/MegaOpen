#include "CarStore.h"
#include <LittleFS.h>
#include <esp_ota_ops.h>
#include "CarFile.h"
#include "Config.h"
#include "../utils/Locks.h"
#include "../utils/Log.h"
#include "../Features.h"
#if FEATURE_LOGFS
#include "../utils/LogFile.h"
#endif
#include "../utils/PsramJson.h"

namespace CarStore
{
    namespace
    {
        constexpr const char* DIR  = "/cfg";
        constexpr const char* PATH = "/cfg/car.json";
        constexpr const char* TMP  = "/cfg/car.tmp";

        JsonDocument* g_doc = nullptr;
        bool          g_ready = false;
        Locks::Mutex  g_lock;
        constexpr uint32_t LOCK_WAIT_MS = 500;

        // The whole document to the file: written beside it, then renamed
        // over it (littlefs renames atomically).
        bool write()
        {
            if (!g_ready) return false;
            File f = LittleFS.open(TMP, "w");
            if (!f) return false;
            const size_t n = serializeJson(*g_doc, f);
            const bool ok = n > 0 && !f.getWriteError();
            f.close();
            if (!ok || !LittleFS.rename(TMP, PATH)) {
                LittleFS.remove(TMP);
                return false;
            }
            return true;
        }

        bool read()
        {
            File f = LittleFS.open(PATH, "r");
            if (!f) return false;
            const DeserializationError e = deserializeJson(*g_doc, f);
            f.close();
            if (e) {
                LOGW("car", "%s does not parse (%s): kept aside, starting from NVS / factory", PATH, e.c_str());
                LittleFS.rename(PATH, "/cfg/car.bad");
                g_doc->clear();
                return false;
            }
            return true;
        }

        // This image boots on an OTA trial (it rolls back if it never gets
        // healthy). OtaTrial::begin runs later in setup; this asks the
        // partition itself.
        bool onTrial()
        {
            const esp_partition_t* run = esp_ota_get_running_partition();
            esp_ota_img_states_t st;
            return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
        }

        // 1.6.x's NVS tables go only once the file is written AND this image
        // is no longer on trial: a rollback during the trial finds them where
        // its firmware looks. So on the first boot after the confirm.
        void dropNvs(Cfg::Config& cfg)
        {
            if (onTrial()) {
                LOGI("car", "NVS decode/huFmap kept until this image is confirmed (rollback-safe)");
                return;
            }
            if (cfg.eraseDecode() && cfg.eraseFieldMap()) return;
            LOGW("car", "NVS decode/huFmap NOT erased: again next boot");
        }

        // 1.6.x kept the tables as NVS blobs, keyed by signal index. Read
        // once into the document by name.
        void migrate(Cfg::Config& cfg)
        {
            JsonObject car = g_doc->to<JsonObject>();
            car["v"] = CarFile::VERSION;
            bool any = false;
            uint8_t* blob = (uint8_t*)heap_caps_malloc(Vehicle::DecodeTable::BLOB_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            Vehicle::DecodeTable* t = (Vehicle::DecodeTable*)heap_caps_malloc(sizeof(Vehicle::DecodeTable), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (blob && t) {
                new (t) Vehicle::DecodeTable();
                const size_t n = cfg.loadDecode(blob, Vehicle::DecodeTable::BLOB_MAX);
                if (n && t->unpack(blob, n)) { CarFile::putDecode(car, *t); any = true; }
                else if (n) LOGW("car", "NVS decode (%u B) does not unpack: not carried over", (unsigned)n);
            }
            heap_caps_free(blob);
            heap_caps_free(t);
            Canbox::FieldMap m;
            if (cfg.loadFieldMap(m) && m.count()) {
                CarFile::putFields(car, Canbox::activeProtocol(), m);
                any = true;
            }
            if (!write()) {
                LOGW("car", "%s not written: the NVS tables stay, read again next boot", PATH);
                return;
            }
            LOGI("car", "%s made%s", PATH, any ? " from NVS" : "");
        }
    }

    void begin(Cfg::Config& cfg)
    {
        void* m = heap_caps_malloc(sizeof(JsonDocument), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!m) m = heap_caps_malloc(sizeof(JsonDocument), MALLOC_CAP_8BIT);
#if FEATURE_LOGFS
        const bool fs = LogFile::mounted();
#else
        const bool fs = false;   // no littlefs partition in this image
#endif
        if (!m || !fs) {
            LOGW("car", "no %s this boot (%s): factory tables, nothing saved", PATH, m ? "no file system" : "no memory");
            heap_caps_free(m);
            return;
        }
        g_doc = new (m) JsonDocument(&PsramJson::instance());
        if (!LittleFS.exists(DIR)) LittleFS.mkdir(DIR);
        g_ready = true;
        if (read()) LOGI("car", "%s: %u B, version %d", PATH, (unsigned)measureJson(*g_doc), (int)((*g_doc)["v"] | 0));
        else        migrate(cfg);
        if (LittleFS.exists(PATH)) dropNvs(cfg);
    }

    bool ready() { return g_ready; }

    void loadDecode(Vehicle::DecodeTable& t, const char*& from)
    {
        from = "factory";
        if (g_ready && g_lock.take(LOCK_WAIT_MS)) {
            uint16_t skipped = 0;
            const char* why = CarFile::getDecode(g_doc->as<JsonObjectConst>(), t, skipped);
            g_lock.give();
            if (!why) {
                from = "file";
                if (skipped) LOGW("car", "decode: %u row(s) name a signal this firmware does not know: skipped", (unsigned)skipped);
                return;
            }
            if (strcmp(why, "decode: missing")) LOGW("car", "decode in %s refused (%s): factory", PATH, why);
        }
        t.factory();
    }

    void loadFields(const Canbox::Protocol& p, Canbox::FieldMap& m)
    {
        m.clearAll();
        if (!g_ready || !g_lock.take(LOCK_WAIT_MS)) return;
        uint16_t skipped = 0;
        const char* why = CarFile::getFields(g_doc->as<JsonObjectConst>(), p, m, skipped);
        g_lock.give();
        if (why) LOGW("car", "%s field map refused (%s): none", p.id(), why);
        else if (skipped) LOGW("car", "%s field map: %u rule(s) this firmware does not know: skipped", p.id(), (unsigned)skipped);
    }

    bool saveDecode(const Vehicle::DecodeTable& t)
    {
        if (!g_ready || !g_lock.take(LOCK_WAIT_MS)) return false;
        CarFile::putDecode(g_doc->as<JsonObject>(), t);
        const bool ok = write();
        g_lock.give();
        return ok;
    }

    bool factoryDecode()
    {
        if (!g_ready || !g_lock.take(LOCK_WAIT_MS)) return false;
        g_doc->remove("decode");
        const bool ok = write();
        g_lock.give();
        return ok;
    }

    bool saveFields(const Canbox::Protocol& p, const Canbox::FieldMap& m)
    {
        if (!g_ready || !g_lock.take(LOCK_WAIT_MS)) return false;
        CarFile::putFields(g_doc->as<JsonObject>(), p, m);
        const bool ok = write();
        g_lock.give();
        return ok;
    }
}
