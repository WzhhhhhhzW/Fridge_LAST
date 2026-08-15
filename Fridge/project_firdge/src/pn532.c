#include "pn532.h"

#include "deepseek_bridge.h"
#include "system_tick.h"

#include <stdio.h>
#include <string.h>

#define PN532_CS_PIN                    BSP_IO_PORT_04_PIN_05
#define PN532_SPI_WRITE                 (0x01U)
#define PN532_SPI_STATUS                (0x02U)
#define PN532_SPI_READ                  (0x03U)
#define PN532_HOST_TO_PN532             (0xD4U)
#define PN532_PN532_TO_HOST             (0xD5U)
#define PN532_COMMAND_SAM_CONFIGURATION (0x14U)
#define PN532_COMMAND_RF_CONFIGURATION  (0x32U)
#define PN532_COMMAND_IN_LIST_PASSIVE   (0x4AU)
#define PN532_SPI_TRANSFER_TIMEOUT_MS   (20U)
#define PN532_RESPONSE_TIMEOUT_MS       (5000U)
#define PN532_COMMAND_WAKE_GUARD_MS     (5U)
#define PN532_ACCESS_GUARD_MS           (1U)
#define PN532_STATUS_POLL_MS            (10U)
#define PN532_RESPONSE_POLL_MS          (50U)
#define PN532_SCAN_INTERVAL_MS          (200U)
#define PN532_RESTART_DELAY_MS          (500U)
#define PN532_FRAME_BUFFER_SIZE         (96U)

typedef enum e_pn532_state
{
    PN532_STATE_DISABLED = 0,
    PN532_STATE_BOOT_WAIT,
    PN532_STATE_IDLE,
    PN532_STATE_COMMAND_WRITE,
    PN532_STATE_WAIT_ACK,
    PN532_STATE_ACK_STATUS,
    PN532_STATE_ACK_READ,
    PN532_STATE_WAIT_RESPONSE,
    PN532_STATE_RESPONSE_STATUS,
    PN532_STATE_RESPONSE_READ,
    PN532_STATE_ABORT_WRITE,
} pn532_state_t;

typedef enum e_pn532_operation
{
    PN532_OPERATION_NONE = 0,
    PN532_OPERATION_SAM,
    PN532_OPERATION_RF_RETRIES,
    PN532_OPERATION_SCAN,
} pn532_operation_t;

typedef enum e_pn532_bus_state
{
    PN532_BUS_IDLE = 0,
    PN532_BUS_SELECT_WAIT,
    PN532_BUS_TRANSFER,
} pn532_bus_state_t;

typedef enum e_pn532_transaction
{
    PN532_TRANSACTION_NONE = 0,
    PN532_TRANSACTION_COMMAND,
    PN532_TRANSACTION_ACK_STATUS,
    PN532_TRANSACTION_ACK_READ,
    PN532_TRANSACTION_RESPONSE_STATUS,
    PN532_TRANSACTION_RESPONSE_READ,
    PN532_TRANSACTION_ABORT,
} pn532_transaction_t;

static volatile bool g_pn532_spi_done = false;
static volatile bool g_pn532_spi_error = false;
static pn532_state_t g_pn532_state = PN532_STATE_DISABLED;
static pn532_operation_t g_pn532_operation = PN532_OPERATION_NONE;
static volatile bool g_pn532_initialized = false;
static volatile bool g_pn532_enabled = false;
static bool g_pn532_ready = false;
static bool g_pn532_card_present = false;
static bool g_pn532_uid_pending = false;
static uint8_t g_pn532_uid[PN532_UID_MAX_BYTES];
static uint8_t g_pn532_uid_length = 0U;
static uint32_t g_pn532_scan_generation = 0U;
static uint32_t g_pn532_state_started_ms = 0U;
static uint32_t g_pn532_last_poll_ms = 0U;
static volatile pn532_bus_state_t g_pn532_bus_state = PN532_BUS_IDLE;
static volatile pn532_transaction_t g_pn532_bus_transaction = PN532_TRANSACTION_NONE;
static volatile pn532_transaction_t g_pn532_bus_completed_transaction = PN532_TRANSACTION_NONE;
static uint8_t g_pn532_bus_tx[PN532_FRAME_BUFFER_SIZE + 1U];
static uint8_t g_pn532_bus_rx[PN532_FRAME_BUFFER_SIZE + 1U];
static volatile uint32_t g_pn532_bus_length = 0U;
static volatile uint32_t g_pn532_bus_select_started_ms = 0U;
static volatile uint32_t g_pn532_bus_transfer_started_ms = 0U;
static volatile uint32_t g_pn532_bus_select_delay_ms = 0U;
static volatile bool g_pn532_bus_completion_pending = false;
static volatile fsp_err_t g_pn532_bus_completed_result = FSP_SUCCESS;
static uint8_t g_pn532_last_status = 0xFFU;
static uint32_t g_pn532_status_poll_count = 0U;

