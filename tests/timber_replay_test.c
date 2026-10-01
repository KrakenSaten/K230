/*
 * PocketTimber replay test: the action vocabulary, the log, a scripted
 * session replayed bit-identically, and two modelled players whose whole
 * sessions are recorded, replayed and timed.
 *
 * The players are models of a player and not players. The careful one
 * reads tells, spends both tests, pulls the loosest block it knows at
 * four fifths of its limit, waits for the tower to settle, and places
 * against the lean. The greedy one tests nothing, goes for the blocks it
 * thinks are worth most, pulls everything at the EASY limit whatever it
 * is, and is sloppy every few pulls. Both refuse to pull the last support
 * from under a stack, because nobody does that on purpose. The numbers
 * they produce are printed and asserted against a wide band, so the test
 * says what the engine's pacing is rather than what it should be.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#include "timber_replay.h"
#include "timber_summit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
}

/* The log is tens of kilobytes: off the stack, and the test aborts rather
 * than dereferencing a failed allocation. */
static struct timber_log *malloc_log(void)
{
    struct timber_log *log = malloc(sizeof(*log));

    if (!log) {
        printf("FAIL cannot allocate a log\n");
        exit(1);
    }
    return log;
}

static void free_log(struct timber_log *log)
{
    free(log);
}

/* A digest over the named fields of a run, so struct padding never affects
 * a comparison and a divergence is caught wherever it happens. */
static uint32_t fnv1a(uint32_t h, uint32_t v)
{
    int i;

    for (i = 0; i < 4; i++) {
        h ^= (v >> (i * 8)) & 0xFFu;
        h *= 16777619u;
    }
    return h;
}

static uint32_t digest(const struct timber_run *run)
{
    uint32_t h = 2166136261u;
    int id;
    int layer;
    int slot;
    int i;

    h = fnv1a(h, run->seed);
    h = fnv1a(h, run->rng.state);
    h = fnv1a(h, run->state);
    h = fnv1a(h, run->turn);
    h = fnv1a(h, run->selected);
    h = fnv1a(h, run->held);
    h = fnv1a(h, run->tests_left);
    h = fnv1a(h, run->turns);
    h = fnv1a(h, run->ticks);
    h = fnv1a(h, run->grip_broken);
    h = fnv1a(h, run->pull_jolted);
    h = fnv1a(h, run->travel_index);
    h = fnv1a(h, (uint32_t)(int32_t)run->grip);
    for (i = 0; i < TIMBER_TRAVEL_WINDOW; i++) {
        h = fnv1a(h, (uint32_t)(int32_t)run->travel[i]);
    }
    h = fnv1a(h, run->disturb);
    h = fnv1a(h, run->disturb_axis);
    h = fnv1a(h, (uint32_t)(int32_t)run->disturb_sign);
    h = fnv1a(h, run->sway_phase);
    h = fnv1a(h, (uint32_t)(int32_t)run->sway);
    h = fnv1a(h, (uint32_t)run->lean_x);
    h = fnv1a(h, (uint32_t)run->lean_y);
    h = fnv1a(h, run->hinge);
    h = fnv1a(h, run->creaking);
    h = fnv1a(h, (uint32_t)run->margin_static);
    h = fnv1a(h, (uint32_t)run->margin_eff);
    h = fnv1a(h, run->last_jolt_tick);
    h = fnv1a(h, run->last_place_tick);
    h = fnv1a(h, run->cause);
    h = fnv1a(h, run->collapse_ticks);
    h = fnv1a(h, run->collapse.active);
    h = fnv1a(h, run->collapse.ticks);
    h = fnv1a(h, run->collapse.falling);
    for (id = 0; id < TIMBER_BLOCKS; id++) {
        const struct timber_fall *f = &run->collapse.blocks[id];
        const struct timber_block *b = &run->tower.blocks[id];

        h = fnv1a(h, (uint32_t)f->x);
        h = fnv1a(h, (uint32_t)f->y);
        h = fnv1a(h, (uint32_t)f->z);
        h = fnv1a(h, f->pose);
        h = fnv1a(h, f->falling);
        h = fnv1a(h, f->rest_tick);
        h = fnv1a(h, b->present);
        h = fnv1a(h, b->layer);
        h = fnv1a(h, b->slot);
        h = fnv1a(h, b->seat);
        h = fnv1a(h, b->tell);
        h = fnv1a(h, b->tested);
        h = fnv1a(h, b->moves);
        h = fnv1a(h, (uint32_t)(int32_t)b->extraction);
        h = fnv1a(h, (uint32_t)(int32_t)b->offset);
    }
    h = fnv1a(h, (uint32_t)run->score.points);
    h = fnv1a(h, run->score.streak);
    h = fnv1a(h, run->score.pulls);
    h = fnv1a(h, run->score.height);
    h = fnv1a(h, run->tower.layers);
    for (layer = 0; layer < TIMBER_LAYERS_MAX; layer++) {
        for (slot = 0; slot < TIMBER_SLOTS; slot++) {
            h = fnv1a(h, run->tower.grid[layer][slot]);
        }
    }
    return h;
}

