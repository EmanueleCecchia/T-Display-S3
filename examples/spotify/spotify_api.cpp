#include "spotify_api.h"
#include "HTTPClient.h"
#include "Preferences.h"
#include "WiFi.h"
#include "WiFiClientSecure.h"
#include "cJSON.h"
#include "esp32s3/rom/tjpgd.h"
#include "pin_config.h"
#include "spotify_certs.h"

// Written by tools/spotify_auth.py into secrets.h (Authorization Code with PKCE: no client secret)
#ifndef SPOTIFY_CLIENT_ID
#define SPOTIFY_CLIENT_ID     ""
#endif
#ifndef SPOTIFY_REFRESH_TOKEN
#define SPOTIFY_REFRESH_TOKEN ""
#endif

#define TOKEN_URL         "https://accounts.spotify.com/api/token"
#define PLAYER_URL        "https://api.spotify.com/v1/me/player"
#define POLL_PLAYING_MS   1500
#define POLL_IDLE_MS      5000
#define AFTER_COMMAND_MS  400              // Spotify needs a moment before the state shows a skip
#define QUEUE_REFRESH_MS  30000            // also catches songs added to the queue from the app
#define LOGIN_RETRY_MS    (10 * 60 * 1000)
#define BACKOFF_MIN_MS    2000
#define BACKOFF_MAX_MS    (2 * 60 * 1000)
#define MAX_ART_BYTES     (200 * 1024)
#define JPEG_WORK_SIZE    4096             // TJpgDec needs ~3.1 KB
#define ART_PIXELS        (SPOTIFY_ART_SIZE * SPOTIFY_ART_SIZE)
#define API_NO_TOKEN      (-100)

static SemaphoreHandle_t state_lock;
static QueueHandle_t cmd_queue;
static spotify_state_t shared_state; // guarded by state_lock
static lv_color_t *shared_art;       // guarded by state_lock

// Everything below belongs to the Spotify task.
static spotify_state_t cur;
static lv_color_t *decoded_art;
static String access_token;
static uint32_t token_expires_at;
static String refresh_token;
static String art_url;
static String track_uri;
static uint32_t queue_fetched_at;
static uint32_t backoff_ms;
static uint32_t retry_after_ms;
static uint8_t failures;
static WiFiClientSecure api_client; // kept alive between polls to skip the TLS handshake
static HTTPClient api_http;

/* ---------------------------------------------------------------- helpers */

static const char *json_str(const cJSON *obj, const char *key)
{
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
    return s ? s : "";
}

static int json_int(const cJSON *obj, const char *key, int def)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item) ? item->valueint : def;
}

// The built-in LVGL fonts only have ASCII glyphs: map accented Latin letters to their base
// letter (è -> e), typographic punctuation to ASCII and anything else to '?'.
static void to_ascii(char *dst, size_t size, const char *src)
{
    static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"  // U+00C0..U+00DF
                                 "aaaaaaaceeeeiiiidnooooo/ouuuuypy"; // U+00E0..U+00FF
    static const char latin_ext_a[] = "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh"
                                      "IiIiIiIiIiIi" "Jj" "Kkk" "LlLlLlLlLl" "NnNnNnnNn" "OoOoOoOo"
                                      "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu" "Ww" "YyY" "ZzZzZz" "s"; // U+0100..U+017F
    static_assert(sizeof(latin1) == 64 + 1, "latin1 table");
    static_assert(sizeof(latin_ext_a) == 128 + 1, "latin_ext_a table");

    const uint8_t *s = (const uint8_t *)src;
    size_t n = 0;
    while (*s && n + 1 < size) {
        uint32_t cp;
        int len;
        if (*s < 0x80) {
            cp = *s, len = 1;
        } else if ((*s & 0xE0) == 0xC0) {
            cp = *s & 0x1F, len = 2;
        } else if ((*s & 0xF0) == 0xE0) {
            cp = *s & 0x0F, len = 3;
        } else {
            cp = *s & 0x07, len = 4;
        }
        for (int i = 1; i < len; i++) {
            if ((s[i] & 0xC0) != 0x80) { // truncated sequence
                cp = '?', len = i;
                break;
            }
            cp = (cp << 6) | (s[i] & 0x3F);
        }
        s += len;

        char c;
        if (cp >= 0x20 && cp < 0x7F)
            c = cp;
        else if (cp >= 0xC0 && cp <= 0xFF)
            c = latin1[cp - 0xC0];
        else if (cp >= 0x100 && cp <= 0x17F)
            c = latin_ext_a[cp - 0x100];
        else if (cp == 0x2018 || cp == 0x2019 || cp == 0xB4)
            c = '\'';
        else if (cp == 0x201C || cp == 0x201D)
            c = '"';
        else if (cp >= 0x2010 && cp <= 0x2015)
            c = '-';
        else if (cp < 0x20 || cp == 0xA0)
            c = ' ';
        else
            c = '?';
        dst[n++] = c;
    }
    dst[n] = '\0';
}

