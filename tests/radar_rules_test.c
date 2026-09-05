/*
 * PocketRadar rules test: the run lifecycle, spawning, movement, track
 * lifetime, selection, acquisition, touch picking and the event queue.
 *
 * It also covers engagement, the difficulty ramp and the end of a run.
 *
 * The central assertions are that a seed reproduces a run exactly, that the
 * spawn stream depends on the tick count and not on how the player plays,
 * that every call refuses a state the rules could not have produced rather
 * than doing something undefined with it, and that the difficulty ramp ends
 * a run by arithmetic rather than by taking anything away from the player.
 *
 * Copyright (c) 2026 PocketOS authors. License: see LICENSE (TBD).
 */
#include "radar_rules.h"

#include <stdio.h>
#include <string.h>

static int failed;

static void check(const char *name, int ok)
{
    printf("%s %s\n", ok ? "ok  " : "FAIL", name);
    failed += !ok;
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

static uint32_t digest(const struct radar_run *run)
{
    uint32_t h = 2166136261u;
    int i;

    h = fnv1a(h, run->seed);
    h = fnv1a(h, run->rng.state);
    h = fnv1a(h, run->state);
    h = fnv1a(h, run->ticks);
    h = fnv1a(h, run->next_id);
    h = fnv1a(h, run->spawn_timer);
    h = fnv1a(h, run->sweep);
    h = fnv1a(h, run->selected);
    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        const struct radar_contact *c = &run->contacts[i];

        h = fnv1a(h, c->id);
        h = fnv1a(h, c->active);
        h = fnv1a(h, c->cls);
        h = fnv1a(h, c->state);
        h = fnv1a(h, c->classified);
        h = fnv1a(h, c->bearing);
        h = fnv1a(h, c->range);
        h = fnv1a(h, c->spawn_range);
        h = fnv1a(h, (uint32_t)(int32_t)c->drift);
        h = fnv1a(h, c->ttl);
        h = fnv1a(h, c->ttl_max);
        h = fnv1a(h, c->lock);
        h = fnv1a(h, c->age);
    }
    return h;
}

static void start(struct radar_run *run, uint32_t seed)
{
    radar_run_new(run, seed);
    radar_run_start(run);
}

static void tick_n(struct radar_run *run, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        radar_run_tick(run);
        radar_run_clear_events(run);
    }
}

/* Hold the sector intact. The generator and pacing tests are about what
 * the engine produces over a long span, not about how long somebody
 * survives, so they refill integrity instead of modelling a player. */
static void sustain(struct radar_run *run)
{
    run->score.integrity = RADAR_INTEGRITY_MAX;
}

/* The first contact on the scope, whatever slot it landed in. */
static const struct radar_contact *first_contact(const struct radar_run *run)
{
    int i;

    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        const struct radar_contact *c = radar_run_slot(run, i);

        if (c && c->active) {
            return c;
        }
    }
    return NULL;
}

static void test_lifecycle(void)
{
    struct radar_run run;
    struct radar_event ev;

    radar_run_new(&run, 2026u);
    check("a new run is ready", run.state == RADAR_RUN_READY);
    check("a new run has no contacts", radar_run_contact_count(&run) == 0);
    check("a new run has nothing selected",
          run.selected == RADAR_NO_CONTACT && radar_run_selected(&run) == NULL);
    check("a new run is not over", !radar_run_is_over(&run));

    /* A ready run must be inert: the UI builds the screen before the player
     * has agreed to start. */
    tick_n(&run, 50);
    check("a ready run does not tick",
          run.ticks == 0 && radar_run_contact_count(&run) == 0);

    check("a ready run starts", radar_run_start(&run) == 0);
    check("a started run is active", run.state == RADAR_RUN_ACTIVE);
    check("a run cannot be started twice", radar_run_start(&run) == -1);

    radar_run_tick(&run);
    check("the first tick counts", run.ticks == 1);
    check("the first tick paints a track", radar_run_contact_count(&run) == 1);
    check("the first tick reports the spawn",
          radar_run_take_event(&run, &ev) == 1 && ev.type == RADAR_EVENT_SPAWN);
    check("the queue empties", radar_run_take_event(&run, &ev) == 0);

    /* A finished run is inert for the same reason a ready one is. */
    run.state = (uint8_t)RADAR_RUN_OVER;
    {
        uint32_t before = digest(&run);

        tick_n(&run, 20);
        check("a finished run does not tick", digest(&run) == before);
        check("a finished run is over", radar_run_is_over(&run));
    }

    check("NULL is safe",
          radar_run_start(NULL) == -1 && radar_run_contact_count(NULL) == 0 &&
          radar_run_slot(NULL, 0) == NULL && radar_run_find(NULL, 1u) == NULL &&
          radar_run_selected(NULL) == NULL && !radar_run_is_over(NULL) &&
          radar_run_pick(NULL, 0, 500) == RADAR_NO_CONTACT &&
          radar_run_select(NULL, 1u) == -1 &&
          radar_run_engage(NULL) == RADAR_ENGAGE_INVALID &&
          radar_run_engage_value(NULL) == 0 && radar_run_level(NULL) == 0 &&
          radar_run_take_event(NULL, &ev) == 0);
    radar_run_tick(NULL);
    radar_run_new(NULL, 1u);
    radar_run_deselect(NULL);
    radar_run_clear_events(NULL);
}

/* The observed class mix against the table the level states. Drops from a
 * full scope do not bias it: the class is drawn before the scope is
 * consulted, so a refused spawn is refused whatever it would have been. */
