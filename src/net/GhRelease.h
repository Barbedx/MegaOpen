#pragma once
#include <stddef.h>
#include <stdint.h>
#include <ArduinoJson.h>

// The decisions of the GitHub update (GhOta.cpp), apart from the network.
// Portable: the host tests feed it release JSON and SHA256SUMS text.
//
// A release is what .github/workflows/release.yml publishes for tag vX.Y.Z:
//   megaopen-<env>-<ver>.bin           the app image, the one this flashes
//   megaopen-<env>-<ver>-factory.bin   for a first USB flash, never this
//   SHA256SUMS                         `sha256sum *.bin`: "<64 hex>  <name>"
namespace GhRelease
{
    // MAJOR.MINOR.PATCH with an optional "-rcN"; a leading 'v' is allowed.
    // An rc orders below its final: 1.0.1-rc2 < 1.0.1.
    struct Ver {
        uint16_t maj = 0, min = 0, pat = 0;
        int16_t  rc  = -1;              // -1: a final
    };
    bool parse(const char* s, Ver& out);
    int  compare(const Ver& a, const Ver& b);   // <0, 0, >0

    // What one GET of /releases/latest needs to keep; ArduinoJson's filter
    // drops the rest (the notes, the author, every other asset field).
    void filter(JsonDocument& f);
    // The same for GET /releases (a list, newest first): every element.
    void filterList(JsonDocument& f);

    enum class Pick : uint8_t {
        OK,
        NO_TAG,       // no tag_name, or one that is not a version
        DRAFT,        // a draft; or a prerelease where only finals are offered
        NO_ASSET,     // no megaopen-<env>-<ver>.bin in this release
        NO_SUMS,      // no SHA256SUMS: nothing to check the image against
    };
    struct Release {
        char     tag[24]     = "";      // as published, "v1.0.1"
        Ver      ver;
        char     asset[64]   = "";      // "megaopen-t2can-1.0.1.bin"
        uint32_t size        = 0;
        // Both are github.com/.../releases/download/... and redirect to the CDN.
        char     binUrl[192]  = "";
        char     sumsUrl[192] = "";
    };
    // `rel` is the filtered /releases/latest document; env is PlatformIO's
    // name for this image ("t2can").
    // allowPre: the test channel, where an rc is offered too (never a draft).
    Pick pick(JsonVariantConst rel, const char* env, Release& out, bool allowPre = false);
    // The highest version in a /releases list that pick() accepts. Nothing
    // accepted: the first refusal (NO_TAG for an empty list).
    Pick pickNewest(JsonArrayConst list, const char* env, bool allowPre, Release& out);

    // Whether `rel` is above the running image. An rc carries its final's
    // version (release.yml: the tag's X.Y.Z must match MEGAOPEN_VERSION), so
    // the image alone cannot tell rc1 from rc2: `installed` is the tag the
    // last GitHub install wrote (NVS), used when its X.Y.Z is this image's.
    // Unknown (null, another version): the image counts as the final, and no
    // rc of its own version is offered. A `mine` that does not parse: newer.
    bool newer(const Ver& rel, const char* mine, const char* installed);
    const char* pickText(Pick p);

    // A URL the update may fetch: https:// and one of GitHub's hosts (the API,
    // github.com's release links, and the CDNs they redirect to). Checked
    // before every request and after every redirect.
    bool trustedUrl(const char* url);

    // The image's hash from SHA256SUMS text (not NUL-terminated). Accepts
    // sha256sum's text ("  name") and binary (" *name") forms, any case.
    bool sumFor(const char* sums, size_t len, const char* asset, uint8_t out[32]);
}
