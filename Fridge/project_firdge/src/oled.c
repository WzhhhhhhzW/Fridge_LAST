#include "variable.h"
#include "ina226_power.h"
#include "deepseek_bridge.h"

#define OLED_INA226_I2C_ADDRESS_N0          (0x40U)
#define OLED_INA226_I2C_ADDRESS_N3          (0x41U)
#define OLED_INA226_REG_SHUNT_VOLTAGE       (0x01U)
#define OLED_INA226_REG_BUS_VOLTAGE         (0x02U)
#define OLED_INA226_SHUNT_RESISTOR_UOHMS    (2000LL)
#define OLED_INA226_SCL_PIN                 BSP_IO_PORT_05_PIN_00
#define OLED_INA226_SDA_PIN                 BSP_IO_PORT_05_PIN_03
#define OLED_INA226_LINE_RELEASE_CFG        ((uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT)
#define OLED_INA226_LINE_LOW_CFG            ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                             (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW)
#define OLED_INA226_I2C_DELAY_US            (5U)
#define OLED_INA226_CLOCK_STRETCH_LOOPS     (200U)
#define OLED_HEALTH_ADVICE_MAX              (256U)
#define OLED_HEALTH_DIAGNOSIS_MAX           (97U)
#define OLED_HEALTH_MEDICINE_MAX            (161U)
#define OLED_INVENTORY_TEXT_MAX              (12U * 1024U)
#define OLED_INVENTORY_TX_SIZE               (OLED_INVENTORY_TEXT_MAX + 16U)
#define OLED_INVENTORY_POST_TX_GUARD_MS       (120U)
#define OLED_AUX_STATUS_TX_SIZE                (64U)

static char g_oled_health_advice[OLED_HEALTH_ADVICE_MAX];
static char g_oled_health_diagnosis[OLED_HEALTH_DIAGNOSIS_MAX];
static char g_oled_health_medicine[OLED_HEALTH_MEDICINE_MAX];
static uint8_t g_oled_health_tx_buf[TX_BUF_SIZE];
static char g_oled_inventory_text[OLED_INVENTORY_TEXT_MAX];
static uint8_t g_oled_inventory_tx_buf[OLED_INVENTORY_TX_SIZE];
static volatile bool g_oled_health_advice_pending = false;
static volatile bool g_oled_health_result_pending = false;
static volatile bool g_oled_inventory_pending = false;
static volatile bool g_oled_inventory_tx_in_progress = false;
static volatile bool g_oled_inventory_tx_complete_pending = false;
static volatile bool g_oled_inventory_post_tx_guard_active = false;
static volatile uint32_t g_oled_inventory_post_tx_guard_start_ms = 0U;
static volatile bool g_oled_uart_tx_busy = false;
static bool g_oled_inventory_write_error_reported = false;
static uint32_t g_oled_inventory_last_tx_length = 0U;
static uint8_t g_oled_power_phase = 0U;
static uint8_t g_oled_aux_status_tx_buf[OLED_AUX_STATUS_TX_SIZE];
static uint32_t g_oled_fan1_rpm = 0U;
static uint32_t g_oled_fan2_rpm = 0U;
static bool g_oled_fan1_rpm_pending = true;
static bool g_oled_fan2_rpm_pending = true;
static uint8_t g_oled_fan_state = 0U;
static bool g_oled_water_enabled = false;
static bool g_oled_actuator_state_initialized = false;
static bool g_oled_fan_state_pending = false;
static bool g_oled_water_state_pending = false;
static bool g_oled_preheat_active = false;
static bool g_oled_preheat_pending = true;
static bool g_oled_door_closed = false;
static bool g_oled_door_pending = false;

typedef enum e_oled_deepseek_navigation_phase
{
    OLED_DEEPSEEK_NAVIGATION_IDLE = 0,
    OLED_DEEPSEEK_NAVIGATION_PAGE,
    OLED_DEEPSEEK_NAVIGATION_CLEAR,
} oled_deepseek_navigation_phase_t;

static volatile oled_deepseek_navigation_phase_t g_oled_deepseek_navigation_phase =
    OLED_DEEPSEEK_NAVIGATION_IDLE;

static void oled_inventory_log(const char * status, uint32_t length, fsp_err_t write_err)
{
    char log_buf[112];

    (void) snprintf(log_buf,
                    sizeof(log_buf),
                    "\r\n[OLED] t20 %s: bytes=%lu, err=%d\r\n> ",
                    status,
                    (unsigned long) length,
                    (int) write_err);
    deepseek_bridge_pc_write(log_buf);
}