/* ---- the vocabulary and the log --------------------------------------- */

static void test_actions(void)
{
    struct timber_run run;
    struct timber_action a;
    int i;
    int named = 1;

    for (i = 0; i < TIMBER_ACTION_COUNT; i++) {
        named &= strcmp(timber_action_name((enum timber_action_type)i), "?") != 0;
    }
    check("every action has a name", named && strcmp(timber_action_name(TIMBER_ACTION_COUNT), "?") == 0);

    timber_run_new(&run, 9u);
    timber_run_start(&run);
    run.tower.blocks[4].seat = 255;
    a.tick = 0;
    a.type = TIMBER_ACTION_SELECT;
    a.arg = 4;
    check("apply selects", timber_run_apply(&run, &a) == 0 && timber_run_selected(&run) == 4);
    a.type = TIMBER_ACTION_TEST;
    check("apply tests", timber_run_apply(&run, &a) == TIMBER_CLASS_FREE && run.tests_left == 1);
    a.type = TIMBER_ACTION_PULL;
    a.arg = 100;
    check("apply pulls", timber_run_apply(&run, &a) == 1 && run.tower.blocks[4].extraction == 100);
    a.arg = -100;
    timber_run_apply(&run, &a);
    a.type = TIMBER_ACTION_DESELECT;
    check("apply deselects", timber_run_apply(&run, &a) == 0 && timber_run_selected(&run) == -1);
    a.type = TIMBER_ACTION_PLACE;
    a.arg = 1;
    check("apply passes a refusal through", timber_run_apply(&run, &a) == -1);
    a.type = TIMBER_ACTION_NONE;
    check("an unknown action is refused",
          timber_run_apply(&run, &a) == -1 && timber_run_apply(NULL, &a) == -1 &&
          timber_run_apply(&run, NULL) == -1);
    timber_run_advance(&run, 30);
    check("advance ticks up to a tick", run.ticks == 30);
    timber_run_advance(&run, 10);
    check("advance never goes back", run.ticks == 30);
    timber_run_advance(NULL, 5);
}

static void test_log(void)
{
    struct timber_log *log = malloc_log();
    int i;
    int full = 1;

    timber_log_init(log, 77u);
    check("a fresh log is empty and keeps its seed",
          log->seed == 77u && log->count == 0 && log->dropped == 0);
    check("recording appends",
          timber_log_record(log, 5, TIMBER_ACTION_SELECT, 3) == 0 && log->count == 1 &&
          log->actions[0].tick == 5 && log->actions[0].type == TIMBER_ACTION_SELECT &&
          log->actions[0].arg == 3);
    check("a wide argument is clamped",
          timber_log_record(log, 6, TIMBER_ACTION_PULL, 100000) == 0 && log->actions[1].arg == 32767);
    for (i = 2; i < TIMBER_LOG_MAX; i++) {
        full &= timber_log_record(log, (uint32_t)i, TIMBER_ACTION_PULL, 10) == 0;
    }
    check("the log holds its stated size", full && log->count == TIMBER_LOG_MAX);
    check("past that it drops and counts",
          timber_log_record(log, 9999, TIMBER_ACTION_PULL, 10) == -1 && log->dropped == 1 &&
          log->count == TIMBER_LOG_MAX);
    check("NULL is safe",
          timber_log_record(NULL, 0, TIMBER_ACTION_PULL, 0) == -1 &&
          timber_log_play(NULL, NULL) == -1 && timber_log_act(NULL, NULL, TIMBER_ACTION_TEST, 0) == -1);
    timber_log_init(NULL, 1u);
    free_log(log);
}