static void check_mix(const char *what, const int *classes, int total, int level)
{
    struct radar_level p;
    int ok = total > 200;
    int i;

    radar_level_params(level, &p);
    for (i = 0; i < RADAR_CLASS_COUNT; i++) {
        int observed = total > 0 ? classes[i] * 100 / total : -1;

        if (observed < (int)p.pct[i] - 5 || observed > (int)p.pct[i] + 5) {
            printf("     %s: %s came out at %d %%, the table says %d %%\n",
                   what, radar_class_name((enum radar_class)i), observed,
                   (int)p.pct[i]);
            ok = 0;
        }
    }
    check(what, ok);
}

static void test_spawning(void)
{
    struct radar_run run;
    int on_schedule = 1;
    int classes[RADAR_CLASS_COUNT];
    int in_band = 1;
    int drift_ok = 1;
    int ids_unique = 1;
    int total = 0;
    int i;

    start(&run, 4242u);

    /* Spawns arrive on the fixed schedule: one on the first tick, then one
     * every RADAR_SPAWN_TICKS_BASE. Nothing else may add a track. */
    for (i = 1; i <= 4 * RADAR_SPAWN_TICKS_BASE; i++) {
        int spawned = 0;
        struct radar_event ev;

        radar_run_tick(&run);
        while (radar_run_take_event(&run, &ev)) {
            spawned += ev.type == RADAR_EVENT_SPAWN;
        }
        if ((i - 1) % RADAR_SPAWN_TICKS_BASE == 0) {
            on_schedule &= spawned == 1;
        } else {
            on_schedule &= spawned == 0;
        }
    }
    check("spawns arrive only on the schedule", on_schedule);
    check("the scope holds no more than the level allows",
          radar_run_contact_count(&run) <= RADAR_CONTACTS_BASE);

    /* Level 0, sampled across many short runs because one run only spends
     * RADAR_LEVEL_TICKS there. Every track starts in the outer band with a
     * drift the level allows, and no id is ever repeated inside a run. */
    memset(classes, 0, sizeof(classes));
    {
        uint32_t seed;
        uint32_t seen[64];
        int seen_n;

        for (seed = 1u; seed <= 60u; seed++) {
            seen_n = 0;
            start(&run, seed * 7u + 1u);
            for (i = 1; i < RADAR_LEVEL_TICKS; i++) {
                struct radar_event ev;
                int s;

                sustain(&run);
                radar_run_tick(&run);
                while (radar_run_take_event(&run, &ev)) {
                    if (ev.type != RADAR_EVENT_SPAWN) {
                        continue;
                    }
                    classes[ev.cls]++;
                    total++;
                    if (seen_n < 64) {
                        int j;

                        for (j = 0; j < seen_n; j++) {
                            ids_unique &= seen[j] != ev.id;
                        }
                        seen[seen_n++] = ev.id;
                    }
                }
                for (s = 0; s < RADAR_CONTACTS_MAX; s++) {
                    const struct radar_contact *c = radar_run_slot(&run, s);

                    if (!c->active || c->age != 0) {
                        continue;
                    }
                    in_band &= c->spawn_range >= RADAR_SPAWN_RANGE_MIN &&
                               c->spawn_range <= RADAR_RANGE_MAX;
                    drift_ok &= c->drift >= -RADAR_DRIFT_BASE &&
                                c->drift <= RADAR_DRIFT_BASE;
                }
            }
        }
    }
    check("every class appears",
          classes[RADAR_CLASS_NORMAL] > 0 && classes[RADAR_CLASS_FAST] > 0 &&
          classes[RADAR_CLASS_DECOY] > 0 && classes[RADAR_CLASS_HIGH_VALUE] > 0);
    check_mix("the opening class mix follows the table", classes, total, 0);
    check("a track starts in the outer band", in_band);
    check("drift stays inside what the opening level allows", drift_ok);
    check("no contact id is reused", ids_unique);

    /* The same, at the top of the ramp, where the table is a different
     * shape: decoys are the commonest thing on the scope. */
    memset(classes, 0, sizeof(classes));
    total = 0;
    drift_ok = 1;
    start(&run, 909u);
    while (radar_run_level(&run) < RADAR_LEVEL_MAX) {
        sustain(&run);
        radar_run_tick(&run);
        radar_run_clear_events(&run);
    }
    for (i = 0; i < 14000; i++) {
        struct radar_event ev;
        struct radar_level p;
        int s;

        sustain(&run);
        radar_run_tick(&run);
        while (radar_run_take_event(&run, &ev)) {
            if (ev.type == RADAR_EVENT_SPAWN) {
                classes[ev.cls]++;
                total++;
            }
        }
        radar_level_params(RADAR_LEVEL_MAX, &p);
        for (s = 0; s < RADAR_CONTACTS_MAX; s++) {
            const struct radar_contact *c = radar_run_slot(&run, s);

            if (c->active && c->age == 0) {
                drift_ok &= c->drift >= -(int)p.drift_dd &&
                            c->drift <= (int)p.drift_dd;
            }
        }
    }
    check_mix("the class mix at the ceiling follows the table", classes,
              total, RADAR_LEVEL_MAX);
    check("decoys are the commonest contact at the ceiling",
          classes[RADAR_CLASS_DECOY] > classes[RADAR_CLASS_NORMAL] &&
          classes[RADAR_CLASS_DECOY] > classes[RADAR_CLASS_FAST]);
    check("high-value contacts stay the rarest everywhere",
          classes[RADAR_CLASS_HIGH_VALUE] < classes[RADAR_CLASS_DECOY] &&
          classes[RADAR_CLASS_HIGH_VALUE] < classes[RADAR_CLASS_FAST]);
    check("drift stays inside what the hardest level allows", drift_ok);
}

