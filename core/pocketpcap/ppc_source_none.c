/*
 * pocketpcap live source for a build without libpcap (a host without its
 * headers): every live open says so. The replay and synthetic sources work
 * as in every build, which is what the host tests and the simulator use.
 * The zbx_http_none.c pattern.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_source.h"

#include <stdio.h>

const char *ppc_source_backend(void)
{
    return "none";
}

enum ppc_open_err ppc_source_open_live(struct ppc_source *s, const struct ppc_open_opts *o)
{
    (void)o;
    snprintf(s->err, sizeof(s->err), "this build has no capture backend (built without libpcap)");
    return PPC_OPEN_NOBACKEND;
}
