/*
 * PocketRadar rules test: the run lifecycle, spawning, movement, track
 * lifetime, selection, acquisition, touch picking and the event queue.
 *
 * The central assertions are that a seed reproduces a run exactly, that the
 * spawn stream depends on the tick count and not on how the player plays,
 * and that every call refuses a state the rules could not have produced
 * rather than doing something undefined with it.
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
          radar_run_take_event(NULL, &ev) == 0);
    radar_run_tick(NULL);
    radar_run_new(NULL, 1u);
    radar_run_deselect(NULL);
    radar_run_clear_events(NULL);
}

static void test_spawning(void)
{
    struct radar_run run;
    int on_schedule = 1;
    int classes[RADAR_CLASS_COUNT];
    int in_band = 1;
    int drift_ok = 1;
    int ids_unique = 1;
    int i;

    memset(classes, 0, sizeof(classes));
    start(&run, 4242u);

    /* Spawns arrive on the fixed schedule: one on tick 1, then one every
     * RADAR_SPAWN_TICKS_BASE. Nothing else may add a track. */
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
    check("the scope holds no more than the baseline limit",
          radar_run_contact_count(&run) <= RADAR_CONTACTS_BASE);

    /* Over a long run every class appears, every track starts in the outer
     * band with a bounded drift, and no id is ever repeated. */
    start(&run, 777u);
    {
        uint32_t seen[64];
        int seen_n = 0;

        for (i = 0; i < 20000; i++) {
            struct radar_event ev;

            radar_run_tick(&run);
            while (radar_run_take_event(&run, &ev)) {
                if (ev.type != RADAR_EVENT_SPAWN) {
                    continue;
                }
                classes[ev.cls]++;
                if (seen_n < 64) {
                    int j;

                    for (j = 0; j < seen_n; j++) {
                        ids_unique &= seen[j] != ev.id;
                    }
                    seen[seen_n++] = ev.id;
                }
            }
            {
                int s;

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
    check("normal contacts are the commonest",
          classes[RADAR_CLASS_NORMAL] > classes[RADAR_CLASS_DECOY] &&
          classes[RADAR_CLASS_NORMAL] > classes[RADAR_CLASS_FAST]);
    check("high-value contacts are the rarest",
          classes[RADAR_CLASS_HIGH_VALUE] < classes[RADAR_CLASS_DECOY] &&
          classes[RADAR_CLASS_HIGH_VALUE] < classes[RADAR_CLASS_FAST]);
    check("a track starts in the outer band", in_band);
    check("drift stays inside the baseline", drift_ok);
    check("no contact id is reused", ids_unique);
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

    /* The spawn stream must not depend on how the player plays, or two
     * players on the same seed would not meet the same contacts. */
    {
        uint32_t quiet[32];
        uint32_t busy[32];
        int quiet_n = 0;
        int busy_n = 0;
        int same;

        start(&a, 8080u);
        start(&b, 8080u);
        for (i = 0; i < 1200; i++) {
            struct radar_event ev;

            radar_run_tick(&a);
            while (radar_run_take_event(&a, &ev)) {
                if (ev.type == RADAR_EVENT_SPAWN && quiet_n < 32) {
                    quiet[quiet_n++] = ((uint32_t)ev.cls << 16) | ev.bearing;
                }
            }
            radar_run_tick(&b);
            while (radar_run_take_event(&b, &ev)) {
                if (ev.type == RADAR_EVENT_SPAWN && busy_n < 32) {
                    busy[busy_n++] = ((uint32_t)ev.cls << 16) | ev.bearing;
                }
            }
            /* b is played hard: select, switch, deselect, every tick. */
            {
                const struct radar_contact *t = first_contact(&b);

                if (t) {
                    radar_run_select(&b, t->id);
                }
                if (i % 5 == 0) {
                    radar_run_deselect(&b);
                }
            }
        }
        same = quiet_n > 20 && quiet_n == busy_n &&
               memcmp(quiet, busy, (size_t)quiet_n * sizeof(quiet[0])) == 0;
        check("the spawn stream does not depend on how the run is played", same);
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
        radar_run_tick(&run);
        radar_run_clear_events(&run);
        bounded &= run.sweep < RADAR_BEARING_MAX;
    }
    check("the sweep stays inside one turn", bounded);
    check("the sweep turns once every four seconds",
          RADAR_SWEEP_DD_PER_TICK * (4000 / RADAR_TICK_MS) == RADAR_BEARING_MAX);
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

    printf("radar_rules_test: %d failure(s)\n", failed);
    return failed ? 1 : 0;
}
