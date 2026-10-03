/*
 * RIFT colour emoji: the generated RGB565A8 pixels (rift_emoji_px.bin, from
 * tools/design/gen_rift_emoji.js) embedded as read-only data. The build puts
 * this file's directory on the assembler's include path (-Wa,-I...).
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
    .section .rodata.rift_emoji_px, "a"
    .balign 4
    .global rift_emoji_px
    .type rift_emoji_px, @object
rift_emoji_px:
    .incbin "rift_emoji_px.bin"
    .size rift_emoji_px, . - rift_emoji_px

    .section .note.GNU-stack, "", @progbits
