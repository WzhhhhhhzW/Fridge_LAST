#include "variable.h"//存放参数和头文件
#include "access_control.h"
#include "audio_voice.h"
#include "deepseek_bridge.h"
#include "health_assistant.h"
#include "ina226_power.h"
#include "medicine_storage.h"
#include "ota_update.h"
#include "system_tick.h"

FSP_CPP_HEADER
void R_BSP_WarmStart(bsp_warm_start_event_t event);
FSP_CPP_FOOTER

#define VISION_TEXT_BUFFER_SIZE (160U)
#define VISION_LINE_QUEUE_DEPTH (8U)
#define VISION_SPEECH_RETRY_MS  (5000U)
#define VISION_UNRECOGNIZED_TEXT "\xE6\x9C\xAA\xE8\xAF\x86\xE5\x88\xAB\xE5\x88\xB0\xE8\x8D\xAF\xE5\x93\x81"
#define LOCAL_CONTROL_PERIOD_MS (100U)
#define TEMPERATURE_DIAGNOSTIC_PERIOD_MS (1000U)
#define HEALTH_FRAME_INTERBYTE_TIMEOUT_MS (500U)

static volatile uint8_t g_vision_line_queue[VISION_LINE_QUEUE_DEPTH][VISION_TEXT_BUFFER_SIZE];
static volatile uint16_t g_vision_line_lengths[VISION_LINE_QUEUE_DEPTH];
static volatile uint8_t g_vision_line_head = 0U;
static volatile uint8_t g_vision_line_tail = 0U;
static volatile uint8_t g_vision_line_count = 0U;
static volatile bool g_vision_line_overflow = false;
static volatile uint16_t g_vision_rx_length = 0U;
static volatile bool g_vision_rx_discard = false;
static volatile uint8_t g_vision_rx_text[VISION_TEXT_BUFFER_SIZE];
static volatile bool g_inventory_list_pending = false;
static volatile bool g_inventory_list_busy = false;
static volatile bool g_health_assistant_pending = false;
static volatile uint8_t g_health_assistant_symptom_code = 0U;

#define INVENTORY_RECORDS_PER_PAGE    (5U)
#define INVENTORY_PAGE_TEXT_MAX       (1300U)

typedef enum e_inventory_page_request
{
    INVENTORY_PAGE_REQUEST_NONE = 0,
    INVENTORY_PAGE_REQUEST_PREVIOUS,
    INVENTORY_PAGE_REQUEST_NEXT,
} inventory_page_request_t;

typedef enum e_health_frame_state
{
    HEALTH_FRAME_WAIT_HEADER_FIRST = 0,
    HEALTH_FRAME_WAIT_HEADER_SECOND,
    HEALTH_FRAME_RECEIVING,
    HEALTH_FRAME_RECEIVE_HEADER_FIRST,
    HEALTH_FRAME_RECEIVE_TAIL_FIRST,
    HEALTH_FRAME_DISCARD,
    HEALTH_FRAME_DISCARD_HEADER_FIRST,
} health_frame_state_t;

static volatile health_frame_state_t g_health_frame_state = HEALTH_FRAME_WAIT_HEADER_FIRST;
static volatile uint16_t g_health_frame_length = 0U;
static volatile uint8_t g_health_frame_active_index = 0U;
static volatile uint8_t g_health_frame_pending_index = 0U;
static volatile uint16_t g_health_frame_pending_length = 0U;
static volatile bool g_health_text_pending = false;
static volatile bool g_health_request_busy = false;
static volatile uint32_t g_health_frame_last_byte_ms = 0U;
static volatile uint8_t g_health_frame_buffers[2][HEALTH_ASSISTANT_SYMPTOM_MAX];

static bool g_local_runtime_active = false;
static uint8_t g_audio_mode_applied = UINT8_MAX;
static bool g_local_control_has_run = false;
static uint32_t g_local_control_last_ms = 0U;
static bool g_temperature_diagnostic_has_run = false;
static uint32_t g_temperature_diagnostic_last_ms = 0U;
static bool g_local_oled_ready = false;
static uint8_t g_local_last_colder_duty = UINT8_MAX;
static uint8_t g_local_last_box_fan_duty = UINT8_MAX;
static uint8_t g_local_last_water_output = UINT8_MAX;

static uint8_t g_medicine_oled_tx_buf[TX_BUF_SIZE];
static char g_inventory_text[MEDICINE_INVENTORY_TEXT_MAX];
static char g_inventory_page_text[INVENTORY_PAGE_TEXT_MAX + 1U];
static volatile inventory_page_request_t g_inventory_page_request = INVENTORY_PAGE_REQUEST_NONE;
static uint8_t g_inventory_record_count = 0U;
static uint8_t g_inventory_current_page = 0U;
static uint8_t g_inventory_total_pages = 0U;
static bool g_inventory_cache_valid = false;

static void health_frame_reset(void)
{
    g_health_frame_state = HEALTH_FRAME_WAIT_HEADER_FIRST;
    g_health_frame_length = 0U;
    g_health_frame_last_byte_ms = 0U;
}

static void health_frame_complete(void)
{
    uint8_t completed_index = g_health_frame_active_index;

    if ((g_health_frame_length > 0U) &&
        (!g_health_text_pending) &&
        (!g_health_request_busy))
    {
        g_health_frame_buffers[completed_index][g_health_frame_length] = '\0';
        g_health_frame_pending_index = completed_index;
        g_health_frame_pending_length = g_health_frame_length;
        g_health_text_pending = true;
        g_health_frame_active_index = (uint8_t) (completed_index ^ 1U);
    }

    health_frame_reset();
}

