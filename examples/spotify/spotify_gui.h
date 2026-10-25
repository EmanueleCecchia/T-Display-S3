#pragma once

#include "spotify_api.h"

/* Builds the now-playing screen. It pulls updates from the Spotify task by itself. */
void ui_begin(void);
/* Highlights the on-screen control matching a button press. */
void ui_button_feedback(spotify_cmd_t cmd);