static void pn532_log(const char * text)
{
    deepseek_bridge_pc_write(text);
}

void pn532_spi_callback(spi_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (SPI_EVENT_TRANSFER_COMPLETE == p_args->event)
    {
        /*
         * P405 is software-controlled SSL.  Release it in the SPI interrupt
         * instead of waiting for the main loop, which can be busy running an
         * Edge Impulse inference for more than 100 ms.
        */
        (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_HIGH);
        __DMB();
        g_pn532_spi_done = true;
    }
    else
    {
        (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_HIGH);
        g_pn532_spi_error = true;
        __DMB();
        g_pn532_spi_done = true;
    }
}

/* Keep the current generated callback name linkable until the user regenerates FSP. */
void spi_callback(spi_callback_args_t * p_args)
{
    pn532_spi_callback(p_args);
}

static bool pn532_cs_is_generated_output_high(void)
{
    for (uint32_t i = 0U; i < IOPORT_CFG_NAME.number_of_pins; i++)
    {
        ioport_pin_cfg_t const * pin_cfg = &IOPORT_CFG_NAME.p_pin_cfg_data[i];
        if (PN532_CS_PIN == pin_cfg->pin)
        {
            uint32_t required = (uint32_t) IOPORT_CFG_PORT_DIRECTION_OUTPUT |
                                (uint32_t) IOPORT_CFG_PORT_OUTPUT_HIGH;
            return ((pin_cfg->pin_cfg & required) == required) &&
                   (0U == (pin_cfg->pin_cfg & (uint32_t) IOPORT_CFG_PERIPHERAL_PIN));
        }
    }

    return false;
}

static bool pn532_validate_fsp(void)
{
    spi_extended_cfg_t const * extend = (spi_extended_cfg_t const *) g_spi0_cfg.p_extend;
    rspck_div_setting_t expected_div;
    bool supported_bitrate = false;
    uint32_t matched_bitrate = 0U;
    static uint32_t const supported_bitrates[] = {250000U, 500000U, 1000000U};

    if ((0U != g_spi0_cfg.channel) ||
        (SPI_MODE_MASTER != g_spi0_cfg.operating_mode) ||
        (SPI_CLK_POLARITY_LOW != g_spi0_cfg.clk_polarity) ||
        (SPI_CLK_PHASE_EDGE_ODD != g_spi0_cfg.clk_phase) ||
        (SPI_BIT_ORDER_LSB_FIRST != g_spi0_cfg.bit_order) ||
        (pn532_spi_callback != g_spi0_cfg.p_callback) ||
        (NULL != g_spi0_cfg.p_transfer_tx) ||
        (NULL != g_spi0_cfg.p_transfer_rx) ||
        (12U != g_spi0_cfg.rxi_ipl) ||
        (12U != g_spi0_cfg.txi_ipl) ||
        (12U != g_spi0_cfg.tei_ipl) ||
        (12U != g_spi0_cfg.eri_ipl) ||
        (NULL == extend) ||
        (SPI_SSL_MODE_CLK_SYN != extend->spi_clksyn) ||
        (SPI_COMMUNICATION_FULL_DUPLEX != extend->spi_comm))
    {
        pn532_log("[NFC] FSP g_spi0 must be RSPI0 master, mode 0, LSB first, full duplex, callback pn532_spi_callback, IRQ 12, without transfer engines.\r\n");
        return false;
    }

    for (uint32_t i = 0U; i < (sizeof(supported_bitrates) / sizeof(supported_bitrates[0])); i++)
    {
        if ((FSP_SUCCESS == R_SPI_CalculateBitrate(supported_bitrates[i], &expected_div)) &&
            (expected_div.spbr == extend->spck_div.spbr) &&
            (expected_div.brdv == extend->spck_div.brdv))
        {
            supported_bitrate = true;
            matched_bitrate = supported_bitrates[i];
            break;
        }
    }

    if (!supported_bitrate)
    {
        pn532_log("[NFC] FSP g_spi0 bitrate must be 250000, 500000, or 1000000; use 250000 for jumper-wire debugging.\r\n");
        return false;
    }

    if (!pn532_cs_is_generated_output_high())
    {
        pn532_log("[NFC] FSP P405 must be a GPIO output with initial level High.\r\n");
        return false;
    }

    if (250000U == matched_bitrate)
    {
        pn532_log("[NFC] FSP link verified: 250000 Hz, mode 0, LSB first.\r\n");
    }
    else if (500000U == matched_bitrate)
    {
        pn532_log("[NFC] FSP link verified: 500000 Hz, mode 0, LSB first.\r\n");
    }
    else
    {
        pn532_log("[NFC] FSP link verified: 1000000 Hz, mode 0, LSB first.\r\n");
    }

    return true;
}