static bool health_frame_receive_byte(uint8_t data)
{
    uint32_t now = system_tick_now();

    /*
     * A truncated FA FA ... FB FB frame must not capture every later HMI
     * command forever.  At 115200 bit/s a valid frame arrives in a few
     * milliseconds, so a 500 ms inter-byte gap unambiguously abandons the
     * damaged frame.  The current byte is then parsed again from the idle
     * state, allowing an independent C1/C2 recording command to pass through.
     */
    if ((HEALTH_FRAME_WAIT_HEADER_FIRST != g_health_frame_state) &&
        system_tick_elapsed(g_health_frame_last_byte_ms,
                            HEALTH_FRAME_INTERBYTE_TIMEOUT_MS))
    {
        health_frame_reset();
    }
    g_health_frame_last_byte_ms = now;

    switch (g_health_frame_state)
    {
        case HEALTH_FRAME_WAIT_HEADER_FIRST:
            if (0xFAU == data)
            {
                g_health_frame_state = HEALTH_FRAME_WAIT_HEADER_SECOND;
                return true;
            }
            return false;

        case HEALTH_FRAME_WAIT_HEADER_SECOND:
            if (0xFAU == data)
            {
                g_health_frame_length = 0U;
                g_health_frame_state = (g_health_request_busy || g_health_text_pending) ?
                                       HEALTH_FRAME_DISCARD : HEALTH_FRAME_RECEIVING;
                return true;
            }

            g_health_frame_state = HEALTH_FRAME_WAIT_HEADER_FIRST;
            return false;

        case HEALTH_FRAME_RECEIVING:
            if (0xFAU == data)
            {
                g_health_frame_state = HEALTH_FRAME_RECEIVE_HEADER_FIRST;
                return true;
            }

            if (0xFBU == data)
            {
                g_health_frame_state = HEALTH_FRAME_RECEIVE_TAIL_FIRST;
                return true;
            }

            if (g_health_frame_length < (HEALTH_ASSISTANT_SYMPTOM_MAX - 1U))
            {
                g_health_frame_buffers[g_health_frame_active_index][g_health_frame_length++] = data;
            }
            else
            {
                g_health_frame_state = HEALTH_FRAME_DISCARD;
            }
            return true;

        case HEALTH_FRAME_RECEIVE_HEADER_FIRST:
            if (0xFAU == data)
            {
                g_health_frame_length = 0U;
                g_health_frame_state = (g_health_request_busy || g_health_text_pending) ?
                                       HEALTH_FRAME_DISCARD : HEALTH_FRAME_RECEIVING;
            }
            else
            {
                g_health_frame_length = 0U;
                g_health_frame_state = HEALTH_FRAME_WAIT_HEADER_FIRST;
            }
            return true;

        case HEALTH_FRAME_RECEIVE_TAIL_FIRST:
            if (0xFBU == data)
            {
                health_frame_complete();
            }
            else if (0xFAU == data)
            {
                g_health_frame_length = 0U;
                g_health_frame_state = HEALTH_FRAME_WAIT_HEADER_SECOND;
            }
            else
            {
                g_health_frame_length = 0U;
                g_health_frame_state = HEALTH_FRAME_WAIT_HEADER_FIRST;
            }
            return true;

        case HEALTH_FRAME_DISCARD:
            if (0xFAU == data)
            {
                g_health_frame_state = HEALTH_FRAME_DISCARD_HEADER_FIRST;
            }
            return true;

        case HEALTH_FRAME_DISCARD_HEADER_FIRST:
            if (0xFAU == data)
            {
                g_health_frame_length = 0U;
                g_health_frame_state = (g_health_request_busy || g_health_text_pending) ?
                                       HEALTH_FRAME_DISCARD : HEALTH_FRAME_RECEIVING;
            }
            else
            {
                g_health_frame_state = HEALTH_FRAME_DISCARD;
            }
            return true;

        default:
            health_frame_reset();
            return false;
    }
}

