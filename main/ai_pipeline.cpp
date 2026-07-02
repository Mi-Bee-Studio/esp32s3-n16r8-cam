/*
 * MiBee Cam v0.1 — AI Pipeline implementation
 *
 * Copyright (C) 2024 MiBee Cam Authors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Architecture:
 *   AI task (Core 1, priority 5, 8192 stack)
 *     └─ subscribes to frame_broadcaster (FRAMESUB_AI_PIPELINE)
 *     └─ loop: get_frame → decode JPEG → face detect + motion + QR
 *     └─ store results in mutex-protected struct
 *
 * Face detection is compiled conditionally behind AI_FACE_DETECT_ENABLED
 * so motion + QR can ship if the ESP-DL model linkage fails.
 */

#include "ai_pipeline.h"
#include "frame_broadcaster.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <new>

/* ---- Face detection (gated) -------------------------------------- */
#ifdef AI_FACE_DETECT_ENABLED
#include "human_face_detect.hpp"
#endif /* AI_FACE_DETECT_ENABLED */

/* ---- QR decode --------------------------------------------------- */
#include "quirc.h"

/* ---- ESP-DL image utilities (needed by both face and motion paths) */
#include "dl_image_define.hpp"
#include "dl_image_jpeg.hpp"
static const char *TAG = "ai_pipe";

/* ------------------------------------------------------------------ */
/*  Constants                                                         */
/* ------------------------------------------------------------------ */

/** Expected frame dimensions (VGA). */
#define AI_FRAME_W   640
#define AI_FRAME_H   480
#define AI_NUM_PIX   (AI_FRAME_W * AI_FRAME_H)

/** Motion detection threshold (0-100). */
#define AI_MOTION_THRESH  15

/** Task watchdog timeout (seconds). */
#define AI_WDT_TIMEOUT_S  5

/* ------------------------------------------------------------------ */
/*  Static state                                                      */
/* ------------------------------------------------------------------ */

/* ---- Mutexes ---------------------------------------------------- */

static SemaphoreHandle_t s_result_mutex  = NULL;
static SemaphoreHandle_t s_config_mutex  = NULL;

/* ---- Current results -------------------------------------------- */

static ai_result_t  s_result    = {};
static bool         s_has_result = false;
static uint32_t     s_last_seq  = 0;

/* ---- Feature enable bits ---------------------------------------- */

static bool s_enabled[AI_FEATURE_COUNT] = {
    true,   /* AI_FEATURE_FACE_DETECT   */
    true,   /* AI_FEATURE_MOTION_DETECT */
    true    /* AI_FEATURE_QR_DECODE     */
};

/* ---- Face detection model --------------------------------------- */

#ifdef AI_FACE_DETECT_ENABLED
static HumanFaceDetect *s_detect = nullptr;
#endif

/* ---- QR decoder -------------------------------------------------- */

static struct quirc *s_qr = NULL;

/* ---- Grayscale buffers (pre-allocated in PSRAM) ------------------ */
static uint8_t *s_gray_buf     = NULL;  /* current frame grayscale */
static uint8_t *s_prev_gray    = NULL;  /* previous frame grayscale */
static bool     s_have_prev    = false; /* true after first frame */

/* ---- Task -------------------------------------------------------- */
/** Task handle waiting for AI task to stop (true-join). */
static TaskHandle_t  s_ai_stop_waiter = NULL;

static TaskHandle_t  s_ai_task    = NULL;
static volatile bool s_ai_running = false;

/* ---- Frame broadcaster subscription ----------------------------- */

static frame_sub_t *s_ai_sub = NULL;

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

/**
 * Convert RGB888 pixel data to 8-bit grayscale using standard
 * luminance weights (ITU-R BT.601):
 *   Y = 0.299 R + 0.587 G + 0.114 B
 * Fixed-point: Y = (77*R + 150*G + 29*B) >> 8
 */
