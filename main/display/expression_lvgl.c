#include "expression_lvgl.h"

#include <stdbool.h>
#include <stdint.h>

#include "display_config.h"
#include "esp_log.h"
#include "esp_random.h"
#include "lvgl.h"

#define EXPRESSION_EYE_LEFT_X          80
#define EXPRESSION_EYE_RIGHT_X         180
#define EXPRESSION_EYE_BASE_Y          60
#define EXPRESSION_EYE_BASE_W          60
#define EXPRESSION_EYE_BASE_H          70
#define EXPRESSION_EYE_HAPPY_W         68
#define EXPRESSION_EYE_HAPPY_H         50
#define EXPRESSION_EYE_SLEEPY_H        42
#define EXPRESSION_EYE_SURPRISED_W     66
#define EXPRESSION_EYE_SURPRISED_H     76
#define EXPRESSION_EYE_SAD_W           54
#define EXPRESSION_EYE_SAD_H           62
#define EXPRESSION_EYE_BORDER_WIDTH    3
#define EXPRESSION_PUPIL_NORMAL_SIZE   20
#define EXPRESSION_PUPIL_SMALL_SIZE    14
#define EXPRESSION_PUPIL_STEP_PX       2
#define EXPRESSION_BLINK_CLOSED_H      4
#define EXPRESSION_BLINK_STEP_PX       25
#define EXPRESSION_TIMER_IDLE_MS       120
#define EXPRESSION_TIMER_HAPPY_MS      100
#define EXPRESSION_TIMER_SLEEPY_MS     150
#define EXPRESSION_TIMER_SURPRISED_MS  80
#define EXPRESSION_TIMER_SAD_MS        150
#define EXPRESSION_TIMER_PERIOD_MS     50
#define EXPRESSION_IDLE_EYE_LOOK_PX    6
#define EXPRESSION_IDLE_PUPIL_LOOK_PX  8
#define EXPRESSION_IDLE_EYE_LOOK_Y_PX  4
#define EXPRESSION_IDLE_PUPIL_LOOK_Y_PX 5
#define EXPRESSION_IDLE_GAZE_STEP_PX   2
#define EXPRESSION_IDLE_GAZE_MIN_MS    1000
#define EXPRESSION_IDLE_GAZE_JITTER_MS 600
#define MOUTH_DEBUG_BOUNDS             1
#define MOUTH_DEBUG_LOG                0
#define MOUTH_CONTAINER_W              130
#define MOUTH_CONTAINER_H              72

static const char *TAG = "EXPRESSION_LVGL";

typedef struct {
    int16_t eye_w;
    int16_t eye_h;
    int16_t pupil_size;
    int16_t pupil_bias_y;
    uint16_t timer_ms;
    uint16_t blink_min_ms;
    uint16_t blink_jitter_ms;
    bool blink_enabled;
    bool quick_scan;
} expression_style_t;

typedef struct {
    const lv_point_precise_t *points;
    uint16_t point_count;
} expression_mouth_frame_t;

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
    uint8_t line_width;
    uint16_t frame_interval_ms;
    const expression_mouth_frame_t *frames;
    uint8_t frame_count;
    const uint8_t *sequence;
    uint8_t sequence_length;
} expression_mouth_anim_t;

typedef enum {
    EXPRESSION_IDLE_GAZE_CENTER_1 = 0,
    EXPRESSION_IDLE_GAZE_LEFT,
    EXPRESSION_IDLE_GAZE_CENTER_2,
    EXPRESSION_IDLE_GAZE_RIGHT,
    EXPRESSION_IDLE_GAZE_CENTER_3,
} expression_idle_gaze_t;

static lv_obj_t *s_container;
static lv_obj_t *s_eye_l;
static lv_obj_t *s_eye_r;
static lv_obj_t *s_pupil_l;
static lv_obj_t *s_pupil_r;
static lv_obj_t *s_mouth_container;
static lv_obj_t *s_mouth;
static lv_timer_t *s_timer;
static display_expression_kind_t s_mode = DISPLAY_EXPRESSION_IDLE;
static bool s_visible;
static bool s_blinking;
static uint8_t s_blink_phase;
static int16_t s_eye_h_current = EXPRESSION_EYE_BASE_H;
static int16_t s_pupil_offset_x;
static int16_t s_pupil_offset_y;
static expression_idle_gaze_t s_idle_gaze = EXPRESSION_IDLE_GAZE_CENTER_1;
static int16_t s_idle_eye_offset_x;
static int16_t s_idle_eye_offset_y;
static int16_t s_idle_pupil_offset_x;
static int16_t s_idle_pupil_offset_y;
static int16_t s_idle_target_eye_offset_x;
static int16_t s_idle_target_eye_offset_y;
static int16_t s_idle_target_pupil_offset_x;
static int16_t s_idle_target_pupil_offset_y;
static uint32_t s_idle_gaze_elapsed_ms;
static uint32_t s_idle_gaze_hold_ms = EXPRESSION_IDLE_GAZE_MIN_MS;
static uint32_t s_elapsed_ms;
static uint32_t s_next_blink_ms = 3000;
static bool s_mouth_debug_logged;
static uint16_t s_motion_accumulator_ms;
static uint16_t s_mouth_frame_accumulator_ms;
static uint16_t s_mouth_debug_frame_count;
static uint8_t s_mouth_sequence_index;

