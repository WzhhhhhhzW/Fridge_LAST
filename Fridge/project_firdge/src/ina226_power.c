#include "ina226_power.h"

#include "deepseek_bridge.h"
#include "system_tick.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define INA226_I2C_ADDRESS_1           (0x40U)
#define INA226_I2C_ADDRESS_2           (0x41U)
#define INA226_CHANNEL_COUNT           (2U)
#define INA226_REG_CONFIGURATION       (0x00U)
#define INA226_REG_SHUNT_VOLTAGE       (0x01U)
#define INA226_REG_BUS_VOLTAGE         (0x02U)

#define INA226_CONFIGURATION_VALUE     (0x4527U)
#define INA226_SHUNT_RESISTOR_UOHMS    (2000LL)
#define INA226_CONFIG_SETTLE_MS        (20U)
#define INA226_REPORT_INTERVAL_MS      (1000U)
#define INA226_RETRY_INTERVAL_MS       (5000U)

#define INA226_SCL_PIN                 BSP_IO_PORT_05_PIN_00
#define INA226_SDA_PIN                 BSP_IO_PORT_05_PIN_03
#define INA226_LINE_RELEASE_CFG        ((uint32_t) IOPORT_CFG_PORT_DIRECTION_INPUT)
#define INA226_LINE_LOW_CFG            ((uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT | \
                                        (uint32_t) IOPORT_CFG_PORT_OUTPUT_LOW)
#define INA226_I2C_DELAY_US            (5U)
#define INA226_CLOCK_STRETCH_LOOPS     (200U)

typedef struct
{
    uint8_t address;
    bool ready;
    bool config_pending;
    uint32_t last_retry_ms;
    uint32_t config_written_ms;
    uint16_t pending_shunt_raw;
    ina226_power_sample_t sample;
} ina226_channel_t;

typedef enum
{
    INA226_SAMPLE_IDLE = 0,
    INA226_SAMPLE_SHUNT,
    INA226_SAMPLE_BUS,
} ina226_sample_phase_t;

static ina226_channel_t g_ina226_channels[INA226_CHANNEL_COUNT] =
{
    {.address = INA226_I2C_ADDRESS_1},
    {.address = INA226_I2C_ADDRESS_2},
};

static uint32_t g_ina226_last_report_ms = 0U;
static uint32_t g_ina226_service_channel = 0U;
static ina226_sample_phase_t g_ina226_sample_phase = INA226_SAMPLE_IDLE;

#if defined(R_I2C_MASTER_API_H)
void ina226_i2c_callback(i2c_master_callback_args_t * p_args)
{
    (void) p_args;
}
#endif

static void ina226_i2c_delay(void)
{
    R_BSP_SoftwareDelay(INA226_I2C_DELAY_US, BSP_DELAY_UNITS_MICROSECONDS);
}

static void ina226_scl_release(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, INA226_SCL_PIN, INA226_LINE_RELEASE_CFG);
}

static void ina226_scl_low(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, INA226_SCL_PIN, INA226_LINE_LOW_CFG);
}

static void ina226_sda_release(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, INA226_SDA_PIN, INA226_LINE_RELEASE_CFG);
}

static void ina226_sda_low(void)
{
    (void) R_IOPORT_PinCfg(&g_ioport_ctrl, INA226_SDA_PIN, INA226_LINE_LOW_CFG);
}

static bool ina226_read_pin(bsp_io_port_pin_t pin)
{
    bsp_io_level_t level = BSP_IO_LEVEL_LOW;

    (void) R_IOPORT_PinRead(&g_ioport_ctrl, pin, &level);
    return (BSP_IO_LEVEL_HIGH == level);
}

static bool ina226_scl_is_high(void)
{
    return ina226_read_pin(INA226_SCL_PIN);
}

static bool ina226_sda_is_high(void)
{
    return ina226_read_pin(INA226_SDA_PIN);
}

