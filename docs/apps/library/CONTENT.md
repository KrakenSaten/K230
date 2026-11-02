# Library: the prototype's content

The collection the Library prototype was built and validated with
(docs/apps/LIBRARY.md). **It is not in this repository and not in any image**:
it was downloaded to the build host only, outside git (`*.zim` is ignored).

## The archive

| Item | Value |
| --- | --- |
| Name | `wikipedia_en_climate-change_maxi_2026-10.zim` |
| What | "Climate change by Wikipedia": a selection of English Wikipedia articles on climate change, full articles with pictures (Kiwix "maxi" flavour) |
| Source URL | https://download.kiwix.org/zim/wikipedia/wikipedia_en_climate-change_maxi_2026-10.zim (redirected to the mirror `chuangtzu.ftp.acc.umu.se/mirror/kiwix.org/...`) |
| Published by | Kiwix / openZIM (`Publisher: openZIM`), made with `mwoffliner 2.1.1` from `en.wikipedia.org` |
| Archive date | `Date: 2026-10-13` (ZIM metadata); file `Last-Modified: Thu, 15 Oct 2026 18:31:09 GMT` |
| Size | 190,425,044 bytes (182 MiB; the download index says 182M) |
| SHA-256 | `882bdbf4d3d1c71362b36022b9d178cf1e3dfd105b7e2882983bd1d1d674c17e`, equal to Kiwix's published `.sha256` beside the file |
| Downloaded | 2026-10-29, to the build host's WSL filesystem |
| Contents (ZIM `Counter`) | 6,842 HTML pages (20,149 titles with redirects), 12,150 WebP and 4 PNG pictures, 875 SVG, 4 PDF, scripts and styles |
| Indexes | title index and full-text index (Xapian), both present |

Chosen because it is a real, current, encyclopaedic collection under the
250 MB budget that has pictures (`_pictures:yes`) and full articles
(`_details:yes`). Smaller candidates were checked and set aside:
`wikipedia_en_chemistry_mini_2026-10.zim` (23,322,446 bytes, SHA-256
`560be082e8bb14cf3736489e680aa83b8f918b0ad00cc2a2adfe811844109259`, also
matching its published hash) was downloaded first and inspected - its "mini"
flavour replaces every picture with an SVG placeholder (`_pictures:no`), so it
cannot show that pictures work. It was used for nothing else. Both downloads
together: 213.7 MB.

## Terms of the content (not of the reader)

These are the content's terms, separate from Doors' (Apache-2.0) and from
the reader's libraries (docs/apps/LIBRARY.md "Licences"):

- **Article text**: Wikipedia's, under the Creative Commons
  Attribution-ShareAlike 4.0 licence (and GFDL), as every article page in the
  archive says in its footer ("This page is issued from Wikipedia. The text is
  available under Creative Commons Attribution-Share Alike 4.0 unless
  otherwise noted. Additional terms may apply for the media files."), which
  the Library shows as part of the article. Attribution: Wikipedia
  contributors, with a link to the source article.
- **Pictures**: from Wikimedia Commons, **each under its own licence**
  (public domain, CC BY, CC BY-SA and others). The archive does not carry the
  per-file licence pages, so the Library cannot show them; redistributing a
  picture needs its Commons page.
- **The archive's metadata has no `License` key**; the Library's "About this
  collection" page says so instead of inventing one.
- Kiwix's own terms for the download service were not examined beyond
  the above; nothing here redistributes the archive.

Consequence: the archive and screenshots of its articles are third-party
material. Neither is committed. Screenshots of the prototype are kept out of
the repository for the same reason; `tests/library_shell_test.sh` with
`LIBRARY_SHOTS=<dir>` makes them again from the archive.

## Replacing it

The Library reads whatever .zim it is pointed at (docs/apps/LIBRARY.md
"The collection"); nothing about this archive is built into Doors. The
helper's tests take the words they search for from `LIBRARY_QUERY`,
`LIBRARY_ARTICLE` and `LIBRARY_LINK` so another archive can be checked.