static void oled_report_inventory_tx_complete(void)
{
    uint32_t tx_length;

    if (!g_oled_inventory_tx_complete_pending)
    {
        return;
    }

    tx_length = g_oled_inventory_last_tx_length;
    g_oled_inventory_tx_complete_pending = false;
    oled_inventory_log("TX_COMPLETE", tx_length, FSP_SUCCESS);
}

static void oled_copy_safe_text(char * destination,
                                size_t destination_size,
                                const char * text,
                                const char * fallback)
{
    size_t read_index = 0U;
    size_t write_index = 0U;

    if ((NULL == destination) || (0U == destination_size))
    {
        return;
    }

    if (NULL == text)
    {
        text = fallback;
    }

    while ((text[read_index] != '\0') && (write_index < (destination_size - 1U)))
    {
        char ch = text[read_index++];
        if ((ch == '"') || (ch == '\r') || (ch == '\n'))
        {
            ch = ' ';
        }

        destination[write_index++] = ch;
    }

    destination[write_index] = '\0';
}

void OLED_SetHealthAdvice(const char * text)
{
    oled_copy_safe_text(g_oled_health_advice,
                        sizeof(g_oled_health_advice),
                        text,
                        "无建议");
    g_oled_health_advice_pending = true;
}

void OLED_ShowDeepseekPage(void)
{
    /* A new dialogue invalidates any result still waiting from the previous
     * one.  A fresh AUD1 HEALTH may arrive while page/clear is pending; the
     * scheduler below deliberately sends that result only after both steps. */
    g_oled_health_diagnosis[0] = '\0';
    g_oled_health_medicine[0] = '\0';
    g_oled_health_result_pending = false;
    g_oled_deepseek_navigation_phase = OLED_DEEPSEEK_NAVIGATION_PAGE;
}

void OLED_SetHealthResult(const char * diagnosis, const char * medicine)
{
    oled_copy_safe_text(g_oled_health_diagnosis,
                        sizeof(g_oled_health_diagnosis),
                        diagnosis,
                        "诊断失败");
    oled_copy_safe_text(g_oled_health_medicine,
                        sizeof(g_oled_health_medicine),
                        medicine,
                        "暂无用药建议");
    g_oled_health_result_pending = true;
}

void OLED_SetInventoryText(const char * text)
{
    size_t read_index = 0U;
    size_t write_index = 0U;

    if (NULL == text)
    {
        text = "库存读取失败";
    }

    while ((text[read_index] != '\0') &&
           (write_index < (sizeof(g_oled_inventory_text) - 1U)))
    {
        char ch = text[read_index++];
        if ('"' == ch)
        {
            ch = '\'';
        }

        g_oled_inventory_text[write_index++] = ch;
    }

    g_oled_inventory_text[write_index] = '\0';
    g_oled_inventory_write_error_reported = false;
    g_oled_inventory_pending = true;
    oled_inventory_log("prepared", (uint32_t) write_index, FSP_SUCCESS);
}

void OLED_SetFanRpm(uint32_t fan1_rpm, uint32_t fan2_rpm)
{
    if (g_oled_fan1_rpm != fan1_rpm)
    {
        g_oled_fan1_rpm = fan1_rpm;
        g_oled_fan1_rpm_pending = true;
    }

    if (g_oled_fan2_rpm != fan2_rpm)
    {
        g_oled_fan2_rpm = fan2_rpm;
        g_oled_fan2_rpm_pending = true;
    }
}

void OLED_SetFanWaterState(uint8_t fan_state_value, bool water_enabled)
{
    if (fan_state_value > 2U)
    {
        fan_state_value = 2U;
    }

    if ((!g_oled_actuator_state_initialized) ||
        (g_oled_fan_state != fan_state_value))
    {
        g_oled_fan_state = fan_state_value;
        g_oled_fan_state_pending = true;
    }

    if ((!g_oled_actuator_state_initialized) ||
        (g_oled_water_enabled != water_enabled))
    {
        g_oled_water_enabled = water_enabled;
        g_oled_water_state_pending = true;
    }

    g_oled_actuator_state_initialized = true;
}

void OLED_SetPreheatActive(bool active)
{
    if (g_oled_preheat_active != active)
    {
        g_oled_preheat_active = active;
        g_oled_preheat_pending = true;
    }
}

void OLED_SetDoorClosed(bool closed)
{
    /* Every unlock request must be acknowledged on p6 even if the previous
     * queued/displayed state was also open.  The single pending slot keeps the
     * newest physical state if the OLED UART is currently busy. */
    g_oled_door_closed = closed;
    g_oled_door_pending = true;
}