// Spotify errors: Web API {"error": {"status", "message"}}, accounts {"error", "error_description"}
static String error_message(int code, const String &body)
{
    cJSON *root = cJSON_Parse(body.c_str());
    cJSON *err = cJSON_GetObjectItem(root, "error");
    String msg = cJSON_IsObject(err) ? json_str(err, "message") : json_str(root, "error_description");
    cJSON_Delete(root);
    msg.replace("Player command failed: ", "");
    if (msg.isEmpty())
        msg = code > 0 ? "HTTP " + String(code) : HTTPClient::errorToString(code);
    return msg;
}

// Tracks list their artists, episodes belong to a show
static String item_artists(const cJSON *item)
{
    String artists;
    const cJSON *artist;
    cJSON_ArrayForEach(artist, cJSON_GetObjectItem(item, "artists")) {
        if (artists.length())
            artists += ", ";
        artists += json_str(artist, "name");
    }
    if (artists.isEmpty())
        artists = json_str(cJSON_GetObjectItem(item, "show"), "name");
    return artists;
}

static uint32_t current_progress(void)
{
    uint32_t p = cur.progress_ms + (cur.is_playing ? millis() - cur.sampled_at : 0);
    return min(p, cur.duration_ms);
}

/* ---------------------------------------------------------------- shared state */

static void publish(void)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    uint32_t art_version = shared_state.art_version;
    bool has_art = shared_state.has_art;
    cur.version = shared_state.version + 1;
    shared_state = cur;
    shared_state.art_version = art_version;
    shared_state.has_art = has_art;
    xSemaphoreGive(state_lock);
}

static void publish_art(bool has_art)
{
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (has_art)
        memcpy(shared_art, decoded_art, ART_PIXELS * sizeof(lv_color_t));
    shared_state.has_art = has_art;
    shared_state.art_version++;
    shared_state.version++;
    xSemaphoreGive(state_lock);
}

static void set_status(spotify_status_t status, const char *title, const char *detail)
{
    if (cur.status == status && !strcmp(cur.title, title) && !strcmp(cur.artist, detail))
        return;
    cur.status = status;
    cur.is_playing = false;
    strlcpy(cur.title, title, sizeof(cur.title));
    strlcpy(cur.artist, detail, sizeof(cur.artist));
    cur.device[0] = '\0';
    cur.up_next[0] = '\0';
    cur.progress_ms = cur.duration_ms = 0;
    track_uri = "";
    publish();
    if (art_url.length()) {
        art_url = "";
        publish_art(false);
    }
}

static void notify(const String &msg)
{
    Serial.printf("[spotify] %s\n", msg.c_str());
    to_ascii(cur.notice, sizeof(cur.notice), msg.c_str());
    cur.notice_version++;
    publish();
}

/* ---------------------------------------------------------------- auth */

// Spotify rotates refresh tokens, so the latest one is kept in NVS. It is only used while
// secrets.h still holds the token it descends from: running spotify_auth.py again resets it.
static void load_refresh_token(void)
{
    Preferences prefs;
    if (prefs.begin("spotify", true)) {
        if (prefs.getString("seed") == SPOTIFY_REFRESH_TOKEN)
            refresh_token = prefs.getString("token");
        prefs.end();
    }
    if (refresh_token.isEmpty())
        refresh_token = SPOTIFY_REFRESH_TOKEN;
}

static void save_refresh_token(const char *token)
{
    refresh_token = token;
    Preferences prefs;
    prefs.begin("spotify", false);
    prefs.putString("seed", SPOTIFY_REFRESH_TOKEN);
    prefs.putString("token", refresh_token);
    prefs.end();
}

