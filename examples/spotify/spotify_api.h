#pragma once

#include "Arduino.h"
#include "lvgl.h"

/* Album art is decoded and center-cropped to SPOTIFY_ART_SIZE x SPOTIFY_ART_SIZE */
#define SPOTIFY_ART_SIZE 150

typedef enum {
    SPOTIFY_CONNECTING,     /* waiting for WiFi / first answer from Spotify */
    SPOTIFY_NOT_CONFIGURED, /* secrets.h has no Spotify credentials */
    SPOTIFY_LOGIN_NEEDED,   /* refresh token rejected: run tools/spotify_auth.py again */
    SPOTIFY_ERROR,          /* network or API error, details in artist */
    SPOTIFY_IDLE,           /* nothing is playing on any device */
    SPOTIFY_ACTIVE,         /* a track or episode is loaded (playing or paused) */
} spotify_status_t;

typedef enum {
    SPOTIFY_REPEAT_OFF,
    SPOTIFY_REPEAT_CONTEXT, /* repeat the album/playlist */
    SPOTIFY_REPEAT_TRACK,
} spotify_repeat_t;

typedef enum {
    SPOTIFY_CMD_PREVIOUS,
    SPOTIFY_CMD_NEXT,
    SPOTIFY_CMD_PLAY_PAUSE,
} spotify_cmd_t;

typedef struct {
    spotify_status_t status;
    bool is_playing;
    char title[96];  /* track/episode name, or a status message */
    char artist[96]; /* artists/show name, or status details */
    char device[48];
    char up_next[128]; /* "Title - Artist" of the next queued item, or a hint */
    char notice[64];   /* short-lived message, e.g. why a button press failed */
    bool shuffle;
    spotify_repeat_t repeat;
    int8_t volume; /* 0..100, -1 when the device doesn't report it */
    uint32_t progress_ms;
    uint32_t duration_ms;
    uint32_t sampled_at; /* millis() at which progress_ms was valid */
    uint32_t version;    /* changes whenever anything above changes */
    uint32_t notice_version;
    uint32_t art_version; /* changes when the album art changes */
    bool has_art;
} spotify_state_t;

/* Starts the Spotify task (core 0). It waits for WiFi by itself. */
void spotify_begin(void);
/* Copies the latest state into *state if it is newer than state->version. */
bool spotify_get_state(spotify_state_t *state);
/* Copies the album art (RGB565, SPOTIFY_ART_SIZE^2) into dst if newer than *art_version. */
bool spotify_get_art(lv_color_t *dst, uint32_t *art_version);
/* Queues a playback command; never blocks. */
void spotify_send(spotify_cmd_t cmd);
