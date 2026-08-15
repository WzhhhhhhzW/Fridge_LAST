#include "variable.h"
#include "system_tick.h"

#define DOOR_LOCK_PIN          BSP_IO_PORT_02_PIN_05
#define DOOR_SENSOR_PIN        BSP_IO_PORT_10_PIN_01
#define DOOR_UNLOCK_PULSE_MS   (500U)
#define DOOR_SENSOR_DEBOUNCE_MS (30U)
#define DOOR_LOCK_OUTPUT_CFG   ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW)

static volatile bool g_door_unlock_active = false;
static volatile uint32_t g_door_unlock_started_ms = 0U;
static bool g_door_sensor_initialized = false;
static bool g_door_sensor_candidate_closed = false;
static bool g_door_sensor_closed = false;
/*
 * A high PA01 level that was already present when an unlock pulse started is
 * not proof that the door has subsequently been closed.  Arm the closed
 * indication only after PA01 has first been stably low, then accept the next
 * stably-high level.
 */
static bool g_door_close_armed = false;
static uint32_t g_door_sensor_candidate_started_ms = 0U;

static void door_sensor_process(void)
{
    bsp_io_level_t level = BSP_IO_LEVEL_LOW;
    bool raw_closed;
    uint32_t now;

    if (FSP_SUCCESS != R_IOPORT_PinRead(&g_ioport_ctrl, DOOR_SENSOR_PIN, &level))
    {
        return;
    }

    raw_closed = (BSP_IO_LEVEL_HIGH == level);
    now = system_tick_now();

    if (!g_door_sensor_initialized)
    {
        g_door_sensor_candidate_closed = raw_closed;
        g_door_sensor_closed = raw_closed;
        g_door_sensor_candidate_started_ms = now;
        g_door_sensor_initialized = true;

        /* A low level intentionally leaves the existing p6 picture unchanged. */
        if (raw_closed && !g_door_unlock_active)
        {
            OLED_SetDoorClosed(true);
        }
        else if (!raw_closed)
        {
            g_door_close_armed = true;
        }
        return;
    }

    if (raw_closed != g_door_sensor_candidate_closed)
    {
        g_door_sensor_candidate_closed = raw_closed;
        g_door_sensor_candidate_started_ms = now;
        return;
    }

    if ((g_door_sensor_closed != g_door_sensor_candidate_closed) &&
        system_tick_elapsed(g_door_sensor_candidate_started_ms,
                            DOOR_SENSOR_DEBOUNCE_MS))
    {
        g_door_sensor_closed = g_door_sensor_candidate_closed;

        /* Low does not change p6; it only proves that the door has opened. */
        if (!g_door_sensor_closed)
        {
            g_door_close_armed = true;
        }
        else if (g_door_close_armed && !g_door_unlock_active)
        {
            OLED_SetDoorClosed(true);
            g_door_close_armed = false;
        }
    }
}

void Door_Init(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, DOOR_LOCK_PIN, DOOR_LOCK_OUTPUT_CFG);
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, DOOR_LOCK_PIN, BSP_IO_LEVEL_LOW);
    g_door_unlock_active = false;
    g_door_sensor_initialized = false;
    g_door_close_armed = false;
    door_sensor_process();
}

void door_open(void)
{
    (void) door_unlock_request();
}

bool door_unlock_request(void)
{
    /*
     * PA01 is active high for "door closed".  Once its debounced state is
     * low, the door is already physically open and another unlock pulse is
     * both unnecessary and potentially harmful.  Do not treat an
     * uninitialized sensor as open so startup behaviour remains unchanged.
     * Also never restart an active pulse, otherwise repeated commands could
     * extend P205 beyond the intended 500 ms safety bound.
     */
    if (g_door_unlock_active ||
        (g_door_sensor_initialized && !g_door_sensor_closed))
    {
        return false;
    }

    g_door_unlock_started_ms = system_tick_now();
    g_door_unlock_active = true;
    /* Discard a stale PA01 high.  If the door is already stably open, arm the
     * next high transition immediately; otherwise wait until a low is seen. */
    g_door_close_armed = g_door_sensor_initialized && !g_door_sensor_closed;
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, DOOR_LOCK_PIN, BSP_IO_LEVEL_HIGH);
    OLED_SetDoorClosed(false);
    return true;
}

void door_process(void)
{
    door_sensor_process();

    if (g_door_unlock_active &&
        system_tick_elapsed(g_door_unlock_started_ms, DOOR_UNLOCK_PULSE_MS))
    {
        door_close();
    }

    /* If a complete low -> high sensor transition occurred during the 500 ms
     * unlock pulse, publish it as soon as the pulse has ended. */
    if (g_door_close_armed && !g_door_unlock_active &&
        g_door_sensor_initialized && g_door_sensor_closed)
    {
        OLED_SetDoorClosed(true);
        g_door_close_armed = false;
    }
}

void door_tick_1ms(uint32_t now_ms)
{
    /*
     * KWS inference can occupy the foreground for more than 100 ms. Close the
     * lock pulse from the existing 1 ms timer interrupt so its high time is
     * bounded independently of foreground/network latency.
     */
    if (g_door_unlock_active &&
        ((uint32_t) (now_ms - g_door_unlock_started_ms) >= DOOR_UNLOCK_PULSE_MS))
    {
        (void) R_IOPORT_PinWrite(&g_ioport_ctrl, DOOR_LOCK_PIN, BSP_IO_LEVEL_LOW);
        g_door_unlock_active = false;
    }
}

void door_close(void)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, DOOR_LOCK_PIN, BSP_IO_LEVEL_LOW);
    g_door_unlock_active = false;
}

bool door_unlock_active(void)
{
    return g_door_unlock_active;
}

bool door_is_closed(void)
{
    return g_door_sensor_initialized && g_door_sensor_closed;
}
