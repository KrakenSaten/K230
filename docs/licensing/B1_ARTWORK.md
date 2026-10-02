# B1: artwork rights for the public source

Status: **RESOLVED** by the product owner on 2026-10-02. Part of
docs/licensing/APACHE_2_READINESS.md (§6, §14). Every asset has a class
in `docs/licensing/asset-inventory.txt`, which `tests/license_audit_test.sh`
holds complete.

## The owner's answers (2026-10-02)

| Question | Answer | Evidence besides the answer |
| --- | --- | --- |
| Who made the Threshold package, the icon extension and the visual pack (v1 with the B package v2)? | The owner, with ChatGPT. | Visual pack: its PROVENANCE.md ("the built-in image-generation tool ... in this conversation") and OpenAI content credentials in the masters and source backgrounds. Threshold and the extension carry no tool marks; the extension's PNGs were rendered with Inkscape. |
| Do the tool's terms give the owner the output? | Yes, from OpenAI's own terms. | OpenAI Terms of Use (effective 1 January 2026) and Europe Terms of Use, "Ownership of content": "As between you and OpenAI, and to the extent permitted by applicable law, you (a) retain your ownership rights in Input and (b) own the Output. We hereby assign to you all our right, title, and interest, if any, in and to Output." Read at openai.com/policies/row-terms-of-use and /eu-terms-of-use on 2026-10-02 (DOCUMENTED). The assignment excludes other users' output and "Third Party Output". |
| What went in? | Only the owner's own material: Doors screenshots, the owner's notes, and images ChatGPT generated for the owner in those conversations. | Threshold's ASSET-NOTES.md: "prepared from the supplied 2026-09-13 CONTEXT.md and screenshots" - CONTEXT.md is Doors' own hand-off (docs/design/astra-handoff/). |
| Who is "Astra"? | A GPT model in ChatGPT, not a person or company. | docs/design/astra-handoff/CONTEXT.md is titled "handoff for Astra". |
| Licence for the art? | Apache-2.0, with the brand reserved. Others may redistribute the brand files unmodified as part of Doors or a work based on it. | docs/licensing/BRAND.md, NOTICE |

Two caveats are recorded rather than resolved, because they are not
licence choices. Whether copyright subsists in AI-generated images is not
settled; the licence covers whatever rights exist, and the owner publishes on
that basis. And "output may not be unique" (OpenAI's terms): another user
may have received similar images.

## Result

| Class | What it is | Total | In the public source | Licence |
| --- | --- | --- | --- | --- |
| ORIGINAL | drawn or generated in this repository (`doors-app-icons`, `doors-glyphs`, `timber-art`), and captures made before the ChatGPT art | 108 | 108 | Apache-2.0 |
| OWNER-VECTOR | Threshold icons, icon extension, B package icons, portal frame, glyphs and UI layers | 158 | 158 | Apache-2.0 |
| OWNER-AI-RASTER | the photographic door and mountain-lake backgrounds and their masters | 25 | 16 | Apache-2.0, to the extent rights subsist |
| BRAND | the Doors mark, the lockups with the wordmark (outlined IBM Plex Sans, OFL) and the boot art | 16 | 16 | **reserved** (BRAND.md) |
| DERIVED-ART | generated here and shipped: `ui/assets/doors/*.bin` (29 icons, 6 backgrounds), `pos_app_icons.c`, `pos_glyphs.c/.h` | 38 | 38 | Apache-2.0 |
| DERIVED-BRAND | generated here and shipped: `logo.xrgb` (boot splash), `pos_brand_mark.c` | 2 | 2 | **reserved** (BRAND.md) |
| CAPTURE | captures of Doors from 2026-09-15 09:09 on | 103 | 103 | Apache-2.0; any Doors mark shown keeps BRAND.md's terms |
| OWNER-REF | mockups, style sheet, overview sheet, archive copies | 170 | 0 | the owner's; excluded as reference only |
| DESIGN-EXPORT | RIFT design package, Design System bundle, design zip (B2) | 14 | 0 | excluded; not covered by these answers |
| THIRD-PARTY | Nimbus Sans fonts (AGPL-3.0 with font exception) | 2 | 0 | its own; excluded |

The reference-only parts stay excluded although their rights are now
clear: nothing needs them, and the archive copies hold the Nimbus Sans
fonts. They can be re-admitted later by editing
docs/licensing/public-source-exclude.txt, without the fonts.

The RIFT design package (made with Claude, 2026-09-19) was not part of
these answers and stays excluded (B2).
