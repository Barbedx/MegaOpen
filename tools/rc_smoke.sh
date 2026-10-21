#!/usr/bin/env bash
# Release-candidate smoke test: every route changed since v1.1.0, against a
# running board. Harmless on a car: no factory reset, no OTA, no WiFi scan,
# nothing drawn on the panel; settings and the settings file only as dry runs.
# The one network action is the GitHub "check" (a download of the release
# list, never an install).
#
#   tools/rc_smoke.sh http://192.168.1.50 [mdns-name]
#   tools/rc_smoke.sh --sim http://127.0.0.1:8077
#
# --sim: against the simulator's mock (`node tools/sim_http.mjs build/ui-sim.html`),
# no board. The checks only the firmware can pass (the Host check, /update, the
# log ring's size) print SKIP by name; every other check counts as on a board.
#
# Needs bash and curl. ASCII output, one line a check; exit 1 when any fails.
set -u

SIM=0
if [ "${1:-}" = "--sim" ]; then SIM=1; shift; fi
BASE="${1:?usage: rc_smoke.sh [--sim] http://<board-ip> [mdns-name]}"
BASE="${BASE%/}"
NAME="${2:-megaopen}"
HOSTPORT="${BASE#*://}"
GATE=(-H "X-MegaOpen: 1")
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
FAILS=0
PASSES=0

pass() { PASSES=$((PASSES + 1)); echo "PASS  $1"; }
fail() { FAILS=$((FAILS + 1)); echo "FAIL  $1: $2"; }

# req NAME METHOD PATH [curl args...] -> sets CODE, BODY (file), HDRS (file)
req() {
    local name="$1" method="$2" path="$3"; shift 3
    BODY="$TMP/$name.body"; HDRS="$TMP/$name.hdrs"
    : > "$BODY"; : > "$HDRS"
    CODE=$(curl -s -m 20 -X "$method" -o "$BODY" -D "$HDRS" -w '%{http_code}' "$@" "$BASE$path")
}

expect_code() {   # NAME WANT
    if [ "$CODE" = "$2" ]; then pass "$1 ($CODE)"; else fail "$1" "want $2, got $CODE: $(head -c 160 "$BODY" | tr '\n' ' ')"; fi
}

# fw_only NAME: true on a board; with --sim prints SKIP and is false, so the
# check after `fw_only ... &&` does not run.
fw_only() { if [ "$SIM" = 1 ]; then echo "SKIP  $1 (firmware only, --sim)"; return 1; fi; return 0; }

has() {           # NAME PATTERN (in BODY)
    if grep -q -- "$2" "$BODY"; then pass "$1"; else fail "$1" "no '$2' in $(head -c 160 "$BODY" | tr '\n' ' ')"; fi
}

hasnt() {         # NAME PATTERN (not in BODY)
    if grep -q -- "$2" "$BODY"; then fail "$1" "'$2' in $(grep -o -- "$2.\{0,60\}" "$BODY" | head -1)"; else pass "$1"; fi
}

echo "rc_smoke: $BASE (mdns '$NAME')"

# ---- reachable, and which build -------------------------------------------
req status GET /api/status
if [ "$CODE" != 200 ]; then echo "FAIL  GET /api/status: got $CODE; is $BASE the board?"; exit 2; fi
pass "GET /api/status (200)"
FW=$(grep -o '"fw": *"[^"]*"' "$BODY" | head -1)
BUILD=$(grep -o '"build": *"[^"]*"' "$BODY" | head -1)
echo "      $FW $BUILD"
case "$BUILD" in
    "")        fail "build stamp" "no build field" ;;
    *-dirty@*) fail "build stamp" "a release image is never -dirty: $BUILD" ;;
    *)         pass "build stamp not -dirty" ;;
esac

# ---- the request gate (src/net/RequestGate.h) ---------------------------------
req gate_nohdr POST '/api/settings?dry=1' -H 'Content-Type: application/json' --data '{}'
expect_code "POST without X-MegaOpen refused" 403
has "  403 is gate_header" "^gate_header$"

