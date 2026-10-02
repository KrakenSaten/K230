# B1: artwork rights for the public source

Status 2026-10-02: **open - needs the owner's answers below.** Part of
docs/licensing/APACHE_2_READINESS.md (§6, §14). Every asset has a class in
`docs/licensing/asset-inventory.txt`, which `tests/license_audit_test.sh`
holds complete: an asset with no class, a rule that matches nothing, or a
reference/export/third-party asset inside the public-source candidate fails
the test.

## What there is

Counts of tracked assets (images, vector files, binary art, design bundles),
and how many are in the public-source candidate
(`tools/legal/public_source_tree.sh`):

| Class | What it is | Total | In the candidate | Status |
| --- | --- | --- | --- | --- |
| ORIGINAL | drawn or generated in this repository (`doors-app-icons`, `doors-glyphs`, `timber-art`), and captures made before any supplied art existed | 108 | 108 | Apache-2.0 |
| CAPTURE-B1 | captures of Doors changed on or after 2026-09-15 09:09 (+02:00), when the first supplied art entered the tree; they may show it | 103 | 103 | follows the answer for the art they show |
| SUPPLIED-VECTOR | Threshold icons, icon extension, B package icons, portal frame, glyphs and UI layers | 158 | 158 | **UNKNOWN** |
| SUPPLIED-BRAND | Threshold mark, lockups (wordmark in outlined IBM Plex Sans) and boot art | 16 | 16 | **UNKNOWN** |
| AI-RASTER | the photographic door and mountain-lake backgrounds and their masters | 25 | 16 | **UNKNOWN** |
| DERIVED-B1 | generated here from the above and shipped: `ui/assets/doors/*.bin` (29 icons, 6 backgrounds), `pos_app_icons.c`, `pos_glyphs.c/.h` | 38 | 38 | follows its inputs |
| DERIVED-BRAND | generated here from the brand art and shipped: `logo.xrgb` (boot splash), `pos_brand_mark.c` | 2 | 2 | follows its inputs |
| SUPPLIED-REF | supplier mockups, style sheet, overview sheet, byte copies of the archives | 170 | 0 | excluded |
| DESIGN-EXPORT | RIFT design package, Design System bundle, design zip (B2) | 14 | 0 | excluded |
| THIRD-PARTY | Nimbus Sans fonts (AGPL-3.0 with font exception) | 2 | 0 | excluded |

### The packages, and what the files themselves say

| Package | Supplied | What the evidence shows | Not shown anywhere |
| --- | --- | --- | --- |
| **Threshold** (`docs/design/brand/doors-threshold/`) | 2026-09-15, by the owner | vector geometry; "prepared from the supplied 2026-09-13 CONTEXT.md and screenshots" (ASSET-NOTES.md); CONTEXT.md is Doors' own handoff "for Astra"; lettering is outlined IBM Plex (OFL); no embedded tool or AI marks | who made it, with what, under what terms |
| **Icon extension** (`doors-icon-extension/`) | 2026-09-15, by the owner | same family and style; PNGs rendered with Inkscape; notes in Norwegian (LES-MEG.md) | same |
| **Visual pack v1 + B package v2** (`doors-visual-pack-v1/`) | 2026-09-22, by the owner | photographic art made "with the built-in image-generation tool from the user's approved DOORS references in this conversation" (PROVENANCE.md); OpenAI content credentials on the masters, the source backgrounds and `approved_B.png`; icons, frame, glyphs and layers are SVG made by the supplier's script `build_b.py` | which service and account; what "the user's approved DOORS references" were |
| RIFT design package (`docs/design/rift/`, excluded) | 2026-09-19 | content credentials "Claude provided this file" on all shots | same; only needed to re-admit `HANDOFF.md`/`README.md` |

Third-party inputs known so far: IBM Plex (OFL-1.1) outlined in the
wordmark and overview sheets, and Nimbus Sans (excluded). Nothing else is
recorded.

## The options

1. **Confirm and license (recommended if the answers allow it).** The owner
   answers the questions below; the art is then published under the chosen
   licence and the inventory classes change to that licence. Nothing in the
   build changes.
2. **Keep the brand reserved.** As option 1, but the Doors mark, wordmark
   and boot splash (SUPPLIED-BRAND, DERIVED-BRAND) stay "all rights
   reserved": they can be in the public source with that statement, while
   the icons, glyphs and backgrounds take an open licence. Common for
   open-source projects; NOTICE already says the licence grants no rights to
   the names or logos.
3. **Leave it out.** The supplied packages can be dropped from the public
   source only together with what is generated from them, and those are
   build inputs: the shell's icons, glyphs, mark and backgrounds and the boot
   splash. That means replacing them with original art first (first-party
   icons exist for 10 apps and the two launcher folders; the other icons,
   the frame, glyphs, mark, backgrounds and splash would be new design work) and regenerating. The 103 CAPTURE-B1
   documentation images can be left out without touching the build, but
   the documents that show them would lose their pictures.

Option 3 is design work and is not done here. Captures are not excluded now:
they follow whichever answer the art gets.

## The questions

Answer per package (Threshold, icon extension, visual pack). One line each
is enough.

1. **Who made it?** (a) you, with an AI service - which one; (b) you,
   without AI; (c) someone else - who.
2. **For each AI service named in 1:** do its terms on the account you used
   give you the rights to the output, including commercial use and
   publishing it? (yes / no / don't know)
3. **Inputs:** did you give the tool any picture, photo, logo or design
   that is not yours or a Doors screenshot? For the visual pack this means
   "the user's approved DOORS references". (no / yes - which)
4. **"Astra"** (docs/design/astra-handoff is a "handoff for Astra"): a
   person, a company, or a tool? If a person or company, did they make any of
   the art?
5. **Licence for the art, once 1-3 allow it:** (a) Apache-2.0 like the code;
   (b) CC BY 4.0; (c) as (a) or (b), but the Doors mark, wordmark and boot
   splash reserved.

If an answer to 2 or 3 is "no" or "don't know" for a package, that package
stays UNKNOWN and option 3 applies to it.