static fsp_err_t ina226_wait_scl_high(void)
{
    uint32_t i;

    ina226_scl_release();
    for (i = 0U; i < INA226_CLOCK_STRETCH_LOOPS; i++)
    {
        if (ina226_scl_is_high())
        {
            return FSP_SUCCESS;
        }

        ina226_i2c_delay();
    }

    return FSP_ERR_TIMEOUT;
}

static fsp_err_t ina226_i2c_start(void)
{
    fsp_err_t err;

    ina226_sda_release();
    err = ina226_wait_scl_high();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    ina226_i2c_delay();
    ina226_sda_low();
    ina226_i2c_delay();
    ina226_scl_low();
    ina226_i2c_delay();

    return FSP_SUCCESS;
}

static void ina226_i2c_stop(void)
{
    ina226_sda_low();
    ina226_i2c_delay();
    ina226_scl_release();
    (void) ina226_wait_scl_high();
    ina226_i2c_delay();
    ina226_sda_release();
    ina226_i2c_delay();
}

static fsp_err_t ina226_i2c_write_bit(bool bit_value)
{
    fsp_err_t err;

    if (bit_value)
    {
        ina226_sda_release();
    }
    else
    {
        ina226_sda_low();
    }

    ina226_i2c_delay();
    err = ina226_wait_scl_high();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    ina226_i2c_delay();
    ina226_scl_low();
    ina226_i2c_delay();

    return FSP_SUCCESS;
}

static fsp_err_t ina226_i2c_read_bit(bool * p_bit_value)
{
    fsp_err_t err;

    if (NULL == p_bit_value)
    {
        return FSP_ERR_ASSERTION;
    }

    ina226_sda_release();
    ina226_i2c_delay();
    err = ina226_wait_scl_high();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    ina226_i2c_delay();
    *p_bit_value = ina226_sda_is_high();
    ina226_scl_low();
    ina226_i2c_delay();

    return FSP_SUCCESS;
}

static fsp_err_t ina226_i2c_write_byte(uint8_t value)
{
    fsp_err_t err;
    uint8_t mask;

    for (mask = 0x80U; mask > 0U; mask >>= 1)
    {
        err = ina226_i2c_write_bit(0U != (value & mask));
        if (FSP_SUCCESS != err)
        {
            return err;
        }
    }

    ina226_sda_release();
    ina226_i2c_delay();
    err = ina226_wait_scl_high();
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    ina226_i2c_delay();
    if (ina226_sda_is_high())
    {
        ina226_scl_low();
        ina226_i2c_delay();
        return FSP_ERR_ABORTED;
    }

    ina226_scl_low();
    ina226_i2c_delay();

    return FSP_SUCCESS;
}

static fsp_err_t ina226_i2c_read_byte(uint8_t * p_value, bool send_ack)
{
    fsp_err_t err;
    uint8_t value = 0U;
    uint8_t i;
    bool bit_value = false;

    if (NULL == p_value)
    {
        return FSP_ERR_ASSERTION;
    }

    for (i = 0U; i < 8U; i++)
    {
        value = (uint8_t) (value << 1);
        err = ina226_i2c_read_bit(&bit_value);
        if (FSP_SUCCESS != err)
        {
            return err;
        }

        if (bit_value)
        {
            value |= 1U;
        }
    }

    err = ina226_i2c_write_bit(!send_ack);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    *p_value = value;
    return FSP_SUCCESS;
}

static fsp_err_t ina226_soft_i2c_init(void)
{
    fsp_err_t err;
    uint8_t i;

    err = R_IOPORT_PinCfg(&g_ioport_ctrl, INA226_SCL_PIN, INA226_LINE_RELEASE_CFG);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_IOPORT_PinCfg(&g_ioport_ctrl, INA226_SDA_PIN, INA226_LINE_RELEASE_CFG);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    if (ina226_scl_is_high() && ina226_sda_is_high())
    {
        return FSP_SUCCESS;
    }

    for (i = 0U; i < 9U; i++)
    {
        ina226_scl_low();
        ina226_i2c_delay();
        ina226_scl_release();
        (void) ina226_wait_scl_high();
        ina226_i2c_delay();
    }

    ina226_i2c_stop();

    if ((!ina226_scl_is_high()) || (!ina226_sda_is_high()))
    {
        return FSP_ERR_INVALID_HW_CONDITION;
    }

    return FSP_SUCCESS;
}