static void pn532_bus_complete(pn532_transaction_t transaction, fsp_err_t result)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_HIGH);
    g_pn532_bus_state = PN532_BUS_IDLE;
    g_pn532_bus_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completed_transaction = transaction;
    g_pn532_bus_completed_result = result;
    g_pn532_bus_completion_pending = true;
}

static fsp_err_t pn532_bus_start(uint8_t const * tx,
                                uint32_t length,
                                uint32_t select_delay_ms,
                                pn532_transaction_t transaction)
{
    if ((NULL == tx) || (0U == length) || (length > sizeof(g_pn532_bus_tx)))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    if ((PN532_BUS_IDLE != g_pn532_bus_state) || g_pn532_bus_completion_pending)
    {
        return FSP_ERR_IN_USE;
    }

    memcpy(g_pn532_bus_tx, tx, length);
    memset(g_pn532_bus_rx, 0, length);
    g_pn532_bus_length = length;
    g_pn532_bus_select_delay_ms = select_delay_ms;
    g_pn532_bus_select_started_ms = system_tick_now();
    g_pn532_bus_transaction = transaction;
    g_pn532_spi_done = false;
    g_pn532_spi_error = false;
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_LOW);
    __DMB();
    g_pn532_bus_state = PN532_BUS_SELECT_WAIT;
    return FSP_SUCCESS;
}

static void pn532_bus_process(void)
{
    if (PN532_BUS_TRANSFER == g_pn532_bus_state)
    {
        if (g_pn532_spi_done)
        {
            __DMB();
            pn532_bus_complete(g_pn532_bus_transaction,
                               g_pn532_spi_error ? FSP_ERR_TRANSFER_ABORTED : FSP_SUCCESS);
        }
        else if (system_tick_elapsed(g_pn532_bus_transfer_started_ms,
                                     PN532_SPI_TRANSFER_TIMEOUT_MS))
        {
            pn532_transaction_t transaction = g_pn532_bus_transaction;

            /* Abort a genuinely stuck RSPI transfer without a delay loop. */
            (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_HIGH);
            (void) R_SPI_Close(&g_spi0_ctrl);
            (void) R_SPI_Open(&g_spi0_ctrl, &g_spi0_cfg);
            pn532_bus_complete(transaction, FSP_ERR_TIMEOUT);
        }
    }
}

void pn532_tick_1ms(void)
{
    fsp_err_t err;

    if ((!g_pn532_initialized) || (!g_pn532_enabled) ||
        (PN532_BUS_SELECT_WAIT != g_pn532_bus_state) ||
        (!system_tick_elapsed(g_pn532_bus_select_started_ms,
                              g_pn532_bus_select_delay_ms)))
    {
        return;
    }

    __DMB();

    /*
     * Start the clock transfer from the existing 1 ms timer callback.  This
     * preserves the PN532's 1/2 ms NSS wake guard without sleeping or relying
     * on the main loop, which may be occupied by an Edge Impulse inference.
     */
    g_pn532_spi_done = false;
    g_pn532_spi_error = false;
    err = R_SPI_WriteRead(&g_spi0_ctrl,
                          g_pn532_bus_tx,
                          g_pn532_bus_rx,
                          g_pn532_bus_length,
                          SPI_BIT_WIDTH_8_BITS);
    if (FSP_SUCCESS != err)
    {
        pn532_bus_complete(g_pn532_bus_transaction, err);
        return;
    }
    g_pn532_bus_transfer_started_ms = system_tick_now();
    g_pn532_bus_state = PN532_BUS_TRANSFER;
}