static size_t health_normalize_symptom(char * text, size_t length)
{
    size_t read_index;
    size_t write_index = 0U;
    bool space_pending = false;

    for (read_index = 0U; read_index < length; read_index++)
    {
        char ch = text[read_index];
        if ((ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n'))
        {
            if (write_index > 0U)
            {
                space_pending = true;
            }
            continue;
        }

        if (space_pending && (write_index < (HEALTH_ASSISTANT_SYMPTOM_MAX - 1U)))
        {
            text[write_index++] = ' ';
        }

        space_pending = false;
        if (write_index < (HEALTH_ASSISTANT_SYMPTOM_MAX - 1U))
        {
            text[write_index++] = ch;
        }
    }

    text[write_index] = '\0';
    return write_index;
}

static bool vision_utf8_is_valid(const char * text)
{
    const uint8_t * data = (const uint8_t *) text;
    size_t index = 0U;

    if ((NULL == text) || ('\0' == text[0]))
    {
        return false;
    }

    while ('\0' != data[index])
    {
        uint8_t first = data[index];
        uint32_t codepoint;
        size_t continuation_count;
        size_t continuation_index;

        if (first < 0x80U)
        {
            if ((first < 0x20U) || (0x7FU == first))
            {
                return false;
            }

            index++;
            continue;
        }

        if ((first >= 0xC2U) && (first <= 0xDFU))
        {
            codepoint = (uint32_t) (first & 0x1FU);
            continuation_count = 1U;
        }
        else if ((first >= 0xE0U) && (first <= 0xEFU))
        {
            codepoint = (uint32_t) (first & 0x0FU);
            continuation_count = 2U;
        }
        else if ((first >= 0xF0U) && (first <= 0xF4U))
        {
            codepoint = (uint32_t) (first & 0x07U);
            continuation_count = 3U;
        }
        else
        {
            return false;
        }

        for (continuation_index = 1U;
             continuation_index <= continuation_count;
             continuation_index++)
        {
            uint8_t continuation = data[index + continuation_index];
            if ((0U == continuation) || ((continuation & 0xC0U) != 0x80U))
            {
                return false;
            }

            codepoint = (codepoint << 6U) | (uint32_t) (continuation & 0x3FU);
        }

        if (((1U == continuation_count) && (codepoint < 0x80U)) ||
            ((2U == continuation_count) && (codepoint < 0x800U)) ||
            ((3U == continuation_count) && (codepoint < 0x10000U)) ||
            ((codepoint >= 0xD800U) && (codepoint <= 0xDFFFU)) ||
            (codepoint > 0x10FFFFU))
        {
            return false;
        }

        index += continuation_count + 1U;
    }

    return true;
}

static bool oled_send_vision_text(const char * name)
{
    char safe_name[VISION_TEXT_BUFFER_SIZE];
    int written;
    uint32_t medicine_len;
    size_t index = 0U;

    if (NULL == name)
    {
        return false;
    }

    while ((name[index] != '\0') && (index < (sizeof(safe_name) - 1U)))
    {
        char ch = name[index];
        if ((ch == '"') || (ch == '\r') || (ch == '\n'))
        {
            ch = ' ';
        }

        safe_name[index] = ch;
        index++;
    }
    safe_name[index] = '\0';

    written = snprintf((char *) g_medicine_oled_tx_buf,
                       TX_BUF_SIZE - 3U,
                       "t10.txt=\"%s\"",
                       safe_name);
    if ((written < 0) || ((uint32_t) written >= (TX_BUF_SIZE - 3U)))
    {
        return false;
    }

    medicine_len = (uint32_t) written;
    g_medicine_oled_tx_buf[medicine_len++] = 0xFF;
    g_medicine_oled_tx_buf[medicine_len++] = 0xFF;
    g_medicine_oled_tx_buf[medicine_len++] = 0xFF;

    if (FSP_SUCCESS != OLED_TryWrite(g_medicine_oled_tx_buf,
                                    medicine_len))
    {
        return false;
    }
    return true;
}

static void vision_result_process(bool allow_speech_start)
{
    static char last_displayed[VISION_TEXT_BUFFER_SIZE] = "";
    static char last_seen[VISION_TEXT_BUFFER_SIZE] = "";
    static char display_pending[VISION_TEXT_BUFFER_SIZE] = "";
    static char candidate[VISION_TEXT_BUFFER_SIZE] = "";
    static char announced[VISION_TEXT_BUFFER_SIZE] = "";
    static char speech_pending[VISION_TEXT_BUFFER_SIZE] = "";
    static char speech_in_flight[VISION_TEXT_BUFFER_SIZE] = "";
    static uint8_t candidate_count = 0U;
    static bool speech_retry_wait = false;
    static uint32_t speech_retry_start_ms = 0U;
    static bool speech_mode_active = false;
    static bool process_active = false;
    char vision_text[VISION_TEXT_BUFFER_SIZE];
    char log_buf[224];
    uint16_t vision_length = 0U;
    uint16_t index;
    bool have_line = false;
    bool overflowed = false;
    bool detection_mode;
    audio_voice_speech_result_t speech_result;
    FSP_CRITICAL_SECTION_DEFINE;

    /*
     * This function also runs from the cooperative W800 wait hook. Prevent a
     * nested invocation when a medicine TTS request is itself waiting on the
     * W800. The cooperative form may update local state/display, but it never
     * starts another network transaction.
     */
    if (process_active)
    {
        return;
    }
    process_active = true;

    detection_mode = (1U == Flag);
    if (detection_mode != speech_mode_active)
    {
        candidate[0] = '\0';
        candidate_count = 0U;
        speech_pending[0] = '\0';
        speech_retry_wait = false;
        if ((!detection_mode) && audio_voice_text_speech_active())
        {
            audio_voice_cancel_text_speech();
        }
        speech_mode_active = detection_mode;
    }

    speech_result = audio_voice_take_speech_result();
    if (AUDIO_VOICE_SPEECH_RESULT_SUCCESS == speech_result)
    {
        if ('\0' != speech_in_flight[0])
        {
            (void) snprintf(announced, sizeof(announced), "%s", speech_in_flight);
            (void) snprintf(log_buf,
                            sizeof(log_buf),
                            "\r\n[VISION] speech complete: %s\r\n> ",
                            speech_in_flight);
            deepseek_bridge_pc_write(log_buf);
        }
        speech_in_flight[0] = '\0';
        speech_retry_wait = false;
        if (0 == strcmp(speech_pending, announced))
        {
            speech_pending[0] = '\0';
        }
    }
    else if (AUDIO_VOICE_SPEECH_RESULT_FAILED == speech_result)
    {
        if (detection_mode &&
            ('\0' != speech_in_flight[0]) &&
            ('\0' == speech_pending[0]) &&
            (0 != strcmp(speech_in_flight, announced)))
        {
            (void) snprintf(speech_pending, sizeof(speech_pending), "%s", speech_in_flight);
        }
        speech_in_flight[0] = '\0';
        if (detection_mode)
        {
            speech_retry_start_ms = system_tick_now();
            speech_retry_wait = true;
            deepseek_bridge_pc_write("\r\n[VISION] medicine speech failed; retry allowed after 5 s.\r\n> ");
        }
        else
        {
            speech_retry_wait = false;
        }
    }

    FSP_CRITICAL_SECTION_ENTER;
    if (g_vision_line_overflow)
    {
        g_vision_line_overflow = false;
        overflowed = true;
    }
    if (g_vision_line_count > 0U)
    {
        uint8_t head = g_vision_line_head;
        vision_length = g_vision_line_lengths[head];
        if (vision_length < VISION_TEXT_BUFFER_SIZE)
        {
            for (index = 0U; index < vision_length; index++)
            {
                vision_text[index] = (char) g_vision_line_queue[head][index];
            }
            have_line = true;
        }
        g_vision_line_head = (uint8_t) ((head + 1U) % VISION_LINE_QUEUE_DEPTH);
        g_vision_line_count--;
    }
    FSP_CRITICAL_SECTION_EXIT;

    if (overflowed)
    {
        candidate[0] = '\0';
        candidate_count = 0U;
        deepseek_bridge_pc_write("\r\n[VISION] receive queue overflow; confirmation reset.\r\n> ");
    }

    if (have_line)
    {
        vision_text[vision_length] = '\0';
        if ((0U == vision_length) || (!vision_utf8_is_valid(vision_text)))
        {
            candidate[0] = '\0';
            candidate_count = 0U;
            have_line = false;
        }
    }

    if (have_line)
    {
        if ((0 != strcmp(vision_text, last_displayed)) &&
            (0 != strcmp(vision_text, display_pending)))
        {
            (void) snprintf(display_pending, sizeof(display_pending), "%s", vision_text);
        }

        if (0 != strcmp(vision_text, last_seen))
        {
            (void) snprintf(last_seen, sizeof(last_seen), "%s", vision_text);
            (void) snprintf(log_buf,
                            sizeof(log_buf),
                            "\r\n[VISION] name=%s\r\n> ",
                            vision_text);
            deepseek_bridge_pc_write(log_buf);
        }

        if (!detection_mode)
        {
            candidate[0] = '\0';
            candidate_count = 0U;
        }
        else if (0 == strcmp(vision_text, VISION_UNRECOGNIZED_TEXT))
        {
            candidate[0] = '\0';
            candidate_count = 0U;
        }
        else
        {
            if (0 == strcmp(candidate, vision_text))
            {
                if (candidate_count < 2U)
                {
                    candidate_count++;
                }
            }
            else
            {
                (void) snprintf(candidate, sizeof(candidate), "%s", vision_text);
                candidate_count = 1U;
            }

            if (candidate_count >= 2U)
            {
                if (0 == strcmp(candidate, announced))
                {
                    if (0 != strcmp(speech_in_flight, candidate))
                    {
                        speech_pending[0] = '\0';
                    }
                }
                else if (0 != strcmp(candidate, speech_in_flight))
                {
                    (void) snprintf(speech_pending, sizeof(speech_pending), "%s", candidate);
                    speech_retry_wait = false;
                }
            }
        }
    }

    if (('\0' != display_pending[0]) &&
        (!OLED_IsTxBusy()) &&
        (!OLED_IsInventoryTxProtected()) &&
        oled_send_vision_text(display_pending))
    {
        (void) snprintf(last_displayed, sizeof(last_displayed), "%s", display_pending);
        display_pending[0] = '\0';
    }

    if (speech_retry_wait &&
        system_tick_elapsed(speech_retry_start_ms, VISION_SPEECH_RETRY_MS))
    {
        speech_retry_wait = false;
    }

    if (('\0' != speech_pending[0]) &&
        ('\0' == speech_in_flight[0]) &&
        detection_mode &&
        allow_speech_start &&
        (!speech_retry_wait) &&
        (!ota_update_active()) &&
        (!audio_voice_text_speech_active()))
    {
        fsp_err_t speech_err = audio_voice_speak_text(speech_pending);
        if (FSP_SUCCESS == speech_err)
        {
            (void) snprintf(speech_in_flight,
                            sizeof(speech_in_flight),
                            "%s",
                            speech_pending);
            speech_pending[0] = '\0';
        }
        else if (FSP_ERR_IN_USE != speech_err)
        {
            speech_retry_start_ms = system_tick_now();
            speech_retry_wait = true;
        }
    }

    process_active = false;
}

static void health_assistant_pending_process(void)
{
    static char symptom[HEALTH_ASSISTANT_SYMPTOM_MAX];
    static char diagnosis[HEALTH_ASSISTANT_DIAGNOSIS_MAX];
    static char medicine[HEALTH_ASSISTANT_NAME_MAX];
    char log_buf[192];
    uint16_t symptom_length = 0U;
    uint8_t pending_index;
    uint8_t symptom_code;
    const char * legacy_symptom;
    health_result_t result;

    if (g_health_request_busy)
    {
        return;
    }

    if (g_health_text_pending)
    {
        g_health_request_busy = true;
        pending_index = g_health_frame_pending_index;
        symptom_length = g_health_frame_pending_length;
        g_health_text_pending = false;

        if (symptom_length >= HEALTH_ASSISTANT_SYMPTOM_MAX)
        {
            symptom_length = HEALTH_ASSISTANT_SYMPTOM_MAX - 1U;
        }

        for (uint16_t i = 0U; i < symptom_length; i++)
        {
            symptom[i] = (char) g_health_frame_buffers[pending_index][i];
        }
        symptom[symptom_length] = '\0';
        symptom_length = (uint16_t) health_normalize_symptom(symptom, symptom_length);
    }
    else if (g_health_assistant_pending)
    {
        g_health_request_busy = true;
        symptom_code = g_health_assistant_symptom_code;
        g_health_assistant_pending = false;
        legacy_symptom = health_assistant_symptom_from_code(symptom_code);
        if (NULL != legacy_symptom)
        {
            (void) snprintf(symptom, sizeof(symptom), "%s", legacy_symptom);
            symptom_length = (uint16_t) strlen(symptom);
        }
    }
    else
    {
        return;
    }

    if (0U == symptom_length)
    {
        g_health_request_busy = false;
        deepseek_bridge_pc_write("\r\n[HA] empty symptom frame ignored\r\n> ");
        return;
    }

    (void) snprintf(log_buf, sizeof(log_buf), "\r\n[HA] symptom=%s\r\n", symptom);
    deepseek_bridge_pc_write(log_buf);

    result = health_assistant_assess_by_text(symptom,
                                             diagnosis,
                                             sizeof(diagnosis),
                                             medicine,
                                             sizeof(medicine));
    if (HEALTH_RESULT_OK == result)
    {
        OLED_SetHealthResult(diagnosis, medicine);
        deepseek_bridge_pc_write("[HA] diagnosis=");
        deepseek_bridge_pc_write(diagnosis);
        deepseek_bridge_pc_write(" medicine=");
        deepseek_bridge_pc_write(medicine);
        deepseek_bridge_pc_write("\r\n> ");
        g_health_request_busy = false;
        return;
    }

    if (HEALTH_RESULT_BUSY == result)
    {
        OLED_SetHealthResult("网络忙", "暂无用药建议");
    }
    else if (HEALTH_RESULT_INVALID_ARGUMENT == result)
    {
        OLED_SetHealthResult("症状无效", "请重新选择");
    }
    else
    {
        OLED_SetHealthResult("诊断失败", "暂无用药建议");
    }

    (void) snprintf(log_buf,
                    sizeof(log_buf),
                    "[HA] failed=%s\r\n> ",
                    health_result_text(result));
    deepseek_bridge_pc_write(log_buf);
    g_health_request_busy = false;
}

static void inventory_page_request_clear(void)
{
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    g_inventory_page_request = INVENTORY_PAGE_REQUEST_NONE;
    FSP_CRITICAL_SECTION_EXIT;
}

static inventory_page_request_t inventory_page_request_take(void)
{
    inventory_page_request_t request;
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    request = g_inventory_page_request;
    g_inventory_page_request = INVENTORY_PAGE_REQUEST_NONE;
    FSP_CRITICAL_SECTION_EXIT;

    return request;
}

static void inventory_pagination_reset(void)
{
    g_inventory_record_count = 0U;
    g_inventory_current_page = 0U;
    g_inventory_total_pages = 0U;
    g_inventory_cache_valid = false;
    g_inventory_page_text[0] = '\0';
    inventory_page_request_clear();
}

static uint8_t inventory_record_count(const char * text)
{
    uint8_t count = 0U;
    const char * cursor;

    if ((NULL == text) || ('\0' == text[0]))
    {
        return 0U;
    }

    count = 1U;
    cursor = text;
    while ((count < MEDICINE_MAX_RECORDS) && (NULL != (cursor = strstr(cursor, "\r\n"))))
    {
        count++;
        cursor += 2;
    }

    return count;
}

static bool inventory_build_page(uint8_t page_index)
{
    const char * record_start = g_inventory_text;
    size_t used = 0U;
    uint8_t first_record;
    uint8_t record_index;
    uint8_t records_written = 0U;

    if ((!g_inventory_cache_valid) || (page_index >= g_inventory_total_pages))
    {
        return false;
    }

    first_record = (uint8_t) (page_index * INVENTORY_RECORDS_PER_PAGE);
    for (record_index = 0U; record_index < first_record; record_index++)
    {
        record_start = strstr(record_start, "\r\n");
        if (NULL == record_start)
        {
            return false;
        }
        record_start += 2;
    }

    while ((records_written < INVENTORY_RECORDS_PER_PAGE) &&
           ((uint8_t) (first_record + records_written) < g_inventory_record_count))
    {
        const char * record_end = strstr(record_start, "\r\n");
        size_t record_length = (NULL == record_end) ? strlen(record_start) :
                                                     (size_t) (record_end - record_start);
        size_t separator_length = (0U == records_written) ? 0U : 2U;

        if ((used + separator_length + record_length) > INVENTORY_PAGE_TEXT_MAX)
        {
            g_inventory_page_text[0] = '\0';
            return false;
        }

        if (separator_length > 0U)
        {
            g_inventory_page_text[used++] = '\r';
            g_inventory_page_text[used++] = '\n';
        }

        memcpy(&g_inventory_page_text[used], record_start, record_length);
        used += record_length;
        records_written++;

        if (NULL == record_end)
        {
            break;
        }
        record_start = record_end + 2;
    }

    g_inventory_page_text[used] = '\0';
    return (records_written > 0U);
}

static void inventory_page_process(void)
{
    inventory_page_request_t request = inventory_page_request_take();
    uint8_t target_page;

    if ((INVENTORY_PAGE_REQUEST_NONE == request) ||
        (!g_inventory_cache_valid) ||
        g_inventory_list_busy ||
        g_inventory_list_pending)
    {
        return;
    }

    target_page = g_inventory_current_page;
    if (INVENTORY_PAGE_REQUEST_PREVIOUS == request)
    {
        if (0U == target_page)
        {
            return;
        }
        target_page--;
    }
    else
    {
        if ((uint8_t) (target_page + 1U) >= g_inventory_total_pages)
        {
            return;
        }
        target_page++;
    }

    if (inventory_build_page(target_page))
    {
        g_inventory_current_page = target_page;
        OLED_SetInventoryText(g_inventory_page_text);
    }
}

static void inventory_list_process(void)
{
    char log_buf[128];
    const char * display_text;
    medicine_result_t result;

    if (g_inventory_list_busy || (!g_inventory_list_pending))
    {
        return;
    }

    g_inventory_list_pending = false;
    g_inventory_list_busy = true;
    result = medicine_inventory_list(g_inventory_text, sizeof(g_inventory_text));
    inventory_pagination_reset();

    if (MEDICINE_RESULT_OK == result)
    {
        if ('\0' == g_inventory_text[0])
        {
            display_text = "暂无库存";
        }
        else
        {
            g_inventory_record_count = inventory_record_count(g_inventory_text);
            g_inventory_total_pages = (uint8_t) ((g_inventory_record_count +
                                                  INVENTORY_RECORDS_PER_PAGE - 1U) /
                                                 INVENTORY_RECORDS_PER_PAGE);
            g_inventory_cache_valid = (g_inventory_record_count > 0U);
            if (g_inventory_cache_valid && inventory_build_page(0U))
            {
                g_inventory_current_page = 0U;
                display_text = g_inventory_page_text;
            }
            else
            {
                inventory_pagination_reset();
                display_text = "库存信息过长";
            }
        }
    }
    else if ((MEDICINE_RESULT_BUSY == result) ||
             (MEDICINE_RESULT_NETWORK_ERROR == result))
    {
        display_text = "网络忙";
    }
    else if (MEDICINE_RESULT_BUFFER_TOO_SMALL == result)
    {
        display_text = "库存信息过长";
    }
    else
    {
        display_text = "库存读取失败";
    }

    OLED_SetInventoryText(display_text);
    (void) snprintf(log_buf,
                    sizeof(log_buf),
                    "\r\n[MED] inventory list: %s\r\n> ",
                    medicine_result_text(result));
    deepseek_bridge_pc_write(log_buf);
    g_inventory_list_busy = false;
}


void tjc_uart_callback(uart_callback_args_t * p_args)  //串口屏回调
{
    if (NULL == p_args)
    {
        return;
    }

    if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        OLED_NotifyTxComplete();
        return;
    }

    if (p_args->event == UART_EVENT_RX_CHAR)
        {
            uint8_t data = (uint8_t)p_args->data;
            if ((2U == Flag) && health_frame_receive_byte(data))
            {
                return;
            }

            /*
             * C1 starts and C2 stops recording only outside a valid
             * FA FA ... FB FB health frame and only in Flag=2.  The ISR merely
             * queues the ordered command; audio work stays in the main loop.
             */
            if (0xC1U == data)
            {
                if (2U == Flag)
                {
                    audio_voice_submit_start_command();
                }
                return;
            }
            if (0xC2U == data)
            {
                if (2U == Flag)
                {
                    audio_voice_submit_stop_command();
                }
                return;
            }

            if (data == 0xBB)
            {
                g_inventory_list_pending = true;
            }
            else if (data == 0xBC)
            {
                g_inventory_page_request = INVENTORY_PAGE_REQUEST_PREVIOUS;
            }
            else if (data == 0xBD)
            {
                g_inventory_page_request = INVENTORY_PAGE_REQUEST_NEXT;
            }
            else if (data == 0x01)
            {
                set_temp++;
            }
            else if (data == 0x02)
            {
               // if (set_temp > 0)
                    set_temp--;
            }
            else if (data == 0x03)
            {
                cooling_interlock_submit_code(data);
            }
            else if (data == 0x09)
            {
                cooling_interlock_submit_code(data);
            }
            else if (data == 0x05)
            {
                door_state = true;
            }

            else if (data == 0x06)
            {
                Flag = 1;
            }
            else if (data == 0x07)
            {
                Flag = 2;
                health_frame_reset();
            }
            else if (data == 0x08)
            {
                Flag = 0;
                health_frame_reset();
            }
            else if(data == 0x0A)
            {
                cooling_interlock_submit_code(data);
            }
        }
}


void uart_callback(uart_callback_args_t * p_args)
{
    deepseek_bridge_uart7_callback(p_args);
}


void w800_uart_callback(uart_callback_args_t * p_args)
{
    deepseek_bridge_w800_callback(p_args);
}

void uart5_callback(uart_callback_args_t * p_args)
{
    if ((NULL != p_args) && (UART_EVENT_RX_CHAR == p_args->event))
    {
        uint8_t data = (uint8_t) p_args->data;

        if ('\n' == data)
        {
            if ((!g_vision_rx_discard) &&
                (g_vision_rx_length > 0U))
            {
                if (g_vision_line_count < VISION_LINE_QUEUE_DEPTH)
                {
                    uint8_t tail = g_vision_line_tail;
                    uint16_t index;

                    for (index = 0U; index < g_vision_rx_length; index++)
                    {
                        g_vision_line_queue[tail][index] = g_vision_rx_text[index];
                    }
                    g_vision_line_queue[tail][g_vision_rx_length] = '\0';
                    g_vision_line_lengths[tail] = g_vision_rx_length;
                    g_vision_line_tail = (uint8_t) ((tail + 1U) % VISION_LINE_QUEUE_DEPTH);
                    g_vision_line_count++;
                }
                else
                {
                    g_vision_line_head = 0U;
                    g_vision_line_tail = 0U;
                    g_vision_line_count = 0U;
                    g_vision_line_overflow = true;
                }
            }

            g_vision_rx_length = 0U;
            g_vision_rx_discard = false;
            return;
        }

        if ('\r' == data)
        {
            return;
        }

        if (g_vision_rx_discard)
        {
            return;
        }

        if ((data < 0x20U) || (0x7FU == data))
        {
            g_vision_rx_length = 0U;
            g_vision_rx_discard = true;
            return;
        }

        if (g_vision_rx_length < (VISION_TEXT_BUFFER_SIZE - 1U))
        {
            g_vision_rx_text[g_vision_rx_length++] = data;
        }
        else
        {
            g_vision_rx_length = 0U;
            g_vision_rx_discard = true;
        }
    }
}

//药物识别与健康助手模块
//0表示未切换到识别模式或健康助手模式   1表示切换到识别模式   2表示切换到健康助手模式//记得在回主界面时加上标志位清零
void Voice_uart_callback(uart_callback_args_t * p_args)
{
    if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        return;
    }

    if (p_args->event == UART_EVENT_RX_CHAR)
    {
        uint8_t data = (uint8_t)p_args->data;

        /* 镜像收到的原始字节到 UART7 */
        switch(Flag)
        {
            case 0:

                break;
            case 1://药物识别模式
                /* Legacy 0x0F -> Medical[0] picture-ID reply is disabled. */
                break;
            case 2://健康助手模式
                if (NULL != health_assistant_symptom_from_code(data))
                {
                    g_health_assistant_symptom_code = data;
                    g_health_assistant_pending = true;
                }
                break;
            default:
                break;
        }
    }
}