/* ---- a scripted session -------------------------------------------------- */

static void test_scripted_replay(void)
{
    struct timber_log *log = malloc_log();
    struct timber_run live;
    struct timber_run again;
    int i;
    int applied;

    timber_log_init(log, 2026u);
    timber_run_new(&live, 2026u);
    timber_run_start(&live);
    /* A turn with everything in it, then a second block left part way out
     * and pushed back, then a hard pull that jolts. Actions happen at the
     * ticks the session is at, refusals included. */
    timber_run_advance(&live, 12);
    timber_log_act(log, &live, TIMBER_ACTION_SELECT, 4);
    timber_log_act(log, &live, TIMBER_ACTION_TEST, 0);
    timber_log_act(log, &live, TIMBER_ACTION_SELECT, 7);
    timber_log_act(log, &live, TIMBER_ACTION_TEST, 0);
    timber_log_act(log, &live, TIMBER_ACTION_TEST, 0);
    timber_log_act(log, &live, TIMBER_ACTION_SELECT, 4);
    for (i = 0; i < 40 && live.turn != TIMBER_TURN_PLACING; i++) {
        timber_run_tick(&live);
        timber_log_act(log, &live, TIMBER_ACTION_PULL, 30);
    }
    timber_run_advance(&live, live.ticks + 15);
    timber_log_act(log, &live, TIMBER_ACTION_PLACE, 3);
    timber_log_act(log, &live, TIMBER_ACTION_PLACE, 0);
    timber_run_advance(&live, live.ticks + 20);
    timber_log_act(log, &live, TIMBER_ACTION_SELECT, 10);
    for (i = 0; i < 5; i++) {
        timber_run_tick(&live);
        timber_log_act(log, &live, TIMBER_ACTION_PULL, 60);
    }
    for (i = 0; i < 6; i++) {
        timber_run_tick(&live);
        timber_log_act(log, &live, TIMBER_ACTION_PULL, -60);
    }
    timber_log_act(log, &live, TIMBER_ACTION_DESELECT, 0);
    timber_log_act(log, &live, TIMBER_ACTION_SELECT, 13);
    for (i = 0; i < 12 && live.turn != TIMBER_TURN_PLACING && live.state == TIMBER_RUN_ACTIVE; i++) {
        timber_run_tick(&live);
        timber_log_act(log, &live, TIMBER_ACTION_PULL, 200);
    }
    timber_run_advance(&live, live.ticks + 30);
    check("the session did things", log->count > 40 && live.turns >= 1 && live.ticks > 100);

    applied = timber_log_play(log, &again);
    timber_run_advance(&again, live.ticks);
    check("every recorded action was replayed", applied == (int)log->count);
    check("the replayed run is the live run, bit for bit", digest(&again) == digest(&live));
    check("and its tower is the same tower", memcmp(&again.tower, &live.tower, sizeof(live.tower)) == 0);

    /* A pull changed enough to change its jolt leans the tower differently
     * for the rest of the run. */
    for (i = 0; i < (int)log->count; i++) {
        if (log->actions[i].type == TIMBER_ACTION_PULL && log->actions[i].arg == 200) {
            break;
        }
    }
    check("the session had a hard pull to change", i < (int)log->count);
    log->actions[i].arg = 250;
    timber_log_play(log, &again);
    timber_run_advance(&again, live.ticks);
    check("a pull changed anywhere changes the run", digest(&again) != digest(&live));
    log->actions[i].arg = 200;
    log->seed = 2027u;
    timber_log_play(log, &again);
    timber_run_advance(&again, live.ticks);
    check("the same actions on another seed are another run", digest(&again) != digest(&live));
    free_log(log);
}

/* ---- modelled players ------------------------------------------------- */

struct profile {
    const char *name;
    int reaction;       /* ticks between acts */
    int careful;        /* reads tells, tests, avoids the last support */
    int impatient;      /* never waits for the tower to settle; pulls everything at the EASY limit */
    int sloppy_every;   /* every nth pull is made at sloppy_travel; 0 never */
    int32_t sloppy_travel;
};

struct outcome {
    uint32_t ticks;
    int pulls;
    int cause;
    int32_t points;
    int height;
    int over;
};