static bool pn532_bus_take_completion(pn532_transaction_t * transaction, fsp_err_t * result)
{
    if ((!g_pn532_bus_completion_pending) || (NULL == transaction) || (NULL == result))
    {
        return false;
    }

    *transaction = g_pn532_bus_completed_transaction;
    *result = g_pn532_bus_completed_result;
    g_pn532_bus_completion_pending = false;
    g_pn532_bus_completed_transaction = PN532_TRANSACTION_NONE;
    return true;
}

static fsp_err_t pn532_write_command(uint8_t command,
                                     uint8_t const * parameters,
                                     uint8_t parameter_length,
                                     pn532_operation_t operation)
{
    uint8_t tx[32] = {0U};
    uint8_t data_length = (uint8_t) (parameter_length + 2U);
    uint8_t checksum = PN532_HOST_TO_PN532;
    uint32_t offset = 0U;

    if ((uint32_t) data_length + 8U > sizeof(tx))
    {
        return FSP_ERR_OVERFLOW;
    }

    tx[offset++] = PN532_SPI_WRITE;
    tx[offset++] = 0x00U;
    tx[offset++] = 0x00U;
    tx[offset++] = 0xFFU;
    tx[offset++] = data_length;
    tx[offset++] = (uint8_t) (0U - data_length);
    tx[offset++] = PN532_HOST_TO_PN532;
    tx[offset++] = command;
    checksum = (uint8_t) (checksum + command);
    for (uint8_t i = 0U; i < parameter_length; i++)
    {
        tx[offset++] = parameters[i];
        checksum = (uint8_t) (checksum + parameters[i]);
    }
    tx[offset++] = (uint8_t) (0U - checksum);
    tx[offset++] = 0x00U;

    if (FSP_SUCCESS != pn532_bus_start(tx,
                                       offset,
                                       PN532_COMMAND_WAKE_GUARD_MS,
                                       PN532_TRANSACTION_COMMAND))
    {
        return FSP_ERR_IN_USE;
    }

    g_pn532_operation = operation;
    g_pn532_state = PN532_STATE_COMMAND_WRITE;
    g_pn532_state_started_ms = system_tick_now();
    return FSP_SUCCESS;
}

static bool pn532_is_ack(uint8_t const * frame, uint32_t length)
{
    static uint8_t const ack[] = {0x00U, 0x00U, 0xFFU, 0x00U, 0xFFU, 0x00U};

    for (uint32_t offset = 0U; offset + sizeof(ack) <= length; offset++)
    {
        if (0 == memcmp(&frame[offset], ack, sizeof(ack)))
        {
            return true;
        }
    }
    return false;
}

static bool pn532_parse_response(uint8_t const * frame,
                                 uint32_t frame_length,
                                 uint8_t expected_command,
                                 uint8_t const ** response,
                                 uint8_t * response_length)
{
    for (uint32_t offset = 0U; offset + 8U <= frame_length; offset++)
    {
        uint8_t length;
        uint8_t checksum = 0U;

        if ((0x00U != frame[offset]) || (0x00U != frame[offset + 1U]) ||
            (0xFFU != frame[offset + 2U]))
        {
            continue;
        }

        length = frame[offset + 3U];
        if ((0U == length) ||
            ((uint8_t) (length + frame[offset + 4U]) != 0U) ||
            (offset + 7U + length > frame_length))
        {
            continue;
        }
        for (uint32_t i = 0U; i < length; i++)
        {
            checksum = (uint8_t) (checksum + frame[offset + 5U + i]);
        }
        checksum = (uint8_t) (checksum + frame[offset + 5U + length]);
        if ((0U != checksum) ||
            (PN532_PN532_TO_HOST != frame[offset + 5U]) ||
            ((uint8_t) (expected_command + 1U) != frame[offset + 6U]))
        {
            continue;
        }

        *response = &frame[offset + 5U];
        *response_length = length;
        return true;
    }
    return false;
}

static void pn532_enter_boot_wait(void)
{
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_HIGH);
    g_pn532_spi_done = false;
    g_pn532_spi_error = false;
    g_pn532_bus_state = PN532_BUS_IDLE;
    g_pn532_bus_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completed_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completion_pending = false;
    g_pn532_ready = false;
    g_pn532_card_present = false;
    g_pn532_uid_pending = false;
    g_pn532_uid_length = 0U;
    g_pn532_operation = PN532_OPERATION_NONE;
    g_pn532_last_status = 0xFFU;
    g_pn532_status_poll_count = 0U;
    g_pn532_state = PN532_STATE_BOOT_WAIT;
    g_pn532_state_started_ms = system_tick_now();
}