/*
 * Local real-time work that is safe to run from the main loop and from a
 * cooperative W800 wait. It never starts a network transaction. Actuator
 * registers are rewritten only when needed, and the temperature PID has an
 * explicit 100 ms sample period instead of depending on main-loop speed.
 */
static void local_realtime_process(void)
{
    uint8_t colder_duty = 0U;
    uint8_t box_fan_duty = 0U;
    uint8_t water_output = 0U;
    float latest_temperature;
    float raw_temperature;

    if (g_local_runtime_active)
    {
        return;
    }
    g_local_runtime_active = true;

    /* Flag is written by the HMI UART interrupt.  Apply the mode change here
     * so SSIE is never opened or stopped from interrupt context.  The same
     * cooperative function also runs during W800 waits, keeping the mode
     * transition responsive while networking is busy. */
    {
        uint8_t mode = Flag;
        if (mode != g_audio_mode_applied)
        {
            /* Free SSIE before another mode (especially health mode) starts. */
            if ((1U == g_audio_mode_applied) && (1U != mode))
            {
                audio_voice_cancel_text_speech();
            }
            if (FSP_SUCCESS == audio_voice_set_health_mode(2U == mode))
            {
                g_audio_mode_applied = mode;
            }
        }
    }

    if (door_state)
    {
        door_unlock_request();
        door_state = false;
    }

    lm35_process();
    if (FSP_SUCCESS == lm35_read_temperature(&latest_temperature))
    {
        temperature = latest_temperature;

        if ((FSP_SUCCESS == lm35_read_raw_temperature(&raw_temperature)) &&
            ((!g_temperature_diagnostic_has_run) ||
             system_tick_elapsed(g_temperature_diagnostic_last_ms,
                                 TEMPERATURE_DIAGNOSTIC_PERIOD_MS)))
        {
            char temperature_log[96];
            int raw_x10 = (raw_temperature >= 0.0f) ?
                          (int) ((raw_temperature * 10.0f) + 0.5f) :
                          (int) ((raw_temperature * 10.0f) - 0.5f);
            int calibrated_x10 = (latest_temperature >= 0.0f) ?
                                 (int) ((latest_temperature * 10.0f) + 0.5f) :
                                 (int) ((latest_temperature * 10.0f) - 0.5f);
            int raw_magnitude = (raw_x10 < 0) ? -raw_x10 : raw_x10;
            int calibrated_magnitude = (calibrated_x10 < 0) ?
                                       -calibrated_x10 : calibrated_x10;

            (void) snprintf(temperature_log,
                            sizeof(temperature_log),
                            "[TEMP][%s] raw=%s%d.%dC calibrated=%s%d.%dC\r\n",
                            lm35_sensor_name(),
                            (raw_x10 < 0) ? "-" : "",
                            raw_magnitude / 10,
                            raw_magnitude % 10,
                            (calibrated_x10 < 0) ? "-" : "",
                            calibrated_magnitude / 10,
                            calibrated_magnitude % 10);
            deepseek_bridge_pc_write(temperature_log);
            g_temperature_diagnostic_last_ms = system_tick_now();
            g_temperature_diagnostic_has_run = true;
        }
    }
    ina226_power_process();

    /* All local and remote actuator requests converge here.  The interlock
     * runs before any PWM register is changed. */
    cooling_interlock_process();

    /* Keep the HMI pictures synchronized with the effective state selected by
     * the interlock as well as with direct HMI/server commands.  This is a
     * queued non-blocking update, so an automatic preheat cannot miss p4/p1
     * merely because the OLED UART was busy at the transition instant. */
    if (g_local_oled_ready)
    {
        OLED_SetFanWaterState(fan_state, (1U == Water_cool_State));
    }

    if (1U == fan_state)
    {
        box_fan_duty = 60U;
    }
    else if (2U == fan_state)
    {
        box_fan_duty = 100U;
    }
    if (box_fan_duty != g_local_last_box_fan_duty)
    {
        black_fan_set_speed(box_fan_duty);
        g_local_last_box_fan_duty = box_fan_duty;
    }

    if (1U == Water_cool_State)
    {
        water_output = 100U;
    }
    if (water_output != g_local_last_water_output)
    {
        if (FSP_SUCCESS == water_cooling_set_enabled(0U != water_output))
        {
            g_local_last_water_output = water_output;
        }
    }

    /* P602 is the sole cooling prerequisite.  Start the five-second window
     * only after its GPIO output has actually been applied. */
    cooling_interlock_water_output_applied(0U != g_local_last_water_output);

    uint32_t now = system_tick_now();
    if ((!g_local_control_has_run) ||
        system_tick_elapsed(g_local_control_last_ms, LOCAL_CONTROL_PERIOD_MS))
    {
        if (cool_state &&
            (temperature < 60.0f) &&
            ((float) set_temp <= temperature))
        {
            float output;

            PID_Calc(&Colder,
                     (float) set_temp * 50.0f,
                     temperature * 50.0f);
            output = Colder.output;
            if (output < 0.0f)
            {
                output = -output;
            }
            if (output > 100.0f)
            {
                output = 100.0f;
            }
            if (output >= 0.0f)
            {
                colder_duty = (uint8_t) output;
            }
        }
        else
        {
            /* Never retain a previous non-zero PWM value after the demand or
             * safety condition disappears. */
            colder_duty = 0U;
        }

        if (colder_duty != g_local_last_colder_duty)
        {
            Colder_DutySet(colder_duty);
            g_local_last_colder_duty = colder_duty;
        }
        g_local_control_last_ms = now;
        g_local_control_has_run = true;
    }

    fan_tach_process();
    if (g_local_oled_ready)
    {
        OLED_SetFanRpm(fan_tach_get_rpm(1U), fan_tach_get_rpm(2U));
        OLED_Start();
    }
    vision_result_process(false);

    g_local_runtime_active = false;
}

