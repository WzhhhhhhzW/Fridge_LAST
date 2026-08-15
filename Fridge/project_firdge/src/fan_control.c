#include "fan_control.h"

#include "cold.h"
#include "oled.h"
#include "system_tick.h"
#include "variable.h"

#define FAN_PWM_CHANNEL                  (0U)
#define FAN_PWM_PERIOD_COUNTS            (4000U)
#define WATER_COOL_OUTPUT_PIN            (BSP_IO_PORT_06_PIN_02)

#define FAN_TACH_SAMPLE_MS               (1000U)
#define FAN_TACH_COUNTER_COUNTS          (0x10000UL)
#define FAN_TACH_CHANNEL_COUNT           (2U)

#define COOLING_PREHEAT_MS               (5000U)
#define COOLING_CONTROL_QUEUE_DEPTH      (8U)

#define FAN_FG1_COUNT_SOURCE_MASK        ((gpt_source_t) (GPT_SOURCE_GTIOCA_FALLING_WHILE_GTIOCB_LOW | \
                                                          GPT_SOURCE_GTIOCA_FALLING_WHILE_GTIOCB_HIGH))
#define FAN_FG2_COUNT_SOURCE_MASK        ((gpt_source_t) (GPT_SOURCE_GTIOCB_FALLING_WHILE_GTIOCA_LOW | \
                                                          GPT_SOURCE_GTIOCB_FALLING_WHILE_GTIOCA_HIGH))

typedef struct st_fan_tach_channel
{
    timer_instance_t const * p_timer;
    uint32_t previous_count;
    uint32_t rpm;
    uint8_t pulses_per_revolution;
    bool ready;
} fan_tach_channel_t;

typedef struct st_cooling_absolute_target
{
    uint8_t fan;
    bool water;
    bool cooling;
} cooling_absolute_target_t;

static uint8_t g_black_fan_speed = 0U;
static bool g_water_cooling_output_enabled = false;

static fan_tach_channel_t g_fan_tach[FAN_TACH_CHANNEL_COUNT];
static uint32_t g_fan_tach_last_sample_ms = 0U;

static cooling_interlock_state_t g_cooling_state = COOLING_INTERLOCK_OFF;
static bool g_cooling_preheat_timer_started = false;
static bool g_cooling_preheat_required = false;
static uint32_t g_cooling_preheat_start_ms = 0U;
static uint8_t g_cooling_previous_water = 0U;
static bool g_cooling_applied_water_enabled = false;

static volatile uint8_t g_cooling_command_queue[COOLING_CONTROL_QUEUE_DEPTH];
static volatile uint8_t g_cooling_command_head = 0U;
static volatile uint8_t g_cooling_command_tail = 0U;
static volatile uint8_t g_cooling_command_count = 0U;
static volatile bool g_cooling_command_overflow = false;

static volatile bool g_cooling_target_pending = false;
static volatile cooling_absolute_target_t g_cooling_pending_target;
static cooling_absolute_target_t g_cooling_last_target;
static bool g_cooling_last_target_valid = false;
static bool g_cooling_remote_rearm_required = false;

static bool fan_tach_source_is_valid(timer_instance_t const * p_timer,
                                     gpt_source_t required_source,
                                     uint8_t required_channel,
                                     bool use_gtioca)
{
    gpt_extended_cfg_t const * p_extend;
    gpt_capture_filter_t input_filter;
    bool output_enabled;

    if ((NULL == p_timer) || (NULL == p_timer->p_cfg) ||
        (NULL == p_timer->p_cfg->p_extend))
    {
        return false;
    }

    p_extend = (gpt_extended_cfg_t const *) p_timer->p_cfg->p_extend;
    input_filter = use_gtioca ? p_extend->capture_filter_gtioca :
                               p_extend->capture_filter_gtiocb;
    output_enabled = use_gtioca ? p_extend->gtioca.output_enabled :
                                  p_extend->gtiocb.output_enabled;
    return ((p_extend->count_up_source & required_source) == required_source) &&
           (GPT_SOURCE_NONE == p_extend->count_down_source) &&
           (GPT_CAPTURE_FILTER_PCLKD_DIV_64 == input_filter) &&
           (!output_enabled) &&
           (required_channel == p_timer->p_cfg->channel) &&
           (NULL == p_timer->p_cfg->p_callback) &&
           (BSP_IRQ_DISABLED == p_timer->p_cfg->cycle_end_ipl) &&
           (FAN_TACH_COUNTER_COUNTS == p_timer->p_cfg->period_counts);
}