static void pn532_restart_later(const char * reason)
{
    /*
     * The PN532 can still be executing a command after the host misses its
     * ACK/response.  Merely resetting our software state leaves the PN532
     * busy, so the next SAMConfiguration command can be ignored as well.
     * The PN532 user manual defines a host ACK frame as the non-blocking way
     * to abort the command currently in progress.
     */
    static uint8_t const abort_frame[] =
    {
        PN532_SPI_WRITE, 0x00U, 0x00U, 0xFFU, 0x00U, 0xFFU, 0x00U
    };

    pn532_log(reason);
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_HIGH);

    /* Reset the RA6M5 RSPI state as well as the PN532 protocol state. */
    if (g_pn532_initialized)
    {
        (void) R_SPI_Close(&g_spi0_ctrl);
        if (FSP_SUCCESS != R_SPI_Open(&g_spi0_ctrl, &g_spi0_cfg))
        {
            pn532_log("[NFC] RSPI reopen failed during recovery.\r\n");
            pn532_enter_boot_wait();
            return;
        }
    }

    g_pn532_spi_done = false;
    g_pn532_spi_error = false;
    g_pn532_bus_state = PN532_BUS_IDLE;
    g_pn532_bus_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completed_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completion_pending = false;
    g_pn532_ready = false;
    g_pn532_card_present = false;
    g_pn532_uid_pending = false;
    g_pn532_uid_length = 0U;
    g_pn532_operation = PN532_OPERATION_NONE;
    g_pn532_last_status = 0xFFU;
    g_pn532_status_poll_count = 0U;

    if (FSP_SUCCESS == pn532_bus_start(abort_frame,
                                       sizeof(abort_frame),
                                       PN532_COMMAND_WAKE_GUARD_MS,
                                       PN532_TRANSACTION_ABORT))
    {
        g_pn532_state = PN532_STATE_ABORT_WRITE;
        g_pn532_state_started_ms = system_tick_now();
        return;
    }

    pn532_log("[NFC] abort transfer could not start; continuing recovery.\r\n");
    pn532_enter_boot_wait();
}

static void pn532_start_sam(void)
{
    static uint8_t const parameters[] = {0x01U, 0x14U, 0x01U};

    if (FSP_SUCCESS == pn532_write_command(PN532_COMMAND_SAM_CONFIGURATION,
                                           parameters,
                                           sizeof(parameters),
                                           PN532_OPERATION_SAM))
    {
        return;
    }
    else
    {
        pn532_restart_later("[NFC] SAMConfiguration send failed; retrying.\r\n");
    }
}

static void pn532_start_scan(void)
{
    static uint8_t const parameters[] = {0x01U, 0x00U};

    if (FSP_SUCCESS == pn532_write_command(PN532_COMMAND_IN_LIST_PASSIVE,
                                           parameters,
                                           sizeof(parameters),
                                           PN532_OPERATION_SCAN))
    {
        return;
    }
    else
    {
        pn532_restart_later("[NFC] card scan send failed; retrying.\r\n");
    }
}

static void pn532_start_rf_retries(void)
{
    /*
     * RFConfiguration, CfgItem 0x05 (MaxRetries):
     *   MxRtyATR               = 0xFF (unchanged/default behavior)
     *   MxRtyPSL               = 0x01 (unchanged/default behavior)
     *   MxRtyPassiveActivation = 0x00 (one passive activation attempt)
     *
     * The PN532 power-on default for passive activation is 0xFF, which makes
     * InListPassiveTarget wait forever when no card is present.  A finite
     * value is required for the application's 200 ms polling state machine.
     */
    static uint8_t const parameters[] = {0x05U, 0xFFU, 0x01U, 0x00U};

    if (FSP_SUCCESS == pn532_write_command(PN532_COMMAND_RF_CONFIGURATION,
                                           parameters,
                                           sizeof(parameters),
                                           PN532_OPERATION_RF_RETRIES))
    {
        return;
    }
    else
    {
        pn532_restart_later("[NFC] RFConfiguration send failed; retrying.\r\n");
    }
}