void OLED_NotifyTxComplete(void)
{
    g_oled_uart_tx_busy = false;
    if (g_oled_inventory_tx_in_progress)
    {
        g_oled_inventory_tx_in_progress = false;
        g_oled_inventory_tx_complete_pending = true;
        g_oled_inventory_post_tx_guard_start_ms = system_tick_now();
        g_oled_inventory_post_tx_guard_active = true;
    }
}

bool OLED_IsTxBusy(void)
{
    return g_oled_uart_tx_busy;
}

fsp_err_t OLED_TryWrite(uint8_t * data, uint32_t length)
{
    fsp_err_t write_err;

    if ((NULL == data) || (0U == length))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    if (g_oled_uart_tx_busy)
    {
        return FSP_ERR_IN_USE;
    }

    /* Set the flag before starting SCI so a very short TX cannot race its ISR. */
    g_oled_uart_tx_busy = true;
    write_err = R_SCI_UART_Write(&oled_control_ctrl, data, length);
    if (FSP_SUCCESS != write_err)
    {
        g_oled_uart_tx_busy = false;
    }
    return write_err;
}

bool OLED_IsInventoryTxProtected(void)
{
    return g_oled_inventory_pending ||
           g_oled_inventory_tx_in_progress ||
           g_oled_inventory_post_tx_guard_active;
}

static bool oled_send_inventory_if_needed(void)
{
    uint32_t tx_length;
    int written;

    if (!g_oled_inventory_pending)
    {
        return false;
    }

    written = snprintf((char *) g_oled_inventory_tx_buf,
                       OLED_INVENTORY_TX_SIZE - 3U,
                       "t20.txt=\"%s\"",
                       g_oled_inventory_text);
    if ((written < 0) || ((uint32_t) written >= (OLED_INVENTORY_TX_SIZE - 3U)))
    {
        return true;
    }

    tx_length = (uint32_t) written;
    g_oled_inventory_tx_buf[tx_length++] = 0xFF;
    g_oled_inventory_tx_buf[tx_length++] = 0xFF;
    g_oled_inventory_tx_buf[tx_length++] = 0xFF;

    err = OLED_TryWrite(g_oled_inventory_tx_buf, tx_length);
    if (FSP_SUCCESS == err)
    {
        g_oled_inventory_last_tx_length = tx_length;
        g_oled_inventory_tx_in_progress = true;
        g_oled_inventory_pending = false;
        oled_inventory_log("write accepted", tx_length, err);
    }
    else if (!g_oled_inventory_write_error_reported)
    {
        g_oled_inventory_write_error_reported = true;
        oled_inventory_log("write rejected", tx_length, err);
    }

    return true;
}

static bool oled_send_aux_status_if_needed(void)
{
    uint32_t tx_length;
    int written;
    bool * p_pending;

    if (g_oled_preheat_pending)
    {
        /* UTF-8 for the preheating status text, expressed as bytes so the
         * editor/compiler code page cannot alter the command. */
        const char * text = g_oled_preheat_active ?
            "\xE7\xB3\xBB\xE7\xBB\x9F\xE6\xAD\xA3\xE5\x9C\xA8\xE9\xA2\x84\xE7\x83\xAD" : "";
        written = snprintf((char *) g_oled_aux_status_tx_buf,
                           OLED_AUX_STATUS_TX_SIZE - 3U,
                           "t93.txt=\"%s\"",
                           text);
        p_pending = &g_oled_preheat_pending;
    }
    else if (g_oled_door_pending)
    {
        written = snprintf((char *) g_oled_aux_status_tx_buf,
                           OLED_AUX_STATUS_TX_SIZE - 3U,
                           "p6.pic=%u",
                           g_oled_door_closed ? 30U : 15U);
        p_pending = &g_oled_door_pending;
    }
    else if (g_oled_fan_state_pending)
    {
        static uint8_t const fan_picture_ids[] = {10U, 11U, 12U};

        written = snprintf((char *) g_oled_aux_status_tx_buf,
                           OLED_AUX_STATUS_TX_SIZE - 3U,
                           "p4.pic=%u",
                           (unsigned int) fan_picture_ids[g_oled_fan_state]);
        p_pending = &g_oled_fan_state_pending;
    }
    else if (g_oled_water_state_pending)
    {
        written = snprintf((char *) g_oled_aux_status_tx_buf,
                           OLED_AUX_STATUS_TX_SIZE - 3U,
                           "p1.pic=%u",
                           g_oled_water_enabled ? 41U : 42U);
        p_pending = &g_oled_water_state_pending;
    }
    else if (g_oled_fan1_rpm_pending)
    {
        written = snprintf((char *) g_oled_aux_status_tx_buf,
                           OLED_AUX_STATUS_TX_SIZE - 3U,
                           "n91.val=%lu",
                           (unsigned long) g_oled_fan1_rpm);
        p_pending = &g_oled_fan1_rpm_pending;
    }
    else if (g_oled_fan2_rpm_pending)
    {
        written = snprintf((char *) g_oled_aux_status_tx_buf,
                           OLED_AUX_STATUS_TX_SIZE - 3U,
                           "n92.val=%lu",
                           (unsigned long) g_oled_fan2_rpm);
        p_pending = &g_oled_fan2_rpm_pending;
    }
    else
    {
        return false;
    }

    if ((written < 0) || ((uint32_t) written >= (OLED_AUX_STATUS_TX_SIZE - 3U)))
    {
        return true;
    }

    tx_length = (uint32_t) written;
    g_oled_aux_status_tx_buf[tx_length++] = 0xFFU;
    g_oled_aux_status_tx_buf[tx_length++] = 0xFFU;
    g_oled_aux_status_tx_buf[tx_length++] = 0xFFU;
    if (FSP_SUCCESS == OLED_TryWrite(g_oled_aux_status_tx_buf, tx_length))
    {
        *p_pending = false;
    }

    return true;
}

