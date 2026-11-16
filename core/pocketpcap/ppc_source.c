/*
 * pocketpcap capture sources: choosing one by its spec. See ppc_source.h.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "ppc_source.h"

#include <stdio.h>
#include <string.h>

const char *ppc_open_err_code(enum ppc_open_err e)
{
    static const char *const codes[] = { "ok",        "nodev",  "down",   "perm",  "notether",
                                         "nobackend", "filter", "badarg", "nomem", "source" };

    return (unsigned)e < sizeof(codes) / sizeof(codes[0]) ? codes[e] : "source";
}

enum ppc_open_err ppc_source_open(struct ppc_source *s, const char *spec,
                                  const struct ppc_open_opts *o)
{
    memset(s, 0, sizeof(*s));
    if (!spec || !*spec) {
        snprintf(s->err, sizeof(s->err), "no source");
        return PPC_OPEN_BADARG;
    }
    if (strncmp(spec, "file:", 5) == 0) {
        return ppc_source_open_file(s, spec + 5);
    }
    if (strncmp(spec, "synth:", 6) == 0) {
        return ppc_source_open_synth(s, spec + 6);
    }
    if (!ppc_iface_ok(spec)) {
        snprintf(s->err, sizeof(s->err), "not an interface name");
        return PPC_OPEN_BADARG;
    }
    {
        struct ppc_open_opts live = *o;

        live.iface = spec;
        return ppc_source_open_live(s, &live);
    }
}

void ppc_source_close(struct ppc_source *s)
{
    if (s && s->ops && s->ops->close) {
        s->ops->close(s);
    }
    if (s) {
        s->ops = NULL;
        s->impl = NULL;
    }
}
