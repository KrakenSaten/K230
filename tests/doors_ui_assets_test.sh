#!/bin/bash
# The DOORS shell's runtime art (ui/assets/doors, ui/pocketui/pos_glyphs.[ch];
# tools/design/gen_doors_ui.py, DS §31.6):
#
#   1. the icons and glyphs are exactly what their sources make today
#      (regenerated in memory and compared; the backgrounds take a minute to
#      dither, so for them the manifest below stands in);
#   2. MANIFEST.txt names every art file with the hash it has and the hash of
#      the source it was made from, and both still hold - so neither the art
#      nor its source can change without the other being regenerated;
#   3. the launcher's colours (home_layout.c) are the ones each icon was
#      drawn in, and the environment's palette (pos_styles.c) is the
#      package's;
#   4. only runtime art is in the runtime directory - no PNG, no SVG, no
#      source - and it stays inside its size budget.
#
# Standard library python3 only. Run from anywhere; make test runs it.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" = "1" ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }
A=ui/assets/doors

# ---- 1. regenerated -------------------------------------------------------------
out=$(python3 tools/design/gen_doors_ui.py --check --only icons 2>&1); rc=$?
[ -n "$out" ] && echo "$out"
check "the portal icons are what gen_doors_ui.py draws from their sources" "$([ $rc = 0 ] && echo 1 || echo 0)"
out=$(python3 tools/design/gen_doors_ui.py --check --only glyphs 2>&1); rc=$?
[ -n "$out" ] && echo "$out"
check "pos_glyphs.c/.h are what gen_doors_ui.py makes from the package's glyphs" "$([ $rc = 0 ] && echo 1 || echo 0)"

# ---- 2..4 ----------------------------------------------------------------------
py=$(python3 - <<'PY'
import hashlib, os, re, sys
sys.dont_write_bytecode = True
A = "ui/assets/doors"
res = []
def check(what, ok):
    res.append(("ok   " if ok else "FAIL ") + what)
sha = lambda p: hashlib.sha256(open(p, "rb").read()).hexdigest()

rows = [l.split() for l in open(A + "/MANIFEST.txt", encoding="utf-8") if l.strip() and not l.startswith("#")]
listed = {r[1] for r in rows}
present = {A + "/" + n for n in os.listdir(A) if n.endswith(".bin")}
check("MANIFEST.txt lists every art file, and only those (%d)" % len(present), listed == present)
bad_out = [r[1] for r in rows if not os.path.isfile(r[1]) or sha(r[1]) != r[0]]
check("every art file has the hash MANIFEST.txt records%s" % (" (not: %s)" % bad_out if bad_out else ""), not bad_out)
bad_src = [r[3] for r in rows if not os.path.isfile(r[3]) or sha(r[3]) != r[2]]
check("every source still has the hash it had when the art was made%s" % (" (changed: %s)" % bad_src if bad_src else ""),
      not bad_src)
check("every background comes from the approved package's device exports",
      all(r[3].startswith("docs/design/brand/doors-visual-pack-v1/device/backgrounds/") for r in rows if "/bg-" in r[1]))

# 3. colours: launcher table vs generator, env palette vs package
gen = open("tools/design/gen_doors_ui.py", encoding="utf-8").read()
app_hue = dict(re.findall(r'^\s+"(\w+)": \([^,]+, "(\w+)"\),', gen, re.M))
table = {i: h.lower() for i, h in re.findall(r'\{ "(\w+)", HOME_GROUP_\w+, HOME_HUE_(\w+) \}',
                                              open("ui/shell/home_layout.c", encoding="utf-8").read())}
check("the launcher's table and the icon generator give every app the same colour (%d apps)" % len(table),
      table == app_hue and len(table) == 14)
palette = re.search(r"PALETTE = \{(.*?)\}", gen, re.S).group(1)
pal = dict(re.findall(r'"(\w+)": "#([0-9a-f]{6})"', palette))
styles = open("ui/pocketui/pos_styles.c", encoding="utf-8").read()
hues = re.findall(r"0x([0-9a-f]{6})", re.search(r"env_hues\[POS_HUE_COUNT\] = \{(.*?)\};", styles, re.S).group(1))
order = ["radio", "mesh", "network", "tools", "ai", "games", "settings", "files", "apps"]
check("the environment's hues are the package's b_ui_layout.json colours, in pos_env_hue order",
      hues == [pal[k] for k in order])
import json
b = json.load(open("docs/design/brand/doors-visual-pack-v1/reference/layout/b_ui_layout.json", encoding="utf-8"))
check("and the generator's palette is b_ui_layout.json's", {k: "#" + v for k, v in pal.items()} == b["colors"])

# 4. what the runtime directory may hold, and how much
others = [n for n in os.listdir(A) if not n.endswith(".bin") and n != "MANIFEST.txt"]
check("the runtime directory holds only .bin art and its manifest%s" % (" (also: %s)" % others if others else ""),
      not others)
total = sum(os.path.getsize(p) for p in present)
check("the art is %d bytes, inside the 9 MiB budget" % total, total <= 9 * 1024 * 1024)
print("\n".join(res))
PY
)
printf '%s\n' "$py"
oks=$(printf '%s\n' "$py" | grep -c '^ok')
failed=$((failed + $(printf '%s\n' "$py" | grep -c '^FAIL')))
check "the manifest, colour and budget checks ran" "$([ "$oks" -ge 9 ] && echo 1 || echo 0)"
echo "doors_ui_assets_test: $failed failure(s)"
exit $((failed > 0))