static bool oled_send_health_result_if_needed(void)
{
    uint32_t tx_length;
    uint32_t remaining;
    int written;

    if (!g_oled_health_result_pending)
    {
        return false;
    }

    written = snprintf((char *) g_oled_health_tx_buf,
                       TX_BUF_SIZE - 3U,
                       "t16.txt=\"%s\"",
                       g_oled_health_diagnosis);
    if ((written < 0) || ((uint32_t) written >= (TX_BUF_SIZE - 3U)))
    {
        return true;
    }

    tx_length = (uint32_t) written;
    g_oled_health_tx_buf[tx_length++] = 0xFF;
    g_oled_health_tx_buf[tx_length++] = 0xFF;
    g_oled_health_tx_buf[tx_length++] = 0xFF;

    remaining = TX_BUF_SIZE - tx_length;
    if (remaining <= 3U)
    {
        return true;
    }

    written = snprintf((char *) &g_oled_health_tx_buf[tx_length],
                       (size_t) (remaining - 3U),
                       "t15.txt=\"%s\"",
                       g_oled_health_medicine);
    if ((written < 0) || ((uint32_t) written >= (remaining - 3U)))
    {
        return true;
    }

    tx_length += (uint32_t) written;
    g_oled_health_tx_buf[tx_length++] = 0xFF;
    g_oled_health_tx_buf[tx_length++] = 0xFF;
    g_oled_health_tx_buf[tx_length++] = 0xFF;

    err = OLED_TryWrite(g_oled_health_tx_buf, tx_length);
    if (FSP_SUCCESS == err)
    {
        g_oled_health_result_pending = false;
    }
    return true;
}

static bool oled_send_deepseek_navigation_if_needed(void)
{
    static uint8_t page_command[] =
    {
        'p', 'a', 'g', 'e', ' ', 'd', 'e', 'e', 'p', 's', 'e', 'e', 'k',
        0xFFU, 0xFFU, 0xFFU,
    };
    static uint8_t clear_command[] =
    {
        't', '1', '6', '.', 't', 'x', 't', '=', '"', '"',
        0xFFU, 0xFFU, 0xFFU,
        't', '1', '5', '.', 't', 'x', 't', '=', '"', '"',
        0xFFU, 0xFFU, 0xFFU,
    };
    fsp_err_t write_err;

    if (OLED_DEEPSEEK_NAVIGATION_IDLE == g_oled_deepseek_navigation_phase)
    {
        return false;
    }

    if (OLED_DEEPSEEK_NAVIGATION_PAGE == g_oled_deepseek_navigation_phase)
    {
        write_err = OLED_TryWrite(page_command, sizeof(page_command));
        if (FSP_SUCCESS == write_err)
        {
            g_oled_deepseek_navigation_phase = OLED_DEEPSEEK_NAVIGATION_CLEAR;
        }
    }
    else
    {
        write_err = OLED_TryWrite(clear_command, sizeof(clear_command));
        if (FSP_SUCCESS == write_err)
        {
            g_oled_deepseek_navigation_phase = OLED_DEEPSEEK_NAVIGATION_IDLE;
        }
    }

    return true;
}