static void test_lifetime_and_motion(void)
{
    struct radar_run run;
    const struct radar_contact *c;
    enum radar_class cls;
    uint16_t ttl_max;
    uint32_t id;
    int expected;
    int monotonic = 1;
    int inside = 1;
    int i;

    start(&run, 31337u);
    radar_run_tick(&run);
    c = first_contact(&run);
    check("there is a track to follow", c != NULL);
    if (!c) {
        return;
    }
    cls = (enum radar_class)c->cls;
    ttl_max = c->ttl_max;
    id = c->id;
    check("a track starts at its full lifetime",
          ttl_max == radar_class_base_ttl(cls) && c->ttl == ttl_max);
    check("a track starts at its spawn range", c->range == c->spawn_range);
    check("a track starts unclassified and unselected",
          !c->classified && c->state == RADAR_CONTACT_NEW);
    check("a fresh track reports full life", radar_contact_ttl_permille(c) == 1000);

    /* Range is derived from the remaining lifetime, so it falls with every
     * tick and never leaves the scope. */
    {
        int previous = c->range;

        for (i = 1; i < ttl_max; i++) {
            radar_run_tick(&run);
            radar_run_clear_events(&run);
            c = radar_run_find(&run, id);
            if (!c) {
                break;
            }
            monotonic &= c->range <= previous;
            inside &= c->range >= RADAR_RANGE_MIN && c->range <= RADAR_RANGE_MAX;
            inside &= c->bearing < RADAR_BEARING_MAX;
            previous = c->range;
        }
    }
    check("a track closes inbound and never turns back", monotonic);
    check("a track stays on the scope", inside);
    check("a track is still alive one tick before its time",
          radar_run_find(&run, id) != NULL);

    /* It lives exactly ttl_max ticks, and the tick that retires it says so. */
    radar_run_tick(&run);
    check("the track is gone on the last tick of its life",
          radar_run_find(&run, id) == NULL);
    {
        struct radar_event ev;
        int faded = 0;
        int right_class = 0;

        while (radar_run_take_event(&run, &ev)) {
            if (ev.type == RADAR_EVENT_FADED && ev.id == id) {
                faded = 1;
                right_class = ev.cls == (uint8_t)cls;
            }
        }
        check("a faded track is reported", faded);
        check("a faded track reports its class, so a decoy can be free",
              right_class);
    }
    /* It appeared on tick 1 and was retired on this one, so it was on the
     * scope for exactly its lifetime. */
    expected = ttl_max;
    check("a track is on the scope for exactly its lifetime",
          (int)run.ticks - 1 == expected);
}

static void test_selection_and_acquisition(void)
{
    struct radar_run run;
    const struct radar_contact *c;
    uint32_t id;
    uint32_t other;
    int i;

    start(&run, 9001u);
    radar_run_tick(&run);
    radar_run_clear_events(&run);
    c = first_contact(&run);
    if (!c) {
        check("there is a track to select", 0);
        return;
    }
    id = c->id;

    check("an unknown id cannot be selected", radar_run_select(&run, 424242u) == -1);
    check("the no-contact id cannot be selected",
          radar_run_select(&run, RADAR_NO_CONTACT) == -1);
    check("nothing is selected yet", radar_run_selected(&run) == NULL);

    check("a track on the scope can be selected", radar_run_select(&run, id) == 0);
    c = radar_run_selected(&run);
    check("the selected track is the one asked for", c && c->id == id);
    check("selecting starts the lock",
          c && c->state == RADAR_CONTACT_SELECTED && c->lock == 0);
    check("an unlocked track reveals nothing", c && !c->classified);
    check("lock progress starts at zero", radar_contact_lock_permille(c) == 0);

    /* Acquisition takes exactly RADAR_ACQUIRE_TICKS of unbroken selection. */
    for (i = 0; i < RADAR_ACQUIRE_TICKS - 1; i++) {
        radar_run_tick(&run);
        radar_run_clear_events(&run);
    }
    c = radar_run_find(&run, id);
    check("one tick short of the lock it is still only selected",
          c && c->state == RADAR_CONTACT_SELECTED && !c->classified);
    check("lock progress is nearly complete",
          c && radar_contact_lock_permille(c) > 850 &&
          radar_contact_lock_permille(c) < 1000);

    radar_run_tick(&run);
    c = radar_run_find(&run, id);
    check("the lock completes on schedule", c && c->state == RADAR_CONTACT_ACQUIRED);
    check("acquiring reveals the class", c && c->classified);
    check("an acquired track reports a full lock",
          radar_contact_lock_permille(c) == 1000);
    {
        struct radar_event ev;
        int acquired = 0;

        while (radar_run_take_event(&run, &ev)) {
            acquired |= ev.type == RADAR_EVENT_ACQUIRED && ev.id == id;
        }
        check("the lock is reported", acquired);
    }

    /* A second tap on the same track must not throw away its lock. */
    check("re-selecting the same track is accepted", radar_run_select(&run, id) == 0);
    c = radar_run_find(&run, id);
    check("re-selecting the same track keeps the lock",
          c && c->state == RADAR_CONTACT_ACQUIRED);

    /* Switching costs the lock but not what was learned. */
    tick_n(&run, RADAR_SPAWN_TICKS_BASE);
    other = RADAR_NO_CONTACT;
    for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
        const struct radar_contact *s = radar_run_slot(&run, i);

        if (s->active && s->id != id) {
            other = s->id;
        }
    }
    check("a second track appeared to switch to", other != RADAR_NO_CONTACT);
    if (other != RADAR_NO_CONTACT && radar_run_find(&run, id)) {
        check("switching selection is accepted", radar_run_select(&run, other) == 0);
        c = radar_run_find(&run, id);
        check("the abandoned track loses its lock",
              c && c->state == RADAR_CONTACT_NEW && c->lock == 0);
        check("the abandoned track keeps what it revealed", c && c->classified);
        check("the new track is selected", run.selected == other);

        radar_run_deselect(&run);
        check("deselecting clears the selection",
              run.selected == RADAR_NO_CONTACT && radar_run_selected(&run) == NULL);
        c = radar_run_find(&run, other);
        check("a deselected track goes back to new",
              c && c->state == RADAR_CONTACT_NEW && c->lock == 0);
    }
}