static const expression_style_t s_styles[] = {
    [DISPLAY_EXPRESSION_IDLE] = {
        .eye_w = EXPRESSION_EYE_BASE_W,
        .eye_h = EXPRESSION_EYE_BASE_H,
        .pupil_size = EXPRESSION_PUPIL_NORMAL_SIZE,
        .pupil_bias_y = 0,
        .timer_ms = EXPRESSION_TIMER_IDLE_MS,
        .blink_min_ms = 6000,
        .blink_jitter_ms = 10000,
        .blink_enabled = true,
        .quick_scan = false,
    },
    [DISPLAY_EXPRESSION_HAPPY] = {
        .eye_w = EXPRESSION_EYE_HAPPY_W,
        .eye_h = EXPRESSION_EYE_HAPPY_H,
        .pupil_size = EXPRESSION_PUPIL_NORMAL_SIZE,
        .pupil_bias_y = -7,
        .timer_ms = EXPRESSION_TIMER_HAPPY_MS,
        .blink_min_ms = 4000,
        .blink_jitter_ms = 1000,
        .blink_enabled = true,
        .quick_scan = false,
    },
    [DISPLAY_EXPRESSION_SLEEPY] = {
        .eye_w = EXPRESSION_EYE_BASE_W,
        .eye_h = EXPRESSION_EYE_SLEEPY_H,
        .pupil_size = EXPRESSION_PUPIL_NORMAL_SIZE,
        .pupil_bias_y = 10,
        .timer_ms = EXPRESSION_TIMER_SLEEPY_MS,
        .blink_min_ms = 8000,
        .blink_jitter_ms = 2000,
        .blink_enabled = true,
        .quick_scan = false,
    },
    [DISPLAY_EXPRESSION_SURPRISED] = {
        .eye_w = EXPRESSION_EYE_SURPRISED_W,
        .eye_h = EXPRESSION_EYE_SURPRISED_H,
        .pupil_size = EXPRESSION_PUPIL_SMALL_SIZE,
        .pupil_bias_y = 0,
        .timer_ms = EXPRESSION_TIMER_SURPRISED_MS,
        .blink_min_ms = 0,
        .blink_jitter_ms = 0,
        .blink_enabled = false,
        .quick_scan = true,
    },
    [DISPLAY_EXPRESSION_SAD] = {
        .eye_w = EXPRESSION_EYE_SAD_W,
        .eye_h = EXPRESSION_EYE_SAD_H,
        .pupil_size = EXPRESSION_PUPIL_NORMAL_SIZE,
        .pupil_bias_y = 11,
        .timer_ms = EXPRESSION_TIMER_SAD_MS,
        .blink_min_ms = 12000,
        .blink_jitter_ms = 4000,
        .blink_enabled = false,
        .quick_scan = false,
    },
};

static const lv_point_precise_t s_mouth_idle_0[] = {
    {18, 30}, {24, 35}, {31, 39}, {38, 43}, {45, 46}, {52, 49},
    {59, 50}, {65, 52}, {72, 52}, {78, 52}, {85, 50}, {92, 49},
    {99, 46}, {106, 43}, {113, 39}, {120, 35}, {126, 30},
};

static const lv_point_precise_t s_mouth_idle_1[] = {
    {18, 29}, {24, 34}, {31, 39}, {38, 43}, {45, 47}, {52, 50},
    {59, 52}, {65, 54}, {72, 54}, {78, 54}, {85, 52}, {92, 50},
    {99, 47}, {106, 43}, {113, 39}, {120, 34}, {126, 29},
};

static const lv_point_precise_t s_mouth_happy_0[] = {
    {0, 24}, {8, 32}, {16, 39}, {24, 44}, {32, 49}, {40, 53},
    {48, 56}, {56, 57}, {65, 58}, {74, 57}, {82, 56}, {90, 53},
    {98, 49}, {106, 44}, {114, 39}, {122, 32}, {130, 24},
};

static const lv_point_precise_t s_mouth_happy_1[] = {
    {0, 20}, {8, 30}, {16, 38}, {24, 45}, {32, 51}, {40, 56},
    {48, 59}, {56, 61}, {65, 62}, {74, 61}, {82, 59}, {90, 56},
    {98, 51}, {106, 45}, {114, 38}, {122, 30}, {130, 20},
};

static const lv_point_precise_t s_mouth_happy_2[] = {
    {0, 16}, {8, 28}, {16, 38}, {24, 46}, {32, 53}, {40, 59},
    {48, 63}, {56, 65}, {65, 66}, {74, 65}, {82, 63}, {90, 59},
    {98, 53}, {106, 46}, {114, 38}, {122, 28}, {130, 16},
};

static const lv_point_precise_t s_mouth_sleepy_0[] = {
    {20, 34}, {31, 35}, {42, 36}, {53, 37}, {65, 38}, {77, 37},
    {88, 36}, {99, 35}, {110, 34},
};

