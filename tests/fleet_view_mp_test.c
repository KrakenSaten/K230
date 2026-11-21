/*
 * PocketFleet multiplayer view model (apps/fleet/ui/fleet_view_mp.h): every
 * UX state of docs/apps/FLEET_MULTIPLAYER.md turned into the words a screen
 * shows, natively. A state that says nothing, or the wrong thing, fails here
 * rather than on a panel.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "fleet_view_mp.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

static void says(const char *name, const char *got, const char *want)
{
    int ok = got && strstr(got, want) != NULL;

    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    if (!ok) {
        printf("     got \"%s\", want it to contain \"%s\"\n", got ? got : "(null)", want);
    }
    failed += !ok;
}

static void battle(struct fleet_match *m, int role)
{
    uint8_t key[FLEET_KEY_BYTES] = { 9 };

    fleet_match_init(m, key, 1);
    m->phase = FLEET_MP_BATTLE;
    m->role = (uint8_t)role;
    m->sid = 0x123;
    m->committed = 1;
    m->have_peer_commit = 1;
    m->peer_has_commit = 1;
}

int main(void)
{
    struct fleet_match m;
    char buf[200];

    /* ---- the Battle note -------------------------------------------------- */
    battle(&m, FLEET_ROLE_GUEST);
    fleet_view_mp_note(&m, "Anna", 0, buf, sizeof(buf));
    check("our turn, link well: the note is left to aiming", buf[0] == '\0');
    m.pending = 23;
    fleet_view_mp_note(&m, "Anna", 0, buf, sizeof(buf));
    says("a shot on its way", buf, "Shot at D3 sent");
    m.ob = FLEET_OB_SHOT;
    m.attempts = 3;
    fleet_view_mp_note(&m, "Anna", 0, buf, sizeof(buf));
    says("a shot being retried says so, with the count", buf, "Retrying (3 of 6)");
    m.lost = 1;
    fleet_view_mp_note(&m, "Anna", 0, buf, sizeof(buf));
    says("out of reach: paused, not lost", buf, "Anna is out of reach. The match is paused");
    m.lost = 0;
    m.resyncing = 1;
    fleet_view_mp_note(&m, "Anna", 0, buf, sizeof(buf));
    says("resyncing", buf, "Checking the match with Anna");
    m.resyncing = 0;
    m.throttled = 1;
    fleet_view_mp_note(&m, "Anna", 0, buf, sizeof(buf));
    says("held back by the airtime governor", buf, "spare the airtime");
    m.throttled = 0;
    m.pending = FLEET_NO_CELL;
    m.ob = FLEET_OB_NONE;
    m.attempts = 0;
    m.resolved = 1;
    m.log_cell[1] = 0;
    m.log_res[1] = fleet_res_make(1, 0, 0);
    fleet_view_mp_note(&m, "Anna", 1000, buf, sizeof(buf));
    says("their turn", buf, "Anna is aiming.");
    m.last_heard = 0;
    fleet_view_mp_note(&m, "Anna", 7 * 60000, buf, sizeof(buf));
    says("their turn, long silent", buf, "No word for 7 min");
    m.phase = FLEET_MP_COMMITTED;
    fleet_view_mp_note(&m, "Anna", 0, buf, sizeof(buf));
    says("waiting for them to deploy", buf, "Waiting for Anna to deploy");
    fleet_view_mp_note(&m, NULL, 0, buf, sizeof(buf));
    says("with no name known, still a sentence", buf, "Your opponent");

    /* ---- the log line and the header ---------------------------------------- */
    battle(&m, FLEET_ROLE_GUEST);
    m.resolved = 2;
    m.log_cell[1] = 36;                 /* G4, ours (the guest fires odd plies) */
    m.log_res[1] = fleet_res_make(2, 0, 0);
    m.log_cell[2] = 11;                 /* B2, theirs */
    m.log_res[2] = fleet_res_make(1, 0, 0);
    fleet_view_mp_exchange(&m, "Anna", buf, sizeof(buf));
    says("the last shot each way", buf, "YOU G4 HIT \xc2\xb7 ANNA B2 MISS");

    /* ---- the status in the header -------------------------------------------- */
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("our turn, with our next shot", buf, "YOUR TURN \xc2\xb7 SHOT 2");
    m.pending = 40;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("our shot fired: waiting for the report", buf, "WAITING FOR ANNA");
    m.pending = FLEET_NO_CELL;
    m.resolved = 3;                     /* ply 4 is the host's */
    m.log_cell[3] = 12;
    m.log_res[3] = fleet_res_make(1, 0, 0);
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("their turn", buf, "WAITING FOR ANNA");
    check("and the state says so, not only the words",
          fleet_view_mp_state(&m, FLEET_LINK_UP) == FLEET_STATUS_WAITING);
    m.throttled = 1;
    check("held back by the governor is still their turn, not a link problem",
          fleet_view_mp_state(&m, FLEET_LINK_UP) == FLEET_STATUS_WAITING);
    m.throttled = 0;
    m.ob = FLEET_OB_SHOT;
    m.attempts = 2;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("a packet of the game being retried", buf, "RECONNECTING");
    m.lost = 1;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("out of reach", buf, "OPPONENT DISCONNECTED");
    m.resyncing = 1;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("comparing records after Check link or a reopen", buf, "SYNCING");
    fleet_view_mp_status(&m, FLEET_LINK_NO_SERVICE, "Anna", buf, sizeof(buf));
    says("the mesh service gone beats everything the match says", buf, "MESH OFFLINE");
    fleet_view_mp_status(&m, FLEET_LINK_RADIO_OFF, "Anna", buf, sizeof(buf));
    says("and so does its radio", buf, "MESH OFFLINE");
    fleet_view_mp_status(&m, FLEET_LINK_CONNECTING, "Anna", buf, sizeof(buf));
    says("the service being looked for", buf, "CONNECTING");
    m.phase = FLEET_MP_REVEAL;
    fleet_view_mp_status(&m, FLEET_LINK_NO_SERVICE, "Anna", buf, sizeof(buf));
    says("but a match that is over is over, whatever the link", buf, "GAME OVER");
    m.phase = FLEET_MP_DONE;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("done", buf, "GAME OVER");
    fleet_view_mp_status(NULL, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("our key not known yet: connecting", buf, "CONNECTING");
    fleet_view_mp_status(NULL, FLEET_LINK_NO_SERVICE, "Anna", buf, sizeof(buf));
    says("and without the service, offline", buf, "MESH OFFLINE");
    battle(&m, FLEET_ROLE_HOST);
    m.phase = FLEET_MP_INVITING;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("an invitation out: connecting", buf, "CONNECTING");
    m.phase = FLEET_MP_ACCEPTING;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("an invitation accepted, START not in yet: connecting", buf, "CONNECTING");
    m.phase = FLEET_MP_IDLE;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("no match: the lobby's own name", buf, "MULTIPLAYER");
    battle(&m, FLEET_ROLE_HOST);
    m.phase = FLEET_MP_DEPLOY;
    m.committed = 0;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("deploying", buf, "DEPLOY YOUR FLEET");
    m.phase = FLEET_MP_COMMITTED;
    m.committed = 1;
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", buf, sizeof(buf));
    says("deployed, they have not", buf, "WAITING FOR ANNA");
    fleet_view_mp_status(&m, FLEET_LINK_UP, "Annabelle Fitzgerald-Hansen", buf, sizeof(buf));
    says("a long name is cut to fit the header", buf, "WAITING FOR ANNABELLE FITZGE\xe2\x80\xa6");
    /* Fifteen letters and then a two-byte one: sixteen bytes would split it. */
    fleet_view_mp_status(&m, FLEET_LINK_UP, "abcdefghijklmno\xc3\xb8pqr", buf, sizeof(buf));
    check("a name is never cut in the middle of a character",
          strcmp(buf, "WAITING FOR ABCDEFGHIJKLMNO\xe2\x80\xa6") == 0);

    /* ---- chat ------------------------------------------------------------------ */
    battle(&m, FLEET_ROLE_GUEST);
    {
        char title[32];
        char preview[120];

        fleet_view_mp_chat_tile(&m, "Anna", title, sizeof(title), preview, sizeof(preview));
        says("no lines yet: the button says CHAT", title, "CHAT");
        says("and what it is for", preview, "Say something to Anna.");
        fleet_chat_add_theirs(&m.chat, 7, (const uint8_t *)"Nice shot", 9);
        fleet_chat_add_theirs(&m.chat, 8, (const uint8_t *)"Your move", 9);
        fleet_view_mp_chat_tile(&m, "Anna", title, sizeof(title), preview, sizeof(preview));
        says("two new lines are counted", title, "CHAT \xc2\xb7 2 NEW");
        says("the newest is shown, with who said it", preview, "ANNA \xc2\xb7 Your move");
        fleet_chat_seen(&m.chat);
        fleet_view_mp_chat_tile(&m, "Anna", title, sizeof(title), preview, sizeof(preview));
        check("once seen, nothing is new", strcmp(title, "CHAT") == 0);
        fleet_chat_add_mine(&m.chat, "Thanks");
        fleet_view_mp_chat_line(&m.chat.line[2], "Anna", buf, sizeof(buf));
        says("ours, not yet sent", buf, "YOU \xc2\xb7 Thanks \xc2\xb7 WAITING");
        m.chat.line[2].state = FLEET_CHAT_SENDING;
        fleet_view_mp_chat_line(&m.chat.line[2], "Anna", buf, sizeof(buf));
        says("ours, on the air", buf, "YOU \xc2\xb7 Thanks \xc2\xb7 SENDING");
        m.chat.line[2].state = FLEET_CHAT_FAILED;
        fleet_view_mp_chat_line(&m.chat.line[2], "Anna", buf, sizeof(buf));
        says("ours, given up", buf, "YOU \xc2\xb7 Thanks \xc2\xb7 NOT DELIVERED");
        m.chat.line[2].state = FLEET_CHAT_DELIVERED;
        fleet_view_mp_chat_line(&m.chat.line[2], "Anna", buf, sizeof(buf));
        check("ours, delivered: the line alone", strcmp(buf, "YOU \xc2\xb7 Thanks") == 0);
        fleet_view_mp_chat_line(&m.chat.line[0], NULL, buf, sizeof(buf));
        says("theirs, with no name known", buf, "THEM \xc2\xb7 Nice shot");
    }

    /* ---- the lobby ---------------------------------------------------------- */
    battle(&m, FLEET_ROLE_HOST);
    m.phase = FLEET_MP_INVITING;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("inviting", buf, "Inviting Anna");
    m.attempts = 3;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("inviting, no answer yet", buf, "no answer yet (try 3 of 6)");
    {
        /* A link quicker than the mesh (ADR-018): the invitation is repeated
         * often and held to a deadline; the screen says how long is left and
         * never counts the copies. The mesh's words above are untouched. */
        m.attempts = 40;
        m.now = 1000000;
        m.human_start = m.now - 3000;
        m.human_deadline = m.human_start + 259000;
        fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
        check("inviting on a fast link: nothing is said in the first seconds",
              strstr(buf, "Inviting Anna") && !strstr(buf, "no answer") && !strstr(buf, "try"));
        m.human_start = m.now - 60000;
        m.human_deadline = m.human_start + 259000;
        fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
        says("inviting on a fast link, a minute in: the time left, to the nearest minute", buf,
             "no answer yet (about 3 min left)");
        check("and no count of tries", !strstr(buf, "try") && !strstr(buf, "40"));
        m.human_start = m.now - 139000;
        m.human_deadline = m.human_start + 259000;
        fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
        says("two minutes left", buf, "(about 2 min left)");
        m.human_start = m.now - 198000;
        m.human_deadline = m.human_start + 259000;
        fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
        says("61 s left is a minute, not two", buf, "no answer yet (about a minute left)");
        m.human_start = m.now - 220000;
        m.human_deadline = m.human_start + 259000;
        fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
        says("39 s left is still about a minute", buf, "(about a minute left)");
        m.human_start = m.now - 240000;
        m.human_deadline = m.human_start + 259000;
        fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
        says("the last half minute", buf, "no answer yet (under a minute left)");
        m.human_start = 0;
        m.human_deadline = 0;
        m.attempts = 3;
        fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
        says("and the mesh still counts its tries", buf, "no answer yet (try 3 of 6)");
        m.now = 0;
    }
    m.phase = FLEET_MP_INVITED;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("an invitation for us", buf, "Anna invites you");
    m.phase = FLEET_MP_ACCEPTING;
    m.notice = FLEET_NOTICE_CROSSED;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("crossed invitations are explained", buf, "You invited each other");
    m.phase = FLEET_MP_IDLE;
    m.notice = FLEET_NOTICE_DECLINED;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("declined", buf, "declined");
    m.notice = FLEET_NOTICE_BUSY;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("busy", buf, "already in an engagement");
    m.notice = FLEET_NOTICE_NO_ANSWER;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("no answer", buf, "No answer");
    m.notice = FLEET_NOTICE_CANCELLED;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("withdrawn", buf, "withdrawn");
    m.notice = FLEET_NOTICE_NONE;
    m.phase = FLEET_MP_BATTLE;
    fleet_view_mp_lobby(&m, "Anna", buf, sizeof(buf));
    says("a match under way", buf, "under way");

    /* ---- the result ---------------------------------------------------------- */
    m.phase = FLEET_MP_DONE;
    m.outcome = FLEET_OUTCOME_WIN;
    says("won", fleet_view_mp_outcome(&m), "Enemy fleet destroyed");
    says("won by sinking everything", fleet_view_mp_ended(&m), "ALL SHIPS SUNK");
    m.outcome = FLEET_OUTCOME_LOSS;
    says("lost", fleet_view_mp_outcome(&m), "Fleet lost");
    /* A forfeit sank nothing, so neither heading may say a fleet went down. */
    m.end_reason = FLEET_END_FORFEIT;
    m.end_by_me = 1;
    says("we forfeited: the heading says so", fleet_view_mp_outcome(&m), "You forfeited");
    says("we forfeited", fleet_view_mp_ended(&m), "YOU FORFEITED");
    m.outcome = FLEET_OUTCOME_WIN;
    m.end_by_me = 0;
    says("they forfeited: the heading says so", fleet_view_mp_outcome(&m), "Opponent forfeited");
    says("they forfeited", fleet_view_mp_ended(&m), "THEY FORFEITED");
    m.outcome = FLEET_OUTCOME_VOID;
    m.end_reason = FLEET_END_VIOLATION;
    says("void", fleet_view_mp_outcome(&m), "No result");
    says("void over impossible reports", fleet_view_mp_ended(&m), "BAD REPORTS");
    m.end_reason = FLEET_END_VOID;
    says("void over records that differ", fleet_view_mp_ended(&m), "RECORDS DIFFER");
    m.verify = FLEET_VERIFY_OK;
    says("verified", fleet_view_mp_verify(&m), "Verified");
    m.verify = FLEET_VERIFY_MISMATCH;
    says("caught", fleet_view_mp_verify(&m), "Reports did not match");
    m.verify = FLEET_VERIFY_NONE;
    says("never revealed", fleet_view_mp_verify(&m), "Not verified");

    /* ---- a lobby row ----------------------------------------------------------- */
    {
        struct fleet_link_peer p;

        memset(&p, 0, sizeof(p));
        strcpy(p.name, "Anna");
        p.hops = 2;
        p.heard_ms = 1000;
        fleet_view_mp_peer_row(&p, 1000 + 3 * 60000, buf, sizeof(buf));
        says("a player heard three minutes ago, two hops out", buf,
             "ANNA \xc2\xb7 2 HOPS \xc2\xb7 HEARD 3 MIN AGO");
        p.hops = 0;
        p.heard_ms = 0;
        fleet_view_mp_peer_row(&p, 5000, buf, sizeof(buf));
        says("in direct range, not heard this run", buf, "DIRECT \xc2\xb7 NOT HEARD LATELY");
    }

    check("nothing is written through a NULL", fleet_view_mp_note(NULL, "a", 0, buf, sizeof(buf)) == -1 &&
          buf[0] == '\0');

    /* ---- the local network (M2-PLAN 1.4) ----------------------------------------- */
    {
        struct fleet_link_peer_info info;
        struct fleet_link_peer p;
        char big[240];
        char mesh[64];

        fleet_view_mp_status(NULL, FLEET_LINK_NO_SERVICE, "Anna", mesh, sizeof(mesh));
        says("with no service the mesh's header is what it was", mesh, "MESH OFFLINE");
        fleet_view_mp_status_on(NULL, FLEET_LINK_NO_SERVICE, "Anna", 0, buf, sizeof(buf));
        check("and unchanged when asked for by transport", strcmp(buf, mesh) == 0);
        fleet_view_mp_status_on(NULL, FLEET_LINK_NO_SERVICE, "Anna", 1, buf, sizeof(buf));
        check("the network's header never names the mesh", strcmp(buf, "NETWORK OFFLINE") == 0);
        battle(&m, FLEET_ROLE_HOST);
        fleet_view_mp_status(&m, FLEET_LINK_UP, "Anna", mesh, sizeof(mesh));
        fleet_view_mp_status_on(&m, FLEET_LINK_UP, "Anna", 1, buf, sizeof(buf));
        check("a match's status reads the same on both", strcmp(buf, mesh) == 0);

        memset(&info, 0, sizeof(info));
        snprintf(info.address, sizeof(info.address), "192.168.1.42");
        fleet_view_mp_lan_link(FLEET_LINK_UP, &info, big, sizeof(big));
        check("link line: ready, with this device's address",
              strcmp(big, "Local network ready. This device is 192.168.1.42.") == 0);
        fleet_view_mp_lan_link(FLEET_LINK_UP, NULL, big, sizeof(big));
        check("link line: ready, from a link with no detail", strcmp(big, "Local network ready.") == 0);
        info.trouble = FLEET_PEER_NO_NETWORK;
        fleet_view_mp_lan_link(FLEET_LINK_UP, &info, big, sizeof(big));
        check("link line: no network",
              strcmp(big, "Wi-Fi is off and no cable is connected.") == 0);
        info.trouble = FLEET_PEER_OK;
        fleet_view_mp_lan_link(FLEET_LINK_CONNECTING, &info, big, sizeof(big));
        says("link line: looking for the service", big, "Looking for the local network service");
        fleet_view_mp_lan_link(FLEET_LINK_NO_SERVICE, &info, big, sizeof(big));
        says("link line: no service", big, "local network service is not running on this device");
        check("which says nothing of the mesh or the radio",
              !strstr(big, "mesh") && !strstr(big, "radio"));
        info.trouble = FLEET_PEER_FAULT;
        snprintf(info.fault, sizeof(info.fault), "identity_fault");
        fleet_view_mp_lan_link(FLEET_LINK_RADIO_OFF, &info, big, sizeof(big));
        says("link line: a fault, named", big, "stopped with a fault (identity_fault)");
        check("and never called a radio", !strstr(big, "radio") && !strstr(big, "Radio"));
        memset(&info, 0, sizeof(info));
        snprintf(info.address, sizeof(info.address), "192.168.1.42");

        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 0, 0, FLEET_PEER_PROBE_NONE, "", NULL, big,
                                 sizeof(big));
        says("nobody listed yet: listening", big, "Listening for players on this network");
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 0, FLEET_VIEW_LAN_SILENCE_MS - 1,
                                 FLEET_PEER_PROBE_NONE, "", NULL, big, sizeof(big));
        says("still listening just under six seconds", big, "Listening");
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 0, FLEET_VIEW_LAN_SILENCE_MS,
                                 FLEET_PEER_PROBE_NONE, "", NULL, big, sizeof(big));
        check("after six seconds of silence: the sentence of the plan, with this address",
              strcmp(big, "No players heard on this network. Enter the other device's address: "
                          "this one is 192.168.1.42.") == 0);
        fleet_view_mp_lan_notice(FLEET_LINK_UP, NULL, 0, 60000, FLEET_PEER_PROBE_NONE, "", NULL, big,
                                 sizeof(big));
        says("silence with no address to give", big, "Both devices need Fleet open on LOCAL NETWORK");
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 2, 60000, FLEET_PEER_PROBE_NONE, "", NULL, big,
                                 sizeof(big));
        check("with players listed there is nothing to add", big[0] == '\0');
        fleet_view_mp_lan_notice(FLEET_LINK_NO_SERVICE, &info, 0, 60000, FLEET_PEER_PROBE_NONE, "",
                                 NULL, big, sizeof(big));
        check("nor when the link is down: the link line says why", big[0] == '\0');
        info.trouble = FLEET_PEER_NO_NETWORK;
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 0, 60000, FLEET_PEER_PROBE_NONE, "", NULL, big,
                                 sizeof(big));
        check("nor with no network at all", big[0] == '\0');
        info.trouble = FLEET_PEER_OK;
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 0, 60000, FLEET_PEER_PROBE_PENDING,
                                 "192.168.1.50", NULL, big, sizeof(big));
        says("an address being asked", big, "Asking 192.168.1.50");
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 0, 60000, FLEET_PEER_PROBE_NO_ANSWER,
                                 "192.168.1.50", NULL, big, sizeof(big));
        check("no answer: the sentence of the plan, no spinner",
              strcmp(big, "No answer from 192.168.1.50. This network may keep devices apart. Try "
                          "another network, a cable, or LoRa.") == 0);
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 1, 0, FLEET_PEER_PROBE_ANSWERED,
                                 "192.168.1.50", "Bob", big, sizeof(big));
        check("an answer names who answered and where",
              strcmp(big, "Bob answered at 192.168.1.50.") == 0);
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 0, 0, FLEET_PEER_PROBE_ANSWERED,
                                 "192.168.1.50", NULL, big, sizeof(big));
        says("or that somebody did, before the name is known", big,
             "A device answered at 192.168.1.50.");
        fleet_view_mp_lan_notice(FLEET_LINK_UP, &info, 0, 0, FLEET_PEER_PROBE_REFUSED, "192.168",
                                 NULL, big, sizeof(big));
        says("what is not an address", big, "Could not ask 192.168. An address is four numbers");

        memset(&p, 0, sizeof(p));
        strcpy(p.name, "Anna");
        p.heard_ms = 1000;
        fleet_view_mp_lan_row(&p, 1, 2000, buf, sizeof(buf));
        check("a row on the network has no hops", strcmp(buf, "ANNA \xc2\xb7 HEARD JUST NOW") == 0);
        fleet_view_mp_lan_row(&p, -1, 1000 + 3 * 60000, buf, sizeof(buf));
        check("a link that does not say who is proved lists them plainly",
              strcmp(buf, "ANNA \xc2\xb7 HEARD 3 MIN AGO") == 0);
        fleet_view_mp_lan_row(&p, 0, 2000, buf, sizeof(buf));
        check("a player only announced is said to be unconfirmed",
              strcmp(buf, "ANNA \xc2\xb7 HEARD JUST NOW \xc2\xb7 UNCONFIRMED") == 0);
        p.heard_ms = 0;
        fleet_view_mp_lan_row(&p, 0, 2000, buf, sizeof(buf));
        says("a kept opponent not heard this run", buf, "NOT HEARD YET");

        fleet_view_mp_command(-1, NULL, 0, 0, 0, big, sizeof(big));
        check("Command, a device without the network: the words it always had",
              strcmp(big, "Play another Doors device over the mesh.") == 0);
        fleet_view_mp_command(0, "Anna", 0, 0, 0, big, sizeof(big));
        check("and for a match in hand", strcmp(big, "An engagement with Anna is waiting.") == 0);
        fleet_view_mp_command(-1, NULL, 1, 1, 0, big, sizeof(big));
        says("Command, both on offer", big, "over LoRa or the local network");
        fleet_view_mp_command(1, "Anna", 1, 0, 1, big, sizeof(big));
        check("a match in hand on the network says where",
              strcmp(big, "An engagement with Anna is waiting on the local network.") == 0);
        fleet_view_mp_command(0, "Anna", 1, 0, 0, big, sizeof(big));
        says("and one on LoRa, on a device that has both", big, "is waiting on LoRa.");
        fleet_view_mp_command(-1, NULL, 1, 0, 1, big, sizeof(big));
        says("an invitation open on the network: one way in, named", big,
             "over the local network.");
    }
    printf("fleet_view_mp_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