static void test_selection_survives_a_fade(void)
{
    struct radar_run run;
    const struct radar_contact *c;
    uint32_t id;
    int i;

    /* A track that fades while it is being acquired must take the selection
     * with it, or the next ENGAGE would fire at a slot that has since been
     * handed to somebody else. */
    start(&run, 606u);
    radar_run_tick(&run);
    radar_run_clear_events(&run);
    c = first_contact(&run);
    if (!c) {
        check("there is a track to lose", 0);
        return;
    }
    id = c->id;
    radar_run_select(&run, id);

    for (i = 0; i < 4000 && radar_run_find(&run, id); i++) {
        sustain(&run);
        radar_run_tick(&run);
        radar_run_clear_events(&run);
    }
    check("the selected track eventually faded", radar_run_find(&run, id) == NULL);
    check("a faded track takes the selection with it",
          run.selected == RADAR_NO_CONTACT && radar_run_selected(&run) == NULL);

    /* The slot it vacated is reusable and the new tenant is a different
     * contact, which is exactly why selection is by id and not by slot. */
    tick_n(&run, 2 * RADAR_SPAWN_TICKS_BASE);
    check("the vacated slot is reused by a different contact",
          radar_run_contact_count(&run) > 0 && radar_run_find(&run, id) == NULL);
}

static void test_picking(void)
{
    struct radar_run run;
    const struct radar_contact *c;
    uint32_t id;
    int bearing;
    int range;

    start(&run, 5150u);
    radar_run_tick(&run);
    radar_run_clear_events(&run);
    c = first_contact(&run);
    if (!c) {
        check("there is a track to pick", 0);
        return;
    }
    id = c->id;
    bearing = c->bearing;
    range = c->range;

    check("a tap on a track picks it", radar_run_pick(&run, bearing, range) == id);
    check("a tap just inside the pick radius still picks it",
          radar_run_pick(&run, bearing, range - RADAR_PICK_RADIUS + 10) == id);
    check("a tap well away picks nothing",
          radar_run_pick(&run, radar_bearing_wrap(bearing + 1800), range) ==
          RADAR_NO_CONTACT);
    check("a tap at the hub picks nothing",
          radar_run_pick(&run, bearing, 0) == RADAR_NO_CONTACT);

    /* Two tracks at the same distance: the older one wins, so the answer
     * does not depend on which slot the array happened to reuse. */
    {
        struct radar_run two;
        struct radar_contact *a;
        struct radar_contact *b;

        radar_run_new(&two, 1u);
        radar_run_start(&two);
        a = (struct radar_contact *)radar_run_slot(&two, 5);
        b = (struct radar_contact *)radar_run_slot(&two, 2);
        memset(a, 0, sizeof(*a));
        memset(b, 0, sizeof(*b));
        a->id = 7u;
        a->active = 1;
        a->bearing = 1000u;
        a->range = 500u;
        b->id = 9u;
        b->active = 1;
        b->bearing = 1000u;
        b->range = 500u;
        check("a tie goes to the older track", radar_run_pick(&two, 1000, 500) == 7u);

        b->range = 480u;
        check("the nearer track wins outright", radar_run_pick(&two, 1000, 480) == 9u);
    }
}

static void test_determinism(void)
{
    struct radar_run a;
    struct radar_run b;
    struct radar_run c;
    int identical = 1;
    uint32_t seed;
    int i;

    /* The same seed and the same actions reproduce the run exactly. */
    for (seed = 1u; seed <= 25u && identical; seed++) {
        start(&a, seed);
        start(&b, seed);
        for (i = 0; i < 400; i++) {
            radar_run_tick(&a);
            radar_run_tick(&b);
            radar_run_clear_events(&a);
            radar_run_clear_events(&b);
            if (i % 17 == 0) {
                const struct radar_contact *t = first_contact(&a);

                if (t) {
                    radar_run_select(&a, t->id);
                    radar_run_select(&b, t->id);
                }
            }
        }
        identical &= digest(&a) == digest(&b);
    }
    check("the same seed and actions reproduce the run", identical);

    start(&a, 100u);
    start(&c, 101u);
    tick_n(&a, 400);
    tick_n(&c, 400);
    check("a different seed produces a different run", digest(&a) != digest(&c));

    /* The generator must not depend on how the run is played, or two
     * players on the same seed would not meet the same contacts. Run b is
     * played hard - locking and engaging everything it can - while a is
     * left alone, and after every tick the two generators must still be in
     * the same place. */
    {
        int aligned = 1;

        start(&a, 8080u);
        start(&b, 8080u);
        for (i = 0; i < 3000; i++) {
            const struct radar_contact *sel;

            sustain(&a);
            sustain(&b);
            radar_run_tick(&a);
            radar_run_tick(&b);
            radar_run_clear_events(&a);
            radar_run_clear_events(&b);
            sel = radar_run_selected(&b);
            if (sel && sel->state == RADAR_CONTACT_ACQUIRED) {
                radar_run_engage(&b);
            } else if (!sel) {
                const struct radar_contact *t = first_contact(&b);

                if (t) {
                    radar_run_select(&b, t->id);
                }
            }
            aligned &= a.rng.state == b.rng.state;
        }
        check("the generator does not depend on how the run is played", aligned);
        check("the played run really did engage things",
              b.score.engaged + b.score.mistakes > 0);
        check("the two runs differ in everything else", digest(&a) != digest(&b));
    }
}