static const lv_point_precise_t s_mouth_sleepy_1[] = {
    {20, 36}, {31, 37}, {42, 38}, {53, 39}, {65, 40}, {77, 39},
    {88, 38}, {99, 37}, {110, 36},
};

static const lv_point_precise_t s_mouth_surprised_0[] = {
    {65, 20}, {77, 24}, {84, 36}, {82, 49}, {72, 58}, {58, 58},
    {48, 49}, {46, 36}, {53, 24}, {65, 20},
};

static const lv_point_precise_t s_mouth_sad_0[] = {
    {20, 52}, {31, 45}, {42, 38}, {53, 32}, {65, 28}, {77, 32},
    {88, 38}, {99, 45}, {110, 52},
};

static const lv_point_precise_t s_mouth_sad_1[] = {
    {20, 56}, {31, 48}, {42, 40}, {53, 34}, {65, 30}, {77, 34},
    {88, 40}, {99, 48}, {110, 56},
};

static const expression_mouth_frame_t s_mouth_idle_frames[] = {
    {s_mouth_idle_0, sizeof(s_mouth_idle_0) / sizeof(s_mouth_idle_0[0])},
    {s_mouth_idle_1, sizeof(s_mouth_idle_1) / sizeof(s_mouth_idle_1[0])},
};

static const expression_mouth_frame_t s_mouth_happy_frames[] = {
    {s_mouth_happy_0, sizeof(s_mouth_happy_0) / sizeof(s_mouth_happy_0[0])},
    {s_mouth_happy_1, sizeof(s_mouth_happy_1) / sizeof(s_mouth_happy_1[0])},
    {s_mouth_happy_2, sizeof(s_mouth_happy_2) / sizeof(s_mouth_happy_2[0])},
};

static const expression_mouth_frame_t s_mouth_sleepy_frames[] = {
    {s_mouth_sleepy_0, sizeof(s_mouth_sleepy_0) / sizeof(s_mouth_sleepy_0[0])},
    {s_mouth_sleepy_1, sizeof(s_mouth_sleepy_1) / sizeof(s_mouth_sleepy_1[0])},
};

static const expression_mouth_frame_t s_mouth_surprised_frames[] = {
    {s_mouth_surprised_0, sizeof(s_mouth_surprised_0) / sizeof(s_mouth_surprised_0[0])},
};

static const expression_mouth_frame_t s_mouth_sad_frames[] = {
    {s_mouth_sad_0, sizeof(s_mouth_sad_0) / sizeof(s_mouth_sad_0[0])},
    {s_mouth_sad_1, sizeof(s_mouth_sad_1) / sizeof(s_mouth_sad_1[0])},
};

static const uint8_t s_mouth_idle_sequence[] = {0, 1, 0};
static const uint8_t s_mouth_happy_sequence[] = {0, 1, 2, 1};
static const uint8_t s_mouth_sleepy_sequence[] = {0, 1, 0};
static const uint8_t s_mouth_surprised_sequence[] = {0};
static const uint8_t s_mouth_sad_sequence[] = {0, 1, 0};

static const expression_mouth_anim_t s_mouth_anims[] = {
    [DISPLAY_EXPRESSION_IDLE] = {
        .x = (DISPLAY_WIDTH - MOUTH_CONTAINER_W) / 2,
        .y = 150,
        .w = MOUTH_CONTAINER_W,
        .h = MOUTH_CONTAINER_H,
        .line_width = 7,
        .frame_interval_ms = 500,
        .frames = s_mouth_idle_frames,
        .frame_count = sizeof(s_mouth_idle_frames) / sizeof(s_mouth_idle_frames[0]),
        .sequence = s_mouth_idle_sequence,
        .sequence_length = sizeof(s_mouth_idle_sequence) / sizeof(s_mouth_idle_sequence[0]),
    },
    [DISPLAY_EXPRESSION_HAPPY] = {
        .x = (DISPLAY_WIDTH - MOUTH_CONTAINER_W) / 2,
        .y = 150,
        .w = MOUTH_CONTAINER_W,
        .h = MOUTH_CONTAINER_H,
        .line_width = 7,
        .frame_interval_ms = 120,
        .frames = s_mouth_happy_frames,
        .frame_count = sizeof(s_mouth_happy_frames) / sizeof(s_mouth_happy_frames[0]),
        .sequence = s_mouth_happy_sequence,
        .sequence_length = sizeof(s_mouth_happy_sequence) / sizeof(s_mouth_happy_sequence[0]),
    },
    [DISPLAY_EXPRESSION_SLEEPY] = {
        .x = (DISPLAY_WIDTH - MOUTH_CONTAINER_W) / 2,
        .y = 156,
        .w = MOUTH_CONTAINER_W,
        .h = MOUTH_CONTAINER_H,
        .line_width = 4,
        .frame_interval_ms = 700,
        .frames = s_mouth_sleepy_frames,
        .frame_count = sizeof(s_mouth_sleepy_frames) / sizeof(s_mouth_sleepy_frames[0]),
        .sequence = s_mouth_sleepy_sequence,
        .sequence_length = sizeof(s_mouth_sleepy_sequence) / sizeof(s_mouth_sleepy_sequence[0]),
    },
    [DISPLAY_EXPRESSION_SURPRISED] = {
        .x = (DISPLAY_WIDTH - MOUTH_CONTAINER_W) / 2,
        .y = 150,
        .w = MOUTH_CONTAINER_W,
        .h = MOUTH_CONTAINER_H,
        .line_width = 5,
        .frame_interval_ms = 1000,
        .frames = s_mouth_surprised_frames,
        .frame_count = sizeof(s_mouth_surprised_frames) / sizeof(s_mouth_surprised_frames[0]),
        .sequence = s_mouth_surprised_sequence,
        .sequence_length = sizeof(s_mouth_surprised_sequence) / sizeof(s_mouth_surprised_sequence[0]),
    },
    [DISPLAY_EXPRESSION_SAD] = {
        .x = (DISPLAY_WIDTH - MOUTH_CONTAINER_W) / 2,
        .y = 154,
        .w = MOUTH_CONTAINER_W,
        .h = MOUTH_CONTAINER_H,
        .line_width = 5,
        .frame_interval_ms = 600,
        .frames = s_mouth_sad_frames,
        .frame_count = sizeof(s_mouth_sad_frames) / sizeof(s_mouth_sad_frames[0]),
        .sequence = s_mouth_sad_sequence,
        .sequence_length = sizeof(s_mouth_sad_sequence) / sizeof(s_mouth_sad_sequence[0]),
    },
};