req gate_hdr POST '/api/settings?dry=1' "${GATE[@]}" -H 'Content-Type: application/json' --data '{}'
expect_code "POST with X-MegaOpen (dry run)" 200

req ota_nohdr GET /ota/start
expect_code "GET /ota/start without the header refused" 403

req mark_nohdr GET '/api/cap/mark?t=rc_smoke'
expect_code "GET /api/cap/mark without the header refused" 403

req scan_get GET /api/wifi/scan
case "$CODE" in 403|404|405) pass "GET /api/wifi/scan is not a scan ($CODE)" ;; *) fail "GET /api/wifi/scan" "a GET must not scan, got $CODE" ;; esac
req scan_nohdr POST /api/wifi/scan
expect_code "POST /api/wifi/scan without the header refused" 403

if fw_only "Host evil.example refused (rebinding)"; then
    req host_evil GET /api/status -H 'Host: evil.example'
    expect_code "Host evil.example refused (rebinding)" 403
    has "  403 is gate_host with where to go" "^gate_host http://"
fi

if fw_only "a router's name refused"; then
    req host_router GET /api/status -H "Host: $NAME.fritz.box"
    expect_code "a router's name refused" 403
fi

req host_local GET /api/status -H "Host: $NAME.local"
expect_code "Host $NAME.local accepted" 200

req host_ip GET /api/status -H "Host: $HOSTPORT"
expect_code "Host $HOSTPORT accepted" 200

req options OPTIONS /api/settings -H 'Origin: http://evil.example' \
    -H 'Access-Control-Request-Method: POST' -H 'Access-Control-Request-Headers: x-megaopen'
case "$CODE" in 2*) fail "CORS preflight" "answered $CODE" ;; *) pass "CORS preflight refused ($CODE)" ;; esac
if grep -qi '^access-control-allow' "$HDRS"; then fail "no CORS headers" "$(grep -i '^access-control' "$HDRS" | tr -d '\r')"; else pass "no Access-Control-Allow-* header"; fi

if fw_only "GET /update redirects"; then
    req update GET /update
    expect_code "GET /update redirects" 302
    if grep -qi '^location: /#board' "$HDRS"; then pass "  to /#board"; else fail "/update Location" "$(grep -i '^location' "$HDRS" | tr -d '\r')"; fi
fi

# ---- settings and the settings file (dry runs only) ----------------------------
req settings GET /api/settings
expect_code "GET /api/settings" 200
DEVMODE=$(grep -o '"devMode": *[a-z]*' "$BODY" | head -1)

req bad_two POST '/api/settings?dry=1' "${GATE[@]}" -H 'Content-Type: application/json' \
    --data '{"keyPopup":"x","logLevel":"NOPE"}'
expect_code "two bad values refused" 400
# Codes, one refusal a line ("key: code"): the page has the words (web/i18n e.*).
has "  keyPopup: bool" "^keyPopup: bool$"
has "  logLevel: enum (every refusal, not the first)" "^logLevel: enum$"

req config GET /api/config
expect_code "GET /api/config" 200
has "  a settings file" '"format": *"megaopen-config"'
if grep -qiE '"(apPass|pass|pass2)"' "$BODY"; then fail "config export" "carries a password key"; else pass "  no password in the export"; fi
cp "$BODY" "$TMP/config.json"

req config_dry POST '/api/config?dry=1' "${GATE[@]}" -H 'Content-Type: application/json' --data-binary "@$TMP/config.json"
expect_code "POST /api/config?dry=1 of the board's own file" 200
has "  nothing would change" '"changes": *\[\]'

req config_newer POST '/api/config?dry=1' "${GATE[@]}" -H 'Content-Type: application/json' \
    --data '{"format":"megaopen-config","schema":99,"settings":{}}'
expect_code "a newer schema refused" 400
has "  file_newer" "^file_newer$"

req config_pass POST '/api/config?dry=1' "${GATE[@]}" -H 'Content-Type: application/json' \
    --data '{"format":"megaopen-config","schema":1,"settings":{"apPass":"xxxxxxxxxx"}}'