static fsp_err_t pn532_start_status_read(pn532_transaction_t transaction)
{
    static uint8_t const tx[] = {PN532_SPI_STATUS, 0x00U};

    return pn532_bus_start(tx, sizeof(tx), PN532_ACCESS_GUARD_MS, transaction);
}

static fsp_err_t pn532_start_frame_read(uint32_t frame_length,
                                       pn532_transaction_t transaction)
{
    uint8_t tx[PN532_FRAME_BUFFER_SIZE + 1U] = {0U};

    if (frame_length > PN532_FRAME_BUFFER_SIZE)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    tx[0] = PN532_SPI_READ;
    return pn532_bus_start(tx,
                           frame_length + 1U,
                           PN532_ACCESS_GUARD_MS,
                           transaction);
}

static uint8_t pn532_expected_response_command(void)
{
    if (PN532_OPERATION_SAM == g_pn532_operation)
    {
        return PN532_COMMAND_SAM_CONFIGURATION;
    }
    if (PN532_OPERATION_RF_RETRIES == g_pn532_operation)
    {
        return PN532_COMMAND_RF_CONFIGURATION;
    }
    return PN532_COMMAND_IN_LIST_PASSIVE;
}

static void pn532_handle_response(uint8_t const * response, uint8_t length)
{
    if (PN532_OPERATION_SAM == g_pn532_operation)
    {
        pn532_start_rf_retries();
        return;
    }

    if (PN532_OPERATION_RF_RETRIES == g_pn532_operation)
    {
        g_pn532_ready = true;
        g_pn532_state = PN532_STATE_IDLE;
        g_pn532_last_poll_ms = system_tick_now() - PN532_SCAN_INTERVAL_MS;
        pn532_log("[NFC] PN532 ready; polling ISO14443A cards.\r\n");
        return;
    }

    if (PN532_OPERATION_SCAN == g_pn532_operation)
    {
        g_pn532_scan_generation++;
        g_pn532_card_present = false;
        if ((length >= 9U) && (response[2] > 0U))
        {
            uint8_t uid_length = response[7];
            if (((4U == uid_length) || (7U == uid_length)) &&
                ((uint16_t) uid_length + 8U <= length))
            {
                memcpy(g_pn532_uid, &response[8], uid_length);
                g_pn532_uid_length = uid_length;
                g_pn532_uid_pending = true;
                g_pn532_card_present = true;
            }
        }
        g_pn532_state = PN532_STATE_IDLE;
        g_pn532_last_poll_ms = system_tick_now();
    }
}

static void pn532_handle_bus_completion(pn532_transaction_t transaction, fsp_err_t result)
{
    uint32_t now = system_tick_now();

    if (PN532_TRANSACTION_ABORT == transaction)
    {
        if (FSP_SUCCESS != result)
        {
            pn532_log("[NFC] abort transfer failed; continuing recovery.\r\n");
        }
        pn532_enter_boot_wait();
        return;
    }

    if (FSP_SUCCESS != result)
    {
        pn532_restart_later("[NFC] PN532 SPI transaction failed; reinitializing.\r\n");
        return;
    }

    switch (transaction)
    {
        case PN532_TRANSACTION_COMMAND:
            g_pn532_last_status = 0xFFU;
            g_pn532_status_poll_count = 0U;
            g_pn532_state = PN532_STATE_WAIT_ACK;
            g_pn532_state_started_ms = now;
            g_pn532_last_poll_ms = now - PN532_STATUS_POLL_MS;
            break;

        case PN532_TRANSACTION_ACK_STATUS:
            g_pn532_last_status = g_pn532_bus_rx[1];
            g_pn532_status_poll_count++;
            if (0x01U == g_pn532_bus_rx[1])
            {
                if (FSP_SUCCESS != pn532_start_frame_read(6U, PN532_TRANSACTION_ACK_READ))
                {
                    pn532_restart_later("[NFC] PN532 ACK read could not start; reinitializing.\r\n");
                    return;
                }
                g_pn532_state = PN532_STATE_ACK_READ;
            }
            else
            {
                g_pn532_state = PN532_STATE_WAIT_ACK;
                g_pn532_last_poll_ms = now;
            }
            break;

        case PN532_TRANSACTION_ACK_READ:
            if (!pn532_is_ack(&g_pn532_bus_rx[1], 6U))
            {
                pn532_restart_later("[NFC] invalid PN532 ACK; reinitializing.\r\n");
                return;
            }
            g_pn532_state = PN532_STATE_WAIT_RESPONSE;
            g_pn532_state_started_ms = now;
            g_pn532_last_poll_ms = now - PN532_STATUS_POLL_MS;
            break;

        case PN532_TRANSACTION_RESPONSE_STATUS:
            if (0x01U == g_pn532_bus_rx[1])
            {
                if (FSP_SUCCESS != pn532_start_frame_read(PN532_FRAME_BUFFER_SIZE,
                                                          PN532_TRANSACTION_RESPONSE_READ))
                {
                    pn532_restart_later("[NFC] PN532 response read could not start; reinitializing.\r\n");
                    return;
                }
                g_pn532_state = PN532_STATE_RESPONSE_READ;
            }
            else
            {
                g_pn532_state = PN532_STATE_WAIT_RESPONSE;
                g_pn532_last_poll_ms = now;
            }
            break;

        case PN532_TRANSACTION_RESPONSE_READ:
        {
            uint8_t const * response = NULL;
            uint8_t response_length = 0U;

            if (!pn532_parse_response(&g_pn532_bus_rx[1],
                                      PN532_FRAME_BUFFER_SIZE,
                                      pn532_expected_response_command(),
                                      &response,
                                      &response_length))
            {
                pn532_restart_later("[NFC] invalid PN532 response frame; reinitializing.\r\n");
                return;
            }
            pn532_handle_response(response, response_length);
            break;
        }

        default:
            pn532_restart_later("[NFC] invalid PN532 SPI state; reinitializing.\r\n");
            break;
    }
}

