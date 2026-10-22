/*
 * RIFT's automatic capital, without a display (rift_autocap.h): where the
 * next letter starts a sentence - an empty message, after ". ", "! ", "? ",
 * Norwegian letters as letters - and where it does not: inside a word, a
 * number, a URL or an address, after an ellipsis, and once a letter has
 * been typed. The caret, not the end of the text, decides; an emoji, a digit
 * or a quotation mark before the first letter leaves the capital due.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rift_autocap.h"

#include <stdio.h>
#include <string.h>

static int failed;
static int checks;

/* Characters in s, as LVGL's caret counts them. */
static unsigned chars(const char *s)
{
    unsigned n = 0;

    for (; *s; s++) {
        n += ((unsigned char)*s & 0xC0u) != 0x80u;
    }
    return n;
}

/* At the end of text. */
static void at_end(const char *text, int want)
{
    int got = rift_autocap_wanted(text, text ? chars(text) : 0);

    checks++;
    if (got != want) {
        failed++;
        printf("FAIL \"%s\" + caret at the end: %s, want %s\n", text, got ? "capital" : "lower",
               want ? "capital" : "lower");
    }
}

/* With the caret where "|" is in marked; the "|" is not part of the text. */
static void at_mark(const char *marked, int want)
{
    char text[256];
    const char *bar = strchr(marked, '|');
    size_t before = (size_t)(bar - marked);
    unsigned caret;
    int got;

    memcpy(text, marked, before);
    text[before] = '\0';
    caret = chars(text);
    snprintf(text + before, sizeof(text) - before, "%s", bar + 1);
    got = rift_autocap_wanted(text, caret);
    checks++;
    if (got != want) {
        failed++;
        printf("FAIL \"%s\": %s, want %s\n", marked, got ? "capital" : "lower", want ? "capital" : "lower");
    }
}

#define THUMB "\xF0\x9F\x91\x8D"   /* U+1F44D */
#define HEART "\xE2\x9D\xA4\xEF\xB8\x8F" /* U+2764 U+FE0F */
#define ELLIPSIS "\xE2\x80\xA6"    /* U+2026 */

int main(void)
{
    /* A new message. */
    at_end("", 1);
    at_end(NULL, 1);
    at_end(" ", 1);
    at_end("h", 0);
    at_end("Hei", 0);

    /* Sentence ends. */
    at_end("Hei. ", 1);
    at_end("Hei! ", 1);
    at_end("Hei? ", 1);
    at_end("Hva?! ", 1);
    at_end("Nei!!! ", 1);
    at_end("Hei.  ", 1);
    at_end("Hei.\n", 1);
    at_end("Hei.", 0);  /* no space yet: could be "vg.no" */
    at_end("Hei. d", 0);
    at_end("Hei. Du", 0);
    at_end("Hei du ", 0);  /* every word is not capitalised */
    at_end("Hei, ", 0);
    at_end("Hei: ", 0);
    at_end("Hei; ", 0);
    at_end("Han sa \"stopp.\" ", 1);
    at_end("(ja!) ", 1);

    /* Norwegian letters are letters, and end nothing. */
    at_end("\xC3\xA6", 0);         /* æ */
    at_end("\xC3\xB8l", 0);        /* øl */
    at_end("\xC3\x85", 0);         /* Å */
    at_end("Bl\xC3\xA5. ", 1);     /* Blå. */
    at_end("S\xC3\xB8t\xC3\xB8. ", 1);
    at_end("Bl\xC3\xA5 ", 0);
    at_end("God tur \xC3\xB8st. ", 1);
    at_end("Hei. \xC3\xA6", 0);    /* typed after the capital was due */

    /* What is not a sentence end. */
    at_end("Det er 3.5 ", 0);      /* a decimal: a letter came before it */
    at_end("3.5 ", 1);             /* leading digits leave the capital due */
    at_end("Pi er 3.14", 0);
    at_end("Se vg.no ", 0);
    at_end("Se www.vg.no/nyheter ", 0);
    at_end("Se https://example.com/a.b ", 0);
    at_end("Mail meg p\xC3\xA5 ola@example.no ", 0);
    at_end("Mail ola@example.no", 0);
    at_end("Vent... ", 0);
    at_end("Vent.. ", 0);
    at_end("Vent" ELLIPSIS " ", 0);
    at_end("Vent" ELLIPSIS, 0);
    at_end("Hva?... ", 1);         /* a question mark ends it, dots or not */
    at_end("Se vg.no. ", 1);       /* the sentence after the address does */

    /* What comes before the first letter does not use the capital up. */
    at_end(THUMB, 1);
    at_end(THUMB " ", 1);
    at_end(HEART " ", 1);
    at_end("12 ", 1);
    at_end("\"", 1);
    at_end("- ", 1);
    at_end("Hei. " THUMB " ", 1);
    at_end("Hei. 12 ", 1);
    at_end("Hei " THUMB " ", 0);
    at_end(THUMB "h", 0);

    /* The caret decides, not the end of the text. */
    at_mark("|hei", 1);
    at_mark("H|ei", 0);
    at_mark("Hei. |du er der", 1);
    at_mark("Hei. d|u er der", 0);
    at_mark("Hei du|. Er", 0);
    at_mark("Hei du. |Er", 1);
    at_mark("vg.|no", 0);
    at_mark("Bl\xC3\xA5. |\xC3\xB8l", 1);
    at_mark(THUMB "|", 1);
    at_mark("Ja. " THUMB "| ok", 1);
    /* A caret past the end is at the end. */
    {
        int got = rift_autocap_wanted("Hei. ", 99);

        checks++;
        if (!got) {
            failed++;
            printf("FAIL a caret past the end is at the end\n");
        }
    }
    /* A stray byte is a character of its own, and not a letter. */
    at_end("Hei. \x80", 1);
    at_end("Hei\xC3", 0); /* a cut-off character after a word: not a sentence end */

    printf("rift_autocap_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
