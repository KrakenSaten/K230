/*
 * The Library's addresses (docs/apps/LIBRARY.md), shared by the screen's
 * view (apps/browser/browser_view.c in Library mode) and its helper
 * (tools/library/pos_library.cpp).
 *
 * An entry of the collection is https://library.invalid/<entry path>. The
 * host is under .invalid, which RFC 6761 reserves so that it never resolves:
 * the address only names an entry, and should one ever reach a program that
 * fetches (it cannot reach Browser's, the two apps keep their own state), no
 * network would answer it. It is http(s) so that the HTML reader resolves an
 * article's relative links against it unchanged.
 *
 * The Library's own pages are under ~library/, a path no ZIM entry in the
 * archives examined uses; an entry of that name would be shadowed.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_WEB_LIBRARY_H
#define POCKETOS_WEB_LIBRARY_H

#define WEB_LIBRARY_HOST "library.invalid"
#define WEB_LIBRARY_ORIGIN "https://library.invalid/"
#define WEB_LIBRARY_OWN "~library/"
/* The collection's main page, as its archive names it. */
#define WEB_LIBRARY_MAIN WEB_LIBRARY_ORIGIN WEB_LIBRARY_OWN "main"
/* Title, date, source and the rest of the archive's metadata. */
#define WEB_LIBRARY_ABOUT WEB_LIBRARY_ORIGIN WEB_LIBRARY_OWN "about"
/* Followed by the query, percent-encoded. */
#define WEB_LIBRARY_SEARCH WEB_LIBRARY_ORIGIN WEB_LIBRARY_OWN "search?q="
/* The longest query the screen sends, in bytes before encoding. */
#define WEB_LIBRARY_QUERY_MAX 200

#endif