/* Nobody pulls the last support from under a stack on purpose: a block
 * may go if its layer keeps two blocks, or keeps only its centre. */
static int safe_to_pull(const struct timber_run *run, int id)
{
    const struct timber_block *b = timber_tower_block(&run->tower, id);
    int fill;
    int other;
    int slot;

    if (!b || !timber_tower_pullable(&run->tower, id)) {
        return 0;
    }
    fill = timber_tower_layer_fill(&run->tower, b->layer);
    if (fill >= 3) {
        return 1;
    }
    if (fill < 2) {
        return 0;
    }
    other = -1;
    for (slot = 0; slot < TIMBER_SLOTS; slot++) {
        int at = timber_tower_at(&run->tower, b->layer, slot);

        if (at >= 0 && at != id) {
            other = at;
        }
    }
    return other >= 0 && timber_tower_block(&run->tower, other)->slot == 1;
}

/* How much the player likes a candidate: higher is better. The careful
 * player likes what it knows is loose and what carries little; the greedy
 * one likes what looks tight and sits deep. */
static int appeal(const struct timber_run *run, const struct profile *p, int id)
{
    const struct timber_block *b = timber_tower_block(&run->tower, id);
    int score = 0;

    if (p->careful) {
        if (b->tested) {
            score += 400 - 100 * timber_run_class(run, id);
        } else if (b->tell) {
            score += 250;
        } else {
            score += 100;
        }
        score += b->layer;
    } else {
        score += b->tested ? 100 * timber_run_class(run, id) : (b->tell ? 50 : 300);
        score += 40 - b->layer;
    }
    return score;
}

static int best_candidate(const struct timber_run *run, const struct profile *p, int untested_only)
{
    int id;
    int best = -1;
    int best_score = -1;

    for (id = 0; id < TIMBER_BLOCKS; id++) {
        int score;

        if (!safe_to_pull(run, id)) {
            continue;
        }
        if (untested_only && run->tower.blocks[id].tested) {
            continue;
        }
        score = appeal(run, p, id);
        if (score > best_score) {
            best_score = score;
            best = id;
        }
    }
    return best;
}

static void rest(struct timber_run *run, int ticks)
{
    timber_run_advance(run, run->ticks + (uint32_t)ticks);
}

static struct outcome play(const struct profile *p, uint32_t seed, struct timber_log *log)
{
    struct timber_run run;
    struct outcome out;
    int pulls = 0;
    int guard;

    timber_log_init(log, seed);
    timber_run_new(&run, seed);
    timber_run_start(&run);
    while (run.state == TIMBER_RUN_ACTIVE && run.ticks < 25 * 600) {
        int id;
        int cls;
        int32_t travel;
        int slot;
        int axis;
        int32_t lean;

        rest(&run, p->reaction);
        /* Look, and test. */
        if (p->careful) {
            int t;

            for (t = 0; t < TIMBER_TESTS_PER_TURN; t++) {
                id = best_candidate(&run, p, 1);
                if (id < 0) {
                    break;
                }
                timber_log_act(log, &run, TIMBER_ACTION_SELECT, id);
                timber_log_act(log, &run, TIMBER_ACTION_TEST, 0);
                rest(&run, p->reaction);
            }
        }
        id = best_candidate(&run, p, 0);
        if (id < 0) {
            /* Nothing safe is left: the only way on is a gamble. */
            for (id = 0; id < TIMBER_BLOCKS && !timber_tower_pullable(&run.tower, id); id++) {
            }
            if (id >= TIMBER_BLOCKS) {
                break;
            }
        }
        timber_log_act(log, &run, TIMBER_ACTION_SELECT, id);
        /* Wait for the tower to settle, if patient. */
        if (!p->impatient) {
            for (guard = 0; guard < 30 && run.disturb > 20; guard++) {
                timber_run_tick(&run);
            }
        }
        /* Pull: at four fifths of the known limit, or at the EASY limit
         * whatever the block is, and sometimes too fast. */
        cls = run.tower.blocks[id].tested ? timber_run_class(&run, id) : -1;
        if (p->impatient) {
            travel = timber_pull_limit(TIMBER_CLASS_EASY);
        } else {
            travel = cls >= 0 ? timber_pull_limit((enum timber_class)cls) * 4 / 5
                              : timber_pull_limit(TIMBER_CLASS_STUCK);
        }
        pulls++;
        if (p->sloppy_every && pulls % p->sloppy_every == 0) {
            travel = p->sloppy_travel;
        }
        for (guard = 0; guard < 400 && run.state == TIMBER_RUN_ACTIVE && run.turn != TIMBER_TURN_PLACING;
             guard++) {
            timber_run_tick(&run);
            timber_log_act(log, &run, TIMBER_ACTION_PULL, travel);
        }
        if (run.state != TIMBER_RUN_ACTIVE) {
            break;
        }
        /* Place against the lean: across the new layer's axis, slot 0 is
         * the low side. */
        rest(&run, p->reaction);
        axis = timber_layer_axis(timber_tower_place_layer(&run.tower)) == TIMBER_AXIS_X ? TIMBER_AXIS_Y
                                                                                       : TIMBER_AXIS_X;
        lean = axis == TIMBER_AXIS_X ? run.lean_x : run.lean_y;
        slot = lean > 0 ? 0 : lean < 0 ? 2 : 1;
        if (!timber_tower_can_place(&run.tower, slot)) {
            for (slot = 0; slot < TIMBER_SLOTS && !timber_tower_can_place(&run.tower, slot); slot++) {
            }
        }
        timber_log_act(log, &run, TIMBER_ACTION_PLACE, slot);
    }
    /* Let a collapse play out. */
    timber_run_advance(&run, run.ticks + TIMBER_COLLAPSE_TICKS_MAX + 1);
    out.ticks = run.ticks;
    out.pulls = run.score.pulls;
    out.cause = timber_run_cause(&run);
    out.points = run.score.points;
    out.height = run.score.height;
    out.over = timber_run_is_over(&run);
    return out;
}