static const expression_style_t *expression_lvgl_style(void)
{
    if (s_mode > DISPLAY_EXPRESSION_SAD) {
        return &s_styles[DISPLAY_EXPRESSION_IDLE];
    }
    return &s_styles[s_mode];
}

static uint32_t expression_lvgl_random_range(uint32_t min_value, uint32_t jitter)
{
    if (jitter == 0U) {
        return min_value;
    }
    return min_value + (esp_random() % jitter);
}

static int16_t expression_lvgl_random_nonzero_i16(int16_t max_abs)
{
    int16_t value;

    if (max_abs <= 0) {
        return 0;
    }

    value = (int16_t)((int)(esp_random() % (uint32_t)((max_abs * 2) + 1)) - max_abs);
    if (value == 0) {
        value = (esp_random() & 1U) == 0U ? -1 : 1;
    }
    return value;
}

static int16_t expression_lvgl_clamp_i16(int16_t value, int16_t min_value, int16_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int16_t expression_lvgl_step_toward_i16(int16_t current, int16_t target, int16_t step)
{
    if (current < target) {
        current += step;
        if (current > target) {
            current = target;
        }
    } else if (current > target) {
        current -= step;
        if (current < target) {
            current = target;
        }
    }
    return current;
}

static void expression_lvgl_set_idle_gaze_target(expression_idle_gaze_t gaze)
{
    int16_t eye_y = 0;
    int16_t pupil_y = 0;

    s_idle_gaze = gaze;
    s_idle_gaze_elapsed_ms = 0;
    s_idle_gaze_hold_ms = expression_lvgl_random_range(EXPRESSION_IDLE_GAZE_MIN_MS,
                                                       EXPRESSION_IDLE_GAZE_JITTER_MS);

    switch (gaze) {
    case EXPRESSION_IDLE_GAZE_LEFT:
        eye_y = expression_lvgl_random_nonzero_i16(EXPRESSION_IDLE_EYE_LOOK_Y_PX);
        pupil_y = (int16_t)(eye_y + (eye_y > 0 ? 1 : -1));
        s_idle_target_eye_offset_x = -EXPRESSION_IDLE_EYE_LOOK_PX;
        s_idle_target_eye_offset_y = eye_y;
        s_idle_target_pupil_offset_x = -EXPRESSION_IDLE_PUPIL_LOOK_PX;
        s_idle_target_pupil_offset_y = expression_lvgl_clamp_i16(pupil_y,
                                                                 -EXPRESSION_IDLE_PUPIL_LOOK_Y_PX,
                                                                 EXPRESSION_IDLE_PUPIL_LOOK_Y_PX);
        break;
    case EXPRESSION_IDLE_GAZE_RIGHT:
        eye_y = expression_lvgl_random_nonzero_i16(EXPRESSION_IDLE_EYE_LOOK_Y_PX);
        pupil_y = (int16_t)(eye_y + (eye_y > 0 ? 1 : -1));
        s_idle_target_eye_offset_x = EXPRESSION_IDLE_EYE_LOOK_PX;
        s_idle_target_eye_offset_y = eye_y;
        s_idle_target_pupil_offset_x = EXPRESSION_IDLE_PUPIL_LOOK_PX;
        s_idle_target_pupil_offset_y = expression_lvgl_clamp_i16(pupil_y,
                                                                 -EXPRESSION_IDLE_PUPIL_LOOK_Y_PX,
                                                                 EXPRESSION_IDLE_PUPIL_LOOK_Y_PX);
        break;
    case EXPRESSION_IDLE_GAZE_CENTER_1:
    case EXPRESSION_IDLE_GAZE_CENTER_2:
    case EXPRESSION_IDLE_GAZE_CENTER_3:
    default:
        s_idle_target_eye_offset_x = 0;
        s_idle_target_eye_offset_y = 0;
        s_idle_target_pupil_offset_x = 0;
        s_idle_target_pupil_offset_y = 0;
        break;
    }
}

static void expression_lvgl_reset_idle_gaze(void)
{
    s_idle_eye_offset_x = 0;
    s_idle_eye_offset_y = 0;
    s_idle_pupil_offset_x = 0;
    s_idle_pupil_offset_y = 0;
    s_idle_target_eye_offset_x = 0;
    s_idle_target_eye_offset_y = 0;
    s_idle_target_pupil_offset_x = 0;
    s_idle_target_pupil_offset_y = 0;
    expression_lvgl_set_idle_gaze_target(EXPRESSION_IDLE_GAZE_CENTER_1);
}

static void expression_lvgl_advance_idle_gaze(uint32_t period_ms)
{
    s_idle_eye_offset_x = expression_lvgl_step_toward_i16(s_idle_eye_offset_x,
                                                          s_idle_target_eye_offset_x,
                                                          EXPRESSION_IDLE_GAZE_STEP_PX);
    s_idle_pupil_offset_x = expression_lvgl_step_toward_i16(s_idle_pupil_offset_x,
                                                            s_idle_target_pupil_offset_x,
                                                            EXPRESSION_IDLE_GAZE_STEP_PX);
    s_idle_eye_offset_y = expression_lvgl_step_toward_i16(s_idle_eye_offset_y,
                                                          s_idle_target_eye_offset_y,
                                                          EXPRESSION_IDLE_GAZE_STEP_PX);
    s_idle_pupil_offset_y = expression_lvgl_step_toward_i16(s_idle_pupil_offset_y,
                                                            s_idle_target_pupil_offset_y,
                                                            EXPRESSION_IDLE_GAZE_STEP_PX);
    s_idle_gaze_elapsed_ms += period_ms;

    if ((s_idle_gaze_elapsed_ms < s_idle_gaze_hold_ms) ||
        (s_idle_eye_offset_x != s_idle_target_eye_offset_x) ||
        (s_idle_eye_offset_y != s_idle_target_eye_offset_y) ||
        (s_idle_pupil_offset_x != s_idle_target_pupil_offset_x) ||
        (s_idle_pupil_offset_y != s_idle_target_pupil_offset_y)) {
        return;
    }

    switch (s_idle_gaze) {
    case EXPRESSION_IDLE_GAZE_CENTER_1:
        expression_lvgl_set_idle_gaze_target((esp_random() & 1U) == 0U ?
                                             EXPRESSION_IDLE_GAZE_LEFT :
                                             EXPRESSION_IDLE_GAZE_RIGHT);
        break;
    case EXPRESSION_IDLE_GAZE_LEFT:
        expression_lvgl_set_idle_gaze_target(EXPRESSION_IDLE_GAZE_CENTER_2);
        break;
    case EXPRESSION_IDLE_GAZE_RIGHT:
        expression_lvgl_set_idle_gaze_target(EXPRESSION_IDLE_GAZE_CENTER_3);
        break;
    case EXPRESSION_IDLE_GAZE_CENTER_2:
        expression_lvgl_set_idle_gaze_target(EXPRESSION_IDLE_GAZE_RIGHT);
        break;
    case EXPRESSION_IDLE_GAZE_CENTER_3:
    default:
        expression_lvgl_set_idle_gaze_target(EXPRESSION_IDLE_GAZE_LEFT);
        break;
    }
}

static void expression_lvgl_style_eye(lv_obj_t *eye)
{
    lv_obj_set_style_bg_color(eye, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(eye, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(eye, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(eye, EXPRESSION_EYE_BORDER_WIDTH, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(eye, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(eye, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void expression_lvgl_style_pupil(lv_obj_t *pupil)
{
    lv_obj_set_style_bg_color(pupil, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(pupil, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(pupil, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(pupil, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(pupil, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
}

static void expression_lvgl_position_eye(lv_obj_t *eye, int16_t center_x, int16_t width, int16_t height)
{
    int16_t y = EXPRESSION_EYE_BASE_Y + (EXPRESSION_EYE_BASE_H - height);

    if (s_mode == DISPLAY_EXPRESSION_IDLE) {
        center_x = (int16_t)(center_x + s_idle_eye_offset_x);
        y = (int16_t)(y + s_idle_eye_offset_y);
    }
    lv_obj_set_size(eye, width, height);
    lv_obj_set_pos(eye, center_x - (width / 2), y);
}

static const expression_mouth_anim_t *expression_lvgl_mouth_anim(void)
{
    if (s_mode > DISPLAY_EXPRESSION_SAD) {
        return &s_mouth_anims[DISPLAY_EXPRESSION_IDLE];
    }
    return &s_mouth_anims[s_mode];
}

static const expression_mouth_frame_t *expression_lvgl_current_mouth_frame(void)
{
    const expression_mouth_anim_t *anim = expression_lvgl_mouth_anim();
    uint8_t frame_index = 0;

    if ((anim->frames == NULL) || (anim->frame_count == 0)) {
        return NULL;
    }
    if ((anim->sequence != NULL) && (anim->sequence_length > 0)) {
        frame_index = anim->sequence[s_mouth_sequence_index % anim->sequence_length];
    }
    if (frame_index >= anim->frame_count) {
        frame_index = 0;
    }
    return &anim->frames[frame_index];
}

static void expression_lvgl_log_mouth_state(const char *stage, bool force)
{
    const expression_mouth_anim_t *anim = expression_lvgl_mouth_anim();
    const expression_mouth_frame_t *frame = expression_lvgl_current_mouth_frame();

    if (!force && s_mouth_debug_logged) {
        return;
    }
#if MOUTH_DEBUG_LOG
    if (frame != NULL) {
        const lv_point_precise_t *first = &frame->points[0];
        const lv_point_precise_t *last = &frame->points[frame->point_count - 1];
        ESP_LOGW(TAG,
                 "Mouth state stage=%s expression=%d sequence_index=%u frame_interval=%u "
                 "container_pos=(%d,%d) container_size=(%d,%d) point_count=%u "
                 "first_point=(%d,%d) last_point=(%d,%d)",
                 stage,
                 (int)s_mode,
                 (unsigned)s_mouth_sequence_index,
                 (unsigned)anim->frame_interval_ms,
                 (int)lv_obj_get_x(s_mouth_container),
                 (int)lv_obj_get_y(s_mouth_container),
                 (int)lv_obj_get_width(s_mouth_container),
                 (int)lv_obj_get_height(s_mouth_container),
                 (unsigned)frame->point_count,
                 (int)first->x,
                 (int)first->y,
                 (int)last->x,
                 (int)last->y);
    }
#else
    (void)stage;
    (void)anim;
    (void)frame;
#endif
    s_mouth_debug_logged = true;
}

static void expression_lvgl_apply_mouth_frame(bool force_log)
{
    const expression_mouth_anim_t *anim = expression_lvgl_mouth_anim();
    const expression_mouth_frame_t *frame = expression_lvgl_current_mouth_frame();

    if ((frame == NULL) || (s_mouth == NULL) || (s_mouth_container == NULL)) {
        return;
    }
    lv_obj_set_pos(s_mouth_container, anim->x, anim->y);
    lv_obj_set_size(s_mouth_container, anim->w, anim->h);
    lv_obj_set_pos(s_mouth, 0, 0);
    lv_obj_set_size(s_mouth, anim->w, anim->h);
    lv_line_set_points(s_mouth, frame->points, frame->point_count);
    lv_obj_set_style_line_width(s_mouth, anim->line_width, LV_PART_MAIN | LV_STATE_DEFAULT);
    expression_lvgl_log_mouth_state("apply_frame", force_log);
}

static void expression_lvgl_update_idle_pupils(void)
{
    const expression_style_t *style = expression_lvgl_style();
    int16_t pupil_size = style->pupil_size;
    int16_t pupil_radius = pupil_size / 2;
    int16_t eye_l_x = (int16_t)lv_obj_get_x(s_eye_l);
    int16_t eye_l_y = (int16_t)lv_obj_get_y(s_eye_l);
    int16_t eye_r_x = (int16_t)lv_obj_get_x(s_eye_r);
    int16_t eye_r_y = (int16_t)lv_obj_get_y(s_eye_r);
    int16_t eye_w = (int16_t)lv_obj_get_width(s_eye_l);
    int16_t eye_h = (int16_t)lv_obj_get_height(s_eye_l);
    int16_t max_x = (eye_w / 2) - pupil_radius - EXPRESSION_EYE_BORDER_WIDTH;
    int16_t max_y = (eye_h / 2) - pupil_radius - EXPRESSION_EYE_BORDER_WIDTH;
    int16_t offset_x;
    int16_t offset_y;

    if (max_x < 0) {
        max_x = 0;
    }
    if (max_y < 0) {
        max_y = 0;
    }

    offset_x = expression_lvgl_clamp_i16(s_idle_pupil_offset_x, -max_x, max_x);
    offset_y = expression_lvgl_clamp_i16(s_idle_pupil_offset_y, -max_y, max_y);

    lv_obj_set_size(s_pupil_l, pupil_size, pupil_size);
    lv_obj_set_size(s_pupil_r, pupil_size, pupil_size);
    lv_obj_set_pos(s_pupil_l,
                   eye_l_x + (eye_w / 2) - pupil_radius + offset_x,
                   eye_l_y + (eye_h / 2) - pupil_radius + offset_y);
    lv_obj_set_pos(s_pupil_r,
                   eye_r_x + (eye_w / 2) - pupil_radius + offset_x,
                   eye_r_y + (eye_h / 2) - pupil_radius + offset_y);
}

static void expression_lvgl_update_pupils(void)
{
    const expression_style_t *style = expression_lvgl_style();
    int16_t pupil_size = style->pupil_size;
    int16_t pupil_radius = pupil_size / 2;
    int16_t eye_l_x = (int16_t)lv_obj_get_x(s_eye_l);
    int16_t eye_l_y = (int16_t)lv_obj_get_y(s_eye_l);
    int16_t eye_r_x = (int16_t)lv_obj_get_x(s_eye_r);
    int16_t eye_r_y = (int16_t)lv_obj_get_y(s_eye_r);
    int16_t eye_w = (int16_t)lv_obj_get_width(s_eye_l);
    int16_t eye_h = (int16_t)lv_obj_get_height(s_eye_l);
    int16_t max_x = (eye_w / 2) - pupil_radius - EXPRESSION_EYE_BORDER_WIDTH;
    int16_t max_y = (eye_h / 2) - pupil_radius - EXPRESSION_EYE_BORDER_WIDTH;
    int16_t offset_x;
    int16_t offset_y;

    if (s_mode == DISPLAY_EXPRESSION_IDLE) {
        expression_lvgl_update_idle_pupils();
        return;
    }

    if (max_x < 0) {
        max_x = 0;
    }
    if (max_y < 0) {
        max_y = 0;
    }

    s_pupil_offset_x += (int16_t)((int)(esp_random() % 5U) - 2) *
                        (style->quick_scan ? EXPRESSION_PUPIL_STEP_PX : 1);
    s_pupil_offset_y += (int16_t)((int)(esp_random() % 5U) - 2);
    s_pupil_offset_x = expression_lvgl_clamp_i16(s_pupil_offset_x, -max_x, max_x);
    s_pupil_offset_y = expression_lvgl_clamp_i16(s_pupil_offset_y, -max_y, max_y);

    offset_x = s_pupil_offset_x;
    offset_y = expression_lvgl_clamp_i16((int16_t)(s_pupil_offset_y + style->pupil_bias_y), -max_y, max_y);

    lv_obj_set_size(s_pupil_l, pupil_size, pupil_size);
    lv_obj_set_size(s_pupil_r, pupil_size, pupil_size);
    lv_obj_set_pos(s_pupil_l,
                   eye_l_x + (eye_w / 2) - pupil_radius + offset_x,
                   eye_l_y + (eye_h / 2) - pupil_radius + offset_y);
    lv_obj_set_pos(s_pupil_r,
                   eye_r_x + (eye_w / 2) - pupil_radius + offset_x,
                   eye_r_y + (eye_h / 2) - pupil_radius + offset_y);
}

static void expression_lvgl_apply_base(bool reset_motion, bool update_mouth)
{
    const expression_style_t *style = expression_lvgl_style();
    int16_t left_center = EXPRESSION_EYE_LEFT_X + (EXPRESSION_EYE_BASE_W / 2);
    int16_t right_center = EXPRESSION_EYE_RIGHT_X + (EXPRESSION_EYE_BASE_W / 2);

    if (reset_motion) {
        s_pupil_offset_x = 0;
        s_pupil_offset_y = 0;
        s_elapsed_ms = 0;
        s_next_blink_ms = expression_lvgl_random_range(style->blink_min_ms, style->blink_jitter_ms);
        s_blinking = false;
        s_blink_phase = 0;
        s_motion_accumulator_ms = 0;
        s_mouth_frame_accumulator_ms = 0;
        s_mouth_sequence_index = 0;
        s_mouth_debug_frame_count = 0;
        if (s_mode == DISPLAY_EXPRESSION_IDLE) {
            expression_lvgl_reset_idle_gaze();
        }
    }
    s_eye_h_current = style->eye_h;
    expression_lvgl_position_eye(s_eye_l, left_center, style->eye_w, s_eye_h_current);
    expression_lvgl_position_eye(s_eye_r, right_center, style->eye_w, s_eye_h_current);
    if (update_mouth) {
        expression_lvgl_apply_mouth_frame(reset_motion);
    }
    expression_lvgl_update_pupils();
}

static void expression_lvgl_update_blink(uint32_t period_ms)
{
    const expression_style_t *style = expression_lvgl_style();
    int16_t left_center = EXPRESSION_EYE_LEFT_X + (EXPRESSION_EYE_BASE_W / 2);
    int16_t right_center = EXPRESSION_EYE_RIGHT_X + (EXPRESSION_EYE_BASE_W / 2);

    if (!style->blink_enabled) {
        return;
    }

    s_elapsed_ms += period_ms;
    if (!s_blinking && (s_elapsed_ms >= s_next_blink_ms)) {
        s_blinking = true;
        s_blink_phase = 1;
        s_elapsed_ms = 0;
    }

    if (!s_blinking) {
        return;
    }

    if (s_blink_phase == 1U) {
        s_eye_h_current -= EXPRESSION_BLINK_STEP_PX;
        if (s_eye_h_current <= EXPRESSION_BLINK_CLOSED_H) {
            s_eye_h_current = EXPRESSION_BLINK_CLOSED_H;
            s_blink_phase = 2;
        }
    } else if (s_blink_phase == 2U) {
        s_blink_phase = 3;
    } else {
        s_eye_h_current += EXPRESSION_BLINK_STEP_PX;
        if (s_eye_h_current >= style->eye_h) {
            s_eye_h_current = style->eye_h;
            s_blink_phase = 0;
            s_blinking = false;
            s_next_blink_ms = expression_lvgl_random_range(style->blink_min_ms, style->blink_jitter_ms);
        }
    }

    expression_lvgl_position_eye(s_eye_l, left_center, style->eye_w, s_eye_h_current);
    expression_lvgl_position_eye(s_eye_r, right_center, style->eye_w, s_eye_h_current);
}

static void expression_lvgl_timer_cb(lv_timer_t *timer)
{
    const expression_style_t *style = expression_lvgl_style();
    const expression_mouth_anim_t *mouth_anim = expression_lvgl_mouth_anim();

    (void)timer;

    if (!s_visible || (s_container == NULL)) {
        return;
    }

    s_motion_accumulator_ms = (uint16_t)(s_motion_accumulator_ms + EXPRESSION_TIMER_PERIOD_MS);
    if (s_motion_accumulator_ms >= style->timer_ms) {
        s_motion_accumulator_ms = 0;
        if (s_mode == DISPLAY_EXPRESSION_IDLE) {
            expression_lvgl_advance_idle_gaze(style->timer_ms);
        }
    }

    if (!s_blinking) {
        expression_lvgl_apply_base(false, false);
    }
    expression_lvgl_update_blink(EXPRESSION_TIMER_PERIOD_MS);
    expression_lvgl_update_pupils();

    s_mouth_frame_accumulator_ms = (uint16_t)(s_mouth_frame_accumulator_ms + EXPRESSION_TIMER_PERIOD_MS);
    if ((mouth_anim->sequence_length > 1) &&
        (s_mouth_frame_accumulator_ms >= mouth_anim->frame_interval_ms)) {
        s_mouth_frame_accumulator_ms = 0;
        s_mouth_sequence_index = (uint8_t)((s_mouth_sequence_index + 1U) % mouth_anim->sequence_length);
        s_mouth_debug_frame_count++;
        expression_lvgl_apply_mouth_frame(false);
#if MOUTH_DEBUG_LOG
        if ((s_mouth_debug_frame_count % 20U) == 0U) {
            expression_lvgl_log_mouth_state("periodic", true);
        }
#endif
    }
}

void expression_lvgl_init(lv_obj_t *parent)
{
    if ((s_container != NULL) || (parent == NULL)) {
        return;
    }

    s_container = lv_obj_create(parent);
    lv_obj_set_pos(s_container, 0, 0);
    lv_obj_set_size(s_container, DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_obj_set_style_bg_color(s_container, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(s_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);

    s_eye_l = lv_obj_create(s_container);
    s_eye_r = lv_obj_create(s_container);
    s_pupil_l = lv_obj_create(s_container);
    s_pupil_r = lv_obj_create(s_container);
    s_mouth_container = lv_obj_create(s_container);
    s_mouth = lv_line_create(s_mouth_container);

    expression_lvgl_style_eye(s_eye_l);
    expression_lvgl_style_eye(s_eye_r);
    expression_lvgl_style_pupil(s_pupil_l);
    expression_lvgl_style_pupil(s_pupil_r);

    lv_obj_remove_style_all(s_mouth_container);
    lv_obj_set_style_bg_opa(s_mouth_container, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_mouth_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_mouth_container, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_mouth_container, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_mouth_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_mouth_container, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
#if MOUTH_DEBUG_BOUNDS
    lv_obj_set_style_border_color(s_mouth_container, lv_color_hex(0x808080), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(s_mouth_container, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(s_mouth_container, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
#endif

    lv_obj_remove_style_all(s_mouth);
    lv_obj_set_pos(s_mouth, 0, 0);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_color(s_mouth, lv_color_white(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_line_rounded(s_mouth, true, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(s_mouth, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE);

    expression_lvgl_apply_base(true, true);
    s_timer = lv_timer_create(expression_lvgl_timer_cb, EXPRESSION_TIMER_PERIOD_MS, NULL);
}

void expression_lvgl_set_visible(bool visible)
{
    if (s_container == NULL) {
        return;
    }

    s_visible = visible;
    if (visible) {
        lv_obj_remove_flag(s_container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_container);
        if (s_mode == DISPLAY_EXPRESSION_IDLE) {
            s_mouth_debug_logged = false;
            expression_lvgl_reset_idle_gaze();
        }
        expression_lvgl_apply_base(false, true);
        lv_obj_update_layout(s_container);
    } else {
        lv_obj_add_flag(s_container, LV_OBJ_FLAG_HIDDEN);
    }
}

void expression_lvgl_set_mode(display_expression_kind_t mode)
{
    if (mode > DISPLAY_EXPRESSION_SAD) {
        mode = DISPLAY_EXPRESSION_IDLE;
    }
    if (s_mode == mode) {
        return;
    }

    s_mode = mode;
    s_mouth_debug_logged = false;
    expression_lvgl_apply_base(true, true);
}
