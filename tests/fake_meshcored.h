/*
 * A meshcored that is not meshcored: a pocketipc server speaking just
 * enough of mesh.* (docs/api/mesh.md) for a client to be tested against a
 * real socket.
 *
 * The real service is not linked - it needs two vendored upstream trees and
 * a radio daemon under it - and it is not what a client test is about. What
 * this supplies is the part a UI has to survive: a service that answers, a
 * service that pushes events, a service that refuses, a service that sends
 * rubbish, and a service that goes away and comes back.
 *
 * It runs in a child process, so the socket, the framing, the disconnection
 * and the reconnect are all real.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#ifndef FAKE_MESHCORED_H
#define FAKE_MESHCORED_H

#include <sys/types.h>

struct fake_meshcored_script {
    /* mesh.status answers with this state, and mesh.state is raised with it
     * once a client subscribes. */
    const char *state;
    const char *reason;
    /* The mesh.nodes result's "nodes" array, as JSON text. */
    const char *nodes_json;
    /* The mesh.messages result's "messages" array, as JSON text. Oldest
     * first, the way the real service answers. NULL is an empty history. */
    const char *messages_json;
    /* The mesh.channels result's "channels" array, as JSON text. NULL is a
     * service holding no channels, which is what a fresh one does. */
    const char *channels_json;
    /* Answer mesh.send with an error instead of accepting it. */
    int refuse_send;
    /* Accept mesh.send and then say nothing more about it: no mesh.message
     * event ever arrives for the id that was handed out. A client must not
     * show that message as sent, because nothing ever said it was. */
    int send_is_silent;
    /* Every mesh.send, one to a line, appended here: "<to>|<text>" for a
     * node and "#<slot>|<text>" for a channel. What a client put on the air
     * is proved by what the service was asked to transmit, not by reading
     * the client's source. */
    const char *send_log;
    /* Raised, in order, after the first subscribe: each is "<event name>|
     * <data as JSON>". NULL-terminated. */
    const char *const *events;
    /* Broadcast verbatim after the events: a well-formed JSON object that
     * is neither an event nor a response, which a client must count and
     * ignore rather than drop the connection over. NULL for none. */
    const char *junk_frame;
    /* Answer mesh.nodes with an error instead of a result. */
    int refuse_nodes;
    /* Stop after this many clients have connected and gone (0: run until
     * killed). Used to make the service disappear under a live client. */
    int serve_clients;
    /* Exit this long after the last scripted event, whatever else happens. */
    int life_ms;
    /* Every method asked for, one to a line, appended here. A client that
     * is not supposed to transmit is not proved by reading its source; it
     * is proved by what the service was asked for. */
    const char *method_log;
};

/* Run the service until the script says to stop. Returns 0. Never returns
 * to the caller in the parent: this is meant for a forked child. */
int fake_meshcored_run(const struct fake_meshcored_script *script);

/* Fork one, with $POCKETOS_RUNTIME_DIR already pointing where it should.
 * Returns the child's pid, or -1. */
pid_t fake_meshcored_spawn(const struct fake_meshcored_script *script);

/* Wait for the socket to exist, up to timeout_ms. Returns 1 when it does. */
int fake_meshcored_wait_ready(int timeout_ms);
/* Stop a child and reap it. */
void fake_meshcored_stop(pid_t pid);

#endif