static bool refresh_access_token(String *error)
{
    WiFiClientSecure client;
    client.setCACert(spotify_root_ca);
    HTTPClient http;
    if (!http.begin(client, TOKEN_URL)) {
        *error = "Can't open " TOKEN_URL;
        return false;
    }
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    int code = http.POST("grant_type=refresh_token&client_id=" SPOTIFY_CLIENT_ID "&refresh_token=" + refresh_token);
    String body = code > 0 ? http.getString() : String();
    http.end();

    if (code != HTTP_CODE_OK) {
        *error = error_message(code, body);
        Serial.printf("[spotify] token refresh failed: %d %s\n", code, error->c_str());
        if (code == 400 || code == 401) { // invalid_grant: refresh token expired (6 months) or revoked
            String detail = *error + " - run tools/spotify_auth.py and flash again";
            set_status(SPOTIFY_LOGIN_NEEDED, "Spotify login needed", detail.c_str());
        }
        return false;
    }

    cJSON *root = cJSON_Parse(body.c_str());
    access_token = json_str(root, "access_token");
    token_expires_at = millis() + (json_int(root, "expires_in", 3600) - 60) * 1000;
    const char *rotated = json_str(root, "refresh_token");
    if (*rotated && refresh_token != rotated)
        save_refresh_token(rotated);
    cJSON_Delete(root);
    if (access_token.isEmpty()) {
        *error = "No access token in response";
        return false;
    }
    Serial.println("[spotify] access token refreshed");
    return true;
}

/* ---------------------------------------------------------------- Web API */

// Calls /v1/me/player{path}. Returns the HTTP status, a negative HTTPClient error, or API_NO_TOKEN.
static int api_call(const char *method, const char *path, String *body)
{
    static const char *collect[] = {"Retry-After"};
    retry_after_ms = 0;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (access_token.isEmpty() || (int32_t)(token_expires_at - millis()) <= 0) {
            if (!refresh_access_token(body))
                return API_NO_TOKEN;
        }
        if (!api_http.begin(api_client, String(PLAYER_URL) + path))
            return HTTPC_ERROR_CONNECTION_REFUSED;
        api_http.addHeader("Authorization", "Bearer " + access_token);
        if (strcmp(method, "GET") != 0)
            api_http.addHeader("Content-Length", "0"); // HTTPClient omits it for empty bodies
        api_http.collectHeaders(collect, 1);
        int code = api_http.sendRequest(method);
        // 204 has no body and no Content-Length: reading it would wait for the timeout
        *body = (code == HTTP_CODE_OK || code >= 400) ? api_http.getString() : String();
        retry_after_ms = api_http.header("Retry-After").toInt() * 1000;
        api_http.end();

        if (code == HTTP_CODE_UNAUTHORIZED && attempt == 0) {
            access_token = ""; // revoked early: refresh and retry once
            continue;
        }
        if (code < 0 && attempt == 0)
            continue; // the kept-alive connection was closed by the server: reconnect
        return code;
    }
    return HTTPC_ERROR_CONNECTION_LOST;
}

/* ---------------------------------------------------------------- album art */

struct jpeg_ctx {
    const uint8_t *data;
    size_t size;
    size_t pos;
    int crop_x, crop_y; // top-left of the art box inside the scaled image
};

static UINT jpeg_input(JDEC *jd, BYTE *buf, UINT len)
{
    jpeg_ctx *ctx = (jpeg_ctx *)jd->device;
    len = min((size_t)len, ctx->size - ctx->pos);
    if (buf)
        memcpy(buf, ctx->data + ctx->pos, len);
    ctx->pos += len;
    return len;
}

static UINT jpeg_output(JDEC *jd, void *bitmap, JRECT *rect)
{
    jpeg_ctx *ctx = (jpeg_ctx *)jd->device;
    const BYTE *rgb = (const BYTE *)bitmap;
    for (int y = rect->top; y <= rect->bottom; y++) {
        int ay = y - ctx->crop_y;
        for (int x = rect->left; x <= rect->right; x++, rgb += 3) {
            int ax = x - ctx->crop_x;
            if (ax >= 0 && ax < SPOTIFY_ART_SIZE && ay >= 0 && ay < SPOTIFY_ART_SIZE)
                decoded_art[ay * SPOTIFY_ART_SIZE + ax] = lv_color_make(rgb[0], rgb[1], rgb[2]);
        }
    }
    return 1;
}