fsp_err_t pn532_init(void)
{
    fsp_err_t err;

    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_HIGH);
    if (!pn532_validate_fsp())
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    err = R_SPI_Open(&g_spi0_ctrl, &g_spi0_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        pn532_log("[NFC] R_SPI_Open failed.\r\n");
        return err;
    }

    g_pn532_initialized = true;
    g_pn532_enabled = true;
    g_pn532_ready = false;
    g_pn532_uid_pending = false;
    g_pn532_spi_done = false;
    g_pn532_spi_error = false;
    g_pn532_bus_state = PN532_BUS_IDLE;
    g_pn532_bus_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completed_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completion_pending = false;
    g_pn532_state = PN532_STATE_BOOT_WAIT;
    g_pn532_state_started_ms = system_tick_now();
    pn532_log("[NFC] SPI initialized; waiting for PN532 startup.\r\n");
    return FSP_SUCCESS;
}

void pn532_process(void)
{
    uint32_t now = system_tick_now();
    pn532_transaction_t transaction;
    fsp_err_t transaction_result;

    if ((!g_pn532_initialized) || (!g_pn532_enabled))
    {
        return;
    }

    pn532_bus_process();
    if (pn532_bus_take_completion(&transaction, &transaction_result))
    {
        pn532_handle_bus_completion(transaction, transaction_result);
        return;
    }
    if (PN532_BUS_IDLE != g_pn532_bus_state)
    {
        return;
    }

    if (PN532_STATE_BOOT_WAIT == g_pn532_state)
    {
        if (system_tick_elapsed(g_pn532_state_started_ms, PN532_RESTART_DELAY_MS))
        {
            pn532_start_sam();
        }
        return;
    }

    if (PN532_STATE_IDLE == g_pn532_state)
    {
        if (g_pn532_ready && system_tick_elapsed(g_pn532_last_poll_ms, PN532_SCAN_INTERVAL_MS))
        {
            pn532_start_scan();
        }
        return;
    }

    if ((PN532_STATE_COMMAND_WRITE == g_pn532_state) ||
        (PN532_STATE_ACK_STATUS == g_pn532_state) ||
        (PN532_STATE_ACK_READ == g_pn532_state) ||
        (PN532_STATE_RESPONSE_STATUS == g_pn532_state) ||
        (PN532_STATE_RESPONSE_READ == g_pn532_state) ||
        (PN532_STATE_ABORT_WRITE == g_pn532_state))
    {
        return;
    }

    /*
     * InListPassiveTarget is allowed to remain pending while no ISO14443A
     * card is in the RF field.  That is an idle/waiting condition, not a
     * PN532 fault.  Keep the timeout for command ACKs and configuration
     * replies, but do not tear down and reinitialize a healthy reader merely
     * because no card has been presented for five seconds.
     */
    if (!((PN532_OPERATION_SCAN == g_pn532_operation) &&
          (PN532_STATE_WAIT_RESPONSE == g_pn532_state)) &&
        system_tick_elapsed(g_pn532_state_started_ms, PN532_RESPONSE_TIMEOUT_MS))
    {
        if (PN532_STATE_WAIT_ACK == g_pn532_state)
        {
            char status_text[144];
            char const * operation_text = "InListPassiveTarget";

            if (PN532_OPERATION_SAM == g_pn532_operation)
            {
                operation_text = "SAMConfiguration";
            }
            else if (PN532_OPERATION_RF_RETRIES == g_pn532_operation)
            {
                operation_text = "RFConfiguration";
            }

            (void) snprintf(status_text,
                            sizeof(status_text),
                            "[NFC] %s ACK timeout: last status=0x%02X, polls=%lu; aborting and recovering.\r\n",
                            operation_text,
                            (unsigned int) g_pn532_last_status,
                            (unsigned long) g_pn532_status_poll_count);
            pn532_restart_later(status_text);
        }
        else
        {
            if (PN532_OPERATION_SAM == g_pn532_operation)
            {
                pn532_restart_later("[NFC] SAMConfiguration response timeout; aborting and recovering.\r\n");
            }
            else
            {
                pn532_restart_later("[NFC] RFConfiguration response timeout; aborting and recovering.\r\n");
            }
        }
        return;
    }
    uint32_t poll_interval_ms = (PN532_STATE_WAIT_RESPONSE == g_pn532_state) ?
                                PN532_RESPONSE_POLL_MS : PN532_STATUS_POLL_MS;
    if ((0U != g_pn532_last_poll_ms) &&
        ((uint32_t) (now - g_pn532_last_poll_ms) < poll_interval_ms))
    {
        return;
    }
    if (PN532_STATE_WAIT_ACK == g_pn532_state)
    {
        if (FSP_SUCCESS != pn532_start_status_read(PN532_TRANSACTION_ACK_STATUS))
        {
            pn532_restart_later("[NFC] PN532 ACK status read could not start; reinitializing.\r\n");
        }
        else
        {
            g_pn532_state = PN532_STATE_ACK_STATUS;
            g_pn532_last_poll_ms = now;
        }
        return;
    }

    if (PN532_STATE_WAIT_RESPONSE == g_pn532_state)
    {
        if (FSP_SUCCESS != pn532_start_status_read(PN532_TRANSACTION_RESPONSE_STATUS))
        {
            pn532_restart_later("[NFC] PN532 response status read could not start; reinitializing.\r\n");
        }
        else
        {
            g_pn532_state = PN532_STATE_RESPONSE_STATUS;
            g_pn532_last_poll_ms = now;
        }
    }
}

