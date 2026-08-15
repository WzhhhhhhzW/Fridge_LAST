#include "deepseek_bridge.h"
#include "DOOR.h"
#include "audio_voice.h"
#include "access_control.h"
#include "health_assistant.h"
#include "ina226_power.h"
#include "ota_update.h"
#include "variable.h"

#include <stdlib.h>

#define BRIDGE_WIFI_SSID              "dou_phone"
#define BRIDGE_WIFI_PASSWORD          "10000086"
#define BRIDGE_SERVER_HOST            "101.245.87.7"
#define BRIDGE_SERVER_PORT            10086
#define BRIDGE_LOCAL_PORT_FIRST       20000U
#define BRIDGE_LOCAL_PORT_LAST        20999U
#define W800_SOCKET_OPEN_ATTEMPTS     3U

#define PC_LINE_BUFFER_SIZE           512U
#define PC_TX_RING_SIZE               4096U
#define W800_RX_BUFFER_SIZE           8192U
#define W800_RESPONSE_BUFFER_SIZE     768U
#define W800_SOCKET_RX_SIZE           512U
#define W800_MAX_SEND_SIZE            4096U
#define W800_AT_DATA_CHUNK_SIZE       512U
#define W800_AT_DATA_GUARD_MS         5U
#define W800_TARGET_BAUD              1000000U
#define W800_LEGACY_BAUD              2000000U
#define W800_BAUD_ERROR_X_1000        5000U

#define UART_TX_TIMEOUT_MS            3000U
#define W800_SHORT_TIMEOUT_MS         1200U
#define W800_BAUD_COMMAND_TIMEOUT_MS  600U
#define W800_BAUD_REBOOT_DELAY_MS     800U
#define W800_BAUD_PROBE_ATTEMPTS      3U
#define W800_CONFIG_TIMEOUT_MS        5000U
#define W800_JOIN_TIMEOUT_MS          5000U
#define W800_SOCKET_TIMEOUT_MS        12000U
#define BRIDGE_REPLY_TIMEOUT_MS       120000U
#define BRIDGE_RX_POLL_INTERVAL_MS    500U
#define PC_LINE_IDLE_SEND_MS          1500U
#define W800_RESET_READY_TIMEOUT_MS   3000U
#define W800_IP_READY_TIMEOUT_MS      3000U
#define W800_IP_POLL_INTERVAL_MS      200U
#define FRIDGE_STATUS_INTERVAL_MS     1000U
#define FRIDGE_OFFLINE_RETRY_MS       30000U
#define FRIDGE_STATUS_TIMEOUT_MS      3000U
#define FRIDGE_VISION_SLOT_NONE       255U
#define W800_DEBUG_AT                 1
#define BRIDGE_ENABLE_UART7_CHAT      0
#define BRIDGE_TEST_REQUEST_TIMEOUT_MS 60000U

static volatile bool g_pc_tx_busy = false;
static volatile bool g_pc_tx_complete_pending = false;
static volatile uint16_t g_pc_tx_head = 0U;
static volatile uint16_t g_pc_tx_tail = 0U;
static volatile uint16_t g_pc_tx_active_length = 0U;
static volatile uint32_t g_pc_tx_dropped_messages = 0U;
static uint8_t g_pc_tx_ring[PC_TX_RING_SIZE];
static volatile bool g_w800_tx_busy = false;

static volatile char g_pc_line_buffer[PC_LINE_BUFFER_SIZE];
static volatile uint16_t g_pc_line_length = 0U;
static volatile bool g_pc_line_ready = false;
static volatile uint32_t g_pc_last_rx_ms = 0U;
static char g_pc_pending_line[PC_LINE_BUFFER_SIZE];

static volatile uint8_t g_w800_rx_buffer[W800_RX_BUFFER_SIZE];
static volatile uint16_t g_w800_rx_head = 0U;
static volatile uint16_t g_w800_rx_tail = 0U;
static volatile uint32_t g_w800_rx_overflow_errors = 0U;
static volatile uint32_t g_w800_rx_framing_errors = 0U;
static volatile uint32_t g_w800_rx_parity_errors = 0U;

static bool g_bridge_online = false;
static bool g_audio_active = false;
static bool g_ota_active = false;
static bool g_w800_high_baud_ready = false;
static bool g_waiting_reply = false;
#if BRIDGE_ENABLE_UART7_CHAT
static uint32_t g_reply_start_ms = 0U;
static uint32_t g_last_rx_poll_ms = 0U;
#endif
static int g_w800_socket = -1;
static uint16_t g_w800_next_local_port = BRIDGE_LOCAL_PORT_FIRST;
static size_t g_end_marker_pos = 0U;
static char g_w800_response[W800_RESPONSE_BUFFER_SIZE];
static char g_w800_command_buffer[128];
static char g_w800_socket_header[128];
static uint8_t g_w800_socket_rx_data[W800_SOCKET_RX_SIZE];
static uint8_t g_w800_tx_stage[W800_MAX_SEND_SIZE];
static uint32_t g_last_fridge_status_ms = 0U;
static uint32_t g_last_status_rtt_ms = 0U;
static bool g_last_status_rtt_valid = false;
static volatile uint8_t g_latest_vision_slot = FRIDGE_VISION_SLOT_NONE;
static bool g_critical_background_service_active = false;
static bool g_pc_uart_ready = false;
static bool g_w800_uart_ready = false;
static deepseek_bridge_cooperative_hook_t g_cooperative_hook = NULL;

static void pc_tx_process(void);

/*
 * The W800 driver still contains synchronous AT transactions.  Keep the
 * real-time local peripherals moving while one of those transactions waits
 * for UART data.  The guard is important because KWS/PN532 diagnostics can
 * themselves write to UART7.
 */
static void bridge_service_critical_background(void)
{
    pc_tx_process();

    if (g_critical_background_service_active)
    {
        return;
    }

    g_critical_background_service_active = true;
    ota_update_watchdog_refresh();
    audio_voice_service_realtime_inputs();
    access_control_process();
    door_process();
    if (NULL != g_cooperative_hook)
    {
        g_cooperative_hook();
    }
    audio_voice_service_kws_capture();
    g_critical_background_service_active = false;
}

static void fridge_apply_control_code(uint8_t data)
{
    if (data == 0x01U)
    {
        set_temp++;
    }
    else if (data == 0x02U)
    {
        set_temp--;
    }
    else if (data == 0x03U)
    {
        cooling_interlock_submit_code(data);
    }
    else if ((data == 0x04U) || (data == 0x09U))
    {
        cooling_interlock_submit_code(data);
    }
    else if (data == 0x05U)
    {
        door_state = true;
    }
    else if (data == 0x0AU)
    {
        cooling_interlock_submit_code(data);
    }
}

