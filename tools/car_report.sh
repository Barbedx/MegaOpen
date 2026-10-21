#!/usr/bin/env bash
# After a drive: everything the board can tell about it, in one folder and
# one archive. Read-only: GETs only, never a POST, so no X-MegaOpen header.
#
#   tools/car_report.sh http://192.168.4.1          (in the car, the board's AP)
#   tools/car_report.sh http://megaopen.local [dir] (at home)
#
# Writes <dir or .>/car-report-<UTC time>/ and the same as .tar.gz beside it:
#   status.json health.json settings.json config.json (the settings file, no
#   passwords) map-live.json logs-live.json (the RAM ring, paged) log-0.txt
#   (this boot, from flash) log-1.txt (the boot before) fw.json cap-list.json
#   summary.txt
# Each request has 5 s; one that fails is listed in summary.txt and the rest
# still run. Needs bash, curl, grep, sed, tar (Git Bash on Windows has them).
#
# The board answers only to its IP or <name>.local (Host check), so use one of
# those, not a router's DNS name. /api/logs/file?n=0 makes the board write its
# buffered log to flash first (up to 500 ms); nothing else here changes state.
set -u

B="${1:-}"
[ -n "$B" ] || { echo "usage: $0 http://<board-ip or name.local> [out-dir]" >&2; exit 2; }
B="${B%/}"
case "$B" in http://*|https://*) ;; *) B="http://$B" ;; esac
OUT="${2:-.}"
STAMP="$(date -u +%Y%m%d-%H%M%SZ)"
D="$OUT/car-report-$STAMP"
mkdir -p "$D" || exit 1

FAILED=""
# get <path> <file>: one GET with a 5 s budget; a non-2xx or no answer is
# recorded, and the file keeps whatever came back for a look.
get() {
    local code
    code=$(curl -s -m 5 -o "$D/$2" -w '%{http_code}' "$B$1") || code="000"
    if [ "${code:0:1}" != "2" ]; then
        FAILED="$FAILED  $1 -> HTTP $code"$'\n'
        return 1
    fi
    return 0
}

echo "car_report: $B -> $D"
if ! get /api/status status.json; then
    echo "car_report: the board does not answer at $B (see $D)" >&2
    printf 'No answer from %s at %s UTC.\n' "$B" "$STAMP" > "$D/summary.txt"
    exit 2
fi
get /api/health   health.json
get /api/settings settings.json
get /api/config   config.json
get "/api/map?live=1" map-live.json
get /api/fw       fw.json
get /api/cap/list cap-list.json
get "/api/logs/file?n=0" log-0.txt
get "/api/logs/file?n=1" log-1.txt

# The RAM ring, 200 lines a page: follow "next" while "more" is true. One
# JSON object per line in logs-live.json, the pages as they came.
: > "$D/logs-live.json"
since=0
for _ in $(seq 1 100); do
    page=$(curl -s -m 5 "$B/api/logs?since=$since") || { FAILED="$FAILED  /api/logs?since=$since -> no answer"$'\n'; break; }
    printf '%s\n' "$page" >> "$D/logs-live.json"
    next=$(printf '%s' "$page" | grep -o '"next":[0-9]*' | tail -1 | cut -d: -f2)
    printf '%s' "$page" | grep -q '"more":true' || break
    [ -n "$next" ] && [ "$next" != "$since" ] || break
    since=$next
done

# ---- summary.txt ------------------------------------------------------------
field() { grep -o "\"$1\":\"[^\"]*\"" "$D/$2" 2>/dev/null | head -1 | cut -d'"' -f4; }
num()   { grep -o "\"$1\":[0-9-]*" "$D/$2" 2>/dev/null | head -1 | cut -d: -f2; }
chain() {   # id -> "state code"
    local o
    o=$(grep -o "{\"id\":\"$1\",\"state\":\"[^\"]*\",\"code\":\"[^\"]*\"" "$D/health.json" 2>/dev/null | head -1)
    [ -n "$o" ] && printf '%s %s' "$(printf '%s' "$o" | cut -d'"' -f8)" "$(printf '%s' "$o" | cut -d'"' -f12)" || printf -- '-'
}
up=$(num uptimeMs status.json)
upt="-"
[ -n "$up" ] && upt=$(printf '%dh %02dm %02ds' $((up / 3600000)) $((up / 60000 % 60)) $((up / 1000 % 60)))
# [hb] lines: "heap=N min=N"; the lowest min this boot, from the ring.
hmin=$(grep -o '\[hb\][^"]*min=[0-9]*' "$D/logs-live.json" 2>/dev/null | grep -o 'min=[0-9]*' | cut -d= -f2 | sort -n | head -1)
cnt()  { grep -o "\"l\":\"$1\"" "$D/logs-live.json" 2>/dev/null | wc -l | tr -d ' '; }
fcnt() { [ -s "$D/$2" ] && grep -oE "(^| )$1 \[" "$D/$2" | wc -l | tr -d ' ' || printf -- '-'; }

{
    echo "MegaOpen car report, $STAMP UTC, $B"
    echo
    echo "fw        $(field fw status.json)  build $(field build status.json)"
    echo "uptime    $upt"
    echo "heap      now $(num heap status.json) B, lowest [hb] min ${hmin:--} B"
    echo "wifi      $(field mode status.json) $(field ssid status.json), rssi $(num rssi status.json)"
    echo
    echo "health (state code)"
    for c in panel media clock keys ancs hu knobs lights phone; do
        printf '  %-7s %s\n' "$c" "$(chain "$c")"
    done
    echo
    echo "log lines  ERR  WRN"
    printf '  ring     %4s %4s\n' "$(cnt ERR)" "$(cnt WRN)"
    printf '  log-0    %4s %4s   (this boot, flash)\n' "$(fcnt E log-0.txt)" "$(fcnt W log-0.txt)"
    printf '  log-1    %4s %4s   (the boot before)\n' "$(fcnt E log-1.txt)" "$(fcnt W log-1.txt)"
    echo
    if [ -n "$FAILED" ]; then echo "failed requests:"; printf '%s' "$FAILED"; else echo "every request answered"; fi
} > "$D/summary.txt"

# --force-local: a Windows path with "C:" is otherwise read as a remote host.
TF=""; tar --help 2>/dev/null | grep -q force-local && TF="--force-local"
tar $TF -czf "$D.tar.gz" -C "$OUT" "car-report-$STAMP" && echo "car_report: $D.tar.gz"
cat "$D/summary.txt"