static void test_events(void)
{
    struct radar_run run;
    struct radar_event ev;
    int ordered = 1;
    int i;

    start(&run, 4711u);
    radar_run_tick(&run);
    check("the queue is not empty after a spawn", run.event_count > 0);
    radar_run_clear_events(&run);
    check("clearing empties the queue",
          run.event_count == 0 && radar_run_take_event(&run, &ev) == 0);
    check("clearing resets the drop counter", run.events_dropped == 0);

    /* Events come back in the order they happened. */
    start(&run, 4712u);
    for (i = 0; i < 3 * RADAR_SPAWN_TICKS_BASE; i++) {
        sustain(&run);
        radar_run_tick(&run);
    }
    {
        uint32_t last = 0;

        while (radar_run_take_event(&run, &ev)) {
            if (ev.type == RADAR_EVENT_SPAWN) {
                ordered &= ev.id > last;
                last = ev.id;
            }
        }
    }
    check("events come back oldest first", ordered);

    /* A caller that never drains is told what it lost rather than losing it
     * silently. */
    start(&run, 4713u);
    for (i = 0; i < 40 * RADAR_SPAWN_TICKS_BASE; i++) {
        sustain(&run);
        radar_run_tick(&run);
    }
    check("an undrained queue stops at its limit",
          run.event_count == RADAR_EVENTS_MAX);
    check("an undrained queue reports the loss", run.events_dropped > 0);
    check("a full queue keeps its oldest events",
          radar_run_take_event(&run, &ev) == 1 && ev.type == RADAR_EVENT_SPAWN &&
          ev.id == 1u);
}

static void test_sweep(void)
{
    struct radar_run run;
    int bounded = 1;
    int i;

    start(&run, 1234u);
    check("the sweep starts at the top", run.sweep == 0);
    for (i = 0; i < 500; i++) {
        sustain(&run);
        radar_run_tick(&run);
        radar_run_clear_events(&run);
        bounded &= run.sweep < RADAR_BEARING_MAX;
    }
    check("the sweep stays inside one turn", bounded);
    check("the sweep turns once every four seconds",
          RADAR_SWEEP_DD_PER_TICK * (4000 / RADAR_TICK_MS) == RADAR_BEARING_MAX);
}

/* Work the scope until a contact of the wanted kind is acquired and armed,
 * holding the sector intact so the search itself cannot end the run.
 * Returns its id, or RADAR_NO_CONTACT if the run never produced one. */
static uint32_t acquire_kind(struct radar_run *run, int want_target)
{
    int guard;

    for (guard = 0; guard < 20000; guard++) {
        const struct radar_contact *sel;

        sustain(run);
        sel = radar_run_selected(run);
        if (sel && sel->state == RADAR_CONTACT_ACQUIRED) {
            if (radar_class_is_target((enum radar_class)sel->cls) == want_target) {
                return sel->id;
            }
            /* What acquisition revealed is sticky, so a rejected contact is
             * not picked up again below. */
            radar_run_deselect(run);
        }
        if (!radar_run_selected(run)) {
            int i;

            for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
                const struct radar_contact *t = radar_run_slot(run, i);

                if (t->active && !t->classified) {
                    radar_run_select(run, t->id);
                    break;
                }
            }
        }
        radar_run_tick(run);
        radar_run_clear_events(run);
    }
    return RADAR_NO_CONTACT;
}