static fsp_err_t uart_write_blocking(sci_uart_instance_ctrl_t * p_ctrl,
                                     volatile bool * p_busy,
                                     const uint8_t * p_data,
                                     uint32_t length,
                                     uint32_t timeout_ms)
{
    uint32_t start_ms;
    fsp_err_t write_err;

    if ((NULL == p_data) || (0U == length))
    {
        return FSP_SUCCESS;
    }

    if (((&g_uart7_ctrl == p_ctrl) && (!g_pc_uart_ready)) ||
        ((&g_uart6_ctrl == p_ctrl) && (!g_w800_uart_ready)))
    {
        return FSP_ERR_NOT_OPEN;
    }

    start_ms = system_tick_now();
    while (*p_busy)
    {
        ota_update_watchdog_refresh();
        if (&g_uart6_ctrl == p_ctrl)
        {
            bridge_service_critical_background();
        }
        if (system_tick_elapsed(start_ms, timeout_ms))
        {
            return FSP_ERR_TIMEOUT;
        }

        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    *p_busy = true;
    write_err = R_SCI_UART_Write(p_ctrl, p_data, length);
    if (FSP_SUCCESS != write_err)
    {
        *p_busy = false;
        return write_err;
    }

    start_ms = system_tick_now();
    while (*p_busy)
    {
        ota_update_watchdog_refresh();
        if (&g_uart6_ctrl == p_ctrl)
        {
            bridge_service_critical_background();
        }
        if (system_tick_elapsed(start_ms, timeout_ms))
        {
            *p_busy = false;
            return FSP_ERR_TIMEOUT;
        }

        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    return FSP_SUCCESS;
}

static void pc_write(const char * text)
{
    uint16_t head;
    uint16_t tail;
    uint32_t length;
    uint32_t free_bytes;
    uint32_t index;

    if ((NULL == text) || (!g_pc_uart_ready))
    {
        return;
    }

    pc_tx_process();

    length = (uint32_t) strlen(text);
    if (0U == length)
    {
        return;
    }

    /*
     * UART7 is diagnostic/control traffic, not a real-time dependency. Copy
     * complete messages into a single-producer ring and let SCI transmit them
     * asynchronously.  The ISR only marks completed spans; it never mutates
     * the producer head, so publishing the new head after the copy is enough
     * to keep partially copied messages invisible to the transmitter.
     */
    head = g_pc_tx_head;
    tail = g_pc_tx_tail;
    if (tail > head)
    {
        free_bytes = (uint32_t) tail - (uint32_t) head - 1U;
    }
    else
    {
        free_bytes = PC_TX_RING_SIZE - ((uint32_t) head - (uint32_t) tail) - 1U;
    }

    if (length > free_bytes)
    {
        g_pc_tx_dropped_messages++;
        pc_tx_process();
        return;
    }

    for (index = 0U; index < length; index++)
    {
        g_pc_tx_ring[head] = (uint8_t) text[index];
        head = (uint16_t) (((uint32_t) head + 1U) % PC_TX_RING_SIZE);
    }

    __DMB();
    g_pc_tx_head = head;
    pc_tx_process();
}

static void pc_tx_process(void)
{
    uint16_t head;
    uint16_t tail;
    uint16_t length;
    fsp_err_t write_err;

    if (!g_pc_uart_ready)
    {
        return;
    }

    if (g_pc_tx_complete_pending)
    {
        g_pc_tx_complete_pending = false;
        tail = g_pc_tx_tail;
        tail = (uint16_t) (((uint32_t) tail + g_pc_tx_active_length) % PC_TX_RING_SIZE);
        g_pc_tx_tail = tail;
        g_pc_tx_active_length = 0U;
        g_pc_tx_busy = false;
    }

    if (g_pc_tx_busy)
    {
        return;
    }

    head = g_pc_tx_head;
    tail = g_pc_tx_tail;
    if (head == tail)
    {
        return;
    }

    if (head > tail)
    {
        length = (uint16_t) (head - tail);
    }
    else
    {
        length = (uint16_t) (PC_TX_RING_SIZE - tail);
    }

    g_pc_tx_active_length = length;
    g_pc_tx_busy = true;
    write_err = R_SCI_UART_Write(&g_uart7_ctrl, &g_pc_tx_ring[tail], length);
    if (FSP_SUCCESS != write_err)
    {
        g_pc_tx_active_length = 0U;
        g_pc_tx_busy = false;
    }
}

void deepseek_bridge_pc_write(const char * text)
{
    pc_write(text);
}

void deepseek_bridge_set_vision_slot(uint8_t slot)
{
    if (slot < 32U)
    {
        g_latest_vision_slot = slot;
    }
}

#if BRIDGE_ENABLE_UART7_CHAT
static void pc_write_bytes(const uint8_t * p_data, uint32_t length)
{
    char chunk_text[161];
    uint32_t offset = 0U;

    while (offset < length)
    {
        uint32_t chunk = length - offset;
        if (chunk > 160U)
        {
            chunk = 160U;
        }

        memcpy(chunk_text, &p_data[offset], chunk);
        chunk_text[chunk] = '\0';
        pc_write(chunk_text);
        offset += chunk;
    }
}
#endif

static void w800_write(const char * text)
{
    if (NULL != text)
    {
        (void) uart_write_blocking(&g_uart6_ctrl,
                                   &g_w800_tx_busy,
                                   (const uint8_t *) text,
                                   (uint32_t) strlen(text),
                                   UART_TX_TIMEOUT_MS);
    }
}

static void w800_rx_push(uint8_t data)
{
    uint16_t next_head = (uint16_t) ((g_w800_rx_head + 1U) % W800_RX_BUFFER_SIZE);

    if (next_head != g_w800_rx_tail)
    {
        g_w800_rx_buffer[g_w800_rx_head] = data;
        g_w800_rx_head = next_head;
    }
}

static bool w800_rx_pop(uint8_t * p_data)
{
    if (g_w800_rx_tail == g_w800_rx_head)
    {
        return false;
    }

    *p_data = g_w800_rx_buffer[g_w800_rx_tail];
    g_w800_rx_tail = (uint16_t) ((g_w800_rx_tail + 1U) % W800_RX_BUFFER_SIZE);
    return true;
}

static void w800_rx_clear(void)
{
    g_w800_rx_tail = g_w800_rx_head;
}

static void w800_background_delay(uint32_t delay_ms)
{
    uint32_t start_ms = system_tick_now();

    do
    {
        bridge_service_critical_background();
        if (system_tick_elapsed(start_ms, delay_ms))
        {
            break;
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    } while (true);
}

static bool w800_read_byte(uint8_t * p_data, uint32_t timeout_ms)
{
    uint32_t start_ms = system_tick_now();

    bridge_service_critical_background();
    while (!w800_rx_pop(p_data))
    {
        bridge_service_critical_background();
        if (system_tick_elapsed(start_ms, timeout_ms))
        {
            return false;
        }

        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    return true;
}

static bool response_has_result(const char * response)
{
    return ((NULL != strstr(response, "+OK")) || (NULL != strstr(response, "+ERR")));
}

static void pc_write_w800_response(const char * prefix, const char * response)
{
    pc_write(prefix);
    if ((NULL != response) && ('\0' != response[0]))
    {
        pc_write(response);
    }
    else
    {
        pc_write("(empty)");
    }

    pc_write("\r\n");
}

static bool w800_collect_response(char * response, size_t response_size, uint32_t timeout_ms)
{
    uint32_t start_ms = system_tick_now();
    size_t index = 0U;
    uint8_t byte = 0U;

    if ((NULL == response) || (0U == response_size))
    {
        return false;
    }

    response[0] = '\0';
    while (!system_tick_elapsed(start_ms, timeout_ms))
    {
        ota_update_watchdog_refresh();
        if (!w800_read_byte(&byte, 20U))
        {
            continue;
        }

        if (index < (response_size - 1U))
        {
            response[index++] = (char) byte;
            response[index] = '\0';
        }

        if (response_has_result(response) && (NULL != strstr(response, "\r\n\r\n")))
        {
            return true;
        }
    }

    return response_has_result(response);
}

static bool w800_command(const char * command, char * response, size_t response_size, uint32_t timeout_ms)
{
    char * p_response = (NULL == response) ? g_w800_response : response;
    size_t target_size = ((NULL == response) || (0U == response_size)) ? sizeof(g_w800_response) : response_size;
    bool collected;
    uint32_t overflow_before = g_w800_rx_overflow_errors;
    uint32_t framing_before = g_w800_rx_framing_errors;
    uint32_t parity_before = g_w800_rx_parity_errors;

    w800_rx_clear();
#if W800_DEBUG_AT
    pc_write("[AT CMD] ");
    pc_write(command);
    pc_write("\r\n");
#endif
    w800_write(command);
    w800_write("\r");

    collected = w800_collect_response(p_response, target_size, timeout_ms);
#if W800_DEBUG_AT
    pc_write_w800_response(collected ? "[AT RSP] " : "[AT TIMEOUT] ", p_response);
#endif

    if (!collected)
    {
        uint32_t overflow_delta = g_w800_rx_overflow_errors - overflow_before;
        uint32_t framing_delta = g_w800_rx_framing_errors - framing_before;
        uint32_t parity_delta = g_w800_rx_parity_errors - parity_before;

        if ((overflow_delta > 0U) || (framing_delta > 0U) || (parity_delta > 0U))
        {
            char error_text[128];

            (void) snprintf(error_text,
                            sizeof(error_text),
                            "[W800] SCI6 RX errors during command: overflow=%lu framing=%lu parity=%lu.\r\n",
                            (unsigned long) overflow_delta,
                            (unsigned long) framing_delta,
                            (unsigned long) parity_delta);
            pc_write(error_text);
        }
        return false;
    }

    return (NULL != strstr(p_response, "+OK"));
}

static bool w800_set_mcu_baud(uint32_t baud)
{
    baud_setting_t setting = {0};
    fsp_err_t baud_err;
    char baud_message[128];
    uint32_t wait_start_ms = system_tick_now();

    while (g_w800_tx_busy)
    {
        ota_update_watchdog_refresh();
        bridge_service_critical_background();
        if (system_tick_elapsed(wait_start_ms, UART_TX_TIMEOUT_MS))
        {
            g_w800_tx_busy = false;
            pc_write("[W800] timed out waiting for SCI6 TX completion.\r\n");
            return false;
        }
        R_BSP_SoftwareDelay(1U, BSP_DELAY_UNITS_MILLISECONDS);
    }

    baud_err = R_SCI_UART_BaudCalculate(baud,
                                        true,
                                        W800_BAUD_ERROR_X_1000,
                                        &setting);
    if (FSP_SUCCESS == baud_err)
    {
        baud_err = R_SCI_UART_BaudSet(&g_uart6_ctrl, &setting);
    }

    if (FSP_SUCCESS != baud_err)
    {
        pc_write("[W800] SCI6 baud switch failed.\r\n");
        return false;
    }

    (void) snprintf(baud_message,
                    sizeof(baud_message),
                    "[W800] SCI6 set to %lu bps (BRR=%u, MDDR=%u, BGDM=%u, ABCS=%u, ABCSE=%u).\r\n",
                    (unsigned long) baud,
                    (unsigned int) setting.brr,
                    (unsigned int) setting.mddr,
                    (unsigned int) setting.semr_baudrate_bits_b.bgdm,
                    (unsigned int) setting.semr_baudrate_bits_b.abcs,
                    (unsigned int) setting.semr_baudrate_bits_b.abcse);
    pc_write(baud_message);

    w800_rx_clear();
    w800_background_delay(30U);
    return true;
}

static bool w800_probe_target_baud(void)
{
    uint32_t attempt;
    char ready_message[80];

    if (!w800_set_mcu_baud(W800_TARGET_BAUD))
    {
        return false;
    }

    for (attempt = 0U; attempt < W800_BAUD_PROBE_ATTEMPTS; attempt++)
    {
        if (w800_command("AT+", g_w800_response, sizeof(g_w800_response), W800_SHORT_TIMEOUT_MS))
        {
            g_w800_high_baud_ready = true;
            (void) snprintf(ready_message,
                            sizeof(ready_message),
                            "[W800] UART is running at %lu bps.\r\n",
                            (unsigned long) W800_TARGET_BAUD);
            pc_write(ready_message);
            return true;
        }

        w800_background_delay(250U);
    }

    return false;
}

/*
 * Older firmware builds saved 2 Mbps in the W800.  The current hardware can
 * still transmit commands to the module at that rate, but receives damaged
 * short replies (typically "+O" instead of "+OK").  Send the persistent
 * 1 Mbps setting without depending on the damaged acknowledgement, then
 * probe the new rate.  If the W800 applies the setting immediately AT+Z is
 * harmless at the old rate; if it defers the setting, AT+Z applies it.
 */
static bool w800_migrate_legacy_baud(void)
{
    char baud_command[48];

    if (!w800_set_mcu_baud(W800_LEGACY_BAUD))
    {
        return false;
    }

    (void) snprintf(baud_command,
                    sizeof(baud_command),
                    "AT+UART=!%lu,0,0,0,0",
                    (unsigned long) W800_TARGET_BAUD);

    pc_write("[W800] attempting automatic migration from saved 2 Mbps to 1 Mbps.\r\n");
    w800_rx_clear();
#if W800_DEBUG_AT
    pc_write("[AT CMD] ");
    pc_write(baud_command);
    pc_write(" (legacy baud; acknowledgement not required)\r\n");
#endif
    w800_write(baud_command);
    w800_write("\r");
    w800_background_delay(100U);
#if W800_DEBUG_AT
    pc_write("[AT CMD] AT+Z (apply saved 1 Mbps setting)\r\n");
#endif
    w800_write("AT+Z\r");

    w800_background_delay(W800_BAUD_REBOOT_DELAY_MS);
    return w800_probe_target_baud();
}

static bool w800_ensure_high_baud(void)
{
    bool setting_accepted;
    char baud_command[48];
    char negotiation_message[80];

    if (g_w800_high_baud_ready)
    {
        /*
         * The module can reboot or lose its saved UART setting independently
         * of the RA6M5.  Never trust the cached flag without checking that the
         * W800 still answers at the currently selected baud rate.
         */
        if (w800_command("AT+",
                         g_w800_response,
                         sizeof(g_w800_response),
                         W800_SHORT_TIMEOUT_MS))
        {
            return true;
        }

        pc_write("[W800] cached high-speed state is stale; re-detecting UART baud.\r\n");
        g_w800_high_baud_ready = false;
    }

    (void) snprintf(negotiation_message,
                    sizeof(negotiation_message),
                    "[W800] negotiate UART at %lu bps...\r\n",
                    (unsigned long) W800_TARGET_BAUD);
    pc_write(negotiation_message);

    /*
     * A saved high-speed setting can already be active after a W800 power
     * cycle.  Probe 115200 first; if it is silent, probe the target rate and
     * finally migrate the legacy saved 2 Mbps setting used by older builds.
     */
    if (!w800_set_mcu_baud(115200U))
    {
        return false;
    }

    if (!w800_command("AT+", g_w800_response, sizeof(g_w800_response), W800_SHORT_TIMEOUT_MS))
    {
        pc_write("[W800] no response at 115200; probing an already-saved 1 Mbps setting.\r\n");
        if (w800_probe_target_baud())
        {
            return true;
        }

        if (w800_migrate_legacy_baud())
        {
            pc_write("[W800] legacy baud migration completed.\r\n");
            return true;
        }

        (void) w800_set_mcu_baud(115200U);
        pc_write("[W800] module is silent at 115200, 1 Mbps, and legacy 2 Mbps.\r\n");
        return false;
    }

    /*
     * Log the current UART setting before changing it.  The persistent '!'
     * form is used so the W800 and RA6M5 stay at the same speed after either
     * side is power-cycled.
     */
    (void) w800_command("AT+UART=?",
                        g_w800_response,
                        sizeof(g_w800_response),
                        W800_SHORT_TIMEOUT_MS);

    (void) snprintf(baud_command,
                    sizeof(baud_command),
                    "AT+UART=!%lu,0,0,0,0",
                    (unsigned long) W800_TARGET_BAUD);
    setting_accepted = w800_command(baud_command,
                                    g_w800_response,
                                    sizeof(g_w800_response),
                                    W800_BAUD_COMMAND_TIMEOUT_MS);

    if (setting_accepted)
    {
        /*
         * Some W800 AT firmware applies a persistent UART setting only after
         * reset.  Send AT+Z at the old speed without waiting for its response:
         * firmware that already switched to the target rate will ignore it,
         * while firmware that deferred the change will reboot and apply it.
         */
        pc_write("[W800] target baud setting accepted; applying saved setting.\r\n");
        w800_rx_clear();
#if W800_DEBUG_AT
        pc_write("[AT CMD] AT+Z (response may move to the new baud rate)\r\n");
#endif
        w800_write("AT+Z\r");
    }
    else
    {
        /*
         * A timeout can also mean the module switched before the acknowledgement
         * was received.  Probe the target rate before declaring failure.
         */
        pc_write("[W800] no old-speed +OK for the UART setting; probing target baud.\r\n");
    }

    w800_background_delay(W800_BAUD_REBOOT_DELAY_MS);
    if (w800_probe_target_baud())
    {
        return true;
    }

    /* Restore the generated baud and report the module's actual remaining state. */
    (void) w800_set_mcu_baud(115200U);
    if (w800_command("AT+", g_w800_response, sizeof(g_w800_response), W800_SHORT_TIMEOUT_MS))
    {
        pc_write("[W800] module remained at 115200; its AT firmware did not apply the target baud.\r\n");
    }
    else
    {
        pc_write("[W800] baud change failed and the module no longer answers at 115200.\r\n");
    }

    return false;
}

static bool w800_station_has_ip(void)
{
    const char * status;
    const char * ip_start;

    if (!w800_command("AT+LKSTT",
                      g_w800_response,
                      sizeof(g_w800_response),
                      W800_SHORT_TIMEOUT_MS))
    {
        return false;
    }

    status = strstr(g_w800_response, "+OK=");
    if (NULL == status)
    {
        return false;
    }

    status += 4;
    if ((status[0] != '1') || (status[1] != ','))
    {
        return false;
    }

    ip_start = strchr(status, '"');
    if (NULL == ip_start)
    {
        return false;
    }

    ip_start++;
    return (0 != strncmp(ip_start, "0.0.0.0\"", 8U));
}

static bool w800_wait_for_ip(uint32_t timeout_ms)
{
    uint32_t start_ms = system_tick_now();

    do
    {
        ota_update_watchdog_refresh();
        if (w800_station_has_ip())
        {
            return true;
        }

        w800_background_delay(W800_IP_POLL_INTERVAL_MS);
    } while (!system_tick_elapsed(start_ms, timeout_ms));

    return false;
}

static bool w800_configure_hotspot(void)
{
    pc_write("[W800] configure STA and hotspot...\r\n");
    if (!w800_command("AT+WPRT=0", g_w800_response, sizeof(g_w800_response), W800_CONFIG_TIMEOUT_MS))
    {
        pc_write("[W800] set STA mode failed.\r\n");
        return false;
    }

    if (!w800_command("AT+SSID=\"" BRIDGE_WIFI_SSID "\"",
                      g_w800_response,
                      sizeof(g_w800_response),
                      W800_CONFIG_TIMEOUT_MS))
    {
        pc_write("[W800] set SSID failed.\r\n");
        return false;
    }

    if (!w800_command("AT+KEY=1,0,\"" BRIDGE_WIFI_PASSWORD "\"",
                      g_w800_response,
                      sizeof(g_w800_response),
                      W800_CONFIG_TIMEOUT_MS))
    {
        pc_write("[W800] set password failed.\r\n");
        return false;
    }

    if (!w800_command("AT+NIP=0", g_w800_response, sizeof(g_w800_response), W800_CONFIG_TIMEOUT_MS))
    {
        pc_write("[W800] enable DHCP failed.\r\n");
        return false;
    }

    if (!w800_command("AT+PMTF", g_w800_response, sizeof(g_w800_response), W800_CONFIG_TIMEOUT_MS))
    {
        pc_write("[W800] save hotspot configuration failed.\r\n");
        return false;
    }

    if (!w800_command("AT+Z", g_w800_response, sizeof(g_w800_response), W800_CONFIG_TIMEOUT_MS))
    {
        pc_write("[W800] reset command failed.\r\n");
        return false;
    }

    w800_rx_clear();
    pc_write("[W800] wait module reboot...\r\n");
    if (!w800_command("AT+",
                      g_w800_response,
                      sizeof(g_w800_response),
                      W800_SHORT_TIMEOUT_MS))
    {
        pc_write("[W800] module did not become ready after reset.\r\n");
        return false;
    }

    return true;
}

static bool w800_join_saved_hotspot(void)
{
    pc_write("[W800] join hotspot using saved configuration...\r\n");
    if (!w800_command("AT+WJOIN",
                      g_w800_response,
                      sizeof(g_w800_response),
                      W800_JOIN_TIMEOUT_MS))
    {
        return false;
    }

    pc_write("[W800] wait for STA IP...\r\n");
    return w800_wait_for_ip(W800_IP_READY_TIMEOUT_MS);
}

static void w800_close_socket(void)
{
    if (g_w800_socket < 0)
    {
        return;
    }

    (void) snprintf(g_w800_command_buffer,
                    sizeof(g_w800_command_buffer),
                    "AT+SKCLS=%d",
                    g_w800_socket);
    (void) w800_command(g_w800_command_buffer,
                        g_w800_response,
                        sizeof(g_w800_response),
                        W800_SHORT_TIMEOUT_MS);
    g_w800_socket = -1;
}

static bool w800_open_socket(void)
{
    uint32_t attempt;

    for (attempt = 0U; attempt < W800_SOCKET_OPEN_ATTEMPTS; attempt++)
    {
        char * ok_pos;
        uint16_t local_port = g_w800_next_local_port;

        g_w800_next_local_port++;
        if (g_w800_next_local_port > BRIDGE_LOCAL_PORT_LAST)
        {
            g_w800_next_local_port = BRIDGE_LOCAL_PORT_FIRST;
        }

        (void) snprintf(g_w800_command_buffer,
                        sizeof(g_w800_command_buffer),
                        "AT+SKCT=0,0,\"%s\",%d,%u",
                        BRIDGE_SERVER_HOST,
                        BRIDGE_SERVER_PORT,
                        (unsigned int) local_port);

        if (w800_command(g_w800_command_buffer,
                         g_w800_response,
                         sizeof(g_w800_response),
                         W800_SOCKET_TIMEOUT_MS))
        {
            ok_pos = strstr(g_w800_response, "+OK=");
            if (NULL != ok_pos)
            {
                g_w800_socket = atoi(ok_pos + 4);
                return (g_w800_socket >= 0);
            }
        }

        if (NULL == strstr(g_w800_response, "+ERR=-13"))
        {
            return false;
        }

        pc_write("[W800] socket connect failed; retrying with a new local port.\r\n");
        w800_background_delay(100U);
    }

    return false;
}

static bool w800_connect(void)
{
    g_bridge_online = false;

    pc_write("\r\n[W800] check module...\r\n");
    if (!w800_ensure_high_baud())
    {
        pc_write("[W800] high-speed UART negotiation failed; TCP audio is paused.\r\n");
        return false;
    }

    /*
     * w800_ensure_high_baud() returns true only after receiving +OK from an
     * AT+ probe at the selected baud.  A second immediate AT+ probe used to
     * be issued here.  Some W800 AT firmware can stop servicing the UART when
     * those two probes are sent back-to-back, leaving the request path stuck
     * before it ever reaches AT+LKSTT.  The successful baud probe is already
     * the required readiness check, so continue directly with the IP query.
     */
    w800_background_delay(30U);

    w800_close_socket();

    pc_write("[W800] check STA IP...\r\n");
    if (w800_station_has_ip())
    {
        pc_write("[W800] hotspot already connected.\r\n");
    }
    else if (!w800_join_saved_hotspot())
    {
        if (NULL == strstr(g_w800_response, "+ERR"))
        {
            pc_write("[W800] hotspot unavailable; keep local controls running and retry later.\r\n");
            return false;
        }

        pc_write("[W800] saved hotspot configuration rejected; applying firmware settings...\r\n");
        if ((!w800_configure_hotspot()) || (!w800_join_saved_hotspot()))
        {
            pc_write("[W800] join failed. Check SSID/password/security mode and 2.4GHz compatibility.\r\n");
            return false;
        }
    }

    pc_write("[W800] connect TCP server 101.245.87.7:10086...\r\n");
    if (!w800_open_socket())
    {
        pc_write("[W800] TCP connect failed. Check server service/firewall/security group 10086/TCP.\r\n");
        return false;
    }

    g_bridge_online = true;
    pc_write("[W800] online. UART7 commands: HA, MED, /ping.\r\n> ");
    return true;
}

bool deepseek_bridge_online(void)
{
    return g_bridge_online;
}

void deepseek_bridge_set_uart_availability(bool pc_uart_ready, bool w800_uart_ready)
{
    g_pc_uart_ready = pc_uart_ready;
    g_w800_uart_ready = w800_uart_ready;
    if (!g_pc_uart_ready)
    {
        g_pc_tx_head = 0U;
        g_pc_tx_tail = 0U;
        g_pc_tx_active_length = 0U;
        g_pc_tx_busy = false;
        g_pc_tx_complete_pending = false;
    }
    if (!g_w800_uart_ready)
    {
        g_bridge_online = false;
        g_w800_socket = -1;
        g_w800_high_baud_ready = false;
    }
}

void deepseek_bridge_set_cooperative_hook(deepseek_bridge_cooperative_hook_t hook)
{
    g_cooperative_hook = hook;
}

void deepseek_bridge_set_audio_active(bool active)
{
    g_audio_active = active;
}

void deepseek_bridge_set_ota_active(bool active)
{
    g_ota_active = active;
}

fsp_err_t deepseek_bridge_audio_prepare(void)
{
    if (!g_w800_uart_ready)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if (g_bridge_online && (g_w800_socket >= 0))
    {
        return FSP_SUCCESS;
    }

    return w800_connect() ? FSP_SUCCESS : FSP_ERR_NOT_OPEN;
}

fsp_err_t deepseek_bridge_ota_prepare(void)
{
    return deepseek_bridge_audio_prepare();
}

void deepseek_bridge_force_reconnect(void)
{
    if (g_w800_uart_ready)
    {
        w800_close_socket();
    }
    g_bridge_online = false;
    g_waiting_reply = false;
    g_w800_high_baud_ready = false;
}

static bool take_pc_line(char * out_line, size_t out_size)
{
    uint16_t length;

    if ((!g_pc_line_ready) || (NULL == out_line) || (0U == out_size))
    {
        return false;
    }

    length = g_pc_line_length;
    if (length >= out_size)
    {
        length = (uint16_t) (out_size - 1U);
    }

    for (uint16_t i = 0U; i < length; i++)
    {
        out_line[i] = (char) g_pc_line_buffer[i];
    }

    out_line[length] = '\0';
    g_pc_line_length = 0U;
    g_pc_line_ready = false;

    return true;
}

static fsp_err_t w800_socket_send_chunk(const uint8_t * p_data,
                                        uint32_t requested,
                                        uint32_t * p_actual,
                                        bool show_log)
{
    char * ok_pos;
    int actual_size;

    if ((g_w800_socket < 0) ||
        (NULL == p_data) ||
        (NULL == p_actual) ||
        (0U == requested) ||
        (requested > W800_MAX_SEND_SIZE))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    /*
     * This W800 AT firmware grants at most 512 bytes for one SKSND data
     * phase.  Requesting 4096 repeatedly works for a while, but under a
     * continuous audio stream the next command can reach the AT parser before
     * it has completely left the binary data phase.  Keep every transaction
     * at the module's native 512-byte boundary.
     */
    if (requested > W800_AT_DATA_CHUNK_SIZE)
    {
        requested = W800_AT_DATA_CHUNK_SIZE;
    }

    (void) snprintf(g_w800_command_buffer,
                    sizeof(g_w800_command_buffer),
                    "AT+SKSND=%d,%u",
                    g_w800_socket,
                    (unsigned int) requested);

    w800_rx_clear();
#if W800_DEBUG_AT
    if (show_log)
    {
        pc_write("[AT CMD] ");
        pc_write(g_w800_command_buffer);
        pc_write("\r\n");
    }
#endif
    w800_write(g_w800_command_buffer);
    w800_write("\r");

    if (!w800_collect_response(g_w800_response, sizeof(g_w800_response), W800_SOCKET_TIMEOUT_MS))
    {
#if W800_DEBUG_AT
        if (show_log)
        {
            pc_write_w800_response("[AT TIMEOUT] ", g_w800_response);
        }
#endif
        return FSP_ERR_TIMEOUT;
    }

#if W800_DEBUG_AT
    if (show_log)
    {
        pc_write_w800_response("[AT RSP] ", g_w800_response);
    }
#endif

    ok_pos = strstr(g_w800_response, "+OK=");
    if (NULL == ok_pos)
    {
        return FSP_ERR_ABORTED;
    }

    actual_size = atoi(ok_pos + 4);
    if (actual_size <= 0)
    {
        return FSP_ERR_ABORTED;
    }

    if ((uint32_t) actual_size > requested)
    {
        actual_size = (int) requested;
    }

    if (FSP_SUCCESS != uart_write_blocking(&g_uart6_ctrl,
                                           &g_w800_tx_busy,
                                           p_data,
                                           (uint32_t) actual_size,
                                           UART_TX_TIMEOUT_MS))
    {
        return FSP_ERR_TIMEOUT;
    }

    /*
     * SKSND has no second acknowledgement after the raw bytes.  Give the
     * module a short guard time to finish the binary phase before another AT
     * command is issued.  Seven 512-byte transactions still fit comfortably
     * inside one 100 ms audio-frame interval.
     */
    w800_background_delay(W800_AT_DATA_GUARD_MS);

    *p_actual = (uint32_t) actual_size;
    return FSP_SUCCESS;
}

fsp_err_t deepseek_bridge_socket_send_all(const uint8_t * p_data, uint32_t length)
{
    uint32_t offset = 0U;

    if ((NULL == p_data) && (length > 0U))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if ((!g_bridge_online) || (g_w800_socket < 0))
    {
        return FSP_ERR_NOT_OPEN;
    }

    while (offset < length)
    {
        uint32_t requested = length - offset;
        uint32_t actual = 0U;
        fsp_err_t send_err;

        ota_update_watchdog_refresh();
        if (requested > W800_MAX_SEND_SIZE)
        {
            requested = W800_MAX_SEND_SIZE;
        }

        send_err = w800_socket_send_chunk(&p_data[offset], requested, &actual, false);
        if ((FSP_SUCCESS != send_err) || (0U == actual))
        {
            g_bridge_online = false;
            return (FSP_SUCCESS == send_err) ? FSP_ERR_ABORTED : send_err;
        }

        offset += actual;
    }

    return FSP_SUCCESS;
}

fsp_err_t deepseek_bridge_socket_sendv_all(const deepseek_bridge_iovec_t * p_vectors,
                                           size_t vector_count)
{
    size_t vector_index = 0U;
    uint32_t vector_offset = 0U;

    if ((NULL == p_vectors) && (vector_count > 0U))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    while (vector_index < vector_count)
    {
        uint32_t staged = 0U;

        while ((vector_index < vector_count) && (staged < sizeof(g_w800_tx_stage)))
        {
            const deepseek_bridge_iovec_t * p_vector = &p_vectors[vector_index];
            uint32_t remaining;
            uint32_t copy_length;

            if ((NULL == p_vector->p_data) && (p_vector->length > 0U))
            {
                return FSP_ERR_INVALID_ARGUMENT;
            }

            if (vector_offset >= p_vector->length)
            {
                vector_index++;
                vector_offset = 0U;
                continue;
            }

            remaining = p_vector->length - vector_offset;
            copy_length = (uint32_t) sizeof(g_w800_tx_stage) - staged;
            if (copy_length > remaining)
            {
                copy_length = remaining;
            }

            memcpy(&g_w800_tx_stage[staged],
                   &p_vector->p_data[vector_offset],
                   copy_length);
            staged += copy_length;
            vector_offset += copy_length;
        }

        if (staged > 0U)
        {
            fsp_err_t send_err = deepseek_bridge_socket_send_all(g_w800_tx_stage, staged);
            if (FSP_SUCCESS != send_err)
            {
                return send_err;
            }
        }
    }

    return FSP_SUCCESS;
}

static bool w800_socket_send_line_internal(const char * line, bool show_log)
{
    size_t line_length;
    uint32_t send_length;
    uint32_t actual = 0U;
    fsp_err_t send_err;

    if (NULL == line)
    {
        return false;
    }

    line_length = strlen(line);
    send_length = (uint32_t) line_length + 1U;
    if (send_length > sizeof(g_w800_tx_stage))
    {
        return false;
    }

    memcpy(g_w800_tx_stage, line, line_length);
    g_w800_tx_stage[line_length] = '\n';
    send_err = w800_socket_send_chunk(g_w800_tx_stage, send_length, &actual, show_log);
    if ((FSP_SUCCESS == send_err) && (actual < send_length))
    {
        send_err = deepseek_bridge_socket_send_all(&g_w800_tx_stage[actual], send_length - actual);
    }

    if ((FSP_SUCCESS == send_err) && show_log)
    {
        pc_write("[W800] payload sent to server.\r\n");
    }

    return (FSP_SUCCESS == send_err);
}

#if BRIDGE_ENABLE_UART7_CHAT
static bool w800_socket_send_line(const char * line)
{
    return w800_socket_send_line_internal(line, true);
}
#endif

static int w800_socket_receive(uint8_t * out_data, uint32_t out_size, uint32_t timeout_ms)
{
    uint8_t byte = 0U;
    size_t header_index = 0U;
    int receive_size = 0;
    char * ok_pos;

    if ((g_w800_socket < 0) || (NULL == out_data) || (0U == out_size))
    {
        return -1;
    }

    (void) snprintf(g_w800_command_buffer,
                    sizeof(g_w800_command_buffer),
                    "AT+SKRCV=%d,%u",
                    g_w800_socket,
                    (unsigned int) out_size);

    w800_rx_clear();
    w800_write(g_w800_command_buffer);
    w800_write("\r");

    g_w800_socket_header[0] = '\0';
    while (header_index < (sizeof(g_w800_socket_header) - 1U))
    {
        ota_update_watchdog_refresh();
        if (!w800_read_byte(&byte, timeout_ms))
        {
            return 0;
        }

        g_w800_socket_header[header_index++] = (char) byte;
        g_w800_socket_header[header_index] = '\0';

        if (NULL != strstr(g_w800_socket_header, "\r\n\r\n"))
        {
            break;
        }

        if (NULL != strstr(g_w800_socket_header, "+ERR"))
        {
            return -1;
        }
    }

    ok_pos = strstr(g_w800_socket_header, "+OK=");
    if (NULL == ok_pos)
    {
        return -1;
    }

    receive_size = atoi(ok_pos + 4);
    if (receive_size <= 0)
    {
        return 0;
    }

    if ((uint32_t) receive_size > out_size)
    {
        receive_size = (int) out_size;
    }

    for (int i = 0; i < receive_size; i++)
    {
        ota_update_watchdog_refresh();
        if (!w800_read_byte(&out_data[i], timeout_ms))
        {
            return i;
        }
    }

    return receive_size;
}

int32_t deepseek_bridge_socket_receive(uint8_t * p_data,
                                       uint32_t length,
                                       uint32_t timeout_ms)
{
    int32_t received;

    if ((!g_bridge_online) || (g_w800_socket < 0))
    {
        return -1;
    }

    if ((NULL == p_data) || (0U == length))
    {
        return -1;
    }

    if (length > W800_MAX_SEND_SIZE)
    {
        length = W800_MAX_SEND_SIZE;
    }

    received = (int32_t) w800_socket_receive(p_data, length, timeout_ms);
    if (received < 0)
    {
        g_bridge_online = false;
    }
    return received;
}

static bool end_marker_seen(const uint8_t * p_data, uint32_t length)
{
    static const char marker[] = "[END]";

    for (uint32_t i = 0U; i < length; i++)
    {
        if ((char) p_data[i] == marker[g_end_marker_pos])
        {
            g_end_marker_pos++;
            if ('\0' == marker[g_end_marker_pos])
            {
                g_end_marker_pos = 0U;
                return true;
            }
        }
        else
        {
            g_end_marker_pos = ((char) p_data[i] == marker[0]) ? 1U : 0U;
        }
    }

    return false;
}

static void remove_end_marker_and_trim(char * response)
{
    char * marker;
    size_t length;

    if (NULL == response)
    {
        return;
    }

    marker = strstr(response, "[END]");
    if (NULL != marker)
    {
        *marker = '\0';
    }

    length = strlen(response);
    while ((length > 0U) &&
           ((response[length - 1U] == '\r') ||
            (response[length - 1U] == '\n') ||
            (response[length - 1U] == ' ')))
    {
        response[length - 1U] = '\0';
        length--;
    }
}

fsp_err_t deepseek_bridge_server_request(const char * request,
                                         char * response,
                                         size_t response_size,
                                         uint32_t timeout_ms)
{
    uint32_t start_ms;
    size_t response_length = 0U;

    if ((NULL == request) || (NULL == response) || (0U == response_size))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    response[0] = '\0';

    if (!g_w800_uart_ready)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if (g_waiting_reply || g_audio_active || g_ota_active)
    {
        return FSP_ERR_IN_USE;
    }

    /*
     * A command entered on UART7 (for example /ping) must also be able to
     * bring an offline bridge up.  Previously only the periodic background
     * poll attempted w800_connect(), so a manual request failed immediately
     * with FSP_ERR_NOT_OPEN even though the W800 could be reached.
     */
    if ((!g_bridge_online) && (!w800_connect()))
    {
        return FSP_ERR_NOT_OPEN;
    }

    if (!w800_socket_send_line_internal(request, false))
    {
        g_bridge_online = false;
        return FSP_ERR_ABORTED;
    }

    start_ms = system_tick_now();
    g_end_marker_pos = 0U;
    while (!system_tick_elapsed(start_ms, timeout_ms))
    {
        int rx_length = w800_socket_receive(g_w800_socket_rx_data,
                                            sizeof(g_w800_socket_rx_data),
                                            W800_SHORT_TIMEOUT_MS);
        ota_update_watchdog_refresh();
        if (rx_length > 0)
        {
            uint32_t copy_length = (uint32_t) rx_length;
            if (response_length < (response_size - 1U))
            {
                size_t remaining = (response_size - 1U) - response_length;
                if (copy_length > remaining)
                {
                    copy_length = (uint32_t) remaining;
                }

                memcpy(&response[response_length], g_w800_socket_rx_data, copy_length);
                response_length += copy_length;
                response[response_length] = '\0';
            }

            if (end_marker_seen(g_w800_socket_rx_data, (uint32_t) rx_length))
            {
                remove_end_marker_and_trim(response);
                return FSP_SUCCESS;
            }
        }

        w800_background_delay(50U);
    }

    remove_end_marker_and_trim(response);
    return FSP_ERR_TIMEOUT;
}

static void bridge_apply_fridge_targets(const char * response)
{
    const char prefix[] = "[FRIDGE] OK ";
    const char * payload;
    int target_set_temp = set_temp;
    int target_fan = fan_state;
    int target_cool = cool_state ? 1 : 0;
    int target_door = 0;
    int target_water_cool = Water_cool_State ? 1 : 0;
    int matched;

    if (NULL == response)
    {
        return;
    }

    payload = strstr(response, prefix);
    if (NULL == payload)
    {
        return;
    }

    payload += strlen(prefix);
    matched = sscanf(payload,
                     "%d|%d|%d|%d|%d",
                     &target_set_temp,
                     &target_fan,
                     &target_cool,
                     &target_door,
                     &target_water_cool);
    if (matched < 4)
    {
        return;
    }

    if (target_set_temp < -40)
    {
        target_set_temp = -40;
    }
    else if (target_set_temp > 80)
    {
        target_set_temp = 80;
    }

    if (target_fan < 0)
    {
        target_fan = 0;
    }
    else if (target_fan > 2)
    {
        target_fan = 2;
    }

    set_temp = target_set_temp;
    if (0 != target_door)
    {
        door_state = true;
    }
    cooling_interlock_set_targets((uint8_t) target_fan,
                                  (0 != target_water_cool),
                                  (0 != target_cool));
}

static void bridge_poll_fridge_status(void)
{
    uint32_t now = system_tick_now();
    uint32_t interval_ms = g_bridge_online ? FRIDGE_STATUS_INTERVAL_MS : FRIDGE_OFFLINE_RETRY_MS;
    int temp_x10;
    uint8_t vision_slot;
    char request[192];
    char response[96];
    char power_40_text[20];
    char power_41_text[20];
    char rtt_text[16];
    ina226_power_sample_t power_40;
    ina226_power_sample_t power_41;
    uint32_t request_start_ms;
    int written;
    fsp_err_t request_err;

    if (!system_tick_elapsed(g_last_fridge_status_ms, interval_ms))
    {
        return;
    }

    g_last_fridge_status_ms = now;

    if (!g_bridge_online)
    {
        (void) w800_connect();
        return;
    }

    temp_x10 = (temperature >= 0.0f) ?
               (int) ((temperature * 10.0f) + 0.5f) :
               (int) ((temperature * 10.0f) - 0.5f);
    vision_slot = g_latest_vision_slot;

    if (ina226_power_get_latest(0x40U, &power_40))
    {
        (void) snprintf(power_40_text, sizeof(power_40_text), "%ld", (long) power_40.power_dW);
    }
    else
    {
        (void) snprintf(power_40_text, sizeof(power_40_text), "NA");
    }

    if (ina226_power_get_latest(0x41U, &power_41))
    {
        (void) snprintf(power_41_text, sizeof(power_41_text), "%ld", (long) power_41.power_dW);
    }
    else
    {
        (void) snprintf(power_41_text, sizeof(power_41_text), "NA");
    }

    if (g_last_status_rtt_valid)
    {
        (void) snprintf(rtt_text, sizeof(rtt_text), "%lu", (unsigned long) g_last_status_rtt_ms);
    }
    else
    {
        (void) snprintf(rtt_text, sizeof(rtt_text), "NA");
    }

    written = snprintf(request,
                       sizeof(request),
                       "FRIDGE STATUS %d|%d|%u|%u|%u|%u|%u|%s|%s|%s",
                       set_temp,
                       temp_x10,
                       (unsigned int) fan_state,
                       cool_state ? 1U : 0U,
                       door_state ? 1U : 0U,
                       (unsigned int) vision_slot,
                       Water_cool_State ? 1U : 0U,
                       power_40_text,
                       power_41_text,
                       rtt_text);
    if ((written < 0) || ((size_t) written >= sizeof(request)))
    {
        return;
    }

    request_start_ms = system_tick_now();
    request_err = deepseek_bridge_server_request(request,
                                                 response,
                                                 sizeof(response),
                                                 FRIDGE_STATUS_TIMEOUT_MS);
    if (FSP_SUCCESS == request_err)
    {
        g_last_status_rtt_ms = system_tick_now() - request_start_ms;
        g_last_status_rtt_valid = true;
        bridge_apply_fridge_targets(response);
    }
    else
    {
        g_last_status_rtt_valid = false;
    }
}

static char ascii_upper(char ch)
{
    if ((ch >= 'a') && (ch <= 'z'))
    {
        return (char) (ch - ('a' - 'A'));
    }

    return ch;
}

static int ascii_strncmp_ci(const char * left, const char * right, size_t length)
{
    for (size_t i = 0U; i < length; i++)
    {
        char l_ch = ascii_upper(left[i]);
        char r_ch = ascii_upper(right[i]);
        if (l_ch != r_ch)
        {
            return (int) ((unsigned char) l_ch - (unsigned char) r_ch);
        }

        if ('\0' == l_ch)
        {
            return 0;
        }
    }

    return 0;
}

static int ascii_strcmp_ci(const char * left, const char * right)
{
    while ((*left != '\0') && (*right != '\0'))
    {
        char l_ch = ascii_upper(*left);
        char r_ch = ascii_upper(*right);
        if (l_ch != r_ch)
        {
            return (int) ((unsigned char) l_ch - (unsigned char) r_ch);
        }

        left++;
        right++;
    }

    return (int) ((unsigned char) ascii_upper(*left) - (unsigned char) ascii_upper(*right));
}

static bool starts_with_ci(const char * text, const char * prefix)
{
    return (0 == ascii_strncmp_ci(text, prefix, strlen(prefix)));
}

static const char * skip_spaces(const char * text)
{
    while ((NULL != text) && ((*text == ' ') || (*text == '\t')))
    {
        text++;
    }

    return text;
}

static bool parse_byte_text(const char * text, uint8_t * p_value)
{
    char * end = NULL;
    unsigned long value;
    int base = 10;

    if ((NULL == text) || (NULL == p_value) || ('\0' == text[0]))
    {
        return false;
    }

    if ((text[0] == '0') && ((text[1] == 'x') || (text[1] == 'X')))
    {
        base = 16;
    }

    value = strtoul(text, &end, base);
    if ((end == text) || ('\0' != *skip_spaces(end)) || (value > 0xFFUL))
    {
        return false;
    }

    *p_value = (uint8_t) value;
    return true;
}

static void bridge_print_test_help(void)
{
    pc_write("\r\n[UART7] test commands:\r\n");
    pc_write("  HA 发烧\r\n");
    pc_write("  HA 0x0C\r\n");
    pc_write("  MED LIST\r\n");
    pc_write("  MED SET 0|布洛芬|2026-05-01|36个月|退烧止痛|3\r\n");
    pc_write("  MED RESTOCK 0|2\r\n");
    pc_write("  MED TAKE 0|1\r\n");
    pc_write("  /ping\r\n");
    pc_write("  /ack   (play local 'wo zai' reply without ASR/TTS or upload)\r\n");
    pc_write("  /kws   (show local wake-word scores and state)\r\n");
    pc_write("  /kws on|off\r\n");
    pc_write("  /nfc | /nfc list | /nfc enroll | /nfc cancel\r\n");
    pc_write("  /silence (1 s digital-zero amplifier/noise isolation test)\r\n");
    pc_write("  /tone  (local 1 kHz SSIE speaker test at 15.944 kHz LRCLK)\r\n");
    pc_write("  /tone8 (local 500 Hz MAX98357A lock test at about 8.013 kHz LRCLK)\r\n");
    pc_write("  /tone32 (standard I2S, temporary 32-bit PCM test)\r\n");
    pc_write("  /tonelj (left-justified 32-bit compatibility test)\r\n> ");
}

static void bridge_print_request_error(const char * tag, fsp_err_t request_err)
{
    char line[80];

    (void) snprintf(line,
                    sizeof(line),
                    "\r\n[%s] request failed, err=%ld\r\n> ",
                    tag,
                    (long) request_err);
    pc_write(line);
}

static void bridge_send_raw_test_request(const char * request)
{
    char response[W800_RESPONSE_BUFFER_SIZE];
    fsp_err_t request_err;

    pc_write("\r\n[REQ] ");
    pc_write(request);
    pc_write("\r\n");

    request_err = deepseek_bridge_server_request(request,
                                                 response,
                                                 sizeof(response),
                                                 BRIDGE_TEST_REQUEST_TIMEOUT_MS);
    if (FSP_SUCCESS != request_err)
    {
        bridge_print_request_error("REQ", request_err);
        return;
    }

    pc_write(response);
    pc_write("\r\n> ");
}

static void bridge_handle_health_test_command(const char * argument)
{
    uint8_t symptom_code = 0U;
    const char * symptom = skip_spaces(argument);
    const char * symptom_from_code = NULL;
    char result_name[HEALTH_ASSISTANT_NAME_MAX];
    health_result_t result;

    if ((NULL == symptom) || ('\0' == symptom[0]))
    {
        bridge_print_test_help();
        return;
    }

    if (parse_byte_text(symptom, &symptom_code))
    {
        symptom_from_code = health_assistant_symptom_from_code(symptom_code);
        if (NULL == symptom_from_code)
        {
            pc_write("\r\n[HA] unknown symptom code\r\n> ");
            return;
        }

        symptom = symptom_from_code;
        result = health_assistant_recommend_by_code(symptom_code, result_name, sizeof(result_name));
    }
    else
    {
        result = health_assistant_recommend_by_text(symptom, result_name, sizeof(result_name));
    }

    pc_write("\r\n[HA] symptom=");
    pc_write(symptom);
    pc_write("\r\n");

    if (HEALTH_RESULT_OK == result)
    {
        pc_write("[HA] result=");
        pc_write(result_name);
        pc_write("\r\n> ");
        return;
    }

    pc_write("[HA] failed=");
    pc_write(health_result_text(result));
    pc_write("\r\n> ");
}

static bool bridge_handle_uart7_test_command(const char * line)
{
    const char * text = skip_spaces(line);

    if ((NULL == text) || ('\0' == text[0]))
    {
        pc_write("> ");
        return true;
    }

    if (starts_with_ci(text, "HA "))
    {
        bridge_handle_health_test_command(text + 3);
        return true;
    }

    if (0 == ascii_strcmp_ci(text, "HA"))
    {
        bridge_print_test_help();
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/ack")) ||
        (0 == ascii_strcmp_ci(text, "ack")))
    {
        fsp_err_t ack_err = audio_voice_play_ack();

        if (FSP_SUCCESS == ack_err)
        {
            pc_write("\r\n[ACK] local 'wo zai' playback accepted; no audio is uploaded.\r\n> ");
        }
        else if (FSP_ERR_IN_USE == ack_err)
        {
            pc_write("\r\n[ACK] audio path is busy; wait and try /ack again.\r\n> ");
        }
        else
        {
            bridge_print_request_error("ACK", ack_err);
        }
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/kws")) ||
        (0 == ascii_strcmp_ci(text, "kws")))
    {
        pc_write("\r\n");
        audio_voice_kws_log_status();
        pc_write("> ");
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/kws on")) ||
        (0 == ascii_strcmp_ci(text, "kws on")))
    {
        fsp_err_t kws_err = audio_voice_kws_set_enabled(true);
        if (FSP_SUCCESS == kws_err)
        {
            if (audio_voice_kws_enabled())
            {
                pc_write("\r\n[KWS] local wake-word listener enabled.\r\n> ");
            }
            else
            {
                pc_write("\r\n[KWS] listener armed; it will start in health mode (Flag=2).\r\n> ");
            }
        }
        else
        {
            bridge_print_request_error("KWS", kws_err);
        }
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/kws off")) ||
        (0 == ascii_strcmp_ci(text, "kws off")))
    {
        fsp_err_t kws_err = audio_voice_kws_set_enabled(false);
        if (FSP_SUCCESS == kws_err)
        {
            pc_write("\r\n[KWS] local wake-word listener disable requested.\r\n> ");
        }
        else
        {
            bridge_print_request_error("KWS", kws_err);
        }
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/nfc")) ||
        (0 == ascii_strcmp_ci(text, "nfc")))
    {
        pc_write("\r\n");
        access_control_log_status();
        pc_write("> ");
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/nfc list")) ||
        (0 == ascii_strcmp_ci(text, "nfc list")))
    {
        pc_write("\r\n");
        access_control_log_list();
        pc_write("> ");
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/nfc enroll")) ||
        (0 == ascii_strcmp_ci(text, "nfc enroll")))
    {
        pc_write("\r\n");
        access_control_enroll_start();
        pc_write("> ");
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/nfc cancel")) ||
        (0 == ascii_strcmp_ci(text, "nfc cancel")))
    {
        pc_write("\r\n");
        access_control_enroll_cancel();
        pc_write("> ");
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/tone")) ||
        (0 == ascii_strcmp_ci(text, "tone")))
    {
        fsp_err_t tone_err = audio_voice_play_test_tone();

        if (FSP_SUCCESS == tone_err)
        {
            pc_write("\r\n[TONE] local SSIE test accepted; listen for a 1 kHz tone.\r\n> ");
        }
        else if (FSP_ERR_IN_USE == tone_err)
        {
            pc_write("\r\n[TONE] audio path is busy; wait and try /tone again.\r\n> ");
        }
        else
        {
            bridge_print_request_error("TONE", tone_err);
        }
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/silence")) ||
        (0 == ascii_strcmp_ci(text, "silence")))
    {
        fsp_err_t silence_err = audio_voice_play_test_silence();

        if (FSP_SUCCESS == silence_err)
        {
            pc_write(
                "\r\n[SILENCE] digital-zero test accepted; listen for hiss/noise.\r\n> ");
        }
        else if (FSP_ERR_IN_USE == silence_err)
        {
            pc_write("\r\n[SILENCE] audio path is busy; wait and try again.\r\n> ");
        }
        else
        {
            bridge_print_request_error("SILENCE", silence_err);
        }
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/tone8")) ||
        (0 == ascii_strcmp_ci(text, "tone8")))
    {
        fsp_err_t tone_err = audio_voice_play_test_tone_8k();

        if (FSP_SUCCESS == tone_err)
        {
            pc_write(
                "\r\n[TONE8] 8 kHz lock test accepted; listen for a louder 500 Hz tone.\r\n> ");
        }
        else if (FSP_ERR_IN_USE == tone_err)
        {
            pc_write("\r\n[TONE8] audio path is busy; wait and try /tone8 again.\r\n> ");
        }
        else
        {
            bridge_print_request_error("TONE8", tone_err);
        }
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/tone32")) ||
        (0 == ascii_strcmp_ci(text, "tone32")))
    {
        fsp_err_t tone_err = audio_voice_play_test_tone_32bit();

        if (FSP_SUCCESS == tone_err)
        {
            pc_write(
                "\r\n[TONE32] 32-bit PCM test accepted; listen for a clear 1 kHz tone.\r\n> ");
        }
        else if (FSP_ERR_IN_USE == tone_err)
        {
            pc_write("\r\n[TONE32] audio path is busy; wait and try /tone32 again.\r\n> ");
        }
        else
        {
            bridge_print_request_error("TONE32", tone_err);
        }
        return true;
    }

    if ((0 == ascii_strcmp_ci(text, "/tonelj")) ||
        (0 == ascii_strcmp_ci(text, "tonelj")))
    {
        fsp_err_t tone_err = audio_voice_play_test_tone_left_justified();

        if (FSP_SUCCESS == tone_err)
        {
            pc_write(
                "\r\n[TONELJ] left-justified test accepted; listen for a clear 1 kHz tone.\r\n> ");
        }
        else if (FSP_ERR_IN_USE == tone_err)
        {
            pc_write("\r\n[TONELJ] audio path is busy; wait and try /tonelj again.\r\n> ");
        }
        else
        {
            bridge_print_request_error("TONELJ", tone_err);
        }
        return true;
    }

    if ((starts_with_ci(text, "MED ")) ||
        (0 == ascii_strcmp_ci(text, "/ping")) ||
        (0 == ascii_strcmp_ci(text, "ping")))
    {
        bridge_send_raw_test_request(text);
        return true;
    }

#if BRIDGE_ENABLE_UART7_CHAT
    return false;
#else
    pc_write("\r\n[UART7] chat disabled. Use HA, MED, /ping, /ack, /kws, /nfc, or speaker test commands.\r\n");
    bridge_print_test_help();
    return true;
#endif
}

void deepseek_bridge_init(void)
{
    pc_write("\r\n==============================\r\n");
    pc_write("Fridge DeepSeek UART Bridge\r\n");
    pc_write("FW: 1.0.0+22-vision-tts-nfc-v2-io-fix-v2-nfc-recover-v4, SSID=" BRIDGE_WIFI_SSID "\r\n");
    pc_write("UART7: PC terminal, W800: SCI6 UART0\r\n");
    pc_write("==============================\r\n");
    pc_write("[W800] WiFi will connect in background.\r\n");
    /* Run the first connection attempt immediately.  The 30-second offline
     * interval is only for retries after a failed attempt, not for startup. */
    g_last_fridge_status_ms = system_tick_now() - FRIDGE_OFFLINE_RETRY_MS;
    g_last_status_rtt_ms = 0U;
    g_last_status_rtt_valid = false;
}

void deepseek_bridge_process(void)
{
#if BRIDGE_ENABLE_UART7_CHAT
    int rx_length;
#endif

    pc_tx_process();

    if (g_audio_active || g_ota_active)
    {
        return;
    }

    if ((!g_pc_line_ready) &&
        (g_pc_line_length > 0U) &&
        system_tick_elapsed(g_pc_last_rx_ms, PC_LINE_IDLE_SEND_MS))
    {
        g_pc_line_ready = true;
    }

    if (take_pc_line(g_pc_pending_line, sizeof(g_pc_pending_line)))
    {
        if (strlen(g_pc_pending_line) == 0U)
        {
            pc_write("> ");
            return;
        }

        pc_write("\r\n[PC] input accepted.\r\n");
        if (bridge_handle_uart7_test_command(g_pc_pending_line))
        {
            return;
        }

#if BRIDGE_ENABLE_UART7_CHAT
        if (g_waiting_reply)
        {
            pc_write("\r\n[系统] 上一条消息还没有返回，请稍等。\r\n> ");
            return;
        }

        if (!g_bridge_online)
        {
            pc_write("\r\n[系统] 网络未连接，正在后台重连，请稍后再试。\r\n> ");
            return;
        }

        pc_write("\r\n[系统] 已发送到服务器，等待 DeepSeek 返回...\r\n");
        if (!w800_socket_send_line(g_pc_pending_line))
        {
            g_bridge_online = false;
            pc_write("[系统] 发送失败，连接将于下次输入时重连。\r\n> ");
            return;
        }

        g_waiting_reply = true;
        g_reply_start_ms = system_tick_now();
        g_last_rx_poll_ms = 0U;
        g_end_marker_pos = 0U;
#endif
    }

    if (!g_w800_uart_ready)
    {
        return;
    }

    bridge_poll_fridge_status();

#if BRIDGE_ENABLE_UART7_CHAT
    if (!g_waiting_reply)
    {
        return;
    }

    if (!system_tick_elapsed(g_last_rx_poll_ms, BRIDGE_RX_POLL_INTERVAL_MS))
    {
        return;
    }

    g_last_rx_poll_ms = system_tick_now();
    rx_length = w800_socket_receive(g_w800_socket_rx_data,
                                    sizeof(g_w800_socket_rx_data),
                                    W800_SHORT_TIMEOUT_MS);
    if (rx_length > 0)
    {
        pc_write_bytes(g_w800_socket_rx_data, (uint32_t) rx_length);
        if (end_marker_seen(g_w800_socket_rx_data, (uint32_t) rx_length))
        {
            g_waiting_reply = false;
            pc_write("\r\n> ");
        }
    }

    if (system_tick_elapsed(g_reply_start_ms, BRIDGE_REPLY_TIMEOUT_MS))
    {
        g_waiting_reply = false;
        pc_write("\r\n[系统] 等待服务器返回超时。\r\n> ");
    }
#endif
}

void deepseek_bridge_uart7_callback(uart_callback_args_t * p_args)
{
    uint8_t data;

    if (NULL == p_args)
    {
        return;
    }

    if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        g_pc_tx_complete_pending = true;
        return;
    }

    if (UART_EVENT_RX_CHAR != p_args->event)
    {
        return;
    }

    data = (uint8_t) p_args->data;
    if ((data >= 0x01U) && (data <= 0x05U) && (0U == g_pc_line_length))
    {
        fridge_apply_control_code(data);
        return;
    }

    if ((data == '\r') || (data == '\n'))
    {
        if ((g_pc_line_length > 0U) && (!g_pc_line_ready))
        {
            g_pc_line_ready = true;
            g_pc_last_rx_ms = system_tick_now();
        }

        return;
    }

    if ((!g_pc_line_ready) && (g_pc_line_length < (PC_LINE_BUFFER_SIZE - 1U)))
    {
        g_pc_line_buffer[g_pc_line_length++] = (char) data;
        g_pc_line_buffer[g_pc_line_length] = '\0';
        g_pc_last_rx_ms = system_tick_now();
    }
}

void deepseek_bridge_w800_callback(uart_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        g_w800_tx_busy = false;
        return;
    }

    if (UART_EVENT_RX_CHAR == p_args->event)
    {
        w800_rx_push((uint8_t) p_args->data);
        return;
    }

    if (UART_EVENT_ERR_OVERFLOW == p_args->event)
    {
        g_w800_rx_overflow_errors++;
        return;
    }

    if (UART_EVENT_ERR_FRAMING == p_args->event)
    {
        g_w800_rx_framing_errors++;
        return;
    }

    if (UART_EVENT_ERR_PARITY == p_args->event)
    {
        g_w800_rx_parity_errors++;
    }
}
