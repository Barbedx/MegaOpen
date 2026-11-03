#include "CarStore.h"
#include "../utils/PsHeap.h"
#include <LittleFS.h>
#include "CarFile.h"
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

        // No file yet (a fresh board, a factory reset): the version only, every
        // part absent = the factory's.
        void create()
        {
            (*g_doc)["v"] = CarFile::VERSION;
            if (write()) LOGI("car", "%s made (factory)", PATH);
            else         LOGW("car", "%s not written: factory tables, made again next boot", PATH);
        }

        bool read()
        {
            File f = LittleFS.open(PATH, "r");
            if (!f) return false;
            const DeserializationError e = deserializeJson(*g_doc, f);
            f.close();
            if (e) {
                LOGW("car", "%s does not parse (%s): kept aside, starting from the factory", PATH, e.c_str());
                LittleFS.rename(PATH, "/cfg/car.bad");
                g_doc->clear();
                return false;
            }
            return true;
        }

    }

    void begin()
    {
        void* m = PsHeap::malloc(sizeof(JsonDocument));
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
        else        create();
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

    void loadFormulas(Vehicle::FormulaSet& set)
    {
        const char* why = "formulas: missing";
        if (g_ready && g_lock.take(LOCK_WAIT_MS)) {
            why = CarFile::getFormulas(g_doc->as<JsonObjectConst>(), set);
            g_lock.give();
        }
        if (!why) return;
        if (strcmp(why, "formulas: missing")) LOGW("car", "formulas in %s refused (%s): factory", PATH, why);
        set.factory();
    }

    bool saveFormulas(const Vehicle::FormulaSet& set)
    {
        if (!g_ready || !g_lock.take(LOCK_WAIT_MS)) return false;
        CarFile::putFormulas(g_doc->as<JsonObject>(), set);
        const bool ok = write();
        g_lock.give();
        return ok;
    }

    bool factoryFormulas()
    {
        if (!g_ready || !g_lock.take(LOCK_WAIT_MS)) return false;
        g_doc->remove("formulas");
        const bool ok = write();
        g_lock.give();
        return ok;
    }

    void loadRules(Vehicle::RuleSet& set)
    {
        const char* why = "rules: missing";
        if (g_ready && g_lock.take(LOCK_WAIT_MS)) {
            why = CarFile::getRules(g_doc->as<JsonObjectConst>(), set);
            g_lock.give();
        }
        if (!why) return;
        if (strcmp(why, "rules: missing")) LOGW("car", "rules in %s refused (%s): factory", PATH, why);
        set.factory();
    }

    bool saveRules(const Vehicle::RuleSet& set)
    {
        if (!g_ready || !g_lock.take(LOCK_WAIT_MS)) return false;
        CarFile::putRules(g_doc->as<JsonObject>(), set);
        const bool ok = write();
        g_lock.give();
        return ok;
    }

    bool factoryRules()
    {
        Vehicle::RuleSet set;
        set.factory();
        return saveRules(set);
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
