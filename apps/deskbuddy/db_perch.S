/* Original indexed artwork from tools/design/deskbuddy. SPDX-License-Identifier: Apache-2.0 */
.section .rodata.db_perch, "a"
.balign 4
.global db_perch_pixels
.type db_perch_pixels, @object
db_perch_pixels:
.incbin "perch.bin"
.size db_perch_pixels, . - db_perch_pixels
.section .note.GNU-stack, "", @progbits