expect_code "a file carrying apPass refused" 400
has "  apPass: file_forbidden" "^apPass: file_forbidden$"

# ---- the dev-only route ---------------------------------------------------------
req inforaw POST '/api/display/inforaw?len=0x0C&text=rc' "${GATE[@]}"
case "$DEVMODE" in
    *true) echo "SKIP  inforaw: devMode is on (switch it off and rerun to check the 403)" ;;
    *)     if [ "$CODE" = 404 ]; then echo "SKIP  inforaw: no display in this build"
           else expect_code "inforaw refused without devMode" 403; has "  dev_off" "^dev_off$"; fi ;;
esac

# ---- health: the HU dialect and the panel bus watch -----------------------------
req health GET /api/health
expect_code "GET /api/health" 200
has "  hu: dialect field" '"dialect"'
has "  hu: notSupported field" '"notSupported"'
has "  hu: climateChanges field" '"climateChanges"'
has "  panel: foreignMasters field" '"foreignMasters"'
has "  panel: clusterPings field" '"clusterPings"'
# Codes only: the page has the words (web/i18n); a chain's "why" is gone.
has "  chains carry a code" '"code":"panel_\|"code":"hu_'
hasnt "  no chain sends prose" '"code":"[a-z_]*","why"'
has "  clock: src field" '"id":"clock"[^}]*"src"'
for code in hu_dialect_vw panel_foreign_master panel_cluster_only; do
    if grep -q "\"$code\"" "$BODY"; then echo "NOTE  health reports $code"; fi
done

# ---- the page's English words (web/i18n/en.json, gzipped) ----------------------
req i18n GET /i18n/en.json --compressed
expect_code "GET /i18n/en.json" 200
has "  English words" '"nav.home":"Overview"'

# ---- logs -----------------------------------------------------------------------
req logs GET /api/logs
expect_code "GET /api/logs" 200
RING=$(grep -o '"ringSize": *[0-9]*' "$BODY" | head -1)
echo "      $RING"
if fw_only "log ring size"; then
    case "$RING" in *32768) pass "  log ring 32 kB (t2can)" ;; *4096) echo "NOTE  log ring 4 kB (a build without the t2can ring)" ;; *) fail "log ring" "$RING" ;; esac
fi
req logs_since GET '/api/logs?since=0'
expect_code "GET /api/logs?since=0" 200
if grep -q 'psram=' "$BODY"; then
    P=$(grep -o 'psram=[0-9]*' "$BODY" | head -1)
    case "$P" in psram=0) fail "boot line" "$P (no PSRAM: settings scratch falls back to the heap)" ;; *) pass "  boot line $P" ;; esac
else
    echo "NOTE  the [boot] psram= line has left the ring"
fi

# ---- GitHub update: check only, never install ---------------------------------
req fw GET /api/fw
if [ "$CODE" = 404 ]; then
    echo "SKIP  GitHub update: not in this build"
else
    expect_code "GET /api/fw" 200
    req fw_check POST /api/fw/check "${GATE[@]}"
    case "$CODE" in
        200) pass "POST /api/fw/check (200)"
             phase=""
             for _ in $(seq 1 20); do
                 req fw_poll GET /api/fw
                 phase=$(grep -o '"phase": *"[a-z]*"' "$BODY" | head -1)
                 case "$phase" in *checking*) sleep 1 ;; *) break ;; esac
             done
             case "$phase" in
                 *current*|*available*) pass "  check finished: $phase $(grep -o '"latest": *"[^"]*"' "$BODY")" ;;
                 *) fail "GitHub check" "ended as $phase $(grep -o '"err": *"[^"]*"' "$BODY")" ;;
             esac ;;
        409) echo "NOTE  GitHub check refused (409): $(head -c 120 "$BODY" | tr '\n' ' ')" ;;
        *)   fail "POST /api/fw/check" "got $CODE" ;;
    esac
fi

echo "rc_smoke: $PASSES passed, $FAILS failed"
[ "$FAILS" -eq 0 ]
