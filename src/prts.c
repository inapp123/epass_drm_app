#include "prts.h"
#include "config.h"
#include "ui.h"
#include <dirent.h> 
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "log.h"

extern long long get_now_us(void);

void prts_init(prts_t *prts,ui_t *ui,mediaplayer_t *mp){
    prts->ui = ui;
    prts->mp = mp;
    prts->status = PRTS_STATUS_IDLE;
    prts->current_operator_index = 0;
    prts->last_change_time = 0;
    prts->change_start_time = 0;
    memset(prts->operator_entries, 0, sizeof(prts->operator_entries));
    prts->operator_entries_count = 0;
    prts->sw_interval = SW_INTERVAL_5MIN;
    prts->sw_mode = SW_MODE_SEQUENCE;
}

static operator_entry_t* load_operator_entry(char *path){
    char pathbuf[128];

    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, PRTS_LOOP_VIDEO_FILENAME);
    if(access(pathbuf, F_OK) != 0){
        log_error("loop video file not found: %s", pathbuf);
        return NULL;
    }

    operator_entry_t* entry = malloc(sizeof(operator_entry_t));

    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, PRTS_OVERLAY_FILENAME);
    if(access(pathbuf, F_OK) == 0){
        log_info("overlay image file found: %s", pathbuf);
        entry->has_overlay_img = 1;
    }

    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, PRTS_INTRO_VIDEO_FILENAME);
    if(access(pathbuf, F_OK) == 0){
        log_info("intro video file found: %s", pathbuf);
        entry->has_intro_video = 1;
    }

    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, PRTS_INTRO_LOGO_FILENAME);
    if(access(pathbuf, F_OK) == 0){
        log_info("intro logo file found: %s", pathbuf);
        entry->has_intro_logo = 1;
    }

    snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, PRTS_CONFIG_FILENAME);
    if(access(pathbuf, F_OK) == 0){
        log_info("config file found: %s", pathbuf);
        FILE *f = fopen(pathbuf, "r");
        if(f == NULL){
            log_error("failed to open config file");
            return NULL;
        }
        fscanf(f, "%lld %x", &entry->intro_video_duration, &entry->background_color);
        log_info("config file: intro video duration: %lld, background color: %u", entry->intro_video_duration, entry->background_color);
        fclose(f);
    }
    else{
        entry->intro_video_duration = PRTS_FALLBACK_INTRO_VIDEO_DURATION;
        entry->background_color = 0xFFFFFFFF;
    }

    int pathlen = strlen(path);
    entry->path = malloc(pathlen + 1);
    strncpy(entry->path, path, pathlen);
    entry->path[pathlen] = '\0';

    return entry;
}

void prts_scan_assets(prts_t *prts){
    char path[128];
    DIR *dir = opendir(PRTS_ASSET_PATH);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL){
        if(entry->d_type == DT_DIR){
            snprintf(path, sizeof(path), "%s/%s", PRTS_ASSET_PATH, entry->d_name);
            operator_entry_t* ret = load_operator_entry(path);
            if(ret != NULL){
                prts->operator_entries[prts->operator_entries_count++] = ret;
            }
        }
    }
    closedir(dir);
}

void prts_next_operator(prts_t *prts){
    if(prts->status != PRTS_STATUS_IDLE){
        return;
    }
    prts->status = PRTS_STATUS_NEXT_PENDING;
    prts->last_change_time = get_now_us();
}

void prts_prev_operator(prts_t *prts){
    if(prts->status != PRTS_STATUS_IDLE){
        return;
    }
    prts->status = PRTS_STATUS_PREV_PENDING;
    prts->last_change_time = get_now_us();
}

extern void prts_transition_play_loop_middle_cb();
extern void prts_transition_play_intro_middle_cb();

