#!/bin/bash
# Every registered app, in both orientations, at every text size (DS §46.6):
# the shell opens it, and the layout audit (shell --audit, pocketui_audit.h)
# measures the screen it settles on.
#
#   - at Medium and Large nothing may be clipped, drawn over something else
#     or left without a size that was not so at Small: a larger size may not
#     cost an app anything Small had;
#   - Small itself is reported, not judged here - it is master's layout, and
#     tests/text_size_shell_test.sh and the Small comparison (DS §46.8)
#     hold it to that;
#   - text shortened with "..." is listed: a label in the dots mode is an
#     app's own rule for a name wider than its place, at any size;
#   - the few things the audit cannot tell from a real problem are named
#     below with the reason, and nothing else is let through.
#
# Requires: SHELL_BIN (the CMake-built pocketos-shell). About five minutes.
set -u
SHELL_BIN=${SHELL_BIN:?set SHELL_BIN to the pocketos-shell binary}
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

# kind|app|orientation|size|path - and why it is not a problem.
#   Calendar's today mark is a bullet glyph under the day's number; their line
#   boxes touch at Large in the landscape cell, the ink does not (checked in
#   the screenshots, DS §46.5).
#   RIFT's traffic graph caption ("HEARD ON AIR · 20 MIN · NOTHING YET") is cut
#   by its legend on one line; master already cuts it so at Small in portrait,
#   and the larger sizes do the same in landscape. Changing how it is cut
#   would change Small (DS §46.8).
ALLOW="overlap|calendar|landscape|large|1.2.1.0.1.1.3.0
clipped|rift|landscape|medium|1.2.0.0.1.0.0.1.2.1.0
clipped|rift|landscape|large|1.2.0.0.1.0.0.1.2.1.0"

WORK=$(mktemp -d)
APPS=$(python3 - <<'PY'
import re
src = open("ui/shell/shell.c", encoding="utf-8").read()
reg = re.search(r"static const struct pocketos_app \*const apps\[\] = \{(.*?)\};", src, re.S).group(1)
ids = re.findall(r"&app_(\w+)", reg)
if "OPTIONAL_APPS" in reg:
    ids.append("zabbix")
print(" ".join("2048" if i == "2048" else i for i in ids))
PY
)
check "the registry lists apps ($(echo $APPS | wc -w))" "$([ "$(echo $APPS | wc -w)" -ge 26 ] && echo 1 || echo 0)"

audit_one() { # app rot size
    local RUN LOGD CFG STATE
    RUN=$(mktemp -d -p "$WORK"); LOGD=$(mktemp -d -p "$WORK"); CFG=$(mktemp -d -p "$WORK")
    STATE=$(mktemp -d -p "$WORK")
    SDL_VIDEODRIVER=dummy POCKETOS_RUNTIME_DIR="$RUN" POCKETOS_LOG_DIR="$LOGD" POCKETOS_CONFIG_DIR="$CFG" \
    POCKETOS_STATE_DIR="$STATE" HOME="$STATE/home" \
        timeout 60 "$SHELL_BIN" --no-lock --rotation "$2" --open "$1" --text-size "$3" \
        --audit "$WORK/$1-$2-$3.json" --exit-after-ms 2500 >/dev/null 2>&1
    rm -rf "$RUN" "$LOGD" "$CFG" "$STATE"
}
for app in $APPS; do
    for rot in portrait landscape; do
        for z in small medium large; do
            audit_one "$app" "$rot" "$z" &
        done
        wait
    done
done

python3 - "$WORK" "$ALLOW" $APPS <<'PY'
import json, os, sys
work, allow, apps = sys.argv[1], set(sys.argv[2].split()), sys.argv[3:]
failed = 0
def check(what, ok):
    global failed
    print(("ok   " if ok else "FAIL ") + what)
    failed += 0 if ok else 1
def load(app, rot, z):
    p = os.path.join(work, "%s-%s-%s.json" % (app, rot, z))
    return json.load(open(p, encoding="utf-8")) if os.path.exists(p) else None
judged = ("clipped", "overlap", "zero")
for app in apps:
    for rot in ("portrait", "landscape"):
        small = load(app, rot, "small")
        check("%s %s: audited at Small" % (app, rot), small is not None and small["current"] == app)
        if small is None:
            continue
        base = {(i["kind"], i["path"]) for i in small["issues"]}
        if any(small["count"][k] for k in judged):
            print("note %s %s small: %s (master's layout)" % (app, rot, json.dumps(small["count"])))
        for z in ("medium", "large"):
            j = load(app, rot, z)
            check("%s %s: audited at %s" % (app, rot, z), j is not None and j["text_size"] == z)
            if j is None:
                continue
            new = [i for i in j["issues"] if i["kind"] in judged and (i["kind"], i["path"]) not in base and
                   "|".join((i["kind"], app, rot, z, i["path"])) not in allow]
            for i in new[:6]:
                print("     %s %s \"%s\" at %d,%d %dx%d" % (i["kind"], i["path"], i["text"], i["x"], i["y"], i["w"], i["h"]))
            check("%s %s %s: nothing clipped, overlapping or sizeless that Small did not have" % (app, rot, z),
                  not new)
            tr = [i["text"] for i in j["issues"] if i["kind"] == "truncated"]
            if tr:
                print("note %s %s %s: shortened with dots: %s" % (app, rot, z, ", ".join(tr)))
sys.exit(1 if failed else 0)
PY
rc=$?
check "every app at every size keeps what Small shows" "$([ $rc = 0 ] && echo 1 || echo 0)"
rm -rf "$WORK"
echo "text_size_layout_shell_test: $failed failure(s)"
exit $((failed > 0))