static fsp_err_t fan_tach_channel_open(fan_tach_channel_t * p_channel,
                                       timer_instance_t const * p_timer,
                                       gpt_source_t required_source,
                                       uint8_t required_channel,
                                       bool use_gtioca,
                                       uint8_t pulses_per_revolution)
{
    fsp_err_t open_err;
    timer_status_t status;

    p_channel->p_timer = p_timer;
    p_channel->previous_count = 0U;
    p_channel->rpm = 0U;
    p_channel->pulses_per_revolution = pulses_per_revolution;
    p_channel->ready = false;

    if (!fan_tach_source_is_valid(p_timer,
                                  required_source,
                                  required_channel,
                                  use_gtioca))
    {
        return FSP_ERR_INVALID_MODE;
    }

    open_err = p_timer->p_api->open(p_timer->p_ctrl, p_timer->p_cfg);
    if ((FSP_SUCCESS != open_err) && (FSP_ERR_ALREADY_OPEN != open_err))
    {
        return open_err;
    }

    open_err = p_timer->p_api->reset(p_timer->p_ctrl);
    if (FSP_SUCCESS != open_err)
    {
        return open_err;
    }

    open_err = p_timer->p_api->start(p_timer->p_ctrl);
    if (FSP_SUCCESS != open_err)
    {
        return open_err;
    }

    open_err = p_timer->p_api->statusGet(p_timer->p_ctrl, &status);
    if (FSP_SUCCESS != open_err)
    {
        return open_err;
    }

    p_channel->previous_count = status.counter & 0xFFFFU;
    p_channel->ready = true;
    return FSP_SUCCESS;
}

static uint32_t fan_tach_calculate_rpm(fan_tach_channel_t * p_channel,
                                       uint32_t elapsed_ms)
{
    timer_status_t status;
    uint32_t current_count;
    uint32_t pulse_count;
    uint64_t denominator;
    uint64_t rpm;

    if ((!p_channel->ready) || (0U == elapsed_ms) ||
        (0U == p_channel->pulses_per_revolution))
    {
        return 0U;
    }

    if (FSP_SUCCESS != p_channel->p_timer->p_api->statusGet(p_channel->p_timer->p_ctrl,
                                                            &status))
    {
        return 0U;
    }

    current_count = status.counter & 0xFFFFU;
    if (current_count >= p_channel->previous_count)
    {
        pulse_count = current_count - p_channel->previous_count;
    }
    else
    {
        pulse_count = (FAN_TACH_COUNTER_COUNTS - p_channel->previous_count) +
                      current_count;
    }
    p_channel->previous_count = current_count;

    if (0U == pulse_count)
    {
        return 0U;
    }

    denominator = (uint64_t) elapsed_ms * p_channel->pulses_per_revolution;
    rpm = (((uint64_t) pulse_count * 60000ULL) + (denominator / 2ULL)) /
          denominator;

    /* The one-second pulse-counting window has 30 RPM resolution at 2 PPR.
     * Round the displayed value to 10 RPM to avoid meaningless last digits. */
    rpm = ((rpm + 5ULL) / 10ULL) * 10ULL;
    if (rpm > UINT32_MAX)
    {
        rpm = UINT32_MAX;
    }
    return (uint32_t) rpm;
}