void hal_entry(void)
{
    bool oled_uart_ready;
    bool pc_uart_ready;
    bool vision_uart_ready;
    bool w800_uart_ready;
    fsp_err_t fan_tach_err;

    /* The IWDT is auto-started by OFS0, so begin feeding it immediately. */
    ota_update_watchdog_refresh();

    /* 打开 UART2 */
    pc_uart_ready = (FSP_SUCCESS == R_SCI_UART_Open(&g_uart7_ctrl, &g_uart7_cfg));
    oled_uart_ready = (FSP_SUCCESS == R_SCI_UART_Open(&oled_control_ctrl, &oled_control_cfg));
    vision_uart_ready = (FSP_SUCCESS == R_SCI_UART_Open(&g_uart5_ctrl, &g_uart5_cfg));
    w800_uart_ready = (FSP_SUCCESS == R_SCI_UART_Open(&g_uart6_ctrl, &g_uart6_cfg));
    deepseek_bridge_set_uart_availability(pc_uart_ready, w800_uart_ready);
    g_local_oled_ready = oled_uart_ready;
    (void) system_tick_init();

    if (pc_uart_ready)
    {
        if (!oled_uart_ready)
        {
            deepseek_bridge_pc_write("[BOOT] OLED UART open failed; display updates disabled.\r\n");
        }
        if (!vision_uart_ready)
        {
            deepseek_bridge_pc_write("[BOOT] vision UART open failed; recognition input disabled.\r\n");
        }
        if (!w800_uart_ready)
        {
            deepseek_bridge_pc_write("[BOOT] W800 UART open failed; network functions disabled.\r\n");
        }
    }

    ina226_power_init();

    lm35_init();
    fan_init();
    Colder_Init();
    Door_Init();
    PID_Init(&Colder, Colder_P, Colder_I, Colder_D, 25, 100);
    (void) cooling_interlock_init();
    fan_tach_err = fan_tach_init();
    (void) ota_update_init();
    deepseek_bridge_init();
    if ((FSP_SUCCESS != fan_tach_err) && pc_uart_ready)
    {
        deepseek_bridge_pc_write(
            "[FAN] FG counter configuration missing/invalid; n91/n92 will remain 0.\r\n");
    }
    (void) access_control_init();
    (void) audio_voice_init();
    Counter_Timer_Init();//定时器延时初始化
    deepseek_bridge_set_cooperative_hook(local_realtime_process);


    while (1)//循环中部分尽量避免阻塞逻辑
    {
        ota_update_watchdog_refresh();
        ota_update_process();
        access_control_process();
        door_process();
        audio_voice_process();
        local_realtime_process();
        vision_result_process(true);
        inventory_list_process();
        inventory_page_process();
        health_assistant_pending_process();
        /* Periodic W800 status traffic is deliberately lowest priority. */
        deepseek_bridge_process();

    }



#if BSP_TZ_SECURE_BUILD
    /* Enter non-secure code */
    R_BSP_NonSecureEnter();
#endif
}