static void rgb888_to_gray(const uint8_t *rgb, uint8_t *gray, int num_pixels)
{
    for (int i = 0; i < num_pixels; i++) {
        int r = rgb[3 * i];
        int g = rgb[3 * i + 1];
        int b = rgb[3 * i + 2];
        gray[i] = (uint8_t)((unsigned)(r * 77u + g * 150u + b * 29u) >> 8u);
    }
}

/**
 * Compute motion score (0-100) by mean-absolute-difference between
 * two grayscale frames.
 */
static int compute_motion_score(const uint8_t *curr, const uint8_t *prev,
                                int num_pixels)
{
    if (!prev) return 0;

    uint32_t sum = 0;
    for (int i = 0; i < num_pixels; i++) {
        int d = (int)curr[i] - (int)prev[i];
        if (d < 0) d = -d;
        sum += (uint32_t)d;
    }
    float avg = (float)sum / (float)num_pixels;
    int score = (int)(avg * 100.0f / 255.0f);
    if (score > 100) score = 100;
    return score;
}

/* ------------------------------------------------------------------ */
/*  Frame processing                                                  */
/* ------------------------------------------------------------------ */

static void process_frame(camera_fb_t *fb)
{
    ai_result_t local_result;
    memset(&local_result, 0, sizeof(local_result));

    /* Snapshot feature enable flags under mutex */
    bool local_enabled[AI_FEATURE_COUNT];
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        memcpy(local_enabled, s_enabled, sizeof(s_enabled));
        xSemaphoreGive(s_config_mutex);
    } else {
        /* Fallback: use current values without mutex (best effort) */
        memcpy(local_enabled, s_enabled, sizeof(s_enabled));
    }


#ifdef AI_FACE_DETECT_ENABLED
    /* ---- Decode JPEG → RGB888 via ESP-DL (uses esp_new_jpeg) ---- */
    dl::image::jpeg_img_t jpeg_in;
    jpeg_in.data     = fb->buf;
    jpeg_in.data_len = fb->len;

    dl::image::img_t decoded = dl::image::sw_decode_jpeg(
        jpeg_in, dl::image::DL_IMAGE_PIX_TYPE_RGB888);

    if (!decoded.data || decoded.width == 0 || decoded.height == 0) {
        ESP_LOGW(TAG, "JPEG decode failed, skipping frame");
        return;
    }
    esp_task_wdt_reset();

    /* ---- Face detection ----------------------------------------- */
    if (s_detect && local_enabled[AI_FEATURE_FACE_DETECT]) {
        try {
            std::list<dl::detect::result_t> &faces = s_detect->run(decoded);
            int idx = 0;
            for (auto it = faces.begin();
                 it != faces.end() && idx < AI_MAX_FACES;
                 ++it, ++idx)
            {
                /* box = [left, top, right, bottom] */
                local_result.face.faces[idx].x          = (*it).box[0];
                local_result.face.faces[idx].y          = (*it).box[1];
                local_result.face.faces[idx].w          = (*it).box[2] - (*it).box[0];
                local_result.face.faces[idx].h          = (*it).box[3] - (*it).box[1];
                local_result.face.faces[idx].confidence  = (*it).score;
            }
            local_result.face.count = idx;
        } catch (const std::exception &e) {
            ESP_LOGW(TAG, "Face detection failed: %s", e.what());
        }
    }

    /* ---- Convert RGB → GRAY (for motion + QR) ------------------- */
    rgb888_to_gray(static_cast<const uint8_t *>(decoded.data),
                   s_gray_buf, AI_NUM_PIX);

    /* Free the decoded RGB buffer — grayscale is now in s_gray_buf */
    heap_caps_free(decoded.data);
    decoded.data = NULL;

