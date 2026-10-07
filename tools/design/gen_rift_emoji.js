// RIFT's colour emoji, generated at build time from Noto Color Emoji
// (googlefonts/noto-emoji, 2D artwork, Apache-2.0; flags from
// third_party/region-flags, public domain): every PNG becomes an RGB565A8
// image LVGL blends straight onto the 16-bit panel, no decoding at run time.
//
//   - Every single-code-point emoji of the 2D/png set, but what Plex Sans
//     already draws (Plex stays the text font) and the skin-tone modifiers.
//   - Every sequence of the set - ZWJ (families, the heart on fire, ...),
//     keycaps, tag flags - but those with a skin tone: RIFT strips the
//     modifier and shows the base, so no toned artwork is made.
//   - Every two-letter region flag as its regional-indicator pair, and the
//     three tag flags of the font (England, Scotland, Wales).
// Nothing is chosen by hand beyond those rules.
//
// Usage: node gen_rift_emoji.js <lv_font_conv binary> <noto-emoji checkout>
//                               <font source dir> <Plex range> <repo root>
// Writes apps/rift/rift_emoji_seq.c (sequence table, plain C, for folding),
// apps/rift/ui/rift_emoji_img.c (image index) and apps/rift/ui/rift_emoji_px.bin
// (the pixels, embedded by rift_emoji_px.S). Prints a summary.
//
// Copyright (c) 2026 PocketOS authors.
// SPDX-License-Identifier: Apache-2.0
'use strict';
const fs = require('fs');
const path = require('path');

const [conv, noto, fonts, plexRange, repo] = process.argv.slice(2);
if (!repo) {
  console.error('usage: gen_rift_emoji.js <lv_font_conv> <noto-emoji> <font dir> <Plex range> <repo>');
  process.exit(2);
}
const convDir = path.dirname(fs.realpathSync(conv));
const { PNG } = require(path.join(convDir, 'node_modules', 'pngjs'));
const opentype = require(path.join(convDir, 'node_modules', 'opentype.js'));

const EMOJI = 18;        // px, square: fits RIFT's shortest line (Mono 14, 18 px)
const FLAG_W = 18;       // a flag fits in 18 x 12, its aspect kept
const FLAG_H = 12;
const PUA = 0xF0000;     // sequences are drawn as U+F0000 + index
const SRC_DIR = path.join(noto, '2D', 'png', '72');
const FLAG_DIR = path.join(noto, 'third_party', 'region-flags', 'png');
const TAG_FLAGS = ['GB-ENG', 'GB-SCT', 'GB-WLS'];

// ---- what Plex Sans draws -------------------------------------------------
const inRange = new Set();
for (const part of plexRange.split(',')) {
  const [a, b] = part.split('-').map(x => parseInt(x, 16));
  for (let c = a; c <= (b === undefined ? a : b); c++) inRange.add(c);
}
const sansMap = opentype.loadSync(path.join(fonts, 'IBMPlexSans-Regular.ttf')).tables.cmap.glyphIndexMap;
const plexDraws = c => inRange.has(c) && sansMap[c] !== undefined;
const isTone = c => c >= 0x1F3FB && c <= 0x1F3FF;

// ---- resampling -----------------------------------------------------------
// Area average with premultiplied alpha, so transparent edges do not darken.
function resample(img, tw, th) {
  const { width: sw, height: sh, data } = img;
  const out = new Float64Array(tw * th * 4);
  const fx = sw / tw, fy = sh / th;
  for (let ty = 0; ty < th; ty++) {
    const y0 = ty * fy, y1 = y0 + fy;
    for (let tx = 0; tx < tw; tx++) {
      const x0 = tx * fx, x1 = x0 + fx;
      let r = 0, g = 0, b = 0, a = 0, wsum = 0;
      for (let sy = Math.floor(y0); sy < Math.ceil(y1); sy++) {
        const wy = Math.min(y1, sy + 1) - Math.max(y0, sy);
        for (let sx = Math.floor(x0); sx < Math.ceil(x1); sx++) {
          const w = wy * (Math.min(x1, sx + 1) - Math.max(x0, sx));
          const i = (sy * sw + sx) * 4;
          const al = data[i + 3] / 255;
          r += data[i] * al * w; g += data[i + 1] * al * w; b += data[i + 2] * al * w;
          a += al * w; wsum += w;
        }
      }
      const o = (ty * tw + tx) * 4;
      out[o + 3] = a / wsum;
      if (a > 0) { out[o] = r / a; out[o + 1] = g / a; out[o + 2] = b / a; }
    }
  }
  return out;
}

// RGB565 plane (stride w*2, little endian) then the A8 plane (stride w), as
// lv_draw_sw_img.c reads LV_COLOR_FORMAT_RGB565A8.
function rgb565a8(px, w, h) {
  const buf = Buffer.alloc(w * h * 3);
  for (let i = 0; i < w * h; i++) {
    const a = Math.round(px[i * 4 + 3] * 255);
    let v = 0;
    if (a > 0) {
      const r = Math.round(px[i * 4] * 31 / 255), g = Math.round(px[i * 4 + 1] * 63 / 255);
      const b = Math.round(px[i * 4 + 2] * 31 / 255);
      v = (r << 11) | (g << 5) | b;
    }
    buf.writeUInt16LE(v, i * 2);
    buf[w * h * 2 + i] = a;
  }
  return buf;
}

