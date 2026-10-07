/*
 * Just enough of libgpiod v2 to compile ui/shell/kbd_bus_k230.c on a host
 * that has no libgpiod, so the bit-banged I2C can be driven with GPIO
 * failures injected (tests/kbd_bus_k230_test.c, cold review F6).
 *
 * This is a fake, and a fake that disagrees with the real header would test
 * the wrong thing. The values that matter are the three line states, because
 * the whole finding is that GPIOD_LINE_VALUE_ERROR must not be folded into
 * GPIOD_LINE_VALUE_INACTIVE; tests/kbd_lint.sh checks these three against
 * vendor/libgpiod/include/gpiod.h wherever that is present, and says so
 * loudly when it is not, rather than letting the fake drift unnoticed.
 *
 * Copyright (c) 2026 PocketOS authors.
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef POCKETOS_FAKE_GPIOD_H
#define POCKETOS_FAKE_GPIOD_H

#include <stddef.h>

enum gpiod_line_value {
    GPIOD_LINE_VALUE_ERROR = -1,
    GPIOD_LINE_VALUE_INACTIVE = 0,
    GPIOD_LINE_VALUE_ACTIVE = 1
};

enum gpiod_line_direction {
    GPIOD_LINE_DIRECTION_AS_IS = 1,
    GPIOD_LINE_DIRECTION_INPUT,
    GPIOD_LINE_DIRECTION_OUTPUT
};

enum gpiod_line_drive {
    GPIOD_LINE_DRIVE_PUSH_PULL = 1,
    GPIOD_LINE_DRIVE_OPEN_DRAIN,
    GPIOD_LINE_DRIVE_OPEN_SOURCE
};

enum gpiod_line_bias {
    GPIOD_LINE_BIAS_AS_IS = 1,
    GPIOD_LINE_BIAS_UNKNOWN,
    GPIOD_LINE_BIAS_DISABLED,
    GPIOD_LINE_BIAS_PULL_UP,
    GPIOD_LINE_BIAS_PULL_DOWN
};

struct gpiod_chip;
struct gpiod_line_request;
struct gpiod_line_settings;
struct gpiod_line_config;
struct gpiod_request_config;

struct gpiod_chip *gpiod_chip_open(const char *path);
void gpiod_chip_close(struct gpiod_chip *chip);
struct gpiod_line_request *gpiod_chip_request_lines(struct gpiod_chip *chip,
                                                    struct gpiod_request_config *req_cfg,
                                                    struct gpiod_line_config *line_cfg);

struct gpiod_line_settings *gpiod_line_settings_new(void);
void gpiod_line_settings_free(struct gpiod_line_settings *settings);
int gpiod_line_settings_set_direction(struct gpiod_line_settings *settings,
                                      enum gpiod_line_direction direction);
int gpiod_line_settings_set_drive(struct gpiod_line_settings *settings,
                                  enum gpiod_line_drive drive);
int gpiod_line_settings_set_bias(struct gpiod_line_settings *settings,
                                 enum gpiod_line_bias bias);
int gpiod_line_settings_set_output_value(struct gpiod_line_settings *settings,
                                         enum gpiod_line_value value);

struct gpiod_line_config *gpiod_line_config_new(void);
void gpiod_line_config_free(struct gpiod_line_config *config);
int gpiod_line_config_add_line_settings(struct gpiod_line_config *config,
                                        const unsigned int *offsets, size_t num_offsets,
                                        struct gpiod_line_settings *settings);

struct gpiod_request_config *gpiod_request_config_new(void);
void gpiod_request_config_free(struct gpiod_request_config *config);
void gpiod_request_config_set_consumer(struct gpiod_request_config *config,
                                       const char *consumer);

enum gpiod_line_value gpiod_line_request_get_value(struct gpiod_line_request *request,
                                                   unsigned int offset);
int gpiod_line_request_set_value(struct gpiod_line_request *request,
                                 unsigned int offset, enum gpiod_line_value value);
void gpiod_line_request_release(struct gpiod_line_request *request);

#endif