#else  /* !AI_FACE_DETECT_ENABLED */

    /*
     * Decode JPEG → RGB888 via ESP-DL, then convert to grayscale
     * manually (sw_decode_jpeg does NOT support GRAY output).
     */
    dl::image::jpeg_img_t jpeg_in;
    jpeg_in.data     = fb->buf;
    jpeg_in.data_len = fb->len;

    dl::image::img_t decoded = dl::image::sw_decode_jpeg(
        jpeg_in, dl::image::DL_IMAGE_PIX_TYPE_RGB888);

    if (!decoded.data || decoded.width == 0 || decoded.height == 0) {
        ESP_LOGW(TAG, "JPEG decode failed, skipping frame");
        return;
    }

    rgb888_to_gray(static_cast<const uint8_t *>(decoded.data),
                   s_gray_buf, AI_NUM_PIX);

    heap_caps_free(decoded.data);
    decoded.data = NULL;

#endif /* AI_FACE_DETECT_ENABLED */

    esp_task_wdt_reset();

    /* ---- Motion detection --------------------------------------- */
    if (local_enabled[AI_FEATURE_MOTION_DETECT]) {
        int score = compute_motion_score(s_gray_buf, s_prev_gray, AI_NUM_PIX);
        local_result.motion.score = score;
    }

    /* ---- QR decode ---------------------------------------------- */
    if (s_qr && local_enabled[AI_FEATURE_QR_DECODE]) {
        int w = 0, h = 0;
        uint8_t *qr_buf = quirc_begin(s_qr, &w, &h);
        if (qr_buf && w == AI_FRAME_W && h == AI_FRAME_H) {
            memcpy(qr_buf, s_gray_buf, AI_NUM_PIX);
            quirc_end(s_qr);

            int num_codes = quirc_count(s_qr);
            if (num_codes > AI_MAX_QR_CODES) num_codes = AI_MAX_QR_CODES;
            local_result.qr.count = num_codes;

            for (int i = 0; i < num_codes; i++) {
                struct quirc_code code;
                struct quirc_data data;
                quirc_extract(s_qr, i, &code);

                quirc_decode_error_t err = quirc_decode(&code, &data);
                if (err == QUIRC_SUCCESS) {
                    int copy_len = data.payload_len;
                    if (copy_len > AI_QR_STRING_LEN - 1) {
                        copy_len = AI_QR_STRING_LEN - 1;
                    }
                    memcpy(local_result.qr.strings[i], data.payload, (size_t)copy_len);
                    local_result.qr.strings[i][copy_len] = '\0';
                } else if (err == QUIRC_ERROR_DATA_ECC) {
                    /* Try flipped decode */
                    quirc_flip(&code);
                    err = quirc_decode(&code, &data);
                    if (err == QUIRC_SUCCESS) {
                        int copy_len = data.payload_len;
                        if (copy_len > AI_QR_STRING_LEN - 1) {
                            copy_len = AI_QR_STRING_LEN - 1;
                        }
                        memcpy(local_result.qr.strings[i], data.payload, (size_t)copy_len);
                        local_result.qr.strings[i][copy_len] = '\0';
                    } else {
                        local_result.qr.count--; /* failed, don't count */
                    }
                } else {
                    local_result.qr.count--; /* failed, don't count */
                }
            }
        } else if (qr_buf) {
            /* Dimensions mismatch — call quirc_end to reset state, but no valid results */
            memset(qr_buf, 128, (size_t)(w * h));
            quirc_end(s_qr);
        }
        /* if qr_buf is NULL, skip entirely — quirc_end NOT called */
    }

    esp_task_wdt_reset();

    /* ---- Update previous grayscale for next motion diff --------- */
    {
        uint8_t *tmp   = s_prev_gray;
        s_prev_gray    = s_gray_buf;
        s_gray_buf     = tmp;
        s_have_prev    = true;
    }

    /* ---- Publish results under mutex ---------------------------- */
    local_result.frame_seq = s_last_seq;

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_result     = local_result;
        s_has_result = true;
        xSemaphoreGive(s_result_mutex);
    }
}

/* ------------------------------------------------------------------ */
/*  AI FreeRTOS task (Core 1, priority 5)                             */
/* ------------------------------------------------------------------ */

