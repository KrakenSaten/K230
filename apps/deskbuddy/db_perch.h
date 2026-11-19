/* Theme-coloured background for the live face. SPDX-License-Identifier: Apache-2.0 */
#ifndef DB_PERCH_H
#define DB_PERCH_H
#include "db_prefs.h"
#include "lvgl.h"

#define DB_PERCH_SIZE 256
struct db_perch {
    lv_image_dsc_t image;
    lv_obj_t *view;
    uint8_t pixels[DB_PERCH_SIZE * DB_PERCH_SIZE * 3];
    uint32_t bg, sky, coat;
    enum db_mode mode;
};
void db_perch_init(struct db_perch *p, lv_obj_t *parent);
void db_perch_tint(struct db_perch *p, enum db_mode mode);
void db_perch_release(struct db_perch *p);
#endif
