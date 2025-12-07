#pragma once

#include "drm_warpper.h"
#include "mediaplayer.h"
#include "ui.h"
#include "config.h"

typedef struct {
    char* path;
    int has_overlay_img;
    int has_intro_video;
    int has_intro_logo;
    long long intro_video_duration;
    uint32_t background_color;
} operator_entry_t;

typedef enum {
    PRTS_STATUS_IDLE,
    PRTS_STATUS_NEXT_PENDING,
    PRTS_STATUS_PREV_PENDING,
    PRTS_STATUS_INTRO_VIDEO,
    PRTS_STATUS_LOOP_START,
} prts_status_t;

typedef struct {
    prts_status_t status;
    ui_t *ui;
    mediaplayer_t *mp;
    sw_interval_t sw_interval;
    sw_mode_t sw_mode;
    operator_entry_t* operator_entries[PRTS_MAX_OPERATORS];
    int operator_entries_count;
    int current_operator_index;
    long long last_change_time;
    long long change_start_time;
} prts_t;

void prts_init(prts_t *prts,ui_t *ui,mediaplayer_t *mp);
void prts_scan_assets(prts_t *prts);
void prts_next_operator(prts_t *prts);
void prts_prev_operator(prts_t *prts);
void prts_tick(prts_t *prts);