static void ai_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "AI task started on core %d", xPortGetCoreID());

    /* Subscribe to frame broadcaster */
    s_ai_sub = frame_broadcaster_subscribe(FRAMESUB_AI_PIPELINE);
    if (!s_ai_sub) {
        ESP_LOGE(TAG, "Failed to subscribe to frame broadcaster");
        s_ai_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    /* Register with task watchdog */
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err != ESP_OK) {
        ESP_LOGW(TAG, "esp_task_wdt_add failed: %s", esp_err_to_name(wdt_err));
    }

    /* ---- Processing loop ---------------------------------------- */
    while (s_ai_running) {
        /* Reset watchdog before potentially blocking on get_frame */
        esp_task_wdt_reset();

        frame_msg_t msg;
        if (!frame_broadcaster_get_frame(s_ai_sub, &msg)) {
            /* No frame yet — yield briefly */
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        s_last_seq = msg.seq;

        /* Build a temporary camera_fb_t view into the PSRAM copy */
        camera_fb_t fb;
        fb.buf    = msg.data;
        fb.len    = msg.len;
        fb.width  = AI_FRAME_W;
        fb.height = AI_FRAME_H;
        fb.format = PIXFORMAT_JPEG;

        process_frame(&fb);

        frame_broadcaster_release(&msg);

        /* Brief yield to let lower-priority tasks run */
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    /* ---- Clean exit --------------------------------------------- */
    /* Signal the waiter that we have exited the loop (true-join) */
    if (s_ai_stop_waiter) {
        xTaskNotifyGive(s_ai_stop_waiter);
        s_ai_stop_waiter = NULL;
    }

    frame_broadcaster_unsubscribe(s_ai_sub);
    s_ai_sub = NULL;

    esp_task_wdt_delete(NULL);

    ESP_LOGI(TAG, "AI task stopped");
    s_ai_task = NULL;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ */
/*  Public API (extern "C")                                           */
/* ------------------------------------------------------------------ */

esp_err_t ai_init(void)
{
    ESP_LOGI(TAG, "AI pipeline init ...");

    size_t heap_before = esp_get_free_internal_heap_size();
    ESP_LOGI(TAG, "Internal heap before AI init: %u",
             (unsigned)heap_before);

    /* ---- Mutexes ------------------------------------------------- */
    s_result_mutex = xSemaphoreCreateMutex();
    s_config_mutex = xSemaphoreCreateMutex();
    if (!s_result_mutex || !s_config_mutex) {
        ESP_LOGE(TAG, "Failed to create mutexes");
        return ESP_FAIL;
    }

    /* ---- Allocate grayscale buffers in PSRAM --------------------- */
    s_gray_buf  = (uint8_t *)heap_caps_malloc(AI_NUM_PIX, MALLOC_CAP_SPIRAM);
    s_prev_gray = (uint8_t *)heap_caps_malloc(AI_NUM_PIX, MALLOC_CAP_SPIRAM);
    if (!s_gray_buf || !s_prev_gray) {
        ESP_LOGE(TAG, "Failed to allocate grayscale buffers in PSRAM");
        return ESP_FAIL;
    }
    memset(s_gray_buf,  0, AI_NUM_PIX);
    memset(s_prev_gray, 0, AI_NUM_PIX);

#ifdef AI_FACE_DETECT_ENABLED
    /* ---- Face detection (load model immediately, not lazy) ------- */
    try {
        s_detect = new HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1,
                                       /* lazy_load = */ false);
        ESP_LOGI(TAG, "Face detection model loaded");
    } catch (const std::exception &e) {
        ESP_LOGE(TAG, "Failed to initialise face detection: %s", e.what());
        s_detect = nullptr;
        /* Not a fatal error — motion + QR will still work */
    }
#endif /* AI_FACE_DETECT_ENABLED */

    /* ---- QR decoder --------------------------------------------- */
    s_qr = quirc_new();
    if (s_qr) {
        if (quirc_resize(s_qr, AI_FRAME_W, AI_FRAME_H) < 0) {
            ESP_LOGW(TAG, "quirc_resize failed");
            quirc_destroy(s_qr);
            s_qr = NULL;
        } else {
            ESP_LOGI(TAG, "QR decoder initialised (%dx%d)", AI_FRAME_W, AI_FRAME_H);
        }
    } else {
        ESP_LOGW(TAG, "quirc_new failed — QR decode disabled");
    }

    /* ---- Heap check --------------------------------------------- */
    size_t heap_after = esp_get_free_internal_heap_size();
    ESP_LOGI(TAG, "Internal heap after AI init: %u  (delta: %d)",
             (unsigned)heap_after, (int)((int64_t)heap_after - (int64_t)heap_before));

    if (heap_after < 30720) {  /* 30 KB */
        ESP_LOGE(TAG, "Only %u bytes internal heap remaining after AI init "
                 "(need > 30720)", (unsigned)heap_after);
        /* Continue anyway — motion + QR may still function */
    }


    /* ---- Start the AI task on Core 1 ----------------------------- */
    esp_err_t task_err = ai_start_task();
    if (task_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start AI task");
        return task_err;
    }

    ESP_LOGI(TAG, "AI pipeline initialised successfully");
    return ESP_OK;
}

void ai_process_frame(camera_fb_t *fb)
{
    if (!fb) return;
    if (fb->format != PIXFORMAT_JPEG) return;
    process_frame(fb);
}

bool ai_get_result(ai_result_t *out)
{
    if (!out) return false;

    if (xSemaphoreTake(s_result_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        bool has = s_has_result;
        if (has) {
            *out = s_result;
        }
        xSemaphoreGive(s_result_mutex);
        return has;
    }
    return false;
}

void ai_enable(ai_feature_t feature, bool enable)
{
    if (feature >= AI_FEATURE_COUNT) return;

    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_enabled[(int)feature] = enable;
        xSemaphoreGive(s_config_mutex);
    }

    ESP_LOGI(TAG, "Feature %d %s", (int)feature,
             enable ? "enabled" : "disabled");
}

bool ai_is_enabled(ai_feature_t feature)
{
    if (feature >= AI_FEATURE_COUNT) return false;

    bool val = false;
    if (xSemaphoreTake(s_config_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        val = s_enabled[(int)feature];
        xSemaphoreGive(s_config_mutex);
    }
    return val;
}

/* ------------------------------------------------------------------ */
/*  Task lifecycle — called from ai_init() or separately               */
/* ------------------------------------------------------------------ */

esp_err_t ai_start_task(void)
{
    if (s_ai_task != NULL) {
        ESP_LOGW(TAG, "AI task already running");
        return ESP_ERR_INVALID_STATE;
    }

    s_ai_running = true;

    BaseType_t ret = xTaskCreatePinnedToCore(
        ai_task,
        "ai_pipe",
        24576,          /* stack size */
        NULL,
        5,              /* priority */
        &s_ai_task,
        1               /* Core 1 */
    );

    if (ret != pdPASS) {
        s_ai_running = false;
        s_ai_task = NULL;
        ESP_LOGE(TAG, "Failed to create AI task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "AI task created on core 1, stack=24576, prio=5");

#if !CONFIG_FREERTOS_UNICORE
    TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
    if (idle1) {
        esp_err_t werr = esp_task_wdt_delete(idle1);
        if (werr == ESP_OK) {
            ESP_LOGI(TAG, "Removed IDLE1 from task watchdog (CPU-intensive AI task)");
        } else if (werr == ESP_ERR_INVALID_STATE) {
            ESP_LOGD(TAG, "IDLE1 not subscribed to task watchdog");
        } else {
            ESP_LOGW(TAG, "esp_task_wdt_delete(IDLE1) failed: %s", esp_err_to_name(werr));
        }
    }
#endif
    return ESP_OK;
}

void ai_stop_task(void)
{
    s_ai_running = false;
    if (s_ai_task) {
        s_ai_stop_waiter = xTaskGetCurrentTaskHandle();
        xTaskNotifyGive(s_ai_task);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        s_ai_task = NULL;
        s_ai_stop_waiter = NULL;
    }
}