static fsp_err_t ina226_write_register(uint8_t i2c_address, uint8_t reg, uint16_t value)
{
    fsp_err_t err;

    err = ina226_i2c_start();
    if (FSP_SUCCESS != err)
    {
        ina226_i2c_stop();
        return err;
    }

    err = ina226_i2c_write_byte((uint8_t) (i2c_address << 1));
    if (FSP_SUCCESS == err)
    {
        err = ina226_i2c_write_byte(reg);
    }

    if (FSP_SUCCESS == err)
    {
        err = ina226_i2c_write_byte((uint8_t) (value >> 8));
    }

    if (FSP_SUCCESS == err)
    {
        err = ina226_i2c_write_byte((uint8_t) (value & 0xFFU));
    }

    ina226_i2c_stop();
    return err;
}

static fsp_err_t ina226_read_register(uint8_t i2c_address, uint8_t reg, uint16_t * p_value)
{
    fsp_err_t err;
    uint8_t msb = 0U;
    uint8_t lsb = 0U;

    if (NULL == p_value)
    {
        return FSP_ERR_ASSERTION;
    }

    err = ina226_i2c_start();
    if (FSP_SUCCESS != err)
    {
        ina226_i2c_stop();
        return err;
    }

    err = ina226_i2c_write_byte((uint8_t) (i2c_address << 1));
    if (FSP_SUCCESS == err)
    {
        err = ina226_i2c_write_byte(reg);
    }

    if (FSP_SUCCESS == err)
    {
        err = ina226_i2c_start();
    }

    if (FSP_SUCCESS == err)
    {
        err = ina226_i2c_write_byte((uint8_t) ((i2c_address << 1) | 1U));
    }

    if (FSP_SUCCESS == err)
    {
        err = ina226_i2c_read_byte(&msb, true);
    }

    if (FSP_SUCCESS == err)
    {
        err = ina226_i2c_read_byte(&lsb, false);
    }

    ina226_i2c_stop();

    if (FSP_SUCCESS != err)
    {
        return err;
    }

    *p_value = (uint16_t) (((uint16_t) msb << 8) | lsb);
    return FSP_SUCCESS;
}

static fsp_err_t ina226_channel_begin_init(ina226_channel_t * p_channel)
{
    fsp_err_t err;

    if (NULL == p_channel)
    {
        return FSP_ERR_ASSERTION;
    }

    p_channel->ready = false;
    p_channel->config_pending = false;
    p_channel->sample.valid = false;
    p_channel->sample.i2c_address = p_channel->address;
    p_channel->last_retry_ms = system_tick_now();

    err = ina226_write_register(p_channel->address,
                                INA226_REG_CONFIGURATION,
                                INA226_CONFIGURATION_VALUE);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    p_channel->config_written_ms = system_tick_now();
    p_channel->config_pending = true;
    return FSP_SUCCESS;
}

