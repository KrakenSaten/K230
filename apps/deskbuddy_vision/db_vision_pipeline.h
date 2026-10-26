/*
 * DeskBuddy's provider on the Vision pipeline: the one place where
 * DeskBuddy's boundary (apps/deskbuddy/db_vision.h) and Vision's helper
 * client (apps/vision/vision_session.h) meet. DeskBuddy includes nothing of
 * Vision and Vision knows nothing of DeskBuddy (tests/deskbuddy_lint.sh);
 * this directory is the bridge between them, and nothing else is.
 *
 * The provider runs the Vision helper (pos-vision, ADR-006: its own
 * process, the camera and the KPU in it) with no picture on screen, in the
 * best mode the unit offers:
 *
 *   RECOGNIZE  faces, and whether one is the owner enrolled in Vision
 *              (the helper's owner.v1): OWNER_RECOGNIZED / UNKNOWN_PERSON
 *              with the similarity as confidence;
 *   FACE       faces only: PERSON_DETECTED;
 *   DETECT     the object detector's people (the class named "person"):
 *              PERSON_DETECTED, never an identity.
 *
 * The detector is the one Vision would open (vision_store.h: R0 on every
 * image from 0.3.6, UPSTREAM only by hand, Vision's last confirmed choice
 * first), named to the helper; with neither file on the unit the helper
 * gets no model and offers no DETECT.
 *
 * and turns what the helper says into DeskBuddy's conclusions - changes
 * only - with the judge below. Every call returns at once; the helper is
 * polled, never waited on (but for stop()'s bounded grace, as Vision's own
 * close). No picture, face or number but the conclusion crosses into
 * DeskBuddy.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef DB_VISION_PIPELINE_H
#define DB_VISION_PIPELINE_H

#include "db_vision.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- the judge: reports into conclusions -------------------------------- *
 *
 * Pure: fed one report per helper line that says who is there, the clock
 * passed in. A new conclusion is said once it has held for
 * DB_JUDGE_HOLD reports in a row (a face lost for one frame, or a score on
 * the line for one round, says nothing); "nobody" once nobody has been
 * seen for DB_JUDGE_ABSENT_MS, confirmed by a new empty report.
 * Silence uses Vision's existing liveness deadline and means unavailable. */
#define DB_JUDGE_HOLD 2
#define DB_JUDGE_ABSENT_MS 1500

struct db_judge_report {
    int present;        /* faces, or people, seen */
    bool identity;      /* the mode can tell the owner (RECOGNIZE with an owner enrolled) */
    bool owner;         /* one of them is the owner */
    bool scored;        /* at least one was compared */
    int16_t owner_pm;   /* the owner's similarity, per-mille */
    int16_t other_pm;   /* the best other's, per-mille */
};

struct db_judge {
    bool said_any;
    enum db_vision_kind said;
    enum db_vision_kind cand;
    int cand_n;
    int64_t last_seen_ms;
    int64_t last_report_ms;
    bool face;          /* the reports are about faces */
};

void db_judge_init(struct db_judge *j, bool face);
/* One report. true with *out filled when a conclusion is to be said. */
bool db_judge_report(struct db_judge *j, const struct db_judge_report *r, int64_t now_ms,
                     struct db_vision_event *out);
/* No report for Vision's silence interval: unavailable, never nobody. */
bool db_judge_tick(struct db_judge *j, int64_t now_ms, struct db_vision_event *out);

/* ---- what the provider asks of Vision (exposed for the test) ------------ */

/* The detector start() names to the helper (enum vision_detector), or -1
 * when neither file is on the unit. */
int db_vision_pipeline_detector(void);
/* Whether a detector class is a person: by its name, never by its index. */
bool db_vision_pipeline_is_person(uint32_t cls);

#endif