static bool decode_jpeg(const uint8_t *data, size_t size)
{
    static uint8_t *work;
    if (!work)
        work = (uint8_t *)heap_caps_malloc(JPEG_WORK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    jpeg_ctx ctx = {data, size, 0, 0, 0};
    JDEC jd;
    JRESULT res = jd_prepare(&jd, jpeg_input, work, JPEG_WORK_SIZE, &ctx);
    if (res != JDR_OK) {
        Serial.printf("[spotify] jpeg prepare failed: %d\n", res);
        return false;
    }
    // Downscale by 1/2^n in the decoder as far as the image still covers the box, then crop.
    uint8_t scale = 0;
    while (scale < 3 && (jd.width >> (scale + 1)) >= SPOTIFY_ART_SIZE && (jd.height >> (scale + 1)) >= SPOTIFY_ART_SIZE)
        scale++;
    ctx.crop_x = ((int)(jd.width >> scale) - SPOTIFY_ART_SIZE) / 2;
    ctx.crop_y = ((int)(jd.height >> scale) - SPOTIFY_ART_SIZE) / 2;

    lv_color_t bg = lv_color_hex(0x282828);
    for (int i = 0; i < ART_PIXELS; i++)
        decoded_art[i] = bg;
    res = jd_decomp(&jd, jpeg_output, scale);
    if (res != JDR_OK) {
        Serial.printf("[spotify] jpeg decode failed: %d\n", res);
        return false;
    }
    return true;
}

static bool download(const String &url, uint8_t **data, size_t *size)
{
    WiFiClientSecure client;
    client.setCACert(spotify_root_ca);
    HTTPClient http;
    if (!http.begin(client, url))
        return false;
    int code = http.GET();
    int len = http.getSize();
    bool ok = false;
    if (code == HTTP_CODE_OK && len > 0 && len <= MAX_ART_BYTES) {
        uint8_t *buf = (uint8_t *)ps_malloc(len);
        WiFiClient *stream = http.getStreamPtr();
        size_t got = 0;
        uint32_t start = millis();
        while (buf && got < (size_t)len && millis() - start < 10000) {
            int n = stream->read(buf + got, len - got);
            if (n > 0)
                got += n;
            else if (!stream->connected())
                break;
            else
                delay(5);
        }
        ok = buf && got == (size_t)len;
        if (ok) {
            *data = buf;
            *size = len;
        } else {
            free(buf);
        }
    }
    if (!ok)
        Serial.printf("[spotify] art download failed: HTTP %d, %d bytes\n", code, len);
    http.end();
    return ok;
}

// Picks the smallest image that still covers the art box (albums come in 640, 300 and 64 px).
static String pick_image(const cJSON *images)
{
    String best, largest;
    int best_w = INT_MAX, largest_w = -1;
    const cJSON *img;
    cJSON_ArrayForEach(img, images) {
        int w = json_int(img, "width", 0);
        if (w >= SPOTIFY_ART_SIZE && w < best_w)
            best_w = w, best = json_str(img, "url");
        if (w > largest_w)
            largest_w = w, largest = json_str(img, "url");
    }
    return best.length() ? best : largest;
}

static void update_art(const String &url)
{
    uint8_t *jpg = NULL;
    size_t size = 0;
    bool ok = url.length() && download(url, &jpg, &size) && decode_jpeg(jpg, size);
    free(jpg);
    publish_art(ok);
}

// "Up next" is the first item of the queue; reading it needs the user-read-currently-playing scope.
static void update_queue(void)
{
    String body;
    int code = api_call("GET", "/queue", &body);
    queue_fetched_at = millis();
    String next;
    if (code == HTTP_CODE_OK) {
        cJSON *root = cJSON_Parse(body.c_str());
        next = json_str(cJSON_GetArrayItem(cJSON_GetObjectItem(root, "queue"), 0), "name");
        cJSON_Delete(root);
    } else if (code == HTTP_CODE_UNAUTHORIZED || code == HTTP_CODE_FORBIDDEN) {
        next = "Run tools/spotify_auth.py again to see the queue"; // token from before the scope was added
    } else {
        if (code == 429)
            backoff_ms = max(backoff_ms, retry_after_ms);
        return; // keep the previous one
    }
    char text[sizeof(cur.up_next)];
    to_ascii(text, sizeof(text), next.c_str());
    if (strcmp(text, cur.up_next) != 0) {
        Serial.printf("[spotify] up next: %s\n", text);
        strlcpy(cur.up_next, text, sizeof(cur.up_next));
        publish();
    }
}

/* ---------------------------------------------------------------- polling & commands */

// A poll failed: back off exponentially (honouring Retry-After) and show persistent errors.
static uint32_t poll_failed(int code, const String &msg)
{
    backoff_ms = backoff_ms ? min(backoff_ms * 2, (uint32_t)BACKOFF_MAX_MS) : BACKOFF_MIN_MS;
    uint32_t wait = max(backoff_ms, retry_after_ms);
    Serial.printf("[spotify] poll failed (%d): %s, retry in %u ms\n", code, msg.c_str(), wait);
    if (code == 429)
        notify("Rate limited, retry in " + String(wait / 1000) + " s");
    else if (++failures >= 3 || cur.status == SPOTIFY_CONNECTING)
        set_status(SPOTIFY_ERROR, "Can't reach Spotify", msg.c_str());
    return wait;
}

// Returns how long to wait before the next poll.
static uint32_t poll_player(void)
{
    String body;
    int code = api_call("GET", "?market=from_token&additional_types=episode", &body);
    if (code == API_NO_TOKEN)
        return cur.status == SPOTIFY_LOGIN_NEEDED ? LOGIN_RETRY_MS : poll_failed(code, body);
    if (code != HTTP_CODE_OK && code != HTTP_CODE_NO_CONTENT)
        return poll_failed(code, error_message(code, body));
    backoff_ms = 0;
    failures = 0;

    if (code == HTTP_CODE_NO_CONTENT) { // no active device
        set_status(SPOTIFY_IDLE, "Nothing playing", "Start Spotify on any device");
        return POLL_IDLE_MS;
    }

    cJSON *root = cJSON_Parse(body.c_str());
    const cJSON *item = cJSON_GetObjectItem(root, "item");
    if (!cJSON_IsObject(item)) { // ads and unknown content have no item
        bool ad = !strcmp(json_str(root, "currently_playing_type"), "ad");
        cJSON_Delete(root);
        set_status(SPOTIFY_IDLE, ad ? "Advertisement" : "Nothing playing", ad ? "Spotify" : "Start Spotify on any device");
        return POLL_PLAYING_MS;
    }

    bool was_shuffled = cur.shuffle;
    const cJSON *device = cJSON_GetObjectItem(root, "device");
    const char *repeat = json_str(root, "repeat_state");
    cur.status = SPOTIFY_ACTIVE;
    cur.is_playing = cJSON_IsTrue(cJSON_GetObjectItem(root, "is_playing"));
    cur.shuffle = cJSON_IsTrue(cJSON_GetObjectItem(root, "shuffle_state"));
    cur.repeat = !strcmp(repeat, "track") ? SPOTIFY_REPEAT_TRACK : !strcmp(repeat, "context") ? SPOTIFY_REPEAT_CONTEXT : SPOTIFY_REPEAT_OFF;
    cur.volume = json_int(device, "volume_percent", -1);
    cur.progress_ms = json_int(root, "progress_ms", 0);
    cur.sampled_at = millis();
    cur.duration_ms = json_int(item, "duration_ms", 0);
    to_ascii(cur.title, sizeof(cur.title), json_str(item, "name"));
    to_ascii(cur.artist, sizeof(cur.artist), item_artists(item).c_str());
    to_ascii(cur.device, sizeof(cur.device), json_str(device, "name"));
    String uri = json_str(item, "uri");
    bool new_track = uri != track_uri;
    // The queue changes with the track and with shuffle; refresh it then, and now and again
    bool queue_stale = new_track || cur.shuffle != was_shuffled || millis() - queue_fetched_at > QUEUE_REFRESH_MS;
    track_uri = uri;

    const cJSON *show = cJSON_GetObjectItem(item, "show");
    const cJSON *album = cJSON_GetObjectItem(item, "album");
    const cJSON *images = cJSON_GetObjectItem(album ? album : item, "images");
    if (cJSON_GetArraySize(images) == 0 && show)
        images = cJSON_GetObjectItem(show, "images");
    String url = pick_image(images);
    cJSON_Delete(root);

    publish();
    if (new_track)
        Serial.printf("[spotify] now playing: %s - %s\n", cur.artist, cur.title);
    if (url != art_url) {
        art_url = url;
        update_art(url);
    }
    if (queue_stale)
        update_queue();
    return cur.is_playing ? POLL_PLAYING_MS : POLL_IDLE_MS;
}

static void run_command(spotify_cmd_t cmd)
{
    const char *method = "POST";
    const char *path;
    if (cmd == SPOTIFY_CMD_PREVIOUS) {
        path = "/previous";
    } else if (cmd == SPOTIFY_CMD_NEXT) {
        path = "/next";
    } else {
        method = "PUT";
        path = cur.is_playing ? "/pause" : "/play";
    }

    String body;
    int code = api_call(method, path, &body);
    if (code == HTTP_CODE_NO_CONTENT || code == HTTP_CODE_OK) {
        if (cmd == SPOTIFY_CMD_PLAY_PAUSE && cur.status == SPOTIFY_ACTIVE) {
            // Flip the icon now instead of waiting for the next poll
            cur.progress_ms = current_progress();
            cur.sampled_at = millis();
            cur.is_playing = !cur.is_playing;
            publish();
        }
    } else if (code != API_NO_TOKEN) {
        if (code == 429)
            backoff_ms = max(backoff_ms, retry_after_ms);
        notify(error_message(code, body)); // e.g. "Premium required", "No active device found"
    }
}

static void spotify_task(void *)
{
    if (!strlen(SPOTIFY_CLIENT_ID) || !strlen(SPOTIFY_REFRESH_TOKEN)) {
        set_status(SPOTIFY_NOT_CONFIGURED, "Spotify not set up", "Run tools/spotify_auth.py, then flash again");
        vTaskDelete(NULL);
    }
    load_refresh_token();

    uint32_t next_poll = millis();
    for (;;) {
        int32_t wait = max((int32_t)(next_poll - millis()), (int32_t)0);
        spotify_cmd_t cmd;
        if (xQueueReceive(cmd_queue, &cmd, pdMS_TO_TICKS(wait)) == pdTRUE) {
            if (WiFi.isConnected() && cur.status != SPOTIFY_LOGIN_NEEDED) {
                run_command(cmd);
                next_poll = millis() + max((uint32_t)AFTER_COMMAND_MS, backoff_ms);
            }
            continue;
        }
        if (!WiFi.isConnected()) {
            set_status(SPOTIFY_CONNECTING, "Connecting to WiFi", WIFI_SSID);
            next_poll = millis() + 500;
            continue;
        }
        next_poll = millis() + poll_player();
    }
}

/* ---------------------------------------------------------------- public API */

void spotify_begin(void)
{
    static cJSON_Hooks hooks = {ps_malloc, free}; // keep internal RAM for TLS
    cJSON_InitHooks(&hooks);

    state_lock = xSemaphoreCreateMutex();
    cmd_queue = xQueueCreate(8, sizeof(spotify_cmd_t));
    shared_art = (lv_color_t *)ps_malloc(ART_PIXELS * sizeof(lv_color_t));
    decoded_art = (lv_color_t *)ps_malloc(ART_PIXELS * sizeof(lv_color_t));
    api_client.setCACert(spotify_root_ca);

    cur.status = SPOTIFY_ERROR; // anything else, so set_status() publishes
    set_status(SPOTIFY_CONNECTING, "Connecting to WiFi", WIFI_SSID);
    xTaskCreatePinnedToCore(spotify_task, "spotify", 16384, NULL, 1, NULL, 0);
}

bool spotify_get_state(spotify_state_t *state)
{
    bool changed = false;
    if (!state_lock)
        return false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (state->version != shared_state.version) {
        *state = shared_state;
        changed = true;
    }
    xSemaphoreGive(state_lock);
    return changed;
}

bool spotify_get_art(lv_color_t *dst, uint32_t *art_version)
{
    bool changed = false;
    if (!state_lock)
        return false;
    xSemaphoreTake(state_lock, portMAX_DELAY);
    if (*art_version != shared_state.art_version) {
        if (shared_state.has_art)
            memcpy(dst, shared_art, ART_PIXELS * sizeof(lv_color_t));
        *art_version = shared_state.art_version;
        changed = true;
    }
    xSemaphoreGive(state_lock);
    return changed;
}

void spotify_send(spotify_cmd_t cmd)
{
    if (cmd_queue)
        xQueueSend(cmd_queue, &cmd, 0);
}