static void cooling_force_colder_off(void)
{
    cool_state = false;
    Colder_DutySet(0U);
}

static void cooling_set_preheat_display(bool active)
{
    OLED_SetPreheatActive(active);
}

static void cooling_finish_stop(bool restore_preheat_snapshot)
{
    bool was_preheating = (COOLING_INTERLOCK_PREHEAT == g_cooling_state);

    cooling_force_colder_off();
    if (restore_preheat_snapshot && was_preheating)
    {
        Water_cool_State = g_cooling_previous_water;
    }

    g_cooling_state = COOLING_INTERLOCK_OFF;
    g_cooling_preheat_timer_started = false;
    g_cooling_preheat_required = false;
    cooling_set_preheat_display(false);
}

static void cooling_request_start(void)
{
    if (COOLING_INTERLOCK_OFF != g_cooling_state)
    {
        return;
    }

    g_cooling_previous_water = Water_cool_State;
    g_cooling_preheat_required = (1U != Water_cool_State);
    g_cooling_preheat_timer_started = false;
    cooling_force_colder_off();

    if (g_cooling_preheat_required)
    {
        Water_cool_State = 1U;
        g_cooling_state = COOLING_INTERLOCK_PREHEAT;
        cooling_set_preheat_display(true);
        return;
    }

    if (g_cooling_applied_water_enabled)
    {
        cool_state = true;
        g_cooling_state = COOLING_INTERLOCK_RUNNING;
        cooling_set_preheat_display(false);
    }
    else
    {
        /* Water is requested on, but the GPIO update has not yet been
         * observed. Enable cooling as soon as it is applied; this path does
         * not add an unnecessary five-second delay. */
        g_cooling_state = COOLING_INTERLOCK_PREHEAT;
        cooling_set_preheat_display(false);
    }
}

static void cooling_handle_fan_command(void)
{
    /* The P415 box fan is independent of the P602-only cooling interlock. */
    fan_state = (fan_state < 2U) ? (uint8_t) (fan_state + 1U) : 0U;
}

static void cooling_handle_water_command(void)
{
    uint8_t next_water = (0U == Water_cool_State) ? 1U : 0U;

    if ((0U == next_water) && (COOLING_INTERLOCK_OFF != g_cooling_state))
    {
        cooling_finish_stop(COOLING_INTERLOCK_PREHEAT == g_cooling_state);
        g_cooling_remote_rearm_required = true;
    }
    Water_cool_State = next_water;
}

static void cooling_handle_command(uint8_t code)
{
    if (0x03U == code)
    {
        cooling_handle_fan_command();
    }
    else if ((0x04U == code) || (0x09U == code))
    {
        if (COOLING_INTERLOCK_OFF == g_cooling_state)
        {
            g_cooling_remote_rearm_required = false;
            cooling_request_start();
        }
        else
        {
            cooling_finish_stop(COOLING_INTERLOCK_PREHEAT == g_cooling_state);
        }
    }
    else if (0x0AU == code)
    {
        cooling_handle_water_command();
    }
}

static bool cooling_command_take(uint8_t * p_code)
{
    bool have_command = false;
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    if (g_cooling_command_count > 0U)
    {
        *p_code = g_cooling_command_queue[g_cooling_command_head];
        g_cooling_command_head =
            (uint8_t) ((g_cooling_command_head + 1U) % COOLING_CONTROL_QUEUE_DEPTH);
        g_cooling_command_count--;
        have_command = true;
    }
    FSP_CRITICAL_SECTION_EXIT;
    return have_command;
}

static bool cooling_target_take(cooling_absolute_target_t * p_target)
{
    bool have_target = false;
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    if (g_cooling_target_pending)
    {
        p_target->fan = g_cooling_pending_target.fan;
        p_target->water = g_cooling_pending_target.water;
        p_target->cooling = g_cooling_pending_target.cooling;
        g_cooling_target_pending = false;
        have_target = true;
    }
    FSP_CRITICAL_SECTION_EXIT;
    return have_target;
}

