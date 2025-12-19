#include "ui.h"
#include "config.h"
#include <stdint.h>

#pragma once

typedef struct {
    uint64_t magic; // EPASCONF
    uint32_t version;
    int brightness;
    sw_interval_t switch_interval;
    sw_mode_t switch_mode;
} settings_t;

void settings_init(settings_t *settings);
void settings_save(settings_t *settings);