static void test_engagement(void)
{
    struct radar_run run;
    const struct radar_contact *c;
    struct radar_event ev;
    uint32_t id;
    int32_t preview;
    int integrity_before;
    int mistakes_before;
    int reported;

    /* A shot needs a run, a selection and a completed lock. Missing any of
     * them must change nothing at all rather than half-fire. */
    radar_run_new(&run, 1212u);
    check("a ready run cannot engage",
          radar_run_engage(&run) == RADAR_ENGAGE_INVALID);
    radar_run_start(&run);
    check("an empty scope cannot engage",
          radar_run_engage(&run) == RADAR_ENGAGE_INVALID);
    check("an empty scope arms nothing", radar_run_engage_value(&run) == 0);

    radar_run_tick(&run);
    radar_run_clear_events(&run);
    c = first_contact(&run);
    if (!c) {
        check("there is a track to work", 0);
        return;
    }
    radar_run_select(&run, c->id);
    check("a selected but unlocked contact cannot be engaged",
          radar_run_engage(&run) == RADAR_ENGAGE_INVALID);
    check("a selected but unlocked contact arms nothing",
          radar_run_engage_value(&run) == 0);
    check("a refused shot leaves the contact alone",
          radar_run_find(&run, c->id) != NULL && run.selected == c->id);

    /* A valid target. */
    id = acquire_kind(&run, 1);
    check("a valid target can be acquired", id != RADAR_NO_CONTACT);
    if (id == RADAR_NO_CONTACT) {
        return;
    }
    preview = radar_run_engage_value(&run);
    check("an armed shot is worth something", preview > 0);
    radar_run_clear_events(&run);
    check("engaging a valid target is a hit",
          radar_run_engage(&run) == RADAR_ENGAGE_HIT);
    check("the target is off the scope", radar_run_find(&run, id) == NULL);
    check("the shot clears the selection",
          run.selected == RADAR_NO_CONTACT && radar_run_selected(&run) == NULL);
    check("the shot scored exactly what it previewed", run.score.points == preview);
    check("the hit is counted", run.score.engaged == 1 && run.score.streak == 1);

    reported = 0;
    while (radar_run_take_event(&run, &ev)) {
        if (ev.type == RADAR_EVENT_HIT && ev.id == id) {
            reported = ev.value == preview;
        }
    }
    check("the hit is reported with its value", reported);
    check("a spent contact cannot be engaged again",
          radar_run_engage(&run) == RADAR_ENGAGE_INVALID);

    /* A decoy. Acquiring it is what tells the player it is one, so a foul
     * is always a decision. */
    id = acquire_kind(&run, 0);
    check("a decoy can be acquired", id != RADAR_NO_CONTACT);
    if (id == RADAR_NO_CONTACT) {
        return;
    }
    c = radar_run_find(&run, id);
    check("an acquired decoy says so", c && c->classified &&
          !radar_class_is_target((enum radar_class)c->cls));
    check("an armed decoy is worth nothing", radar_run_engage_value(&run) == 0);
    integrity_before = run.score.integrity;
    mistakes_before = run.score.mistakes;
    radar_run_clear_events(&run);
    check("engaging a decoy is a foul", radar_run_engage(&run) == RADAR_ENGAGE_FOUL);
    check("the decoy is off the scope", radar_run_find(&run, id) == NULL);
    check("a foul is counted", run.score.mistakes == mistakes_before + 1);
    check("a foul breaks the streak", run.score.streak == 0);
    check("a foul costs sector integrity",
          run.score.integrity == integrity_before - RADAR_INTEGRITY_FOUL);

    reported = 0;
    while (radar_run_take_event(&run, &ev)) {
        if (ev.type == RADAR_EVENT_FOUL && ev.id == id) {
            reported = ev.value < 0;
        }
    }
    check("the foul is reported with its cost", reported);
}

static void test_difficulty_table(void)
{
    struct radar_level p;
    struct radar_level prev;
    struct radar_level low;
    struct radar_level high;
    struct radar_level first;
    struct radar_level last;
    int sums = 1;
    int faster = 1;
    int shorter = 1;
    int driftier = 1;
    int decoyer = 1;
    int roomier = 1;
    int level;

    radar_level_params(0, &p);
    check("level 0 is the rate the run opens at",
          p.spawn_ticks == RADAR_SPAWN_TICKS_BASE &&
          p.contacts_max == RADAR_CONTACTS_BASE &&
          p.ttl_pct == 100 && p.drift_dd == RADAR_DRIFT_BASE &&
          p.pct[RADAR_CLASS_DECOY] == RADAR_DECOY_PCT_BASE &&
          p.pct[RADAR_CLASS_HIGH_VALUE] == RADAR_HIGH_VALUE_PCT_BASE &&
          p.pct[RADAR_CLASS_FAST] == RADAR_FAST_PCT_BASE);

    prev = p;
    for (level = 0; level <= RADAR_LEVEL_MAX; level++) {
        int sum = 0;
        int i;

        radar_level_params(level, &p);
        for (i = 0; i < RADAR_CLASS_COUNT; i++) {
            sum += p.pct[i];
        }
        sums &= sum == 100;
        if (level > 0) {
            faster &= p.spawn_ticks < prev.spawn_ticks;
            shorter &= p.ttl_pct < prev.ttl_pct;
            driftier &= p.drift_dd > prev.drift_dd;
            decoyer &= p.pct[RADAR_CLASS_DECOY] > prev.pct[RADAR_CLASS_DECOY];
            roomier &= p.contacts_max >= prev.contacts_max;
        }
        prev = p;
    }
    check("the spawn shares always sum to 100", sums);
    check("contacts arrive faster at every step", faster);
    check("tracks live less long at every step", shorter);
    check("tracks drift harder at every step", driftier);
    check("decoys grow more common at every step", decoyer);
    check("the scope never holds fewer", roomier);

    radar_level_params(RADAR_LEVEL_MAX, &p);
    check("the hardest level still leaves a track worth working",
          p.ttl_pct > 0 && p.spawn_ticks > 0);
    check("the hardest level fits the scope",
          p.contacts_max <= RADAR_CONTACTS_MAX);
    check("normal targets never disappear entirely",
          p.pct[RADAR_CLASS_NORMAL] > 0);
    /* The whole reason the ramp ends a run: above this line contacts arrive
     * faster than the acquisition time, so nobody can hold the sector. */
    check("the hardest level outruns even a perfect operator",
          p.spawn_ticks < RADAR_ACQUIRE_TICKS);

    radar_level_params(-5, &low);
    radar_level_params(0, &first);
    radar_level_params(RADAR_LEVEL_MAX + 100, &high);
    radar_level_params(RADAR_LEVEL_MAX, &last);
    check("a level below zero clamps to the first",
          low.spawn_ticks == first.spawn_ticks && low.ttl_pct == first.ttl_pct);
    check("a level above the ceiling clamps to the last",
          high.spawn_ticks == last.spawn_ticks && high.ttl_pct == last.ttl_pct);
    radar_level_params(0, NULL);
}