bool pn532_ready(void)
{
    return g_pn532_ready && g_pn532_enabled;
}

bool pn532_card_present(void)
{
    return g_pn532_card_present;
}

uint32_t pn532_scan_generation(void)
{
    return g_pn532_scan_generation;
}

bool pn532_take_uid(uint8_t * uid, uint8_t * uid_length)
{
    if ((!g_pn532_uid_pending) || (NULL == uid) || (NULL == uid_length))
    {
        return false;
    }

    memcpy(uid, g_pn532_uid, g_pn532_uid_length);
    *uid_length = g_pn532_uid_length;
    g_pn532_uid_pending = false;
    return true;
}

void pn532_suspend(void)
{
    g_pn532_enabled = false;
    g_pn532_ready = false;
    g_pn532_card_present = false;
    g_pn532_uid_pending = false;
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl, PN532_CS_PIN, BSP_IO_LEVEL_HIGH);
    if (PN532_BUS_TRANSFER == g_pn532_bus_state)
    {
        (void) R_SPI_Close(&g_spi0_ctrl);
        (void) R_SPI_Open(&g_spi0_ctrl, &g_spi0_cfg);
    }
    g_pn532_spi_done = false;
    g_pn532_spi_error = false;
    g_pn532_bus_state = PN532_BUS_IDLE;
    g_pn532_bus_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completed_transaction = PN532_TRANSACTION_NONE;
    g_pn532_bus_completion_pending = false;
}

void pn532_resume(void)
{
    if (g_pn532_initialized && (!g_pn532_enabled))
    {
        g_pn532_enabled = true;
        g_pn532_ready = false;
        g_pn532_state = PN532_STATE_BOOT_WAIT;
        g_pn532_state_started_ms = system_tick_now();
    }
}