void prts_tick(prts_t *prts){
    char pathbuf[128];
    long long now = get_now_us();

    if(prts->status == PRTS_STATUS_INTRO_VIDEO){
        if(now - prts->change_start_time > prts->operator_entries[prts->current_operator_index]->intro_video_duration){
            prts->status = PRTS_STATUS_LOOP_START;
            ui_add_transition_middle_cb(prts->ui, prts_transition_play_loop_middle_cb);
            ui_start_transition(prts->ui, TRANSITION_FILL_LEFT_RIGHT);
            prts->last_change_time = now;
        }
        return;
    }

    if(prts->status == PRTS_STATUS_LOOP_START){
        log_info("loop start");
        if(prts->operator_entries[prts->current_operator_index]->has_overlay_img){
            if(now - prts->last_change_time < PRTS_OVERLAY_DISPLAY_OFFSET){
                return;
            }
            snprintf(pathbuf, sizeof(pathbuf), "%s/%s", prts->operator_entries[prts->current_operator_index]->path, PRTS_OVERLAY_FILENAME);
            log_info("overlay image set to: %s", pathbuf);
            ui_set_operator_info_path(prts->ui, pathbuf);
            ui_start_transition(prts->ui, TRANSITION_OPERATOR_INFO);
        }
        else{
            log_info("no overlay image found");
            ui_set_operator_info_path(prts->ui, "");
        }
        prts->status = PRTS_STATUS_IDLE;
        prts->last_change_time = now;
        return;
    }
    
    int prts_triggered = 0;
    if(prts->status == PRTS_STATUS_NEXT_PENDING){
        prts->current_operator_index++;
        if(prts->current_operator_index >= prts->operator_entries_count){
            prts->current_operator_index = 0;
        }
        prts_triggered = 1;
    }
    else if(prts->status == PRTS_STATUS_PREV_PENDING){
        prts->current_operator_index--;
        if(prts->current_operator_index < 0){
            prts->current_operator_index = prts->operator_entries_count - 1;
        }
        prts_triggered = 1;
    }
    if(prts->sw_mode != SW_MODE_MANUAL){
        long long swint;
        switch (prts->sw_interval){
            case SW_INTERVAL_1MIN:
                swint = 60 * 1000 * 1000;
                break;
            case SW_INTERVAL_3MIN:
                swint = 3 * 60 * 1000 * 1000;
                break;
            case SW_INTERVAL_5MIN:
                swint = 5 * 60 * 1000 * 1000;
                break;
            case SW_INTERVAL_10MIN:
                swint = 10 * 60 * 1000 * 1000;
                break;
        }
        if(now - prts->last_change_time > swint){
            if(prts->sw_mode == SW_MODE_RANDOM){
                int random_index = rand() % prts->operator_entries_count;
                while(random_index == prts->current_operator_index){
                    random_index = rand() % prts->operator_entries_count;
                }
                prts->current_operator_index = random_index;
            }
            else{
                prts->current_operator_index++;
                if(prts->current_operator_index >= prts->operator_entries_count){
                    prts->current_operator_index = 0;
                }
            }
            prts_triggered = 1;
        }
    }

    if(!prts_triggered){
        return;
    }

    prts->change_start_time = now;
    operator_entry_t* entry = prts->operator_entries[prts->current_operator_index];
    ui_set_transition_bg_color(prts->ui, entry->background_color);
    if(entry->has_intro_logo){
        snprintf(pathbuf, sizeof(pathbuf), "%s/%s", entry->path, PRTS_INTRO_LOGO_FILENAME);
        log_info("transition image set to: %s", pathbuf);
        ui_set_transition_bitmap_path(prts->ui, pathbuf);
    }
    else{
        ui_set_transition_bitmap_path(prts->ui, PRTS_FALLBACK_INTRO_LOGO_PATH);
    }

    if(entry->has_intro_video){
        prts->status = PRTS_STATUS_INTRO_VIDEO;
        ui_add_transition_middle_cb(prts->ui, prts_transition_play_intro_middle_cb);
    }
    else{
        prts->status = PRTS_STATUS_LOOP_START;
        ui_add_transition_middle_cb(prts->ui, prts_transition_play_loop_middle_cb);
    }

    ui_start_transition(prts->ui, TRANSITION_FILL_TOP_BOTTOM);

}