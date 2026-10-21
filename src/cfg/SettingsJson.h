#pragma once
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>
#include "SettingsRegistry.h"

// The registry as JSON, both ways: what GET /api/settings sends, what POST
// /api/settings and POST /api/config accept, and the settings file
// (GET /api/config). One reader for every way in, so a file cannot carry a
// value the form would refuse.
//
// Pure (ArduinoJson, no Arduino): host-tested in test/test_settings.
//
// Reading checks EVERY key before anything is applied and collects every
// refusal, so a 400 lists them all and a refused request changes nothing.
// Table cells are written into the model while they are checked: callers
// pass copies (the alert rules, the emitter's tuning), as WebUi does.
namespace Cfg
{
    namespace SettingsJson
    {
        // The registry's web entries as /api/settings sends them. `forFile`:
        // what a settings file carries, i.e. not the read-only entries (the
        // board's own findings) nor the volatile ones (RAM only by design).
        void write(const Model& m, JsonObject out, bool forFile = false);

        // One scalar or text value, checked, not yet written.
        struct Change {
            const SEntry* e = nullptr;
            int32_t       v = 0;
            const char*   text = nullptr;   // Str: points into the request's document
        };

        // Every refusal, one "key: why" line each.
        struct Errors {
            static constexpr uint8_t MAX = 24;
            static constexpr uint8_t LEN = 80;
            char     line[MAX][LEN];
            uint8_t  n = 0;
            uint16_t more = 0;              // past MAX, counted only
            void add(const char* fmt, ...);
            bool any() const { return n || more; }
            // "a: why\nb: why" (+ "... and N more"), cut to cap.
            void join(char* out, size_t cap) const;
        };

        struct Read {
            Change   ch[Registry::MAX_CHANGES];
            uint8_t  n = 0;
            uint8_t  apT = 0;               // tables: what their changed cells need
            // Tables with a changed cell, for a preview.
            const SEntry* tables[8];
            uint8_t  nTables = 0;
            // Keys this registry does not know (a newer file, or apPass).
            uint8_t  unknown = 0;
            // A file: entries this board does not have (its knobs, its
            // capture), skipped rather than refused, for the preview.
            const SEntry* skipped[16];
            uint8_t  nSkipped = 0;
            bool     tooMany = false;
        };

        // Checks every key of `in` against the registry; collects changes into
        // `r` and refusals into `err`. Keys the registry does not know are
        // counted, not refused.
        // `file` (a settings file): RAM-only entries are ignored; entries this
        // board does not have are skipped (r.skipped), not refused; and the
        // keys a file must never carry (FILE_FORBIDDEN: the AP/OTA password,
        // the head unit's learner reset) are refused. The caller has made
        // sure nothing is merely busy (a model part missing because a peek
        // failed reads as "not on this board" here).
        void read(Model& m, JsonObjectConst in, Read& r, Errors& err, bool file = false);

        // Not registry entries, but read by POST /api/settings: never from a file.
        inline constexpr const char* FILE_FORBIDDEN[] = {"apPass", "huForget"};

        // Writes what read() collected (only when it reported no errors);
        // ORs the apply bits, tables' included, into *ap.
        void apply(Model& m, const Read& r, uint8_t* ap);

        // Would this change alter the stored value?
        bool differs(const Model& m, const Change& c);

        // ---- the settings file ------------------------------------------------
        // {"format":"megaopen-config","schema":1,"fw":..,"build":..,
        //  "settings":{..write(forFile)..},"knobs":{..GET /api/knobs..}}
        inline constexpr const char* FORMAT = "megaopen-config";
        // Bumped when a key changes meaning (not when one is added: unknown
        // keys are skipped, missing ones keep their value).
        inline constexpr int SCHEMA = 1;

        enum class FileCheck : uint8_t { Ok, NotAConfig, Newer, Older };
        FileCheck checkFile(JsonObjectConst f);
        const char* fileCheckText(FileCheck c);
    }
}