/*******************************************************************************************************************//**
 * This function is called at various points during the startup process.  This implementation uses the event that is
 * called right before main() to set up the pins.
 *
 * @param[in]  event    Where at in the start up process the code is currently at
 **********************************************************************************************************************/
void R_BSP_WarmStart(bsp_warm_start_event_t event)
{
    if (BSP_WARM_START_RESET == event)
    {
#if BSP_FEATURE_FLASH_LP_VERSION != 0

        /* Enable reading from data flash. */
        R_FACI_LP->DFLCTL = 1U;

        /* Would normally have to wait tDSTOP(6us) for data flash recovery. Placing the enable here, before clock and
         * C runtime initialization, should negate the need for a delay since the initialization will typically take more than 6us. */
#endif
    }

    if (BSP_WARM_START_POST_C == event)
    {
        /* C runtime environment and system clocks are setup. */

        /* Configure pins. */
        R_IOPORT_Open (&g_ioport_ctrl, &IOPORT_CFG_NAME);
    }
}

#if BSP_TZ_SECURE_BUILD

FSP_CPP_HEADER
BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable ();

/* Trustzone Secure Projects require at least one nonsecure callable function in order to build (Remove this if it is not required to build). */
BSP_CMSE_NONSECURE_ENTRY void template_nonsecure_callable ()
{

}
FSP_CPP_FOOTER

#endif