static void oled_send_health_advice_if_needed(void)
{
    int advice_len;
    uint32_t advice_tx_len;

    if (!g_oled_health_advice_pending)
    {
        return;
    }

    advice_len = snprintf((char *) g_oled_health_tx_buf,
                          TX_BUF_SIZE - 4,
                          "t11.txt=\"%s\"",
                          g_oled_health_advice);
    if (advice_len < 0)
    {
        return;
    }

    advice_tx_len = (uint32_t) advice_len;
    if (advice_tx_len >= (TX_BUF_SIZE - 4))
    {
        advice_tx_len = TX_BUF_SIZE - 4;
    }

    g_oled_health_tx_buf[advice_tx_len++] = 0xFF;
    g_oled_health_tx_buf[advice_tx_len++] = 0xFF;
    g_oled_health_tx_buf[advice_tx_len++] = 0xFF;
    err = OLED_TryWrite(g_oled_health_tx_buf, advice_tx_len);
    if (FSP_SUCCESS == err)
    {
        g_oled_health_advice_pending = false;
    }
}

/* Legacy local INA226 reader is disabled. ina226_power.c now owns the software I2C bus. */
#if 0
static void oled_ina226_i2c_delay(void)
{
    R_BSP_SoftwareDelay(OLED_INA226_I2C_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
}

static void oled_ina226_scl_release(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, OLED_INA226_SCL_PIN, OLED_INA226_LINE_RELEASE_CFG);
}

static void oled_ina226_scl_low(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, OLED_INA226_SCL_PIN, OLED_INA226_LINE_LOW_CFG);
}

static void oled_ina226_sda_release(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, OLED_INA226_SDA_PIN, OLED_INA226_LINE_RELEASE_CFG);
}

static void oled_ina226_sda_low(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, OLED_INA226_SDA_PIN, OLED_INA226_LINE_LOW_CFG);
}

static bool oled_ina226_read_pin(bsp_io_port_pin_t pin)
{
    bsp_io_level_t level = BSP_IO_LEVEL_LOW;

    (void) R_IOPORT_PinRead(&g_ioport_ctrl, pin, &level);
    return (BSP_IO_LEVEL_HIGH == level);
}

static fsp_err_t oled_ina226_wait_scl_high(void)
{
    oled_ina226_scl_release();
    for (uint32_t i = 0U; i < OLED_INA226_CLOCK_STRETCH_LOOPS; i++)
    {
        if (oled_ina226_read_pin(OLED_INA226_SCL_PIN))
        {
            return FSP_SUCCESS;
        }

        oled_ina226_i2c_delay();
    }

    return FSP_ERR_TIMEOUT;
}

static fsp_err_t oled_ina226_i2c_start(void)
{
    fsp_err_t i2c_err;

    oled_ina226_sda_release();
    i2c_err = oled_ina226_wait_scl_high();
    if (FSP_SUCCESS != i2c_err)
    {
        return i2c_err;
    }

    oled_ina226_i2c_delay();
    oled_ina226_sda_low();
    oled_ina226_i2c_delay();
    oled_ina226_scl_low();
    oled_ina226_i2c_delay();

    return FSP_SUCCESS;
}

static void oled_ina226_i2c_stop(void)
{
    oled_ina226_sda_low();
    oled_ina226_i2c_delay();
    oled_ina226_scl_release();
    (void) oled_ina226_wait_scl_high();
    oled_ina226_i2c_delay();
    oled_ina226_sda_release();
    oled_ina226_i2c_delay();
}

static fsp_err_t oled_ina226_i2c_write_bit(bool bit_value)
{
    fsp_err_t i2c_err;

    if (bit_value)
    {
        oled_ina226_sda_release();
    }
    else
    {
        oled_ina226_sda_low();
    }

    oled_ina226_i2c_delay();
    i2c_err = oled_ina226_wait_scl_high();
    if (FSP_SUCCESS != i2c_err)
    {
        return i2c_err;
    }

    oled_ina226_i2c_delay();
    oled_ina226_scl_low();
    oled_ina226_i2c_delay();

    return FSP_SUCCESS;
}

static fsp_err_t oled_ina226_i2c_read_bit(bool * p_bit_value)
{
    fsp_err_t i2c_err;

    if (NULL == p_bit_value)
    {
        return FSP_ERR_ASSERTION;
    }

    oled_ina226_sda_release();
    oled_ina226_i2c_delay();
    i2c_err = oled_ina226_wait_scl_high();
    if (FSP_SUCCESS != i2c_err)
    {
        return i2c_err;
    }

    oled_ina226_i2c_delay();
    *p_bit_value = oled_ina226_read_pin(OLED_INA226_SDA_PIN);
    oled_ina226_scl_low();
    oled_ina226_i2c_delay();

    return FSP_SUCCESS;
}