static void test_progression(void)
{
    struct radar_run run;
    struct radar_level p;
    struct radar_event ev;
    int announced = 0;
    int gaps_match = 1;
    int seen = 0;
    uint32_t last = 0;
    int shorter_tracks = 1;
    uint16_t opening_ttl = 0;
    int i;

    start(&run, 6060u);
    check("a run starts at level 0", radar_run_level(&run) == 0);
    radar_run_tick(&run);
    {
        const struct radar_contact *c = first_contact(&run);

        opening_ttl = c ? c->ttl_max : 0;
    }
    radar_run_clear_events(&run);

    for (i = 2; i < RADAR_LEVEL_TICKS; i++) {
        sustain(&run);
        radar_run_tick(&run);
        radar_run_clear_events(&run);
    }
    check("the level holds for the whole step",
          radar_run_level(&run) == 0 && run.ticks == RADAR_LEVEL_TICKS - 1);

    sustain(&run);
    radar_run_tick(&run);
    check("the level steps up on schedule", radar_run_level(&run) == 1);
    while (radar_run_take_event(&run, &ev)) {
        if (ev.type == RADAR_EVENT_LEVEL) {
            announced = ev.value == 1;
        }
    }
    check("a step up is announced with its new level", announced);

    /* Run to the ceiling and past it. */
    while (run.ticks < (uint32_t)RADAR_LEVEL_TICKS * (RADAR_LEVEL_MAX + 3)) {
        sustain(&run);
        radar_run_tick(&run);
        radar_run_clear_events(&run);
    }
    check("the ramp stops at its ceiling", radar_run_level(&run) == RADAR_LEVEL_MAX);

    /* At the top, spawns really do arrive at the stated interval, and the
     * tracks really are shorter-lived than the opening ones. A gap that is
     * a multiple of the interval is a spawn the full scope refused, which
     * is the rule and not a missed beat. */
    radar_level_params(RADAR_LEVEL_MAX, &p);
    for (i = 0; i < 40 * p.spawn_ticks; i++) {
        sustain(&run);
        radar_run_tick(&run);
        while (radar_run_take_event(&run, &ev)) {
            if (ev.type != RADAR_EVENT_SPAWN) {
                continue;
            }
            if (last != 0) {
                gaps_match &= (run.ticks - last) % p.spawn_ticks == 0;
            }
            last = run.ticks;
            seen++;
        }
        {
            int s;

            for (s = 0; s < RADAR_CONTACTS_MAX; s++) {
                const struct radar_contact *c = radar_run_slot(&run, s);

                if (c->active && c->age == 0 && opening_ttl > 0) {
                    shorter_tracks &= c->ttl_max < opening_ttl;
                }
            }
        }
    }
    check("spawns arrive at the interval the table states", gaps_match && seen > 10);
    check("tracks at the ceiling are shorter-lived than at the opening",
          shorter_tracks && seen > 10);
}

static void test_game_over(void)
{
    struct radar_run run;
    struct radar_event ev;
    uint32_t before;
    int over_reported = 0;
    int32_t final_score = -1;
    int guard;

    /* Left alone, a run ends: every valid target that fades costs sector
     * integrity, and five of them are all it takes. */
    start(&run, 2468u);
    for (guard = 0; guard < 40000 && !radar_run_is_over(&run); guard++) {
        radar_run_tick(&run);
        while (radar_run_take_event(&run, &ev)) {
            if (ev.type == RADAR_EVENT_OVER) {
                over_reported = 1;
                final_score = ev.value;
            }
        }
    }
    check("an unattended run ends", radar_run_is_over(&run));
    check("the end is announced once", over_reported);
    check("the end carries the final score", final_score == run.score.points);
    check("a run ends when the sector is spent", run.score.integrity == 0);
    check("it took exactly the leakers the constants allow",
          run.score.missed == RADAR_INTEGRITY_MAX / RADAR_INTEGRITY_MISS);
    check("a decoy that faded was free", run.score.mistakes == 0);

    /* Nothing works afterwards and nothing changes. A run that kept
     * counting leakers after reporting its own final score would make the
     * result screen disagree with the event that produced it. */
    before = digest(&run);
    radar_run_tick(&run);
    radar_run_tick(&run);
    check("a finished run does not tick", digest(&run) == before);
    check("a finished run cannot select", radar_run_select(&run, 1u) == -1);
    check("a finished run cannot engage",
          radar_run_engage(&run) == RADAR_ENGAGE_INVALID);
    check("a finished run arms nothing", radar_run_engage_value(&run) == 0);
    check("a finished run cannot be restarted", radar_run_start(&run) == -1);
    check("a finished run holds nothing selected",
          run.selected == RADAR_NO_CONTACT);
    check("none of that changed anything", digest(&run) == before);
    check("the final score survived", run.score.points == final_score);
}

/* However a run ends, it must not leave a track on the scope still holding
 * a lock. A run ends on the fade of some valid target, and that is very
 * often NOT the contact the player was working: the operator is behind,
 * which is precisely why a track leaked. A UI that walks the slots to paint
 * the final scope would then draw a full acquisition ring on a contact that
 * radar_run_engage() will refuse for ever.
 *
 * The scenario cannot be built by hand from one seed, because whether the
 * fading track is the selected one depends on the whole run, so it is swept
 * instead, with an operator that always holds a selection and never fires. */