static void cooling_apply_absolute_target(cooling_absolute_target_t const * p_target)
{
    bool cooling_rising;
    bool cooling_falling;
    bool fan_changed;
    bool water_changed;
    bool water_falling;

    cooling_rising = p_target->cooling &&
                     ((!g_cooling_last_target_valid) ||
                      (!g_cooling_last_target.cooling));
    cooling_falling = g_cooling_last_target_valid &&
                      g_cooling_last_target.cooling &&
                      (!p_target->cooling);
    fan_changed = (!g_cooling_last_target_valid) ||
                  (g_cooling_last_target.fan != p_target->fan);
    water_changed = (!g_cooling_last_target_valid) ||
                    (g_cooling_last_target.water != p_target->water);
    water_falling = g_cooling_last_target_valid &&
                    g_cooling_last_target.water &&
                    (!p_target->water);

    /* A remote fan edge always applies independently.  Repeated heartbeat
     * values do not overwrite a local 0x03 change because fan_changed is
     * false until the server target itself changes. */
    if (fan_changed)
    {
        fan_state = p_target->fan;
    }

    if (!p_target->cooling)
    {
        /* A normal server heartbeat echoes cool_state=0 throughout a locally
         * requested preheat.  Only a real remote 1 -> 0 edge may cancel that
         * local request; otherwise every one-second heartbeat would make a
         * five-second local preheat impossible. */
        if ((COOLING_INTERLOCK_OFF != g_cooling_state) &&
            (cooling_falling || water_falling))
        {
            cooling_finish_stop(COOLING_INTERLOCK_PREHEAT == g_cooling_state);
        }

        if (COOLING_INTERLOCK_OFF == g_cooling_state)
        {
            /* The server returns the same absolute target on every status
             * heartbeat.  Reapplying an unchanged value here used to undo a
             * local HMI 0x03/0x0A command one second later.  Only a real
             * remote target edge is allowed to take ownership. */
            if (water_changed)
            {
                Water_cool_State = p_target->water ? 1U : 0U;
            }
        }
        g_cooling_remote_rearm_required = false;
    }
    else if ((COOLING_INTERLOCK_OFF != g_cooling_state) &&
             water_falling)
    {
        /* A water-off edge has priority over a stale cooling=1 target. */
        cooling_finish_stop(COOLING_INTERLOCK_PREHEAT == g_cooling_state);
        Water_cool_State = p_target->water ? 1U : 0U;
        g_cooling_remote_rearm_required = true;
    }
    else if ((COOLING_INTERLOCK_OFF == g_cooling_state) &&
             cooling_rising &&
              (!g_cooling_remote_rearm_required))
    {
        Water_cool_State = p_target->water ? 1U : 0U;
        cooling_request_start();
    }

    g_cooling_last_target = *p_target;
    g_cooling_last_target_valid = true;
}

void fan_init(void)
{
    (void) Black_Fan.p_api->open(Black_Fan.p_ctrl, Black_Fan.p_cfg);
    (void) Black_Fan.p_api->periodSet(Black_Fan.p_ctrl, FAN_PWM_PERIOD_COUNTS);
    (void) Black_Fan.p_api->dutyCycleSet(Black_Fan.p_ctrl, 0U, FAN_PWM_CHANNEL);
    (void) Black_Fan.p_api->start(Black_Fan.p_ctrl);

    black_fan_set_speed(0U);
    (void) water_cooling_set_enabled(false);
}

void black_fan_set_speed(uint8_t speed_percent)
{
    uint32_t duty_cycle;

    if (speed_percent > 100U)
    {
        speed_percent = 100U;
    }

    g_black_fan_speed = speed_percent;
    duty_cycle = (FAN_PWM_PERIOD_COUNTS * speed_percent) / 100U;
    (void) Black_Fan.p_api->dutyCycleSet(Black_Fan.p_ctrl,
                                        duty_cycle,
                                        FAN_PWM_CHANNEL);
}

