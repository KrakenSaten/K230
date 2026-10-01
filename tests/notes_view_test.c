/*
 * PocketNotes text rules: titles, blankness and what counts as text.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "notes_view.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

static void title_is(const char *what, const char *text, const char *want)
{
    char got[NOTES_TITLE_MAX];

    checks++;
    notes_title_from_text(text, got, sizeof(got));
    if (strcmp(got, want) != 0) {
        failed++;
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
    }
}

static void check(const char *what, int ok)
{
    checks++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    }
}

int main(void)
{
    char small[8];

    /* ---- the title is the first line that says something ---- */
    title_is("a single line is the title", "Shopping", "Shopping");
    title_is("only the first line", "Shopping\nmilk\nbread", "Shopping");
    title_is("a trailing newline is not part of it", "Shopping\n", "Shopping");
    title_is("leading blank lines are skipped", "\n\n  \nShopping\nmilk", "Shopping");
    title_is("surrounding whitespace is trimmed", "   Shopping   \nmilk", "Shopping");
    title_is("inner spaces are kept", "buy  milk today", "buy  milk today");
    title_is("tabs count as whitespace", "\t\tShopping", "Shopping");
    title_is("carriage returns do not survive", "Shopping\r\nmilk", "Shopping");

    /* ---- nothing to title ---- */
    title_is("empty text is untitled", "", NOTES_UNTITLED);
    title_is("whitespace only is untitled", "   \n\t\n  ", NOTES_UNTITLED);
    title_is("NULL is untitled", NULL, NOTES_UNTITLED);

    /* ---- truncation never splits a character ---- */
    notes_title_from_text("abcdefghijklmnop", small, sizeof(small));
    check("a long title is truncated to fit", strlen(small) == sizeof(small) - 1);

    /* æøæøæø: two bytes each, so a 8-byte buffer holds three of them and the
     * terminator, never half of the fourth. */
    notes_title_from_text("\xC3\xA6\xC3\xB8\xC3\xA6\xC3\xB8\xC3\xA6", small, sizeof(small));
    check("truncation lands on a character boundary", notes_text_is_utf8(small));
    check("and keeps whole characters only", strlen(small) % 2 == 0);

    /* ---- blankness ---- */
    check("empty is blank", notes_text_is_blank(""));
    check("spaces are blank", notes_text_is_blank("   "));
    check("newlines are blank", notes_text_is_blank("\n\n\n"));
    check("mixed whitespace is blank", notes_text_is_blank(" \t\r\n\v\f"));
    check("NULL is blank", notes_text_is_blank(NULL));
    check("a letter is not blank", !notes_text_is_blank("a"));
    check("a letter among spaces is not blank", !notes_text_is_blank("   a   "));
    check("a Latin-1 letter is not blank", !notes_text_is_blank("\xC3\xA6"));

    /* ---- UTF-8 ---- */
    check("ASCII is UTF-8", notes_text_is_utf8("plain text"));
    check("empty is UTF-8", notes_text_is_utf8(""));
    check("æøå is UTF-8", notes_text_is_utf8("\xC3\xA6\xC3\xB8\xC3\xA5"));
    check("three-byte characters are UTF-8", notes_text_is_utf8("\xE2\x82\xAC"));
    check("a lone continuation byte is not", !notes_text_is_utf8("\xA6"));
    check("a truncated two-byte character is not", !notes_text_is_utf8("\xC3"));
    check("a lead byte followed by ASCII is not", !notes_text_is_utf8("\xC3z"));
    check("an invalid lead byte is not", !notes_text_is_utf8("\xFF"));
    check("NULL is not", !notes_text_is_utf8(NULL));

    /* ---- characters, which is what the editor's cap counts ---- */
    check("an empty note holds no characters", notes_text_chars("") == 0);
    check("NULL holds none", notes_text_chars(NULL) == 0);
    check("ASCII is one character a byte", notes_text_chars("Shopping\nmilk") == 13);
    check("æøå are three characters in six bytes",
          notes_text_chars("\xC3\xA6\xC3\xB8\xC3\xA5") == 3);
    check("a three-byte character is one", notes_text_chars("\xE2\x82\xAC") == 1);
    check("a four-byte character is one", notes_text_chars("\xF0\x9F\x93\x9D") == 1);
    check("mixed text counts characters, not bytes",
          notes_text_chars("a\xC3\xA6\xE2\x82\xAC b") == 5);

    printf("notes_view_test: %d checks, %d failure(s)\n", checks, failed);
    return failed ? 1 : 0;
}