static void test_game_over_leaves_no_lock_held(void)
{
    int clean = 1;
    int ended_holding = 0;
    uint32_t seed;

    for (seed = 1u; seed <= 24u; seed++) {
        struct radar_run run;
        int guard;
        int i;

        start(&run, seed * 37u + 5u);
        for (guard = 0; guard < 20000 && !radar_run_is_over(&run); guard++) {
            if (!radar_run_selected(&run)) {
                for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
                    const struct radar_contact *t = radar_run_slot(&run, i);

                    if (t->active) {
                        radar_run_select(&run, t->id);
                        break;
                    }
                }
            }
            /* Record that the sweep actually reached the state that matters:
             * a lock held with the sector one leaker from the end. */
            if (radar_run_selected(&run) &&
                run.score.integrity <= RADAR_INTEGRITY_MISS) {
                ended_holding = 1;
            }
            radar_run_tick(&run);
            radar_run_clear_events(&run);
        }
        for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
            const struct radar_contact *t = radar_run_slot(&run, i);

            if (t->active && t->state != RADAR_CONTACT_NEW) {
                printf("     seed %u: contact %u left %s with lock %d\n",
                       (unsigned)(seed * 37u + 5u), t->id,
                       radar_contact_state_name((enum radar_contact_state)t->state),
                       radar_contact_lock_permille(t));
                clean = 0;
            }
        }
        clean &= run.selected == RADAR_NO_CONTACT;
    }
    check("the sweep reached runs that ended while a track was being worked",
          ended_holding);
    check("a finished run leaves no track holding a lock", clean);
}

/* A simulated operator. It works the most urgent contact it has not
 * identified, engages what turns out to be a target and leaves what turns
 * out to be a decoy. reaction_ticks is the pause before it reaches for the
 * next one: 0 is a machine, 10 (500 ms) is roughly a person on a good day.
 * Returns the tick the run ended on, or 0 if it never did. */
static uint32_t play(uint32_t seed, int reaction_ticks)
{
    struct radar_run run;
    int idle = 0;
    int guard;

    start(&run, seed);
    for (guard = 0; guard < 60000 && !radar_run_is_over(&run); guard++) {
        const struct radar_contact *sel = radar_run_selected(&run);

        if (sel && sel->state == RADAR_CONTACT_ACQUIRED) {
            if (radar_class_is_target((enum radar_class)sel->cls)) {
                radar_run_engage(&run);
            } else {
                radar_run_deselect(&run);
            }
            idle = reaction_ticks;
        } else if (!sel) {
            if (idle > 0) {
                idle--;
            } else {
                uint32_t best = RADAR_NO_CONTACT;
                int urgency = 0;
                int i;

                for (i = 0; i < RADAR_CONTACTS_MAX; i++) {
                    const struct radar_contact *t = radar_run_slot(&run, i);

                    if (!t->active || t->classified) {
                        continue;
                    }
                    if (best == RADAR_NO_CONTACT || t->ttl < urgency) {
                        best = t->id;
                        urgency = t->ttl;
                    }
                }
                if (best != RADAR_NO_CONTACT) {
                    radar_run_select(&run, best);
                }
            }
        }
        radar_run_tick(&run);
        radar_run_clear_events(&run);
    }
    return radar_run_is_over(&run) ? run.ticks : 0u;
}

static void test_pacing(void)
{
    static const uint32_t seeds[5] = { 11u, 202u, 3003u, 40004u, 500005u };
    uint32_t machine;
    int in_band = 1;
    int i;

    /* The claim the difficulty model has to support: a competent operator
     * gets a two-to-five minute round. The band below is wide because this
     * is a model of a player, not a player; what it really asserts is that
     * the ramp neither ends a run in seconds nor lets one go on for ever.
     * A human is worse than this at tapping a drifting marker, so real
     * rounds land at the shorter end. */
    for (i = 0; i < 5; i++) {
        uint32_t ticks = play(seeds[i], 10);
        int seconds = (int)(ticks * RADAR_TICK_MS / 1000);

        printf("     seed %u: an operator with a 500 ms reaction lasted %d s\n",
               seeds[i], seconds);
        in_band &= ticks > 0 && seconds >= 90 && seconds <= 420;
    }
    check("a modelled operator gets a round of the intended length", in_band);

    /* And the ceiling really does bite: even an operator with no reaction
     * time at all loses, because above RADAR_LEVEL_MAX contacts arrive
     * faster than acquisition can possibly service them. */
    machine = play(seeds[0], 0);
    check("even a perfect operator loses eventually", machine > 0);
    printf("     a perfect operator lasted %d s\n",
           (int)(machine * RADAR_TICK_MS / 1000));
    check("a perfect operator reaches the difficulty ceiling",
          machine >= (uint32_t)RADAR_LEVEL_TICKS * RADAR_LEVEL_MAX);
}

int main(void)
{
    test_lifecycle();
    test_spawning();
    test_lifetime_and_motion();
    test_selection_and_acquisition();
    test_selection_survives_a_fade();
    test_picking();
    test_determinism();
    test_events();
    test_sweep();
    test_engagement();
    test_difficulty_table();
    test_progression();
    test_game_over();
    test_game_over_leaves_no_lock_held();
    test_pacing();

    printf("radar_rules_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