uint8_t fan_get_speed(void)
{
    return g_black_fan_speed;
}

fsp_err_t water_cooling_set_enabled(bool enabled)
{
    fsp_err_t pin_write_err;

    pin_write_err = R_IOPORT_PinWrite(&g_ioport_ctrl,
                                      WATER_COOL_OUTPUT_PIN,
                                      enabled ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
    if (FSP_SUCCESS == pin_write_err)
    {
        g_water_cooling_output_enabled = enabled;
    }

    return pin_write_err;
}

bool water_cooling_output_is_enabled(void)
{
    return g_water_cooling_output_enabled;
}

fsp_err_t fan_tach_init(void)
{
    fsp_err_t first_err;
    fsp_err_t second_err;

    first_err = fan_tach_channel_open(&g_fan_tach[0],
                                      &fan_fg1,
                                      FAN_FG1_COUNT_SOURCE_MASK,
                                      7U,
                                      true,
                                      FAN_FG1_PULSES_PER_REV);
    second_err = fan_tach_channel_open(&g_fan_tach[1],
                                       &fan_fg2,
                                       FAN_FG2_COUNT_SOURCE_MASK,
                                       8U,
                                       false,
                                       FAN_FG2_PULSES_PER_REV);
    g_fan_tach_last_sample_ms = system_tick_now();

    if (FSP_SUCCESS != first_err)
    {
        return first_err;
    }
    return second_err;
}

void fan_tach_process(void)
{
    uint32_t now = system_tick_now();
    uint32_t elapsed_ms = now - g_fan_tach_last_sample_ms;

    if (elapsed_ms < FAN_TACH_SAMPLE_MS)
    {
        return;
    }

    g_fan_tach[0].rpm = fan_tach_calculate_rpm(&g_fan_tach[0], elapsed_ms);
    g_fan_tach[1].rpm = fan_tach_calculate_rpm(&g_fan_tach[1], elapsed_ms);
    g_fan_tach_last_sample_ms = now;
}

uint32_t fan_tach_get_rpm(uint8_t fan_index)
{
    if ((0U == fan_index) || (fan_index > FAN_TACH_CHANNEL_COUNT))
    {
        return 0U;
    }
    return g_fan_tach[fan_index - 1U].rpm;
}

fsp_err_t cooling_interlock_init(void)
{
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    g_cooling_command_head = 0U;
    g_cooling_command_tail = 0U;
    g_cooling_command_count = 0U;
    g_cooling_command_overflow = false;
    g_cooling_target_pending = false;
    FSP_CRITICAL_SECTION_EXIT;

    g_cooling_state = COOLING_INTERLOCK_OFF;
    g_cooling_preheat_timer_started = false;
    g_cooling_preheat_required = false;
    g_cooling_last_target_valid = false;
    g_cooling_remote_rearm_required = false;
    cooling_force_colder_off();
    cooling_set_preheat_display(false);
    return FSP_SUCCESS;
}

void cooling_interlock_submit_code(uint8_t code)
{
    FSP_CRITICAL_SECTION_DEFINE;

    if ((0x03U != code) && (0x04U != code) &&
        (0x09U != code) && (0x0AU != code))
    {
        return;
    }

    FSP_CRITICAL_SECTION_ENTER;
    if (g_cooling_command_count < COOLING_CONTROL_QUEUE_DEPTH)
    {
        g_cooling_command_queue[g_cooling_command_tail] = code;
        g_cooling_command_tail =
            (uint8_t) ((g_cooling_command_tail + 1U) % COOLING_CONTROL_QUEUE_DEPTH);
        g_cooling_command_count++;
    }
    else
    {
        g_cooling_command_overflow = true;
    }
    FSP_CRITICAL_SECTION_EXIT;
}

void cooling_interlock_set_targets(uint8_t fan, bool water, bool cooling)
{
    FSP_CRITICAL_SECTION_DEFINE;

    if (fan > 2U)
    {
        fan = 2U;
    }

    FSP_CRITICAL_SECTION_ENTER;
    g_cooling_pending_target.fan = fan;
    g_cooling_pending_target.water = water;
    g_cooling_pending_target.cooling = cooling;
    g_cooling_target_pending = true;
    FSP_CRITICAL_SECTION_EXIT;
}

void cooling_interlock_process(void)
{
    cooling_absolute_target_t target;
    uint8_t code;
    bool overflowed;
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    overflowed = g_cooling_command_overflow;
    g_cooling_command_overflow = false;
    if (overflowed)
    {
        g_cooling_command_head = 0U;
        g_cooling_command_tail = 0U;
        g_cooling_command_count = 0U;
    }
    FSP_CRITICAL_SECTION_EXIT;

    if (overflowed)
    {
        /* A lost control command means the requested actuator ordering is no
         * longer trustworthy.  Fail closed instead of restoring a preheat
         * snapshot: P601 is stopped first by cooling_finish_stop(), then P602
         * is commanded off in this same main-loop pass.  P415 remains an
         * independent user-controlled output. */
        cooling_finish_stop(false);
        Water_cool_State = 0U;
        g_cooling_remote_rearm_required = true;
    }

    if (cooling_target_take(&target))
    {
        cooling_apply_absolute_target(&target);
    }

    while (cooling_command_take(&code))
    {
        cooling_handle_command(code);
    }

    if (COOLING_INTERLOCK_PREHEAT == g_cooling_state)
    {
        if (1U != Water_cool_State)
        {
            cooling_finish_stop(true);
            g_cooling_remote_rearm_required = true;
        }
        else if (g_cooling_preheat_required &&
                 g_cooling_preheat_timer_started &&
                 system_tick_elapsed(g_cooling_preheat_start_ms,
                                     COOLING_PREHEAT_MS))
        {
            cool_state = true;
            g_cooling_state = COOLING_INTERLOCK_RUNNING;
            g_cooling_preheat_timer_started = false;
            cooling_set_preheat_display(false);
        }
    }
    else if ((COOLING_INTERLOCK_RUNNING == g_cooling_state) &&
             (1U != Water_cool_State))
    {
        cooling_finish_stop(false);
        g_cooling_remote_rearm_required = true;
    }
}

void cooling_interlock_water_output_applied(bool enabled)
{
    g_cooling_applied_water_enabled = enabled;

    if ((COOLING_INTERLOCK_PREHEAT != g_cooling_state) ||
        (!enabled))
    {
        return;
    }

    if (!g_cooling_preheat_required)
    {
        cool_state = true;
        g_cooling_state = COOLING_INTERLOCK_RUNNING;
        cooling_set_preheat_display(false);
    }
    else if (!g_cooling_preheat_timer_started)
    {
        g_cooling_preheat_start_ms = system_tick_now();
        g_cooling_preheat_timer_started = true;
    }
}

void cooling_interlock_abort(void)
{
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    g_cooling_command_head = 0U;
    g_cooling_command_tail = 0U;
    g_cooling_command_count = 0U;
    g_cooling_command_overflow = false;
    g_cooling_target_pending = false;
    FSP_CRITICAL_SECTION_EXIT;

    cooling_force_colder_off();
    fan_state = 0U;
    Water_cool_State = 0U;
    g_cooling_state = COOLING_INTERLOCK_OFF;
    g_cooling_preheat_timer_started = false;
    g_cooling_preheat_required = false;
    g_cooling_remote_rearm_required = true;
    cooling_set_preheat_display(false);
}

cooling_interlock_state_t cooling_interlock_state_get(void)
{
    return g_cooling_state;
}