static fsp_err_t oled_ina226_i2c_write_byte(uint8_t value)
{
    fsp_err_t i2c_err;

    for (uint8_t mask = 0x80U; mask > 0U; mask >>= 1)
    {
        i2c_err = oled_ina226_i2c_write_bit(0U != (value & mask));
        if (FSP_SUCCESS != i2c_err)
        {
            return i2c_err;
        }
    }

    oled_ina226_sda_release();
    oled_ina226_i2c_delay();
    i2c_err = oled_ina226_wait_scl_high();
    if (FSP_SUCCESS != i2c_err)
    {
        return i2c_err;
    }

    oled_ina226_i2c_delay();
    if (oled_ina226_read_pin(OLED_INA226_SDA_PIN))
    {
        oled_ina226_scl_low();
        oled_ina226_i2c_delay();
        return FSP_ERR_ABORTED;
    }

    oled_ina226_scl_low();
    oled_ina226_i2c_delay();

    return FSP_SUCCESS;
}

static fsp_err_t oled_ina226_i2c_read_byte(uint8_t * p_value, bool send_ack)
{
    fsp_err_t i2c_err;
    uint8_t value = 0U;
    bool bit_value = false;

    if (NULL == p_value)
    {
        return FSP_ERR_ASSERTION;
    }

    for (uint8_t i = 0U; i < 8U; i++)
    {
        value = (uint8_t) (value << 1);
        i2c_err = oled_ina226_i2c_read_bit(&bit_value);
        if (FSP_SUCCESS != i2c_err)
        {
            return i2c_err;
        }

        if (bit_value)
        {
            value |= 1U;
        }
    }

    i2c_err = oled_ina226_i2c_write_bit(!send_ack);
    if (FSP_SUCCESS != i2c_err)
    {
        return i2c_err;
    }

    *p_value = value;
    return FSP_SUCCESS;
}

static fsp_err_t oled_ina226_read_register(uint8_t i2c_address, uint8_t reg, uint16_t * p_value)
{
    fsp_err_t i2c_err;
    uint8_t msb = 0U;
    uint8_t lsb = 0U;

    if (NULL == p_value)
    {
        return FSP_ERR_ASSERTION;
    }

    i2c_err = oled_ina226_i2c_start();
    if (FSP_SUCCESS != i2c_err)
    {
        oled_ina226_i2c_stop();
        return i2c_err;
    }

    i2c_err = oled_ina226_i2c_write_byte((uint8_t) (i2c_address << 1));
    if (FSP_SUCCESS == i2c_err)
    {
        i2c_err = oled_ina226_i2c_write_byte(reg);
    }

    if (FSP_SUCCESS == i2c_err)
    {
        i2c_err = oled_ina226_i2c_start();
    }

    if (FSP_SUCCESS == i2c_err)
    {
        i2c_err = oled_ina226_i2c_write_byte((uint8_t) ((i2c_address << 1) | 1U));
    }

    if (FSP_SUCCESS == i2c_err)
    {
        i2c_err = oled_ina226_i2c_read_byte(&msb, true);
    }

    if (FSP_SUCCESS == i2c_err)
    {
        i2c_err = oled_ina226_i2c_read_byte(&lsb, false);
    }

    oled_ina226_i2c_stop();

    if (FSP_SUCCESS != i2c_err)
    {
        return i2c_err;
    }

    *p_value = (uint16_t) (((uint16_t) msb << 8) | lsb);
    return FSP_SUCCESS;
}

static bool oled_ina226_read_power_dW(uint8_t i2c_address, int32_t * p_power_dW)
{
    uint16_t shunt_raw_u16 = 0U;
    uint16_t bus_raw = 0U;
    int16_t shunt_raw;
    int32_t shunt_uv_x10;
    int32_t bus_mv;
    int32_t current_mA;
    int64_t power_uW;

    if (NULL == p_power_dW)
    {
        return false;
    }

    if (FSP_SUCCESS != oled_ina226_read_register(i2c_address, OLED_INA226_REG_SHUNT_VOLTAGE, &shunt_raw_u16))
    {
        return false;
    }

    if (FSP_SUCCESS != oled_ina226_read_register(i2c_address, OLED_INA226_REG_BUS_VOLTAGE, &bus_raw))
    {
        return false;
    }

    shunt_raw = (int16_t) shunt_raw_u16;
    shunt_uv_x10 = (int32_t) shunt_raw * 25;
    bus_mv = (int32_t) ((((int64_t) bus_raw * 125LL) + 50LL) / 100LL);
    current_mA = (int32_t) (((int64_t) shunt_uv_x10 * 100LL) / OLED_INA226_SHUNT_RESISTOR_UOHMS);
    power_uW = (int64_t) bus_mv * (int64_t) current_mA;
    *p_power_dW = (power_uW >= 0LL) ?
                  (int32_t) ((power_uW + 50000LL) / 100000LL) :
                  (int32_t) ((power_uW - 50000LL) / 100000LL);

    if (*p_power_dW < 0)
    {
        *p_power_dW = -*p_power_dW;
    }

    return true;
}
#endif