// ---- the set --------------------------------------------------------------
const singles = [];   // {cp, file}
const seqs = [];      // {cps, file, flag}
let skipped = { tone: 0, plex: 0 };
for (const f of fs.readdirSync(SRC_DIR).sort()) {
  const m = /^emoji_u([0-9a-f_]+)\.png$/.exec(f);
  if (!m) continue;
  const cps = m[1].split('_').map(x => parseInt(x, 16));
  if (cps.some(isTone)) { skipped.tone++; continue; }
  if (cps.length === 1) {
    if (plexDraws(cps[0])) { skipped.plex++; continue; }
    singles.push({ cp: cps[0], file: path.join(SRC_DIR, f) });
  } else {
    seqs.push({ cps, file: path.join(SRC_DIR, f), flag: false });
  }
}
const RI = c => 0x1F1E6 + c.charCodeAt(0) - 65;
const TAG = c => 0xE0000 + c.toLowerCase().charCodeAt(0);
let flagsTwo = 0;
for (const f of fs.readdirSync(FLAG_DIR).sort()) {
  const m = /^([A-Z][A-Z])\.png$/.exec(f);
  if (m) {
    seqs.push({ cps: [RI(m[1][0]), RI(m[1][1])], file: path.join(FLAG_DIR, f), flag: true });
    flagsTwo++;
  }
}
for (const t of TAG_FLAGS) {
  const letters = t.replace('-', '');
  seqs.push({ cps: [0x1F3F4, ...[...letters].map(TAG), 0xE007F], file: path.join(FLAG_DIR, t + '.png'), flag: true });
}
const key = s => s.cps.map(c => c.toString(16).padStart(6, '0')).join(' ');
seqs.sort((a, b) => (key(a) < key(b) ? -1 : key(a) > key(b) ? 1 : 0));
for (let i = 1; i < seqs.length; i++) {
  if (key(seqs[i]) === key(seqs[i - 1])) throw new Error('duplicate sequence ' + key(seqs[i]));
}
const maxLen = Math.max(...seqs.map(s => s.cps.length));

// ---- images ---------------------------------------------------------------
const images = []; // {key, w, h, buf}
function convert(file, flag) {
  const img = PNG.sync.read(fs.readFileSync(file));
  let w = EMOJI, h = EMOJI;
  if (flag) {
    const s = Math.min(FLAG_W / img.width, FLAG_H / img.height);
    w = Math.max(1, Math.round(img.width * s));
    h = Math.max(1, Math.round(img.height * s));
  }
  return { w, h, buf: rgb565a8(resample(img, w, h), w, h) };
}
for (const s of singles) images.push({ key: s.cp, ...convert(s.file, false) });
seqs.forEach((s, i) => images.push({ key: PUA + i, ...convert(s.file, s.flag) }));
images.sort((a, b) => a.key - b.key);

const parts = [];
let off = 0;
for (const im of images) {
  im.off = off;
  parts.push(im.buf);
  off += im.buf.length;
  const pad = (4 - (off % 4)) % 4; // RGB565 wants even, keep words aligned
  if (pad) { parts.push(Buffer.alloc(pad)); off += pad; }
}
const blob = Buffer.concat(parts);

// ---- output ---------------------------------------------------------------
const head = (what) => `/* ${what}
 * GENERATED by tools/design/gen_rift_emoji.js from googlefonts/noto-emoji
 * (2D/png/72, Apache-2.0; third_party/region-flags, public domain). Do not
 * edit; regenerate with tools/design/gen_rift_emoji.sh. */\n`;
const hex = (c) => '0x' + c.toString(16).toUpperCase();

let seqC = head('RIFT colour emoji: the sequences drawn as one image.') +
  '#include "rift_emoji.h"\n\n' +
  `const uint32_t rift_emoji_seq_cps[] = {\n`;
let at = 0;
const entries = [];
for (const s of seqs) {
  seqC += '    ' + s.cps.map(hex).join(', ') + ',\n';
  entries.push(`    { ${at}, ${s.cps.length} },`);
  at += s.cps.length;
}
seqC += `};\n\nconst struct rift_emoji_seq rift_emoji_seqs[] = {\n${entries.join('\n')}\n};\n\n` +
  `const unsigned rift_emoji_seq_count = ${seqs.length};\n`;
fs.writeFileSync(path.join(repo, 'apps', 'rift', 'rift_emoji_seq.c'), seqC);

let imgC = head('RIFT colour emoji: the image index (key, offset into rift_emoji_px, size).') +
  '#include "rift_emoji_img.h"\n\nconst struct rift_emoji_img rift_emoji_imgs[] = {\n';
for (const im of images) imgC += `    { ${hex(im.key)}, ${im.off}, ${im.w}, ${im.h} },\n`;
imgC += `};\n\nconst unsigned rift_emoji_img_count = ${images.length};\n` +
  `const unsigned rift_emoji_px_size = ${blob.length};\n`;
fs.writeFileSync(path.join(repo, 'apps', 'rift', 'ui', 'rift_emoji_img.c'), imgC);
fs.writeFileSync(path.join(repo, 'apps', 'rift', 'ui', 'rift_emoji_px.bin'), blob);

console.log(`gen_rift_emoji: ${images.length} images (${singles.length} single code points, ` +
  `${seqs.length} sequences: ${flagsTwo} region flags, ${TAG_FLAGS.length} tag flags, ` +
  `${seqs.length - flagsTwo - TAG_FLAGS.length} ZWJ/keycap/other), longest ${maxLen}; ` +
  `skipped ${skipped.tone} with a skin tone, ${skipped.plex} Plex draws; ${blob.length} bytes of pixels`);
if (maxLen > 8) throw new Error('a sequence longer than RIFT_EMOJI_SEQ_MAX');