static fsp_err_t ina226_channel_finish_init(ina226_channel_t * p_channel)
{
    fsp_err_t err;
    uint16_t config_readback = 0U;
    char line[96];

    if (NULL == p_channel)
    {
        return FSP_ERR_ASSERTION;
    }

    if ((!p_channel->config_pending) ||
        (!system_tick_elapsed(p_channel->config_written_ms, INA226_CONFIG_SETTLE_MS)))
    {
        return FSP_ERR_IN_USE;
    }

    err = ina226_read_register(p_channel->address,
                               INA226_REG_CONFIGURATION,
                               &config_readback);
    if (FSP_SUCCESS != err)
    {
        p_channel->config_pending = false;
        p_channel->last_retry_ms = system_tick_now();
        return err;
    }

    if (INA226_CONFIGURATION_VALUE != config_readback)
    {
        p_channel->config_pending = false;
        p_channel->last_retry_ms = system_tick_now();
        return FSP_ERR_INVALID_HW_CONDITION;
    }

    p_channel->config_pending = false;
    p_channel->ready = true;
    (void) snprintf(line,
                    sizeof(line),
                    "[PWR 0x%02X] ready, cfg=0x%04X, Rshunt=0.002 ohm\r\n",
                    (unsigned int) p_channel->address,
                    (unsigned int) config_readback);
    deepseek_bridge_pc_write(line);
    return FSP_SUCCESS;
}

static fsp_err_t ina226_channel_read_shunt(ina226_channel_t * p_channel)
{
    if (NULL == p_channel)
    {
        return FSP_ERR_ASSERTION;
    }

    return ina226_read_register(p_channel->address,
                                INA226_REG_SHUNT_VOLTAGE,
                                &p_channel->pending_shunt_raw);
}

static fsp_err_t ina226_channel_read_bus_and_update(ina226_channel_t * p_channel)
{
    fsp_err_t err;
    uint16_t bus_raw = 0U;
    int16_t shunt_raw;
    int32_t shunt_uv_x10;
    int32_t bus_mv;
    int32_t current_mA;
    int64_t power_uW;
    int32_t power_dW;

    if (NULL == p_channel)
    {
        return FSP_ERR_ASSERTION;
    }

    err = ina226_read_register(p_channel->address,
                               INA226_REG_BUS_VOLTAGE,
                               &bus_raw);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    shunt_raw = (int16_t) p_channel->pending_shunt_raw;
    shunt_uv_x10 = (int32_t) shunt_raw * 25;
    bus_mv = (int32_t) ((((int64_t) bus_raw * 125LL) + 50LL) / 100LL);
    current_mA = (int32_t) (((int64_t) shunt_uv_x10 * 100LL) / INA226_SHUNT_RESISTOR_UOHMS);
    power_uW = (int64_t) bus_mv * (int64_t) current_mA;
    power_dW = (power_uW >= 0LL) ?
               (int32_t) ((power_uW + 50000LL) / 100000LL) :
               (int32_t) ((power_uW - 50000LL) / 100000LL);

    p_channel->sample.valid = true;
    p_channel->sample.i2c_address = p_channel->address;
    p_channel->sample.bus_mv = bus_mv;
    p_channel->sample.current_mA = current_mA;
    p_channel->sample.power_dW = power_dW;
    return FSP_SUCCESS;
}

static void ina226_print_sample(const ina226_power_sample_t * p_sample)
{
    int32_t abs_current_mA;
    int32_t abs_power_dW;
    char line[112];

    if ((NULL == p_sample) || (!p_sample->valid))
    {
        return;
    }

    abs_current_mA = (p_sample->current_mA < 0) ?
                     -p_sample->current_mA :
                     p_sample->current_mA;
    abs_power_dW = (p_sample->power_dW < 0) ?
                   -p_sample->power_dW :
                   p_sample->power_dW;

    (void) snprintf(line,
                    sizeof(line),
                    "[PWR 0x%02X] V=%ld.%03ldV I=%s%ld.%03ldA P=%s%ld.%1ldW\r\n",
                    (unsigned int) p_sample->i2c_address,
                    (long) (p_sample->bus_mv / 1000),
                    (long) (p_sample->bus_mv % 1000),
                    (p_sample->current_mA < 0) ? "-" : "",
                    (long) (abs_current_mA / 1000),
                    (long) (abs_current_mA % 1000),
                    (p_sample->power_dW < 0) ? "-" : "",
                    (long) (abs_power_dW / 10),
                    (long) (abs_power_dW % 10));
    deepseek_bridge_pc_write(line);
}