static fsp_err_t oled_send_ina226_power(uint8_t i2c_address, const char * p_object_name)
{
    ina226_power_sample_t sample = {0};
    int32_t power_dW = 0;
    int32_t power_w = 0;
    int power_len;

    if (ina226_power_get_latest(i2c_address, &sample))
    {
        power_dW = sample.power_dW;
        if (power_dW < 0)
        {
            power_dW = -power_dW;
        }

        power_w = power_dW / 10;
    }

    power_len = snprintf((char *)tx_buf4,
                         TX_BUF_SIZE - 4,
                         "%s.val=%ld",
                         p_object_name,
                         (long) power_w);
    if (power_len < 0)
    {
        power_len = 0;
    }

    len4 = (uint32_t) power_len;
    if (len4 >= (TX_BUF_SIZE - 4))
    {
        len4 = TX_BUF_SIZE - 4;
    }

    tx_buf4[len4++] = 0xFF;
    tx_buf4[len4++] = 0xFF;
    tx_buf4[len4++] = 0xFF;
    return OLED_TryWrite(tx_buf4, len4);
}

void OLED_Start()
{
    oled_report_inventory_tx_complete();

    /*
     * A long t20 assignment needs time to be parsed after the final byte leaves
     * the SCI peripheral.  Keep all regular screen refreshes silent while the
     * inventory frame is being transmitted and for a short guard time after
     * TX_COMPLETE, otherwise the following temperature/fan command can arrive
     * too soon and make some T/K series screens discard the long command.
     */
    if (g_oled_inventory_tx_in_progress)
    {
        return;
    }

    /* Every SCI transfer is completed by interrupt; never submit a second
     * command while the previous command is still leaving the UART. */
    if (OLED_IsTxBusy())
    {
        return;
    }

    if (g_oled_inventory_post_tx_guard_active)
    {
        if (!system_tick_elapsed(g_oled_inventory_post_tx_guard_start_ms,
                                 OLED_INVENTORY_POST_TX_GUARD_MS))
        {
            return;
        }

        g_oled_inventory_post_tx_guard_active = false;
    }

    /* Inventory remains exclusive.  A completed microphone upload owns the
     * next slots so the new page and its cleared controls are always visible
     * before a fast server result. */
    if (!OLED_IsInventoryTxProtected() && oled_send_deepseek_navigation_if_needed())
    {
        return;
    }

    if (!OLED_IsInventoryTxProtected() && oled_send_health_result_if_needed())
    {
        return;
    }

    /* Preheat status is next, followed by the door picture, tachometer values,
     * and ordinary refresh. */
    if (!OLED_IsInventoryTxProtected() && oled_send_aux_status_if_needed())
    {
        return;
    }

    switch(OLED_Send_Mode)//检测串口数据模式
    {
        case 0://发送设定温度数据
            OLED_Send_Mode = 6;//设定为无状态
            len = (uint32_t) snprintf((char *)tx_buf,
                           TX_BUF_SIZE - 4,  // 预留空间
                           "n1.val=%d",  //%lu为无符号整形
                           set_temp);
            // 3. 检查长度是否安全
            if (len >= (TX_BUF_SIZE - 4))
            {
                len = TX_BUF_SIZE - 4;  // 截断
            }
            // 4. 追加 3 个 0xFF
            tx_buf[len++] = 0xFF;
            tx_buf[len++] = 0xFF;
            tx_buf[len++] = 0xFF;
            //不可以用uart_send_string
            err = OLED_TryWrite(tx_buf, len);
            break;
        case 1://发送当前温度数据
            OLED_Send_Mode = 6;//设定为无状态
            err = lm35_read_temperature(&temperature);
            if (err != FSP_SUCCESS)
            {
              //保持上次的值
            }
            len1 = (uint32_t) snprintf((char *)tx_buf1,
                           TX_BUF_SIZE - 4,  // 预留空间
                           //"n2.val=%lu",
                           "n2.val=%d",
                           (int)temperature);
            // 3. 检查长度是否安全
            if (len1 >= (TX_BUF_SIZE - 4))
            {
                len1 = TX_BUF_SIZE - 4;  // 截断
            }
            // 4. 追加 3 个 0xFF
            tx_buf1[len1++] = 0xFF;
            tx_buf1[len1++] = 0xFF;
            tx_buf1[len1++] = 0xFF;
            err = OLED_TryWrite(tx_buf1, len1);
            break;
        case 2://发送风扇控制数据
            OLED_Send_Mode = 6;//设定为无状态
            if (fan_state == 0)
            {
                len2 = (uint32_t) snprintf((char *)tx_buf2,
                               TX_BUF_SIZE - 4,  // 预留空间
                               "p4.pic=10"//无风
                               );
            }
            else if (fan_state == 1)
            {
                len2 = (uint32_t) snprintf((char *)tx_buf2,
                               TX_BUF_SIZE - 4,  // 预留空间
                               "p4.pic=11"//弱风
                               );
            }
            else if (fan_state == 2)
            {
                len2 = (uint32_t) snprintf((char *)tx_buf2,
                               TX_BUF_SIZE - 4,  // 预留空间
                               "p4.pic=12"//强风
                               );
            }
            // 3. 检查长度是否安全
            if (len2 >= (TX_BUF_SIZE - 4))
            {
                len2 = TX_BUF_SIZE - 4;  // 截断
            }
            // 4. 追加 3 个 0xFF
            tx_buf2[len2++] = 0xFF;
            tx_buf2[len2++] = 0xFF;
            tx_buf2[len2++] = 0xFF;
            err = OLED_TryWrite(tx_buf2, len2);
            break;
        case 3://发送制冷片开关数据
            OLED_Send_Mode = 6;//设定为无状态
            if (cool_state == false)
            {
                len3 = (uint32_t) snprintf((char *)tx_buf3,
                               TX_BUF_SIZE - 4,  // 预留空间
                               "p5.pic=13"  //关
                               );
            }
            else if (cool_state == true)
            {
                len3 = (uint32_t) snprintf((char *)tx_buf3,
                               TX_BUF_SIZE - 4,  // 预留空间
                               "p5.pic=14"//开
                               );
            }
            // 3. 检查长度是否安全
            if (len3 >= (TX_BUF_SIZE - 4))
            {
                len3 = TX_BUF_SIZE - 4;  // 截断
            }
            // 4. 追加 3 个 0xFF
            tx_buf3[len3++] = 0xFF;
            tx_buf3[len3++] = 0xFF;
            tx_buf3[len3++] = 0xFF;
            err = OLED_TryWrite(tx_buf3, len3);
            break;
        case 4://发送当前功率数据
            OLED_Send_Mode = 6;//设定为无状态
            if (!oled_send_inventory_if_needed() &&
                !oled_send_health_result_if_needed())
            {
                if (g_oled_health_advice_pending)
                {
                    oled_send_health_advice_if_needed();
                }
                else if (0U == g_oled_power_phase)
                {
                    if (FSP_SUCCESS ==
                        oled_send_ina226_power(OLED_INA226_I2C_ADDRESS_N0, "n0"))
                    {
                        g_oled_power_phase = 1U;
                    }
                }
                else if (FSP_SUCCESS ==
                         oled_send_ina226_power(OLED_INA226_I2C_ADDRESS_N3, "n3"))
                {
                    g_oled_power_phase = 0U;
                }
            }
            break;
        case 5://发送水冷系统开关数据
            OLED_Send_Mode = 6;//设定为无状态
            if (Water_cool_State == 0)
            {
               len5 = (uint32_t) snprintf((char *)tx_buf5,
                              TX_BUF_SIZE - 4,  // 预留空间
                              "p1.pic=37"//关闭
                              );
            }
            else
            {
               len5 = (uint32_t) snprintf((char *)tx_buf5,
                              TX_BUF_SIZE - 4,  // 预留空间
                              "p1.pic=36"//开启
                              );
            }
            // 3. 检查长度是否安全
            if (len5 >= (TX_BUF_SIZE - 4))
            {
                len5 = TX_BUF_SIZE - 4;  // 截断
            }
            // 4. 追加 3 个 0xFF
            tx_buf5[len5++] = 0xFF;
            tx_buf5[len5++] = 0xFF;
            tx_buf5[len5++] = 0xFF;
            err = OLED_TryWrite(tx_buf5, len5);
            break;
        case 6://无发送需求
            break;
        default:
            OLED_Send_Mode = 0;//模式复位
            break;
   }
}


