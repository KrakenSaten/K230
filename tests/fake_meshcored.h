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
    /* mesh.status answers with this state. No mesh.state event is raised on
     * its own: a client subscribing receives the scripted events below and
     * nothing else. */
    const char *state;
    const char *reason;
    /* How long mesh.status says this service had been up when it started,
     * in seconds; 0 means the default, 42. What it reports grows with this
     * process, as a real one's does. It is how a client tells one run of the
     * service from the next - the message ids restart with the process - so
     * a script standing in for a service that has just come up gives a small
     * one, and one standing in for a service that has been running for hours
     * gives a large one. */
    int uptime_s;
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
    /* Hold the events until this service has answered mesh.nodes,
     * mesh.channels and mesh.messages. Without it they start as soon as a
     * client subscribes - which a client does before it asks for anything
     * else - so they interleave with those answers, and a snapshot of fixed
     * text that lands after an event undoes it. A real service's snapshot
     * would already say what its event said; this one's cannot. Set it where
     * a check needs an event to land on top of the snapshot. */
    int events_after_snapshot;
    /* Broadcast verbatim after the events: a well-formed JSON object that
     * is neither an event nor a response, which a client must count and
     * ignore rather than drop the connection over. NULL for none. */
    const char *junk_frame;
    /* Answer mesh.nodes with an error instead of a result. */
    int refuse_nodes;
    /* Stop after this many clients have connected and gone (0: run until
     * killed). Used to make the service disappear under a live client. */
    int serve_clients;
    /* Exit this long after starting, whatever else happens. */
    int life_ms;
    /* Every method asked for, one to a line, appended here. A client that
     * is not supposed to transmit is not proved by reading its source; it
     * is proved by what the service was asked for. */
    const char *method_log;
    /* Every mesh.advert, one to a line, appended here: "zero_hop" or
     * "flood", as the request asked. */
    const char *advert_log;
    /* Answer mesh.advert with an error instead of accepting it. */
    int refuse_advert;
    /* Take mesh.node_remove and mesh.node_reset_path requests and never
     * answer them, the way a service that dies mid-request would not. */
    int node_ops_silent;

    /* App datagrams (docs/api/mesh.md, "App datagrams"). */
    /* mesh.status run_id; NULL is "fake-run-1". */
    const char *run_id;
    /* The datagrams mesh.app_inbox holds, as a JSON array in the shape the
     * real service answers; it answers those on the asked port with an id
     * above after_id. A negative mono_ms is "this long ago". */
    const char *app_inbox_json;
    /* Every mesh.app_send, one to a line: "<to>|<port>|<payload_hex>". */
    const char *app_log;
    /* Answer mesh.app_send with an error instead of accepting it. */
    int refuse_app_send;
    /* What mesh.app_send answers as est_timeout_ms; 0 is 9000. */
    int app_est_timeout_ms;
    /* mesh.status says the radio is not online (state "waiting_for_lease"). */
    int radio_off;
    /* Hold the events until mesh.app_inbox has been answered, the way
     * events_after_snapshot holds them for the snapshots. */
    int events_after_inbox;

    /* Managing the node (mesh.channel_add / channel_remove / set_name /
     * set_path_hash). Every such request is appended here, one to a line:
     * "<method>|<params as JSON>" - which is how a test proves what a
     * reader's press asked for, key and all, and that nothing else did. */
    const char *manage_log;
    /* mesh.identity says the name came from the command line; a rename is
     * taken all the same, and the name is the stored one afterwards. */
    int name_pinned;
    /* mesh.set_name renames and answers "persisted": false - the name could
     * not be written, and the old one returns at the service's next start. */
    int rename_unsaved;
    /* Answer mesh.path_hash as a service too old to know it: unknown method. */
    int no_path_hash;
    /* Take management requests and never answer them. */
    int manage_silent;

    /* Repeater control (docs/api/mesh.md, "Repeater control"). By default
     * the service knows the methods: mesh.discover opens a round and, when
     * repeater_json is set, raises one mesh.discover "reply" event for it;
     * a login with the password "hunter2" is answered OK (admin), any other
     * is answered as the 20 s wait running out (outcome "timeout") at once;
     * status, neighbours, owner and a command are answered at once. */
    /* One repeater, as a mesh.discovered entry (JSON object text). */
    const char *repeater_json;
    /* Every repeater-control request, one to a line: "<method>|<params>". */
    const char *remote_log;
    /* Accept repeater requests and raise nothing about them. */
    int remote_silent;
    /* Answer every repeater method as a service too old to know it. */
    int no_remote;
    /* The first command (mesh.remote_cli) ends as meshcored ends one whose
     * answer was lost on the air: outcome "timeout". Later ones answer. */
    int cli_timeout_first;

    /* The receive log (docs/api/mesh.md, "The receive log"). A service that
     * keeps one answers mesh.subscribe {"rx_log": true} with "rx_log": true;
     * without this it answers as a meshcored older than the log, with no
     * rx_log at all. The mesh.rx events themselves are scripted in events. */
    int rx_log;
    /* Every mesh.subscribe's params, one to a line, as JSON ("{}" for none). */
    const char *subscribe_log;
};

/* Run the service until the script says to stop. Returns 0. Never returns
 * to the caller in the parent: this is meant for a forked child. */
int fake_meshcored_run(const struct fake_meshcored_script *script);

/* Fork one, with $POCKETOS_RUNTIME_DIR already pointing where it should.
 * Returns the child's pid, or -1. The child does not outlive the process
 * that forked it. */
pid_t fake_meshcored_spawn(const struct fake_meshcored_script *script);

/* Wait for the socket to exist, up to timeout_ms. Returns 1 when it does. */
int fake_meshcored_wait_ready(int timeout_ms);
/* Stop a child, reap it, and remove the socket it was killed before it
 * could remove itself. */
void fake_meshcored_stop(pid_t pid);

#endif