void ina226_power_init(void)
{
    uint32_t now = system_tick_now();
    fsp_err_t err = ina226_soft_i2c_init();

    g_ina226_last_report_ms = now;
    g_ina226_service_channel = 0U;
    g_ina226_sample_phase = INA226_SAMPLE_IDLE;
    for (uint32_t i = 0U; i < INA226_CHANNEL_COUNT; i++)
    {
        g_ina226_channels[i].ready = false;
        g_ina226_channels[i].config_pending = false;
        g_ina226_channels[i].last_retry_ms = now - INA226_RETRY_INTERVAL_MS;
        g_ina226_channels[i].sample.valid = false;
        g_ina226_channels[i].sample.i2c_address = g_ina226_channels[i].address;
    }

    if (FSP_SUCCESS != err)
    {
        return;
    }

    (void) ina226_channel_begin_init(&g_ina226_channels[0]);
}

void ina226_power_process(void)
{
    uint32_t now = system_tick_now();
    ina226_channel_t * p_channel;

    /* Perform at most one initialization transaction per main-loop pass. */
    for (uint32_t i = 0U; i < INA226_CHANNEL_COUNT; i++)
    {
        p_channel = &g_ina226_channels[i];

        if (p_channel->config_pending)
        {
            if (system_tick_elapsed(p_channel->config_written_ms, INA226_CONFIG_SETTLE_MS))
            {
                (void) ina226_channel_finish_init(p_channel);
            }
            return;
        }

        if ((!p_channel->ready) &&
            system_tick_elapsed(p_channel->last_retry_ms, INA226_RETRY_INTERVAL_MS))
        {
            p_channel->last_retry_ms = now;
            if (FSP_SUCCESS == ina226_soft_i2c_init())
            {
                (void) ina226_channel_begin_init(p_channel);
            }
            return;
        }
    }

    if (INA226_SAMPLE_IDLE == g_ina226_sample_phase)
    {
        if (!system_tick_elapsed(g_ina226_last_report_ms, INA226_REPORT_INTERVAL_MS))
        {
            return;
        }

        g_ina226_last_report_ms = now;
        g_ina226_service_channel = 0U;
        g_ina226_sample_phase = INA226_SAMPLE_SHUNT;
    }

    while (g_ina226_service_channel < INA226_CHANNEL_COUNT)
    {
        p_channel = &g_ina226_channels[g_ina226_service_channel];

        if (!p_channel->ready)
        {
            g_ina226_service_channel++;
            continue;
        }

        if (INA226_SAMPLE_SHUNT == g_ina226_sample_phase)
        {
            if (FSP_SUCCESS != ina226_channel_read_shunt(p_channel))
            {
                p_channel->ready = false;
                p_channel->sample.valid = false;
                p_channel->last_retry_ms = now;
                g_ina226_service_channel++;
                continue;
            }

            g_ina226_sample_phase = INA226_SAMPLE_BUS;
            return;
        }

        if (FSP_SUCCESS != ina226_channel_read_bus_and_update(p_channel))
        {
            p_channel->ready = false;
            p_channel->sample.valid = false;
            p_channel->last_retry_ms = now;
        }
        else
        {
            ina226_print_sample(&p_channel->sample);
        }

        g_ina226_service_channel++;
        g_ina226_sample_phase = INA226_SAMPLE_SHUNT;
        return;
    }

    g_ina226_sample_phase = INA226_SAMPLE_IDLE;
}

bool ina226_power_get_latest(uint8_t i2c_address, ina226_power_sample_t * p_sample)
{
    if (NULL == p_sample)
    {
        return false;
    }

    for (uint32_t i = 0U; i < INA226_CHANNEL_COUNT; i++)
    {
        if (g_ina226_channels[i].address == i2c_address)
        {
            *p_sample = g_ina226_channels[i].sample;
            return p_sample->valid;
        }
    }

    p_sample->valid = false;
    return false;
}
