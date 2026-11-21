/*
 * A peerd that is not peerd: a pocketipc server speaking just enough of
 * peer.* (docs/api/peer.md) for PocketFleet's LAN link to be tested against a
 * real socket, in a child process - the pattern of fake_meshcored.h.
 *
 * The real service needs libsodium and a network; neither is what a client
 * test is about. This one answers, refuses, keeps a kept list and a
 * visibility flag as the real one would, and writes down every request it
 * was sent.
 *
 * Copyright (c) 2026 Doors authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef FAKE_PEERD_H
#define FAKE_PEERD_H

#include <sys/types.h>

#define FAKE_PEERD_SELF "5e000000000000000000000000000000000000000000000000000000000000ee"

struct fake_peerd_script {
    const char *service;        /* the socket's name; NULL is "peerd" */
    const char *state;          /* peer.status state; NULL is "ready" */
    const char *run_id;         /* NULL is "00000000000000a1" */
    int no_interfaces;          /* ready and bound, with nothing to announce on */
    /* peer.nodes "nodes", as JSON text. A negative heard_ms_ago is not
     * rewritten: the field is already relative. */
    const char *nodes_json;
    /* peer.app_inbox "datagrams", as JSON text; a negative mono_ms means
     * "this long ago". */
    const char *inbox_json;
    /* Raised once a client has subscribed and its inbox has been answered,
     * one per pass: each is the data of a peer.app event. NULL-terminated. */
    const char *const *events;
    /* Raise them as soon as a client has subscribed, before its inbox has
     * been answered: the race a client's catch-up has to survive. */
    int events_early;
    /* The kept list peerd starts with: whole keys, comma separated. */
    const char *kept;
    int refuse_send;            /* peer.app_send fails with code 5 */
    int refuse_keep;            /* peer.keep fails with code 4 */
    /* A probe of this address is answered by probe_key; any other address
     * ends as no_answer. With probe_pending, nothing ever ends. */
    const char *probe_address;
    const char *probe_key;
    int probe_pending;
    /* Every request, one per line: "<method>|<params as JSON>". */
    const char *log;
    int life_ms;                /* backstop; 0 is forever */
};

int fake_peerd_run(const struct fake_peerd_script *script);
pid_t fake_peerd_spawn(const struct fake_peerd_script *script);
int fake_peerd_wait_ready(const struct fake_peerd_script *script, int timeout_ms);
void fake_peerd_stop(const struct fake_peerd_script *script, pid_t pid);

#endif
