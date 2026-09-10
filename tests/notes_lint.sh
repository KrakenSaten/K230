#!/bin/bash
# PocketNotes layering, the same rules the other apps are held to.
set -u
cd "$(dirname "$0")/.." || exit 1
failed=0
check() { if [ "$2" -eq 1 ]; then echo "ok   $1"; else echo "FAIL $1"; failed=$((failed + 1)); fi; }

hits=$(grep -lE 'lvgl|lv_obj|lv_label' apps/notes/notes_view.c apps/notes/notes_view.h apps/notes/notes_store.c apps/notes/notes_store.h 2>/dev/null)
check "the text rules and the store are free of LVGL" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

hits=$(grep -lE 'fopen|open\(|unlink|mkdir|rename|opendir' apps/notes/notes_view.c apps/notes/notes_app.c 2>/dev/null)
check "only notes_store.c touches the filesystem" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits"

# DS 17.4: an app asks the shell for the keyboard and never holds one.
hits=$(grep -nE 'pos_keyboard|lv_keyboard' apps/notes/*.c apps/notes/*.h 2>/dev/null)
check "Notes never names a keyboard (DS 17.4)" "$([ -z "$hits" ] && echo 1 || echo 0)"
[ -n "$hits" ] && echo "$hits" | head -5

check "Notes asks the shell for it instead" \
    "$(grep -q 'pocketos_shell_keyboard_show' apps/notes/notes_app.c && echo 1 || echo 0)"

# A title is user input; it must never reach a path.
check "note filenames are generated, not derived from a title" \
    "$(grep -q 'NOTES_FILE_FMT' apps/notes/notes_store.c && echo 1 || echo 0)"
hits=$(grep -nE 'title.*(fopen|path)|path.*title' apps/notes/notes_store.c 2>/dev/null)
check "no title ever reaches a filename" "$([ -z "$hits" ] && echo 1 || echo 0)"

# The write must stay atomic.
for want in 'fsync' 'rename' '\.tmp'; do
    check "the store write uses $want" \
        "$(grep -qE "$want" apps/notes/notes_store.c && echo 1 || echo 0)"
done

# Scope: the MVP has none of these. Prose is not code, so comment lines are
# dropped first - the editor's confirmation says a delete cannot be undone,
# and that sentence is not an undo system.
code() { grep -vE '^[[:space:]]*(/\*|\*|//)' apps/notes/*.c apps/notes/*.h 2>/dev/null; }
for banned in search folder tag undo sort; do
    hits=$(code | grep -niE "[a-z_]*${banned}[a-z_]*[[:space:]]*[(=]|${banned}_")
    check "no $banned machinery in the MVP" "$([ -z "$hits" ] && echo 1 || echo 0)"
    [ -n "$hits" ] && echo "$hits" | head -3
done

echo "notes_lint: $failed failure(s)"
exit $((failed > 0))