static void report(const struct profile *p, uint32_t seed, const struct outcome *o)
{
    printf("     seed %u %-9s %2d pulls, %2d layers, %6ld points, %-9s after %3u s%s\n", seed,
           p->name, o->pulls, o->height, (long)o->points,
           timber_cause_name((enum timber_cause)o->cause), (unsigned)(o->ticks * TIMBER_TICK_MS / 1000),
           o->over ? "" : " (still standing)");
}

/* Replay a session from its log and compare it with a second replay of
 * the same log, and with what the live session reported. */
static int replays_exactly(const struct timber_log *log, const struct outcome *o)
{
    struct timber_run again;
    struct timber_run twice;

    timber_log_play(log, &again);
    timber_run_advance(&again, o->ticks);
    timber_log_play(log, &twice);
    timber_run_advance(&twice, o->ticks);
    return digest(&again) == digest(&twice) && again.score.points == o->points &&
           timber_run_cause(&again) == o->cause && timber_run_is_over(&again) == o->over &&
           again.score.height == o->height && timber_tower_validate(&again.tower) == TIMBER_VALID &&
           again.lean_x == twice.lean_x && again.lean_y == twice.lean_y;
}

/* Apply a log's actions to a run that already exists, at their ticks. */
static int replay_into(struct timber_run *run, const struct timber_log *log)
{
    uint32_t i;

    for (i = 0; i < log->count; i++) {
        timber_run_advance(run, log->actions[i].tick);
        timber_run_apply(run, &log->actions[i]);
    }
    return (int)log->count;
}

static void test_replay_through_summit(void)
{
    struct timber_log *log = malloc_log();
    struct timber_run live;
    struct timber_run again;

    /* The summit is not reached from a seed by any modelled player with
     * the approved constants, so the session starts from the constructed
     * state and the log carries only what the player did from there. */
    summit_build(&live, 81u);
    timber_log_init(log, 81u);
    timber_log_act(log, &live, TIMBER_ACTION_PLACE, 1);
    timber_run_advance(&live, live.ticks + 3);
    timber_log_act(log, &live, TIMBER_ACTION_SELECT, 0);
    timber_log_act(log, &live, TIMBER_ACTION_PULL, 50);
    check("the live session ended standing, and the log kept what came after",
          timber_run_is_over(&live) && timber_run_cause(&live) == TIMBER_CAUSE_NONE && log->count == 3);

    summit_build(&again, 81u);
    replay_into(&again, log);
    check("replayed, the session reaches the same summit, bit for bit",
          digest(&again) == digest(&live) && timber_run_is_over(&again) &&
          timber_run_cause(&again) == TIMBER_CAUSE_NONE && timber_run_held(&again) == -1 &&
          again.lean_x == live.lean_x && again.lean_y == live.lean_y &&
          again.score.points == live.score.points && again.ticks == live.ticks);
    free_log(log);
}

static void test_modelled_players(void)
{
    /* careful: reads tells, tests, waits, pulls at four fifths of the known
     * limit. ordinary: chooses like the careful player but never waits and
     * pulls everything at the EASY limit. greedy: no tests, the deepest
     * tight-looking block, every pull at the EASY limit, every fourth one
     * yanked. A model of a player is not a player: a bot turn takes about
     * two seconds where a person takes five to fifteen, so the pull counts
     * are the numbers to read, not the seconds. */
    static const struct profile careful = { "careful", 12, 1, 0, 0, 0 };
    static const struct profile ordinary = { "ordinary", 12, 1, 1, 0, 0 };
    static const struct profile greedy = { "greedy", 6, 0, 1, 4, TIMBER_SPEED_EASY * 14 / 10 };
    static const uint32_t seeds[5] = { 101u, 202u, 303u, 404u, 505u };
    struct timber_log *log = malloc_log();
    int i;
    int careful_falls = 1;
    int careful_late = 1;
    int ordinary_falls = 1;
    int greedy_falls = 1;
    int greedy_early = 1;
    int fits = 1;
    int replays = 1;
    long careful_total = 0;
    long ordinary_total = 0;
    long greedy_total = 0;
    long careful_pulls = 0;
    long ordinary_pulls = 0;
    long greedy_pulls = 0;

    for (i = 0; i < 5; i++) {
        struct outcome c = play(&careful, seeds[i], log);
        struct outcome o;
        struct outcome g;

        report(&careful, seeds[i], &c);
        careful_total += (long)c.ticks;
        careful_pulls += c.pulls;
        careful_falls &= c.over && c.cause != TIMBER_CAUSE_NONE;
        careful_late &= c.pulls >= 25 && c.pulls <= TIMBER_BLOCKS;
        fits &= log->dropped == 0;
        replays &= replays_exactly(log, &c);

        o = play(&ordinary, seeds[i], log);
        report(&ordinary, seeds[i], &o);
        ordinary_total += (long)o.ticks;
        ordinary_pulls += o.pulls;
        ordinary_falls &= o.over && o.cause != TIMBER_CAUSE_NONE;
        fits &= log->dropped == 0;
        replays &= replays_exactly(log, &o);

        g = play(&greedy, seeds[i], log);
        report(&greedy, seeds[i], &g);
        greedy_total += (long)g.ticks;
        greedy_pulls += g.pulls;
        greedy_falls &= g.over && g.cause != TIMBER_CAUSE_NONE;
        greedy_early &= g.pulls < c.pulls;
        fits &= log->dropped == 0;
        replays &= replays_exactly(log, &g);
    }
    printf("     mean pulls: careful %ld, ordinary %ld, greedy %ld; mean bot time %ld / %ld / %ld s\n",
           careful_pulls / 5, ordinary_pulls / 5, greedy_pulls / 5, careful_total * TIMBER_TICK_MS / 5000,
           ordinary_total * TIMBER_TICK_MS / 5000, greedy_total * TIMBER_TICK_MS / 5000);
    /* The shift lean (timber_stability.h, D3) is what brings a careful
     * player down at all; without it the run ends only at the summit. */
    check("even a careful player is brought down in the end", careful_falls);
    check("but late: after twenty-five pulls or more", careful_late);
    check("an ordinary player is brought down", ordinary_falls);
    check("a greedy player is brought down", greedy_falls);
    check("and sooner than a careful one on the same tower", greedy_early);
    check("care buys pulls: careful lasts at least as long as ordinary, and longer than greedy",
          careful_pulls >= ordinary_pulls && ordinary_pulls > greedy_pulls);
    check("every session fits the log", fits);
    check("every recorded session replays bit for bit, collapse and score included", replays);
    free_log(log);
}

int main(void)
{
    test_actions();
    test_log();
    test_scripted_replay();
    test_replay_through_summit();
    test_modelled_players();

    printf("timber_replay_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
