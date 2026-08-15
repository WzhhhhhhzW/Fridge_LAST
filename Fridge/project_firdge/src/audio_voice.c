#include "audio_voice.h"
#include "audio_voice_config.h"
#include "deepseek_bridge.h"
#include "oled.h"
#include "system_tick.h"
#include "wake_ack_pcm.h"
#include "wake_word.h"

#include <stdio.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE                 16000U
#define AUDIO_SAMPLES_PER_FRAME           1600U
#define AUDIO_CHANNELS                    2U
#define AUDIO_RAW_WORDS_PER_FRAME         (AUDIO_SAMPLES_PER_FRAME * AUDIO_CHANNELS)
#define AUDIO_RAW_BYTES_PER_FRAME         (AUDIO_RAW_WORDS_PER_FRAME * sizeof(int32_t))
#define AUDIO_PCM_BYTES_PER_FRAME         (AUDIO_SAMPLES_PER_FRAME * sizeof(int16_t))
#define AUDIO_CAPTURE_BUFFER_COUNT        10U
#define AUDIO_PLAY_QUEUE_COUNT            12U
#define AUDIO_TX_BUFFER_COUNT             2U
#define AUDIO_PREBUFFER_FRAMES            3U
#define AUDIO_BUTTON_DEBOUNCE_MS          30U
#define AUDIO_SPEECH_CONFIRM_FRAMES       2U
#define AUDIO_MAX_RECORD_FRAMES           300U
#define AUDIO_NETWORK_TIMEOUT_MS          10000U
#define AUDIO_NETWORK_POLL_MS             80U
#define AUDIO_SERVER_IDLE_TIMEOUT_MS      120000U
#define AUDIO_PROTOCOL_LINE_SIZE          192U
#define AUDIO_NETWORK_RX_SIZE             4096U
#define AUDIO_SESSION_ID_SIZE             24U
#define AUDIO_SAY_TEXT_MAX_BYTES          160U
#define AUDIO_HEALTH_DIAGNOSIS_MAX_BYTES  96U
#define AUDIO_HEALTH_MEDICINE_MAX_BYTES   160U
#define AUDIO_VAD_MIN_RMS                 150U
#define AUDIO_VAD_MAX_RMS                 2500U
#define AUDIO_INPUT_GAIN                  2
#define AUDIO_TEST_TONE_FRAMES            10U
#define AUDIO_NORMAL_MCLK_PERIOD_COUNTS   98U
#define AUDIO_8K_MCLK_PERIOD_COUNTS       195U
#define AUDIO_AMP_CLOCK_SETTLE_MS          5U
#define AUDIO_KWS_FIFO_SAMPLES             (WAKE_WORD_SLICE_SAMPLES + AUDIO_SAMPLES_PER_FRAME)
#define AUDIO_KWS_LOG_INTERVAL_MS          5000U
#define AUDIO_KWS_COOLDOWN_MS              1000U
#define AUDIO_DIALOGUE_ACK_DELAY_MS         500U
#define AUDIO_RECORD_COMMAND_QUEUE_DEPTH       4U
#define AUDIO_LOCAL_ACK_GAIN_NUMERATOR     5
#define AUDIO_LOCAL_ACK_GAIN_DENOMINATOR   2

typedef enum e_audio_voice_state
{
    AUDIO_VOICE_STATE_DISABLED = 0,
    AUDIO_VOICE_STATE_IDLE,
    AUDIO_VOICE_STATE_KWS_LISTEN,
    AUDIO_VOICE_STATE_KWS_STOPPING,
    AUDIO_VOICE_STATE_KWS_COOLDOWN,
    AUDIO_VOICE_STATE_ACK_WAIT,
    AUDIO_VOICE_STATE_STARTING,
    AUDIO_VOICE_STATE_RECORDING,
    AUDIO_VOICE_STATE_FINISHING,
    AUDIO_VOICE_STATE_WAIT_RESULT,
    AUDIO_VOICE_STATE_PLAYBACK_STARTING,
    AUDIO_VOICE_STATE_PLAYBACK,
    AUDIO_VOICE_STATE_RECOVERING,
} audio_voice_state_t;

typedef enum e_audio_kws_pending_action
{
    AUDIO_KWS_PENDING_NONE = 0,
    AUDIO_KWS_PENDING_DIALOGUE,
    AUDIO_KWS_PENDING_ACK,
    AUDIO_KWS_PENDING_TEXT_SPEECH,
    AUDIO_KWS_PENDING_DISABLE,
} audio_kws_pending_action_t;

typedef enum e_audio_record_command
{
    AUDIO_RECORD_COMMAND_NONE = 0,
    AUDIO_RECORD_COMMAND_START,
    AUDIO_RECORD_COMMAND_STOP,
} audio_record_command_t;

/*
 * g_audio_mclk is deliberately weak: the source remains linkable before the
 * user generates the requested GPT2 instance.  audio_voice_init() refuses to
 * enable audio until the generated instance and all SSIE settings are valid.
 */
extern const timer_instance_t g_audio_mclk __attribute__((weak));

static audio_voice_state_t g_audio_state = AUDIO_VOICE_STATE_DISABLED;
static bool g_audio_initialized = false;
static bool g_ssie_open = false;
static volatile bool g_ssie_idle = true;
static volatile bool g_capture_running = false;
static volatile bool g_capture_stop_requested = false;
static volatile bool g_capture_overflow = false;
static volatile uint8_t g_capture_current = 0U;
static uint8_t g_capture_consumer = 0U;
static volatile bool g_capture_ready[AUDIO_CAPTURE_BUFFER_COUNT];
static int32_t g_capture_raw[AUDIO_CAPTURE_BUFFER_COUNT][AUDIO_RAW_WORDS_PER_FRAME]
    __attribute__((aligned(4)));
static int16_t g_capture_pcm[AUDIO_SAMPLES_PER_FRAME] __attribute__((aligned(4)));

static int16_t g_play_queue[AUDIO_PLAY_QUEUE_COUNT][AUDIO_SAMPLES_PER_FRAME]
    __attribute__((aligned(4)));
static uint8_t g_pcm_receive_frame[AUDIO_PCM_BYTES_PER_FRAME] __attribute__((aligned(4)));
static uint16_t g_play_queue_samples[AUDIO_PLAY_QUEUE_COUNT];
static volatile uint8_t g_play_queue_head = 0U;
static volatile uint8_t g_play_queue_tail = 0U;
static volatile uint8_t g_play_queue_count = 0U;

static int32_t g_tx_raw[AUDIO_TX_BUFFER_COUNT][AUDIO_RAW_WORDS_PER_FRAME]
    __attribute__((aligned(4)));
static const int32_t g_tx_silence[AUDIO_RAW_WORDS_PER_FRAME]
    __attribute__((aligned(4))) = {0};
static volatile bool g_tx_ready[AUDIO_TX_BUFFER_COUNT];
static volatile int8_t g_tx_current = -1;
static volatile bool g_playback_running = false;
static volatile bool g_playback_stop_requested = false;
static volatile bool g_suffix_silence_inflight = false;
static volatile uint32_t g_playback_underruns = 0U;
static uint32_t g_playback_clock_ready_ms = 0U;
static const char * g_playback_start_message = NULL;
static bool g_internal_8k_test_active = false;
static bool g_internal_32bit_test_active = false;
static bool g_internal_left_justified_test_active = false;
static bool g_tx_pcm_width_32 = false;
static i2s_cfg_t g_i2s0_diag_32_cfg;
static bool g_kws_model_ready = false;
static bool g_kws_requested = true;
static bool g_kws_health_mode = false;
static bool g_kws_armed = false;
static bool g_kws_enabled = false;
static bool g_local_ack_playback = false;
static bool g_ack_starts_dialogue = false;
static char g_text_speech_text[AUDIO_SAY_TEXT_MAX_BYTES + 1U];
static bool g_text_speech_active = false;
static audio_voice_speech_result_t g_text_speech_result = AUDIO_VOICE_SPEECH_RESULT_NONE;
static audio_kws_pending_action_t g_kws_pending_action = AUDIO_KWS_PENDING_NONE;
static int16_t g_kws_fifo[AUDIO_KWS_FIFO_SAMPLES] __attribute__((aligned(4)));
static uint32_t g_kws_fifo_samples = 0U;
static uint32_t g_kws_last_log_ms = 0U;
static uint32_t g_kws_cooldown_start_ms = 0U;
static uint32_t g_ack_wait_start_ms = 0U;
static bool g_kws_first_inference_pending = true;
static uint32_t g_kws_capture_overflows = 0U;
static volatile uint32_t g_kws_capture_dropped_frames = 0U;
static volatile bool g_kws_capture_mode = false;

static uint8_t g_network_rx[AUDIO_NETWORK_RX_SIZE];
static uint32_t g_network_rx_offset = 0U;
static uint32_t g_network_rx_length = 0U;
static char g_protocol_line[AUDIO_PROTOCOL_LINE_SIZE];
static char g_session_id[AUDIO_SESSION_ID_SIZE];
static uint32_t g_session_counter = 0U;
static uint32_t g_record_sequence = 0U;
static uint32_t g_record_total_samples = 0U;
static uint32_t g_tts_expected_sequence = 0U;
static uint32_t g_tts_total_bytes = 0U;
static volatile bool g_server_done = false;
static bool g_text_received = false;
static bool g_transcript_nonempty = false;
static bool g_health_dialogue_active = false;
static bool g_health_page_opened = false;
static bool g_health_received = false;
static char g_health_diagnosis[AUDIO_HEALTH_DIAGNOSIS_MAX_BYTES + 1U];
static char g_health_medicine[AUDIO_HEALTH_MEDICINE_MAX_BYTES + 1U];
static uint32_t g_last_server_activity_ms = 0U;

static bool g_button_raw_pressed = false;
static bool g_button_stable_pressed = false;
static uint32_t g_button_change_ms = 0U;
static volatile bool g_button_press_pending = false;
static volatile audio_record_command_t g_record_command_queue[AUDIO_RECORD_COMMAND_QUEUE_DEPTH];
static volatile uint8_t g_record_command_head = 0U;
static volatile uint8_t g_record_command_tail = 0U;
static volatile uint8_t g_record_command_count = 0U;
static volatile bool g_record_command_overflow = false;

static uint8_t g_selected_channel = 0U;
static bool g_channel_locked = false;
static uint8_t g_channel_probe_frames = 0U;
static uint64_t g_channel_energy[2] = {0U, 0U};
static uint32_t g_noise_rms = AUDIO_VAD_MIN_RMS / 3U;
static uint8_t g_speech_candidate_frames = 0U;
static uint8_t g_silence_frames = 0U;
static bool g_speech_detected = false;
static uint32_t g_vad_left_rms = 0U;
static uint32_t g_vad_right_rms = 0U;
static uint32_t g_vad_rms = 0U;
static uint32_t g_vad_threshold = AUDIO_VAD_MIN_RMS;

static fsp_err_t audio_start_kws_capture(void);
static fsp_err_t audio_start_local_ack_playback(bool starts_dialogue);
static fsp_err_t audio_begin_text_speech(void);
static fsp_err_t audio_apply_kws_policy(void);

static void audio_log(const char * text)
{
    deepseek_bridge_pc_write(text);
}

static void audio_log_init_error(const char * stage, fsp_err_t err)
{
    char log_line[112];

    (void) snprintf(log_line,
                    sizeof(log_line),
                    "[AUDIO] init failed at %s: err=%d (0x%08lX).\r\n",
                    stage,
                    (int) err,
                    (unsigned long) err);
    audio_log(log_line);
}

static void audio_amp_set_enabled(bool enabled)
{
    /*
     * SD_MODE is active high on MAX98357A.  The external 10 kOhm pull-down is
     * still required: software cannot guarantee a low level during reset or
     * when the RA6M5 is unpowered.
     */
    (void) R_IOPORT_PinWrite(&g_ioport_ctrl,
                             AUDIO_AMP_SD_PIN,
                             enabled ? BSP_IO_LEVEL_HIGH : BSP_IO_LEVEL_LOW);
}

static bool audio_validate_fsp(void)
{
    const ssi_extended_cfg_t * p_ssi_extend;
    const timer_instance_t * p_mclk = &g_audio_mclk;
    const gpt_extended_cfg_t * p_gpt_extend;

    if ((0U == (uintptr_t) p_mclk) ||
        (NULL == p_mclk->p_ctrl) ||
        (NULL == p_mclk->p_cfg) ||
        (NULL == p_mclk->p_api))
    {
        audio_log("[AUDIO] FSP missing g_audio_mclk (GPT2).\r\n");
        return false;
    }

    if ((0U != g_i2s0_cfg.channel) ||
        (I2S_PCM_WIDTH_24_BITS != g_i2s0_cfg.pcm_width) ||
        (I2S_WORD_LENGTH_32_BITS != g_i2s0_cfg.word_length) ||
        (I2S_MODE_MASTER != g_i2s0_cfg.operating_mode) ||
        (I2S_WS_CONTINUE_ON != g_i2s0_cfg.ws_continue) ||
        (audio_ssie_callback != g_i2s0_cfg.p_callback) ||
        (NULL == g_i2s0_cfg.p_transfer_tx) ||
        (NULL == g_i2s0_cfg.p_transfer_rx) ||
        (2U != g_i2s0_cfg.txi_ipl) ||
        (2U != g_i2s0_cfg.rxi_ipl) ||
        (2U != g_i2s0_cfg.idle_err_ipl) ||
        (NULL == g_i2s0_cfg.p_extend))
    {
        audio_log("[AUDIO] FSP g_i2s0 must be SSIE0 master, 24/32-bit, WS continue, callback and TX/RX DTC.\r\n");
        return false;
    }

    p_ssi_extend = (const ssi_extended_cfg_t *) g_i2s0_cfg.p_extend;
    if ((SSI_AUDIO_CLOCK_INTERNAL != p_ssi_extend->audio_clock) ||
        (SSI_CLOCK_DIV_1 != p_ssi_extend->bit_clock_div))
    {
        audio_log("[AUDIO] FSP g_i2s0 clock must be Internal with divider /1.\r\n");
        return false;
    }

    if ((2U != p_mclk->p_cfg->channel) ||
        (TIMER_MODE_PERIODIC != p_mclk->p_cfg->mode) ||
        (98U != p_mclk->p_cfg->period_counts) ||
        ((48U > p_mclk->p_cfg->duty_cycle_counts) ||
         (50U < p_mclk->p_cfg->duty_cycle_counts)) ||
        (NULL == p_mclk->p_cfg->p_extend))
    {
        audio_log("[AUDIO] FSP g_audio_mclk must be GPT2 periodic, period 98 and near-50% duty.\r\n");
        return false;
    }

    p_gpt_extend = (const gpt_extended_cfg_t *) p_mclk->p_cfg->p_extend;
    if ((!p_gpt_extend->gtioca.output_enabled) ||
        p_gpt_extend->gtiocb.output_enabled)
    {
        audio_log("[AUDIO] FSP g_audio_mclk must enable GTIOCA only.\r\n");
        return false;
    }

    if (2U == Cold_Maker_cfg.channel)
    {
        audio_log("[AUDIO] GPT2 conflicts with Cold_Maker; keep Cold_Maker on GPT6.\r\n");
        return false;
    }

    return true;
}

static uint32_t audio_crc32(const uint8_t * p_data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;

    for (uint32_t i = 0U; i < length; i++)
    {
        crc ^= p_data[i];
        for (uint8_t bit = 0U; bit < 8U; bit++)
        {
            uint32_t mask = (uint32_t) (-(int32_t) (crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
        }
    }

    return ~crc;
}

static uint32_t audio_isqrt_u64(uint64_t value)
{
    uint64_t bit = (uint64_t) 1U << 62U;
    uint64_t result = 0U;

    while (bit > value)
    {
        bit >>= 2U;
    }

    while (0U != bit)
    {
        if (value >= (result + bit))
        {
            value -= result + bit;
            result = (result >> 1U) + bit;
        }
        else
        {
            result >>= 1U;
        }
        bit >>= 2U;
    }

    return (uint32_t) result;
}

static int16_t audio_sample_to_pcm16(int32_t raw)
{
    int32_t sample24 = raw & 0x00FFFFFFL;
    int32_t pcm;

    if (0 != (sample24 & 0x00800000L))
    {
        sample24 |= (int32_t) 0xFF000000UL;
    }

    pcm = (sample24 >> 8) * AUDIO_INPUT_GAIN;
    if (pcm > 32767)
    {
        pcm = 32767;
    }
    else if (pcm < -32768)
    {
        pcm = -32768;
    }

    return (int16_t) pcm;
}

static void audio_button_poll(void)
{
    bsp_io_level_t level = BSP_IO_LEVEL_HIGH;
    bool pressed;
    uint32_t now = system_tick_now();

    if (FSP_SUCCESS != R_IOPORT_PinRead(&g_ioport_ctrl,
                                        BSP_IO_PORT_00_PIN_00,
                                        &level))
    {
        return;
    }

    pressed = (BSP_IO_LEVEL_LOW == level);
    if (pressed != g_button_raw_pressed)
    {
        g_button_raw_pressed = pressed;
        g_button_change_ms = now;
    }

    if ((g_button_stable_pressed != g_button_raw_pressed) &&
        system_tick_elapsed(g_button_change_ms, AUDIO_BUTTON_DEBOUNCE_MS))
    {
        g_button_stable_pressed = g_button_raw_pressed;
        if (g_button_stable_pressed)
        {
            /*
             * Keep the press latched until audio_voice_process() consumes it.
             * W800 waits call audio_voice_service_realtime_inputs(), so a
             * valid 30 ms press is not lost while an AT/TCP transaction owns
             * the foreground for several seconds.
             */
            g_button_press_pending = true;
        }
    }
}

static bool audio_button_take_pressed_event(void)
{
    bool pressed = g_button_press_pending;

    g_button_press_pending = false;
    return pressed;
}

static void audio_record_command_submit(audio_record_command_t command)
{
    FSP_CRITICAL_SECTION_DEFINE;

    if ((!g_audio_initialized) ||
        ((AUDIO_RECORD_COMMAND_START != command) &&
         (AUDIO_RECORD_COMMAND_STOP != command)))
    {
        return;
    }

    /*
     * Called by the HMI UART interrupt.  Preserve command order, but never
     * perform playback, SSIE state changes or networking in interrupt context.
     * On overflow discard the entire queue so stale commands cannot trigger a
     * dialogue later.
     */
    FSP_CRITICAL_SECTION_ENTER;
    if (g_record_command_count < AUDIO_RECORD_COMMAND_QUEUE_DEPTH)
    {
        g_record_command_queue[g_record_command_tail] = command;
        g_record_command_tail =
            (uint8_t) ((g_record_command_tail + 1U) % AUDIO_RECORD_COMMAND_QUEUE_DEPTH);
        g_record_command_count++;
    }
    else
    {
        g_record_command_head = 0U;
        g_record_command_tail = 0U;
        g_record_command_count = 0U;
        g_record_command_overflow = true;
    }
    FSP_CRITICAL_SECTION_EXIT;
}

void audio_voice_submit_start_command(void)
{
    audio_record_command_submit(AUDIO_RECORD_COMMAND_START);
}

void audio_voice_submit_stop_command(void)
{
    audio_record_command_submit(AUDIO_RECORD_COMMAND_STOP);
}

static audio_record_command_t audio_record_command_take_event(void)
{
    audio_record_command_t command = AUDIO_RECORD_COMMAND_NONE;
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    if (g_record_command_count > 0U)
    {
        command = g_record_command_queue[g_record_command_head];
        g_record_command_head =
            (uint8_t) ((g_record_command_head + 1U) % AUDIO_RECORD_COMMAND_QUEUE_DEPTH);
        g_record_command_count--;
    }
    FSP_CRITICAL_SECTION_EXIT;
    return command;
}

static bool audio_record_command_take_overflow(void)
{
    bool overflow;
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    overflow = g_record_command_overflow;
    g_record_command_overflow = false;
    FSP_CRITICAL_SECTION_EXIT;
    return overflow;
}

static void audio_record_command_clear_events(void)
{
    FSP_CRITICAL_SECTION_DEFINE;

    FSP_CRITICAL_SECTION_ENTER;
    g_record_command_head = 0U;
    g_record_command_tail = 0U;
    g_record_command_count = 0U;
    g_record_command_overflow = false;
    FSP_CRITICAL_SECTION_EXIT;
}

static void audio_network_reset(void)
{
    g_network_rx_offset = 0U;
    g_network_rx_length = 0U;
}

static int audio_network_get_byte(uint8_t * p_byte, uint32_t timeout_ms)
{
    uint32_t start_ms = system_tick_now();

    while (!system_tick_elapsed(start_ms, timeout_ms))
    {
        if (g_network_rx_offset < g_network_rx_length)
        {
            *p_byte = g_network_rx[g_network_rx_offset++];
            return 1;
        }

        g_network_rx_offset = 0U;
        g_network_rx_length = 0U;
        {
            int32_t received = deepseek_bridge_socket_receive(g_network_rx,
                                                              sizeof(g_network_rx),
                                                              AUDIO_NETWORK_POLL_MS);
            if (received < 0)
            {
                return -1;
            }
            if (received > 0)
            {
                g_network_rx_length = (uint32_t) received;
            }
        }
    }

    return 0;
}

static int audio_network_read_line(char * p_line, size_t line_size, uint32_t timeout_ms)
{
    uint32_t start_ms = system_tick_now();
    size_t index = 0U;

    if ((NULL == p_line) || (line_size < 2U))
    {
        return -1;
    }

    while (!system_tick_elapsed(start_ms, timeout_ms))
    {
        uint8_t byte;
        uint32_t elapsed = system_tick_now() - start_ms;
        uint32_t remaining = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0U;
        int result = audio_network_get_byte(&byte, remaining);

        if (result <= 0)
        {
            return result;
        }

        if ('\n' == byte)
        {
            if ((index > 0U) && ('\r' == p_line[index - 1U]))
            {
                index--;
            }
            p_line[index] = '\0';
            return 1;
        }

        if (index >= (line_size - 1U))
        {
            return -1;
        }
        p_line[index++] = (char) byte;
    }

    return 0;
}

static bool audio_network_read_exact(uint8_t * p_data, uint32_t length, uint32_t timeout_ms)
{
    uint32_t start_ms = system_tick_now();

    for (uint32_t i = 0U; i < length; i++)
    {
        uint32_t elapsed = system_tick_now() - start_ms;
        uint32_t remaining = (elapsed < timeout_ms) ? (timeout_ms - elapsed) : 0U;
        if (1 != audio_network_get_byte(&p_data[i], remaining))
        {
            return false;
        }
    }

    return true;
}

static bool audio_network_discard(uint32_t length, uint32_t timeout_ms)
{
    uint8_t scratch[64];

    while (length > 0U)
    {
        uint32_t chunk = length;
        if (chunk > sizeof(scratch))
        {
            chunk = sizeof(scratch);
        }
        if (!audio_network_read_exact(scratch, chunk, timeout_ms))
        {
            return false;
        }
        length -= chunk;
    }

    return true;
}

static void audio_clear_capture(void)
{
    g_kws_capture_mode = false;
    memset((void *) g_capture_ready, 0, sizeof(g_capture_ready));
    g_capture_current = 0U;
    g_capture_consumer = 0U;
    g_capture_running = false;
    g_capture_stop_requested = false;
    g_capture_overflow = false;
}

static void audio_clear_playback(void)
{
    memset((void *) g_tx_ready, 0, sizeof(g_tx_ready));
    g_play_queue_head = 0U;
    g_play_queue_tail = 0U;
    g_play_queue_count = 0U;
    g_tx_current = -1;
    g_playback_running = false;
    g_playback_stop_requested = false;
    g_suffix_silence_inflight = false;
    g_playback_underruns = 0U;
    g_playback_clock_ready_ms = 0U;
    g_playback_start_message = NULL;
}

static void audio_reset_vad(void)
{
    g_selected_channel = 0U;
    g_channel_locked = false;
    g_channel_probe_frames = 0U;
    g_channel_energy[0] = 0U;
    g_channel_energy[1] = 0U;
    g_noise_rms = AUDIO_VAD_MIN_RMS / 3U;
    g_speech_candidate_frames = 0U;
    g_silence_frames = 0U;
    g_speech_detected = false;
    g_vad_left_rms = 0U;
    g_vad_right_rms = 0U;
    g_vad_rms = 0U;
    g_vad_threshold = AUDIO_VAD_MIN_RMS;
}

static void audio_schedule_kws_cooldown(void)
{
    g_kws_pending_action = AUDIO_KWS_PENDING_NONE;
    g_local_ack_playback = false;
    g_ack_starts_dialogue = false;
    if (g_kws_enabled && g_kws_model_ready)
    {
        g_kws_cooldown_start_ms = system_tick_now();
        g_audio_state = AUDIO_VOICE_STATE_KWS_COOLDOWN;
    }
    else
    {
        g_audio_state = AUDIO_VOICE_STATE_IDLE;
    }
}

static void audio_finish_text_speech(audio_voice_speech_result_t result)
{
    if (g_text_speech_active)
    {
        g_text_speech_active = false;
        g_text_speech_result = result;
    }
}

static bool audio_utf8_text_valid(const char * text, size_t length)
{
    size_t offset = 0U;

    while (offset < length)
    {
        uint8_t first = (uint8_t) text[offset];
        size_t continuation = 0U;

        if ((first < 0x20U) || (0x7FU == first))
        {
            return false;
        }
        if (first < 0x80U)
        {
            offset++;
            continue;
        }
        if ((first >= 0xC2U) && (first <= 0xDFU))
        {
            continuation = 1U;
        }
        else if ((first >= 0xE0U) && (first <= 0xEFU))
        {
            continuation = 2U;
        }
        else if ((first >= 0xF0U) && (first <= 0xF4U))
        {
            continuation = 3U;
        }
        else
        {
            return false;
        }

        if ((offset + continuation) >= length)
        {
            return false;
        }
        for (size_t i = 1U; i <= continuation; i++)
        {
            if (((uint8_t) text[offset + i] & 0xC0U) != 0x80U)
            {
                return false;
            }
        }
        if (((0xE0U == first) && ((uint8_t) text[offset + 1U] < 0xA0U)) ||
            ((0xEDU == first) && ((uint8_t) text[offset + 1U] >= 0xA0U)) ||
            ((0xF0U == first) && ((uint8_t) text[offset + 1U] < 0x90U)) ||
            ((0xF4U == first) && ((uint8_t) text[offset + 1U] >= 0x90U)))
        {
            return false;
        }
        offset += continuation + 1U;
    }

    return true;
}

static void audio_stop_ssie(void)
{
    /*
     * MAX98357A must enter shutdown before LRCLK/BCLK are removed.  If the
     * clocks disappear while SD_MODE is still high, its Class-D output can
     * remain in an abnormal state and produce strong noise or DC at the
     * speaker.  Keep this ordering for every normal and error stop path.
     */
    audio_amp_set_enabled(false);

    if (g_ssie_open && (!g_ssie_idle))
    {
        (void) R_SSI_Stop(&g_i2s0_ctrl);
    }
}

/*
 * Diagnostic-only clock change for MAX98357A sample-rate lock testing.
 * SSIE is closed before GPT2 changes so BCLK/LRCLK stop together, allowing
 * the amplifier to leave and then re-enter its clock-detection state cleanly.
 * R_GPT_PeriodSet() also updates periodic-output duty to approximately 50%.
 */
static fsp_err_t audio_reconfigure_internal_clock(uint32_t period_counts)
{
    const timer_instance_t * p_mclk = &g_audio_mclk;
    fsp_err_t err;

    if (!g_ssie_idle)
    {
        return FSP_ERR_IN_USE;
    }

    if (g_ssie_open)
    {
        err = R_SSI_Close(&g_i2s0_ctrl);
        if (FSP_SUCCESS != err)
        {
            return err;
        }
        g_ssie_open = false;
    }

    err = p_mclk->p_api->stop(p_mclk->p_ctrl);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    /* Give the MAX98357A enough clock-free time to discard its old lock. */
    R_BSP_SoftwareDelay(5U, BSP_DELAY_UNITS_MILLISECONDS);

    err = p_mclk->p_api->periodSet(p_mclk->p_ctrl, period_counts);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = p_mclk->p_api->start(p_mclk->p_ctrl);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_SSI_Open(&g_i2s0_ctrl, &g_i2s0_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_ssie_open = true;
    g_ssie_idle = true;
    return FSP_SUCCESS;
}

/*
 * Re-open SSIE with a diagnostic PCM width without touching generated FSP
 * files.  R_SSI_Open() retains the configuration pointer, so the copied
 * 32-bit configuration must have static storage duration.
 */
static fsp_err_t audio_reconfigure_pcm_width(bool use_32_bit_pcm)
{
    i2s_cfg_t const * p_cfg = &g_i2s0_cfg;
    fsp_err_t err;

    if (!g_ssie_idle)
    {
        return FSP_ERR_IN_USE;
    }

    if (g_ssie_open)
    {
        err = R_SSI_Close(&g_i2s0_ctrl);
        if (FSP_SUCCESS != err)
        {
            return err;
        }
        g_ssie_open = false;
    }

    /* Let MAX98357 discard the previous serial-audio format lock. */
    R_BSP_SoftwareDelay(5U, BSP_DELAY_UNITS_MILLISECONDS);

    if (use_32_bit_pcm)
    {
        g_i2s0_diag_32_cfg = g_i2s0_cfg;
        g_i2s0_diag_32_cfg.pcm_width = I2S_PCM_WIDTH_32_BITS;
        p_cfg = &g_i2s0_diag_32_cfg;
    }

    err = R_SSI_Open(&g_i2s0_ctrl, p_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_ssie_open = true;
    g_ssie_idle = true;
    g_tx_pcm_width_32 = use_32_bit_pcm;
    return FSP_SUCCESS;
}

static void audio_health_session_reset(bool dialogue_active)
{
    g_health_dialogue_active = dialogue_active;
    g_health_page_opened = false;
    g_health_received = false;
    g_transcript_nonempty = false;
    g_health_diagnosis[0] = '\0';
    g_health_medicine[0] = '\0';
}

static void audio_health_show_failure_if_needed(void)
{
    if (g_health_dialogue_active &&
        g_health_page_opened &&
        (!g_health_received))
    {
        OLED_SetHealthResult("分析失败", "");
        g_health_received = true;
    }

    g_health_dialogue_active = false;
}

static void audio_recover(const char * reason, bool reconnect)
{
    audio_log(reason);
    audio_record_command_clear_events();
    audio_health_show_failure_if_needed();
    audio_finish_text_speech(AUDIO_VOICE_SPEECH_RESULT_FAILED);
    g_capture_stop_requested = true;
    g_capture_running = false;
    g_playback_stop_requested = true;
    audio_stop_ssie();
    if (reconnect)
    {
        deepseek_bridge_force_reconnect();
    }
    g_audio_state = AUDIO_VOICE_STATE_RECOVERING;
}

static fsp_err_t audio_send_start(void)
{
    int written = snprintf(g_protocol_line,
                           sizeof(g_protocol_line),
                           "AUD1 START %s %u 1 S16LE %s\n",
                           g_session_id,
                           (unsigned int) AUDIO_SAMPLE_RATE,
                           AUDIO_VOICE_DEVICE_TOKEN);
    if ((written <= 0) || ((size_t) written >= sizeof(g_protocol_line)))
    {
        return FSP_ERR_OVERFLOW;
    }

    return deepseek_bridge_socket_send_all((const uint8_t *) g_protocol_line,
                                           (uint32_t) written);
}

static fsp_err_t audio_send_frame(const int16_t * p_pcm)
{
    uint32_t crc = audio_crc32((const uint8_t *) p_pcm, AUDIO_PCM_BYTES_PER_FRAME);
    int written = snprintf(g_protocol_line,
                           sizeof(g_protocol_line),
                           "AUD1 DATA %s %lu %u %08lX\n",
                           g_session_id,
                           (unsigned long) g_record_sequence,
                           (unsigned int) AUDIO_SAMPLES_PER_FRAME,
                           (unsigned long) crc);
    deepseek_bridge_iovec_t vectors[2];

    if ((written <= 0) || ((size_t) written >= sizeof(g_protocol_line)))
    {
        return FSP_ERR_OVERFLOW;
    }

    vectors[0].p_data = (const uint8_t *) g_protocol_line;
    vectors[0].length = (uint32_t) written;
    vectors[1].p_data = (const uint8_t *) p_pcm;
    vectors[1].length = AUDIO_PCM_BYTES_PER_FRAME;
    return deepseek_bridge_socket_sendv_all(vectors, 2U);
}

static fsp_err_t audio_send_end(void)
{
    int written = snprintf(g_protocol_line,
                           sizeof(g_protocol_line),
                           "AUD1 END %s %lu %lu\n",
                           g_session_id,
                           (unsigned long) g_record_sequence,
                           (unsigned long) g_record_total_samples);
    if ((written <= 0) || ((size_t) written >= sizeof(g_protocol_line)))
    {
        return FSP_ERR_OVERFLOW;
    }

    return deepseek_bridge_socket_send_all((const uint8_t *) g_protocol_line,
                                           (uint32_t) written);
}

static fsp_err_t audio_start_capture(void)
{
    fsp_err_t err;

    /* Recording uses the INMP441 only; keep the speaker amplifier muted. */
    audio_amp_set_enabled(false);
    audio_clear_capture();
    audio_clear_playback();
    audio_reset_vad();
    audio_network_reset();
    g_record_sequence = 0U;
    g_record_total_samples = 0U;
    g_tts_expected_sequence = 0U;
    g_tts_total_bytes = 0U;
    g_server_done = false;
    g_text_received = false;
    audio_health_session_reset(true);
    g_last_server_activity_ms = system_tick_now();
    g_ssie_idle = false;
    g_capture_running = true;

    err = R_SSI_Read(&g_i2s0_ctrl,
                     g_capture_raw[0],
                     AUDIO_RAW_BYTES_PER_FRAME);
    if (FSP_SUCCESS != err)
    {
        g_capture_running = false;
        g_ssie_idle = true;
    }
    return err;
}

static fsp_err_t audio_start_kws_capture(void)
{
    fsp_err_t err;

    if ((!g_kws_enabled) || (!g_kws_model_ready) || (!g_ssie_idle))
    {
        return FSP_ERR_IN_USE;
    }

    audio_amp_set_enabled(false);
    audio_clear_capture();
    audio_clear_playback();
    audio_reset_vad();
    audio_network_reset();
    g_kws_fifo_samples = 0U;
    g_kws_last_log_ms = system_tick_now();
    g_kws_first_inference_pending = true;
    g_ssie_idle = false;
    g_capture_running = true;
    g_kws_capture_mode = true;

    err = R_SSI_Read(&g_i2s0_ctrl,
                     g_capture_raw[0],
                     AUDIO_RAW_BYTES_PER_FRAME);
    if (FSP_SUCCESS != err)
    {
        g_capture_running = false;
        g_ssie_idle = true;
        return err;
    }

    g_audio_state = AUDIO_VOICE_STATE_KWS_LISTEN;
    return FSP_SUCCESS;
}

static fsp_err_t audio_begin_text_speech(void)
{
    char expected[64];
    size_t text_length = strlen(g_text_speech_text);
    int written;
    int ready;
    deepseek_bridge_iovec_t vectors[2];
    fsp_err_t err;

    if ((!g_text_speech_active) || (0U == text_length) || (!g_ssie_idle))
    {
        return FSP_ERR_IN_USE;
    }

    g_audio_state = AUDIO_VOICE_STATE_STARTING;
    deepseek_bridge_set_audio_active(true);
    if (FSP_SUCCESS != deepseek_bridge_audio_prepare())
    {
        audio_recover("[AUDIO] text speech could not prepare W800/TCP.\r\n", true);
        return FSP_ERR_ABORTED;
    }

    audio_clear_capture();
    audio_clear_playback();
    audio_network_reset();
    g_tts_expected_sequence = 0U;
    g_tts_total_bytes = 0U;
    g_server_done = false;
    g_text_received = false;
    audio_health_session_reset(false);
    g_local_ack_playback = false;
    g_ack_starts_dialogue = false;

    g_session_counter++;
    (void) snprintf(g_session_id,
                    sizeof(g_session_id),
                    "S%07lX-%04lX",
                    (unsigned long) (system_tick_now() & 0x0FFFFFFFUL),
                    (unsigned long) (g_session_counter & 0xFFFFU));

    written = snprintf(g_protocol_line,
                       sizeof(g_protocol_line),
                       "AUD1 SAY %s %u %s\n",
                       g_session_id,
                       (unsigned int) text_length,
                       AUDIO_VOICE_DEVICE_TOKEN);
    if ((written <= 0) || ((size_t) written >= sizeof(g_protocol_line)))
    {
        audio_recover("[AUDIO] text speech header overflowed.\r\n", false);
        return FSP_ERR_OVERFLOW;
    }

    vectors[0].p_data = (const uint8_t *) g_protocol_line;
    vectors[0].length = (uint32_t) written;
    vectors[1].p_data = (const uint8_t *) g_text_speech_text;
    vectors[1].length = (uint32_t) text_length;
    err = deepseek_bridge_socket_sendv_all(vectors, 2U);
    if (FSP_SUCCESS != err)
    {
        audio_recover("[AUDIO] failed to send AUD1 SAY.\r\n", true);
        return err;
    }

    ready = audio_network_read_line(g_protocol_line,
                                    sizeof(g_protocol_line),
                                    AUDIO_NETWORK_TIMEOUT_MS);
    (void) snprintf(expected, sizeof(expected), "AUD1 READY %s", g_session_id);
    if ((1 != ready) || (0 != strcmp(g_protocol_line, expected)))
    {
        audio_recover("[AUDIO] server rejected text speech.\r\n", true);
        return FSP_ERR_ABORTED;
    }

    g_last_server_activity_ms = system_tick_now();
    g_audio_state = AUDIO_VOICE_STATE_WAIT_RESULT;
    audio_log("[AUDIO] medicine-name TTS accepted; waiting for PCM.\r\n");
    return FSP_SUCCESS;
}

static void audio_begin_session(void)
{
    int ready;
    char expected[64];

    g_audio_state = AUDIO_VOICE_STATE_STARTING;
    deepseek_bridge_set_audio_active(true);
    if (FSP_SUCCESS != deepseek_bridge_audio_prepare())
    {
        audio_recover("[AUDIO] W800/TCP is not ready.\r\n", true);
        return;
    }

    g_session_counter++;
    (void) snprintf(g_session_id,
                    sizeof(g_session_id),
                    "%08lX-%04lX",
                    (unsigned long) system_tick_now(),
                    (unsigned long) (g_session_counter & 0xFFFFU));

    if (FSP_SUCCESS != audio_start_capture())
    {
        audio_recover("[AUDIO] SSIE receive could not start.\r\n", false);
        return;
    }

    if (FSP_SUCCESS != audio_send_start())
    {
        audio_recover("[AUDIO] failed to send AUD1 START.\r\n", true);
        return;
    }

    ready = audio_network_read_line(g_protocol_line,
                                    sizeof(g_protocol_line),
                                    AUDIO_NETWORK_TIMEOUT_MS);
    (void) snprintf(expected, sizeof(expected), "AUD1 READY %s", g_session_id);
    if ((1 != ready) || (0 != strcmp(g_protocol_line, expected)))
    {
        audio_recover("[AUDIO] server did not accept the audio session.\r\n", true);
        return;
    }

    audio_log("[AUDIO] recording started.\r\n");
    g_audio_state = AUDIO_VOICE_STATE_RECORDING;
}

static bool audio_capture_frame_available(void)
{
    return g_capture_ready[g_capture_consumer];
}

static uint32_t audio_convert_capture_frame(const int32_t * p_raw)
{
    uint64_t frame_energy[2] = {0U, 0U};
    int64_t frame_sum[2] = {0, 0};
    uint32_t rms;
    uint32_t threshold;

    for (uint32_t i = 0U; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        int16_t left = audio_sample_to_pcm16(p_raw[(i * 2U)]);
        int16_t right = audio_sample_to_pcm16(p_raw[(i * 2U) + 1U]);
        int32_t left32 = left;
        int32_t right32 = right;
        frame_energy[0] += (uint64_t) (left32 * left32);
        frame_energy[1] += (uint64_t) (right32 * right32);
        frame_sum[0] += left32;
        frame_sum[1] += right32;
    }

    /*
     * Remove each channel's DC component before calculating VAD energy.
     * Digital microphones can have a small DC offset; including it in RMS
     * makes the adaptive noise floor rise and can prevent speech detection.
     */
    for (uint32_t channel = 0U; channel < 2U; channel++)
    {
        uint64_t sum_squared = (uint64_t) (frame_sum[channel] * frame_sum[channel]);
        uint64_t dc_energy = sum_squared / AUDIO_SAMPLES_PER_FRAME;
        frame_energy[channel] = (frame_energy[channel] > dc_energy) ?
                                (frame_energy[channel] - dc_energy) : 0U;
    }

    g_vad_left_rms = audio_isqrt_u64(frame_energy[0] / AUDIO_SAMPLES_PER_FRAME);
    g_vad_right_rms = audio_isqrt_u64(frame_energy[1] / AUDIO_SAMPLES_PER_FRAME);

    if (!g_channel_locked)
    {
        g_channel_energy[0] += frame_energy[0];
        g_channel_energy[1] += frame_energy[1];
        g_channel_probe_frames++;
        g_selected_channel = (g_channel_energy[1] > g_channel_energy[0]) ? 1U : 0U;
        if (g_channel_probe_frames >= 3U)
        {
            g_channel_locked = true;
        }
    }

    for (uint32_t i = 0U; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        int16_t sample = audio_sample_to_pcm16(p_raw[(i * 2U) + g_selected_channel]);
        g_capture_pcm[i] = sample;
    }

    rms = (0U == g_selected_channel) ? g_vad_left_rms : g_vad_right_rms;
    threshold = g_noise_rms * 2U;
    if (threshold < AUDIO_VAD_MIN_RMS)
    {
        threshold = AUDIO_VAD_MIN_RMS;
    }
    else if (threshold > AUDIO_VAD_MAX_RMS)
    {
        threshold = AUDIO_VAD_MAX_RMS;
    }

    if (rms >= threshold)
    {
        if (g_speech_candidate_frames < AUDIO_SPEECH_CONFIRM_FRAMES)
        {
            g_speech_candidate_frames++;
        }
        if (g_speech_candidate_frames >= AUDIO_SPEECH_CONFIRM_FRAMES)
        {
            g_speech_detected = true;
            g_silence_frames = 0U;
        }
    }
    else
    {
        g_speech_candidate_frames = 0U;
        if (g_speech_detected)
        {
            if (g_silence_frames < UINT8_MAX)
            {
                g_silence_frames++;
            }
        }
        else
        {
            /*
             * Only learn samples close to the existing noise estimate.
             * Otherwise quiet speech below the threshold would be learned as
             * noise and continuously push the threshold away from speech.
             */
            if (rms <= (g_noise_rms + (g_noise_rms / 2U) + 20U))
            {
                g_noise_rms = ((g_noise_rms * 7U) + rms) / 8U;
            }
        }
    }

    g_vad_rms = rms;
    g_vad_threshold = threshold;

    return rms;
}

static void audio_convert_kws_frame(const int32_t * p_raw)
{
    /*
     * The first three frames still use the full two-channel energy analysis
     * to locate the populated INMP441 slot.  Once that choice is locked, KWS
     * needs PCM only: repeating the 64-bit two-channel RMS/VAD work ten times
     * per second wastes enough CPU to make continuous inference fall behind.
     */
    if (!g_channel_locked)
    {
        (void) audio_convert_capture_frame(p_raw);
        return;
    }

    for (uint32_t i = 0U; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        g_capture_pcm[i] =
            audio_sample_to_pcm16(p_raw[(i * AUDIO_CHANNELS) + g_selected_channel]);
    }
}

static void audio_request_capture_stop(void)
{
    if (!g_capture_stop_requested)
    {
        g_capture_stop_requested = true;
        audio_log("[AUDIO] finishing current 100 ms frame.\r\n");
    }
}

static void audio_process_capture_frame(void)
{
    uint8_t index = g_capture_consumer;
    fsp_err_t send_err;

    if (!g_capture_ready[index])
    {
        return;
    }

    (void) audio_convert_capture_frame(g_capture_raw[index]);
    send_err = audio_send_frame(g_capture_pcm);
    if (FSP_SUCCESS != send_err)
    {
        char log_line[112];

        g_capture_ready[index] = false;
        (void) snprintf(log_line,
                        sizeof(log_line),
                        "[AUDIO] PCM upload failed at frame=%lu, err=%d.\r\n",
                        (unsigned long) g_record_sequence,
                        (int) send_err);
        audio_recover(log_line, true);
        return;
    }

    g_record_sequence++;
    g_record_total_samples += AUDIO_SAMPLES_PER_FRAME;
    g_capture_ready[index] = false;
    g_capture_consumer = (uint8_t) ((index + 1U) % AUDIO_CAPTURE_BUFFER_COUNT);

    if (0U == (g_record_sequence % 5U))
    {
        char vad_log[144];
        (void) snprintf(vad_log,
                        sizeof(vad_log),
                        "[AUDIO] VAD L=%lu R=%lu ch=%c rms=%lu noise=%lu threshold=%lu speech=%u.\r\n",
                        (unsigned long) g_vad_left_rms,
                        (unsigned long) g_vad_right_rms,
                        (0U == g_selected_channel) ? 'L' : 'R',
                        (unsigned long) g_vad_rms,
                        (unsigned long) g_noise_rms,
                        (unsigned long) g_vad_threshold,
                        g_speech_detected ? 1U : 0U);
        audio_log(vad_log);
    }

    /* C2/P000 is the normal stop path.  Keep only the 30 s safety ceiling. */
    if (g_record_sequence >= AUDIO_MAX_RECORD_FRAMES)
    {
        audio_request_capture_stop();
    }
}

static void audio_request_kws_stop(audio_kws_pending_action_t action)
{
    if (AUDIO_VOICE_STATE_KWS_LISTEN != g_audio_state)
    {
        return;
    }

    g_kws_pending_action = action;
    g_capture_stop_requested = true;
    g_audio_state = AUDIO_VOICE_STATE_KWS_STOPPING;
}

static void audio_log_kws_scores(const char * prefix)
{
    wake_word_status_t status;
    char log_line[272];
    char phase[32];

    wake_word_get_status(&status);
    if (status.slices_processed < WAKE_WORD_WARMUP_SLICES)
    {
        (void) snprintf(phase,
                        sizeof(phase),
                        "warming-up(%lu/%u)",
                        (unsigned long) status.slices_processed,
                        (unsigned int) WAKE_WORD_WARMUP_SLICES);
    }
    else
    {
        (void) snprintf(phase, sizeof(phase), "active");
    }

    (void) snprintf(log_line,
                    sizeof(log_line),
                    "%s enabled=%u listening=%u health_mode=%u requested=%u armed=%u phase=%s nihao_xiaou=%u.%03u noise=%u.%03u unknown=%u.%03u threshold=0.650 hits=%u/2 slices=%lu overflows=%lu dropped=%lu err=%ld.\r\n",
                    prefix,
                    g_kws_enabled ? 1U : 0U,
                    (AUDIO_VOICE_STATE_KWS_LISTEN == g_audio_state) ? 1U : 0U,
                    g_kws_health_mode ? 1U : 0U,
                    g_kws_requested ? 1U : 0U,
                    g_kws_armed ? 1U : 0U,
                    phase,
                    status.wake_permille / 1000U,
                    status.wake_permille % 1000U,
                    status.noise_permille / 1000U,
                    status.noise_permille % 1000U,
                    status.unknown_permille / 1000U,
                    status.unknown_permille % 1000U,
                    status.consecutive_hits,
                    (unsigned long) status.slices_processed,
                    (unsigned long) g_kws_capture_overflows,
                    (unsigned long) g_kws_capture_dropped_frames,
                    (long) status.last_error);
    audio_log(log_line);
}

static void audio_process_kws_frame(void)
{
    uint8_t index = g_capture_consumer;
    bool triggered = false;
    wake_word_status_t status;

    if (!g_capture_ready[index])
    {
        return;
    }

    audio_convert_kws_frame(g_capture_raw[index]);
    g_capture_ready[index] = false;
    g_capture_consumer = (uint8_t) ((index + 1U) % AUDIO_CAPTURE_BUFFER_COUNT);

    if ((g_kws_fifo_samples + AUDIO_SAMPLES_PER_FRAME) > AUDIO_KWS_FIFO_SAMPLES)
    {
        audio_log("[KWS] PCM FIFO overflow; restarting listener.\r\n");
        audio_request_kws_stop(AUDIO_KWS_PENDING_NONE);
        return;
    }

    memcpy(&g_kws_fifo[g_kws_fifo_samples],
           g_capture_pcm,
           AUDIO_PCM_BYTES_PER_FRAME);
    g_kws_fifo_samples += AUDIO_SAMPLES_PER_FRAME;

    if (g_kws_fifo_samples >= WAKE_WORD_SLICE_SAMPLES)
    {
        if (g_kws_first_inference_pending)
        {
            audio_log("[KWS] first inference started.\r\n");
        }
        triggered = wake_word_process(g_kws_fifo,
                                      WAKE_WORD_SLICE_SAMPLES);
        g_kws_fifo_samples -= WAKE_WORD_SLICE_SAMPLES;
        if (g_kws_fifo_samples > 0U)
        {
            memmove(g_kws_fifo,
                    &g_kws_fifo[WAKE_WORD_SLICE_SAMPLES],
                    g_kws_fifo_samples * sizeof(g_kws_fifo[0]));
        }

        wake_word_get_status(&status);
        if (0 != status.last_error)
        {
            audio_log_kws_scores("[KWS] inference failed;");
            g_kws_enabled = false;
            audio_request_kws_stop(AUDIO_KWS_PENDING_DISABLE);
            return;
        }

        if (g_kws_first_inference_pending)
        {
            g_kws_first_inference_pending = false;
            audio_log("[KWS] first inference completed; MFE window is warming up (1/4).\r\n");
        }

        if (WAKE_WORD_WARMUP_SLICES == status.slices_processed)
        {
            audio_log("[KWS] warm-up complete; wake-word scores are now valid.\r\n");
        }

        if (triggered)
        {
            /* A successful wake consumes this Flag=2 entry immediately. */
            g_kws_armed = false;
            g_kws_enabled = false;
            audio_log_kws_scores("[KWS] wake word detected;");
            audio_request_kws_stop(AUDIO_KWS_PENDING_DIALOGUE);
            return;
        }
    }

    if (system_tick_elapsed(g_kws_last_log_ms, AUDIO_KWS_LOG_INTERVAL_MS))
    {
        g_kws_last_log_ms = system_tick_now();
        audio_log_kws_scores("[KWS]");
    }
}

static bool audio_play_queue_push(const uint8_t * p_pcm, uint32_t bytes)
{
    uint8_t tail;

    if ((0U != (bytes & 1U)) ||
        (bytes > AUDIO_PCM_BYTES_PER_FRAME) ||
        (g_play_queue_count >= AUDIO_PLAY_QUEUE_COUNT))
    {
        return false;
    }

    tail = g_play_queue_tail;
    memcpy(g_play_queue[tail], p_pcm, bytes);
    g_play_queue_samples[tail] = (uint16_t) (bytes / sizeof(int16_t));
    g_play_queue_tail = (uint8_t) ((tail + 1U) % AUDIO_PLAY_QUEUE_COUNT);
    g_play_queue_count++;
    return true;
}

static int audio_find_free_tx(void)
{
    for (uint8_t i = 0U; i < AUDIO_TX_BUFFER_COUNT; i++)
    {
        if ((!g_tx_ready[i]) && (g_tx_current != (int8_t) i))
        {
            return (int) i;
        }
    }
    return -1;
}

static void audio_prepare_tx_buffers(void)
{
    while (g_play_queue_count > 0U)
    {
        int slot = audio_find_free_tx();
        uint8_t head;
        uint16_t samples;

        if (slot < 0)
        {
            return;
        }

        head = g_play_queue_head;
        samples = g_play_queue_samples[head];
        for (uint32_t i = 0U; i < AUDIO_SAMPLES_PER_FRAME; i++)
        {
            int32_t value = 0;
            if (i < samples)
            {
                int32_t sample = (int32_t) g_play_queue[head][i];

                /*
                 * FSP right-justifies 24-bit PCM in the 32-bit FIFO word.
                 * The diagnostic 32-bit mode instead scales PCM16 over the
                 * complete 32-bit sample.  Multiplication avoids undefined
                 * behaviour from left-shifting a negative signed value.
                 */
                value = sample * (g_tx_pcm_width_32 ? 65536 : 256);
            }
            g_tx_raw[slot][i * 2U] = value;
            g_tx_raw[slot][(i * 2U) + 1U] = value;
        }

        g_tx_ready[slot] = true;
        g_play_queue_head = (uint8_t) ((head + 1U) % AUDIO_PLAY_QUEUE_COUNT);
        g_play_queue_count--;
    }
}

static fsp_err_t audio_start_playback(const char * start_message)
{
    fsp_err_t err;

    /*
     * Start SSIE with a silent frame while the amplifier is shut down.  Once
     * BCLK/LRCLK/DIN are stable, enable the MAX98357A.  The rest of the first
     * 100 ms silent frame acts as an anti-pop prefix.
     */
    audio_amp_set_enabled(false);
    audio_prepare_tx_buffers();
    g_ssie_idle = false;
    g_playback_running = true;
    g_tx_current = -1;
    err = R_SSI_Write(&g_i2s0_ctrl,
                      g_tx_silence,
                      AUDIO_RAW_BYTES_PER_FRAME);
    if (FSP_SUCCESS != err)
    {
        g_playback_running = false;
        g_ssie_idle = true;
        audio_recover("[AUDIO] SSIE playback could not start.\r\n", false);
        return err;
    }

    g_playback_clock_ready_ms = system_tick_now();
    g_playback_start_message = start_message;
    g_audio_state = AUDIO_VOICE_STATE_PLAYBACK_STARTING;
    return FSP_SUCCESS;
}

static fsp_err_t audio_start_local_ack_playback(bool starts_dialogue)
{
    uint32_t offset = 0U;

    if (!g_ssie_idle)
    {
        return FSP_ERR_IN_USE;
    }

    audio_clear_capture();
    audio_clear_playback();
    audio_network_reset();
    g_tts_expected_sequence = 0U;
    g_tts_total_bytes = 0U;
    g_text_received = false;

    while (offset < g_wake_ack_pcm_samples)
    {
        uint32_t samples = g_wake_ack_pcm_samples - offset;
        if (samples > AUDIO_SAMPLES_PER_FRAME)
        {
            samples = AUDIO_SAMPLES_PER_FRAME;
        }

        /*
         * The original prompt has plenty of peak headroom but a much lower
         * average level than the diagnostic sine tone.  Apply a local-only
         * 2.5x gain while copying it to the queue so that "wo zai" is not
         * masked by the MAX98357A/speaker noise floor.  The source peak is
         * 12000, so this gain remains below PCM16 full scale.
         */
        for (uint32_t i = 0U; i < samples; i++)
        {
            int32_t scaled =
                ((int32_t) g_wake_ack_pcm[offset + i] *
                 AUDIO_LOCAL_ACK_GAIN_NUMERATOR) /
                AUDIO_LOCAL_ACK_GAIN_DENOMINATOR;

            if (scaled > INT16_MAX)
            {
                scaled = INT16_MAX;
            }
            else if (scaled < INT16_MIN)
            {
                scaled = INT16_MIN;
            }
            g_capture_pcm[i] = (int16_t) scaled;
        }

        if (!audio_play_queue_push(
                (const uint8_t *) g_capture_pcm,
                samples * sizeof(g_wake_ack_pcm[0])))
        {
            audio_clear_playback();
            return FSP_ERR_OVERFLOW;
        }
        offset += samples;
    }

    g_server_done = true;
    g_local_ack_playback = true;
    g_ack_starts_dialogue = starts_dialogue;
    deepseek_bridge_set_audio_active(true);
    return audio_start_playback(
        starts_dialogue ?
        "[AUDIO] local reply started: wo zai; recording will follow.\r\n" :
        "[ACK] local reply started: wo zai (no network audio upload).\r\n");
}

static int audio_receive_server_message(uint32_t timeout_ms)
{
    int result = audio_network_read_line(g_protocol_line,
                                         sizeof(g_protocol_line),
                                         timeout_ms);
    char session[AUDIO_SESSION_ID_SIZE];
    unsigned long sequence;
    unsigned long length;
    unsigned long checksum;

    if (result <= 0)
    {
        return result;
    }

    if (4 == sscanf(g_protocol_line,
                    "AUD1 PCM %23s %lu %lu %lx",
                    session,
                    &sequence,
                    &length,
                    &checksum))
    {
        if ((0 != strcmp(session, g_session_id)) ||
            (sequence != g_tts_expected_sequence) ||
            (0U == length) ||
            (length > AUDIO_PCM_BYTES_PER_FRAME) ||
            (0U != (length & 1U)) ||
            (g_play_queue_count >= AUDIO_PLAY_QUEUE_COUNT))
        {
            return -1;
        }

        if (!audio_network_read_exact(g_pcm_receive_frame,
                                      (uint32_t) length,
                                      AUDIO_NETWORK_TIMEOUT_MS) ||
            (audio_crc32(g_pcm_receive_frame, (uint32_t) length) != (uint32_t) checksum) ||
            (!audio_play_queue_push(g_pcm_receive_frame, (uint32_t) length)))
        {
            return -1;
        }

        g_tts_expected_sequence++;
        g_tts_total_bytes += (uint32_t) length;
        return 1;
    }

    if (2 == sscanf(g_protocol_line,
                    "AUD1 TEXT %23s %lu",
                    session,
                    &length))
    {
        if ((0 != strcmp(session, g_session_id)) ||
            (length > 65535UL) ||
            (!audio_network_discard((uint32_t) length, AUDIO_NETWORK_TIMEOUT_MS)))
        {
            return -1;
        }
        g_text_received = true;
        g_transcript_nonempty = (length > 0UL);
        return 1;
    }

    {
        unsigned long diagnosis_length;
        unsigned long medicine_length;
        if (3 == sscanf(g_protocol_line,
                        "AUD1 HEALTH %23s %lu %lu",
                        session,
                        &diagnosis_length,
                        &medicine_length))
        {
            if ((!g_health_dialogue_active) ||
                (!g_health_page_opened) ||
                g_health_received ||
                (0 != strcmp(session, g_session_id)) ||
                (0UL == diagnosis_length) ||
                (diagnosis_length > AUDIO_HEALTH_DIAGNOSIS_MAX_BYTES) ||
                (0UL == medicine_length) ||
                (medicine_length > AUDIO_HEALTH_MEDICINE_MAX_BYTES) ||
                (!audio_network_read_exact((uint8_t *) g_health_diagnosis,
                                           (uint32_t) diagnosis_length,
                                           AUDIO_NETWORK_TIMEOUT_MS)) ||
                (!audio_network_read_exact((uint8_t *) g_health_medicine,
                                           (uint32_t) medicine_length,
                                           AUDIO_NETWORK_TIMEOUT_MS)))
            {
                return -1;
            }

            g_health_diagnosis[diagnosis_length] = '\0';
            g_health_medicine[medicine_length] = '\0';
            if ((!audio_utf8_text_valid(g_health_diagnosis,
                                        (size_t) diagnosis_length)) ||
                (!audio_utf8_text_valid(g_health_medicine,
                                        (size_t) medicine_length)))
            {
                return -1;
            }

            OLED_SetHealthResult(g_health_diagnosis, g_health_medicine);
            g_health_received = true;
            return 1;
        }
    }

    {
        unsigned long frames;
        unsigned long total_bytes;
        if (3 == sscanf(g_protocol_line,
                        "AUD1 DONE %23s %lu %lu",
                        session,
                        &frames,
                        &total_bytes))
        {
            if ((0 != strcmp(session, g_session_id)) ||
                (frames != g_tts_expected_sequence) ||
                (total_bytes != g_tts_total_bytes))
            {
                return -1;
            }
            g_server_done = true;
            if (g_health_dialogue_active &&
                g_health_page_opened &&
                (!g_health_received))
            {
                OLED_SetHealthResult(
                    (g_text_received && (!g_transcript_nonempty)) ?
                    "未识别到有效语音" : "分析失败",
                    "");
                g_health_received = true;
            }
            return 1;
        }
    }

    {
        char error_code[32];
        unsigned long message_bytes;
        if (3 == sscanf(g_protocol_line,
                        "AUD1 ERROR %23s %31s %lu",
                        session,
                        error_code,
                        &message_bytes))
        {
            if ((0 != strcmp(session, g_session_id)) ||
                (message_bytes > 65535UL) ||
                (!audio_network_discard((uint32_t) message_bytes,
                                        AUDIO_NETWORK_TIMEOUT_MS)))
            {
                return -1;
            }
            return -1;
        }
    }

    return -1;
}

fsp_err_t audio_voice_init(void)
{
    fsp_err_t err;
    bsp_io_level_t button_level = BSP_IO_LEVEL_HIGH;
    const timer_instance_t * p_mclk = &g_audio_mclk;

    /* FSP configures P311 low; write it low again before any failure path. */
    audio_amp_set_enabled(false);
    audio_log("[AUDIO] init begin (audio-amp-mute-v1).\r\n");

    if (('\0' == AUDIO_VOICE_DEVICE_TOKEN[0]) ||
        (0 == strcmp(AUDIO_VOICE_DEVICE_TOKEN,
                     "replace-with-the-same-random-token-as-server")))
    {
        audio_log("[AUDIO] Set AUDIO_VOICE_DEVICE_TOKEN before flashing.\r\n");
        g_audio_state = AUDIO_VOICE_STATE_DISABLED;
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if (!audio_validate_fsp())
    {
        g_audio_state = AUDIO_VOICE_STATE_DISABLED;
        return FSP_ERR_INVALID_ARGUMENT;
    }

    err = p_mclk->p_api->open(p_mclk->p_ctrl, p_mclk->p_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        audio_log_init_error("GPT2 open", err);
        return err;
    }
    err = p_mclk->p_api->start(p_mclk->p_ctrl);
    if (FSP_SUCCESS != err)
    {
        audio_log_init_error("GPT2 start", err);
        return err;
    }

    err = R_SSI_Open(&g_i2s0_ctrl, &g_i2s0_cfg);
    if ((FSP_SUCCESS != err) && (FSP_ERR_ALREADY_OPEN != err))
    {
        audio_log_init_error("SSIE0/DTC open", err);
        return err;
    }

    g_ssie_open = true;
    g_ssie_idle = true;
    audio_clear_capture();
    audio_clear_playback();
    audio_network_reset();
    (void) R_IOPORT_PinRead(&g_ioport_ctrl,
                            BSP_IO_PORT_00_PIN_00,
                            &button_level);
    g_button_raw_pressed = (BSP_IO_LEVEL_LOW == button_level);
    g_button_stable_pressed = g_button_raw_pressed;
    g_button_change_ms = system_tick_now();
    g_button_press_pending = false;
    g_record_command_head = 0U;
    g_record_command_tail = 0U;
    g_record_command_count = 0U;
    g_record_command_overflow = false;
    g_audio_initialized = true;
    g_audio_state = AUDIO_VOICE_STATE_IDLE;
    audio_log("[AUDIO] SSIE voice path ready; P000 or Flag=2 0xC1 starts dialogue, 0xC2 stops recording.\r\n");

#if AUDIO_VOICE_ENABLE_KWS
    if (wake_word_init())
    {
        g_kws_model_ready = true;
        g_kws_requested = true;
        g_kws_health_mode = false;
        g_kws_armed = false;
        g_kws_enabled = false;
        audio_log("[KWS] Edge Impulse int8 model ready: label=nihao_xiaou, threshold=0.650, hits=2; waiting for health mode (Flag=2).\r\n");
    }
    else
    {
        g_kws_model_ready = false;
        g_kws_enabled = false;
        audio_log("[KWS] Edge Impulse model initialization failed; local wake disabled, P000 cloud voice remains available.\r\n");
    }
#else
    g_kws_model_ready = false;
    g_kws_requested = false;
    g_kws_health_mode = false;
    g_kws_armed = false;
    g_kws_enabled = false;
    audio_log("[KWS] disabled by build configuration; use P000 or Flag=2 0xC1.\r\n");
#endif
    return FSP_SUCCESS;
}

bool audio_voice_busy(void)
{
    return g_audio_initialized &&
           (AUDIO_VOICE_STATE_IDLE != g_audio_state) &&
           (AUDIO_VOICE_STATE_KWS_LISTEN != g_audio_state) &&
           (AUDIO_VOICE_STATE_KWS_COOLDOWN != g_audio_state) &&
           (AUDIO_VOICE_STATE_DISABLED != g_audio_state);
}

fsp_err_t audio_voice_play_ack(void)
{
    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if (AUDIO_VOICE_STATE_KWS_LISTEN == g_audio_state)
    {
        audio_request_kws_stop(AUDIO_KWS_PENDING_ACK);
        return FSP_SUCCESS;
    }

    if ((AUDIO_VOICE_STATE_KWS_STOPPING == g_audio_state) &&
        (AUDIO_KWS_PENDING_ACK == g_kws_pending_action))
    {
        return FSP_SUCCESS;
    }

    if (((AUDIO_VOICE_STATE_IDLE == g_audio_state) ||
         (AUDIO_VOICE_STATE_KWS_COOLDOWN == g_audio_state)) &&
        g_ssie_idle)
    {
        return audio_start_local_ack_playback(false);
    }

    return FSP_ERR_IN_USE;
}

fsp_err_t audio_voice_speak_text(const char * utf8_text)
{
    size_t length;
    fsp_err_t err;

    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }
    if (NULL == utf8_text)
    {
        return FSP_ERR_ASSERTION;
    }

    length = strlen(utf8_text);
    if ((0U == length) || (length > AUDIO_SAY_TEXT_MAX_BYTES) ||
        (!audio_utf8_text_valid(utf8_text, length)))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    if (g_text_speech_active)
    {
        return FSP_ERR_IN_USE;
    }
    if ((AUDIO_VOICE_STATE_KWS_LISTEN != g_audio_state) &&
        (AUDIO_VOICE_STATE_IDLE != g_audio_state) &&
        (AUDIO_VOICE_STATE_KWS_COOLDOWN != g_audio_state))
    {
        return FSP_ERR_IN_USE;
    }

    memcpy(g_text_speech_text, utf8_text, length + 1U);
    g_text_speech_result = AUDIO_VOICE_SPEECH_RESULT_NONE;
    g_text_speech_active = true;

    if (AUDIO_VOICE_STATE_KWS_LISTEN == g_audio_state)
    {
        audio_request_kws_stop(AUDIO_KWS_PENDING_TEXT_SPEECH);
        return FSP_SUCCESS;
    }

    err = audio_begin_text_speech();
    if (FSP_SUCCESS != err)
    {
        audio_finish_text_speech(AUDIO_VOICE_SPEECH_RESULT_FAILED);
    }
    return err;
}

bool audio_voice_text_speech_active(void)
{
    return g_text_speech_active;
}

void audio_voice_cancel_text_speech(void)
{
    if ((!g_audio_initialized) || (!g_text_speech_active))
    {
        return;
    }

    g_capture_stop_requested = true;
    g_capture_running = false;
    g_playback_stop_requested = true;
    g_playback_running = false;
    g_kws_pending_action = AUDIO_KWS_PENDING_NONE;
    g_kws_fifo_samples = 0U;
    g_local_ack_playback = false;
    g_ack_starts_dialogue = false;
    audio_finish_text_speech(AUDIO_VOICE_SPEECH_RESULT_FAILED);
    audio_stop_ssie();
    g_ssie_idle = true;
    audio_clear_capture();
    audio_clear_playback();
    audio_reset_vad();
    audio_network_reset();
    g_server_done = false;
    g_text_received = false;
    audio_health_session_reset(false);
    deepseek_bridge_set_audio_active(false);
    deepseek_bridge_force_reconnect();
    g_audio_state = AUDIO_VOICE_STATE_IDLE;
    audio_log("[VISION] medicine speech cancelled; audio path released.\r\n");
}

audio_voice_speech_result_t audio_voice_take_speech_result(void)
{
    audio_voice_speech_result_t result = g_text_speech_result;
    g_text_speech_result = AUDIO_VOICE_SPEECH_RESULT_NONE;
    return result;
}

fsp_err_t audio_voice_kws_set_enabled(bool enabled)
{
    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if (enabled)
    {
        if (!g_kws_model_ready)
        {
            return FSP_ERR_NOT_OPEN;
        }
        if (g_kws_health_mode && (!g_kws_armed))
        {
            audio_log("[KWS] this Flag=2 wake opportunity is already consumed; leave and re-enter Flag=2 to re-arm.\r\n");
            return FSP_ERR_IN_USE;
        }
    }

    g_kws_requested = enabled;
    return audio_apply_kws_policy();
}

static fsp_err_t audio_apply_kws_policy(void)
{
    fsp_err_t err;
    bool should_enable = g_kws_model_ready &&
                         g_kws_requested &&
                         g_kws_health_mode &&
                         g_kws_armed;

    if (should_enable)
    {
        if (g_kws_enabled)
        {
            return FSP_SUCCESS;
        }

        g_kws_enabled = true;
        if ((AUDIO_VOICE_STATE_IDLE == g_audio_state) && g_ssie_idle)
        {
            wake_word_reset();
            err = audio_start_kws_capture();
            if (FSP_SUCCESS != err)
            {
                g_kws_enabled = false;
                return err;
            }
        }
        return FSP_SUCCESS;
    }

    if (!g_kws_enabled)
    {
        return FSP_SUCCESS;
    }

    g_kws_enabled = false;
    if (AUDIO_VOICE_STATE_KWS_LISTEN == g_audio_state)
    {
        audio_request_kws_stop(AUDIO_KWS_PENDING_DISABLE);
    }
    else if (AUDIO_VOICE_STATE_KWS_COOLDOWN == g_audio_state)
    {
        g_audio_state = AUDIO_VOICE_STATE_IDLE;
    }
    else if ((AUDIO_VOICE_STATE_KWS_STOPPING == g_audio_state) &&
             (AUDIO_KWS_PENDING_DIALOGUE != g_kws_pending_action))
    {
        g_kws_pending_action = AUDIO_KWS_PENDING_DISABLE;
    }
    return FSP_SUCCESS;
}

fsp_err_t audio_voice_set_health_mode(bool active)
{
    fsp_err_t err;
    bool changed;

    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    changed = (active != g_kws_health_mode);
    if (changed && active)
    {
        /* Every real non-2 -> 2 transition restores the default listener. */
        g_kws_armed = true;
        g_kws_requested = true;
    }
    else if (changed)
    {
        g_kws_armed = false;
        audio_record_command_clear_events();
    }
    g_kws_health_mode = active;
    err = audio_apply_kws_policy();

    if (changed)
    {
        if (!active)
        {
            audio_log("[KWS] health mode (Flag=2) exited; listener stopping.\r\n");
        }
        else if (!g_kws_model_ready)
        {
            audio_log("[KWS] health mode (Flag=2) entered, but model is unavailable.\r\n");
        }
        else if (FSP_SUCCESS == err)
        {
            audio_log("[KWS] health mode (Flag=2) entered; armed=1, listener enabled.\r\n");
        }
        else
        {
            audio_log_init_error("KWS health-mode listener start", err);
        }
    }

    return err;
}

bool audio_voice_kws_enabled(void)
{
    return g_kws_enabled;
}

void audio_voice_kws_log_status(void)
{
    if (!g_kws_model_ready)
    {
        audio_log("[KWS] model is not ready; local wake is unavailable.\r\n");
        return;
    }
    audio_log_kws_scores("[KWS] status:");
}

fsp_err_t audio_voice_play_test_silence(void)
{
    fsp_err_t err;

    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if ((AUDIO_VOICE_STATE_IDLE != g_audio_state) || (!g_ssie_idle))
    {
        return FSP_ERR_IN_USE;
    }

    audio_clear_capture();
    audio_clear_playback();
    audio_network_reset();
    g_tts_expected_sequence = 0U;
    g_tts_total_bytes = 0U;
    g_text_received = false;
    memset(g_capture_pcm, 0, sizeof(g_capture_pcm));

    for (uint32_t frame = 0U; frame < AUDIO_TEST_TONE_FRAMES; frame++)
    {
        if (!audio_play_queue_push((const uint8_t *) g_capture_pcm,
                                   AUDIO_PCM_BYTES_PER_FRAME))
        {
            audio_clear_playback();
            return FSP_ERR_OVERFLOW;
        }
    }

    g_server_done = true;
    deepseek_bridge_set_audio_active(true);
    err = audio_start_playback(
        "[AUDIO] digital-zero speaker noise test started (1.0 s).\r\n");
    return err;
}

fsp_err_t audio_voice_play_test_tone(void)
{
    static const int16_t tone_cycle[16] =
    {
        0, 3061, 5657, 7391, 8000, 7391, 5657, 3061,
        0, -3061, -5657, -7391, -8000, -7391, -5657, -3061
    };
    fsp_err_t err;

    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if ((AUDIO_VOICE_STATE_IDLE != g_audio_state) || (!g_ssie_idle))
    {
        return FSP_ERR_IN_USE;
    }

    audio_clear_capture();
    audio_clear_playback();
    audio_network_reset();
    g_tts_expected_sequence = 0U;
    g_tts_total_bytes = 0U;
    g_text_received = false;

    for (uint32_t i = 0U; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        g_capture_pcm[i] = tone_cycle[i % 16U];
    }

    for (uint32_t frame = 0U; frame < AUDIO_TEST_TONE_FRAMES; frame++)
    {
        if (!audio_play_queue_push((const uint8_t *) g_capture_pcm,
                                   AUDIO_PCM_BYTES_PER_FRAME))
        {
            audio_clear_playback();
            return FSP_ERR_OVERFLOW;
        }
    }

    g_server_done = true;
    deepseek_bridge_set_audio_active(true);
    err = audio_start_playback("[AUDIO] local 1 kHz test tone started (1.0 s).\r\n");
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    return FSP_SUCCESS;
}

fsp_err_t audio_voice_play_test_tone_8k(void)
{
    static const int16_t tone_cycle[16] =
    {
        0, 6123, 11314, 14782, 16000, 14782, 11314, 6123,
        0, -6123, -11314, -14782, -16000, -14782, -11314, -6123
    };
    fsp_err_t err;

    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if ((AUDIO_VOICE_STATE_IDLE != g_audio_state) || (!g_ssie_idle))
    {
        return FSP_ERR_IN_USE;
    }

    audio_clear_capture();
    audio_clear_playback();
    audio_network_reset();
    g_tts_expected_sequence = 0U;
    g_tts_total_bytes = 0U;
    g_text_received = false;

    err = audio_reconfigure_internal_clock(AUDIO_8K_MCLK_PERIOD_COUNTS);
    if (FSP_SUCCESS != err)
    {
        /*
         * Make a best-effort return to the generated 15.944 kHz clock before
         * giving control back to the rest of the application.
         */
        (void) audio_reconfigure_internal_clock(AUDIO_NORMAL_MCLK_PERIOD_COUNTS);
        return err;
    }

    for (uint32_t i = 0U; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        g_capture_pcm[i] = tone_cycle[i % 16U];
    }

    for (uint32_t frame = 0U; frame < AUDIO_TEST_TONE_FRAMES; frame++)
    {
        if (!audio_play_queue_push((const uint8_t *) g_capture_pcm,
                                   AUDIO_PCM_BYTES_PER_FRAME))
        {
            audio_clear_playback();
            (void) audio_reconfigure_internal_clock(AUDIO_NORMAL_MCLK_PERIOD_COUNTS);
            return FSP_ERR_OVERFLOW;
        }
    }

    g_server_done = true;
    g_internal_8k_test_active = true;
    deepseek_bridge_set_audio_active(true);
    err = audio_start_playback(
        "[AUDIO] internal-clock 8 kHz diagnostic: 500 Hz tone started (2.0 s).\r\n");
    if (FSP_SUCCESS != err)
    {
        g_internal_8k_test_active = false;
        (void) audio_reconfigure_internal_clock(AUDIO_NORMAL_MCLK_PERIOD_COUNTS);
        return err;
    }

    return FSP_SUCCESS;
}

fsp_err_t audio_voice_play_test_tone_32bit(void)
{
    fsp_err_t err;

    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if ((AUDIO_VOICE_STATE_IDLE != g_audio_state) || (!g_ssie_idle))
    {
        return FSP_ERR_IN_USE;
    }

    audio_clear_capture();
    audio_clear_playback();
    audio_network_reset();
    g_tts_expected_sequence = 0U;
    g_tts_total_bytes = 0U;
    g_text_received = false;

    err = audio_reconfigure_pcm_width(true);
    if (FSP_SUCCESS != err)
    {
        (void) audio_reconfigure_pcm_width(false);
        return err;
    }

    /* 16 samples per cycle at 15.944 kHz: about 996.5 Hz square wave. */
    for (uint32_t i = 0U; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        g_capture_pcm[i] = ((i % 16U) < 8U) ? 16000 : -16000;
    }

    for (uint32_t frame = 0U; frame < AUDIO_TEST_TONE_FRAMES; frame++)
    {
        if (!audio_play_queue_push((const uint8_t *) g_capture_pcm,
                                   AUDIO_PCM_BYTES_PER_FRAME))
        {
            audio_clear_playback();
            (void) audio_reconfigure_pcm_width(false);
            return FSP_ERR_OVERFLOW;
        }
    }

    g_server_done = true;
    g_internal_32bit_test_active = true;
    deepseek_bridge_set_audio_active(true);
    err = audio_start_playback(
        "[AUDIO] 32-bit PCM diagnostic: about 1 kHz square tone started (1.0 s).\r\n");
    if (FSP_SUCCESS != err)
    {
        g_internal_32bit_test_active = false;
        (void) audio_reconfigure_pcm_width(false);
        return err;
    }

    return FSP_SUCCESS;
}

fsp_err_t audio_voice_play_test_tone_left_justified(void)
{
    fsp_err_t err;

    if (!g_audio_initialized)
    {
        return FSP_ERR_NOT_OPEN;
    }

    if ((AUDIO_VOICE_STATE_IDLE != g_audio_state) || (!g_ssie_idle))
    {
        return FSP_ERR_IN_USE;
    }

    audio_clear_capture();
    audio_clear_playback();
    audio_network_reset();
    g_tts_expected_sequence = 0U;
    g_tts_total_bytes = 0U;
    g_text_received = false;

    err = audio_reconfigure_pcm_width(true);
    if (FSP_SUCCESS != err)
    {
        (void) audio_reconfigure_pcm_width(false);
        return err;
    }

    /*
     * The generated/FSP default is DEL=0: standard I2S with one BCLK of
     * delay after the LRCLK transition.  DEL=1 transmits the MSB on the
     * LRCLK transition for MAX98357B/left-justified compatibility testing.
     */
    g_i2s0_ctrl.p_reg->SSICR_b.DEL = 1U;

    for (uint32_t i = 0U; i < AUDIO_SAMPLES_PER_FRAME; i++)
    {
        g_capture_pcm[i] = ((i % 16U) < 8U) ? 16000 : -16000;
    }

    for (uint32_t frame = 0U; frame < AUDIO_TEST_TONE_FRAMES; frame++)
    {
        if (!audio_play_queue_push((const uint8_t *) g_capture_pcm,
                                   AUDIO_PCM_BYTES_PER_FRAME))
        {
            audio_clear_playback();
            (void) audio_reconfigure_pcm_width(false);
            return FSP_ERR_OVERFLOW;
        }
    }

    g_server_done = true;
    g_internal_left_justified_test_active = true;
    deepseek_bridge_set_audio_active(true);
    err = audio_start_playback(
        "[AUDIO] left-justified diagnostic: about 1 kHz square tone started (1.0 s).\r\n");
    if (FSP_SUCCESS != err)
    {
        g_internal_left_justified_test_active = false;
        (void) audio_reconfigure_pcm_width(false);
        return err;
    }

    return FSP_SUCCESS;
}

void audio_voice_abort(void)
{
    if (!g_audio_initialized)
    {
        return;
    }

    g_capture_stop_requested = true;
    g_capture_running = false;
    g_playback_stop_requested = true;
    g_playback_running = false;
    g_kws_enabled = false;
    g_kws_armed = false;
    audio_record_command_clear_events();
    g_kws_pending_action = AUDIO_KWS_PENDING_NONE;
    g_kws_fifo_samples = 0U;
    g_local_ack_playback = false;
    g_ack_starts_dialogue = false;
    g_ack_wait_start_ms = 0U;
    audio_finish_text_speech(AUDIO_VOICE_SPEECH_RESULT_FAILED);
    audio_stop_ssie();
    g_ssie_idle = true;
    audio_clear_capture();
    audio_clear_playback();
    audio_reset_vad();
    audio_network_reset();
    g_server_done = false;
    g_text_received = false;
    audio_health_session_reset(false);
    if (g_internal_8k_test_active)
    {
        g_internal_8k_test_active = false;
        (void) audio_reconfigure_internal_clock(AUDIO_NORMAL_MCLK_PERIOD_COUNTS);
    }
    if (g_internal_32bit_test_active)
    {
        g_internal_32bit_test_active = false;
        (void) audio_reconfigure_pcm_width(false);
    }
    if (g_internal_left_justified_test_active)
    {
        g_internal_left_justified_test_active = false;
        (void) audio_reconfigure_pcm_width(false);
    }
    deepseek_bridge_set_audio_active(false);
    deepseek_bridge_force_reconnect();
    g_audio_state = AUDIO_VOICE_STATE_IDLE;
    audio_log("[AUDIO] aborted for firmware update.\r\n");
}

static void audio_preempt_text_speech_for_dialogue(void)
{
    /* P000 health dialogue has priority over a medicine-name announcement. */
    audio_voice_cancel_text_speech();

    if (FSP_SUCCESS != audio_start_local_ack_playback(true))
    {
        audio_log("[AUDIO] P000 could not start after interrupting medicine speech.\r\n");
        deepseek_bridge_set_audio_active(false);
        audio_schedule_kws_cooldown();
    }
    else
    {
        audio_log("[AUDIO] P000 interrupted medicine speech; starting health dialogue.\r\n");
    }
}

void audio_voice_service_kws_capture(void)
{
    uint32_t drained = 0U;

    /*
     * W800 commands and OTA/status requests use synchronous AT waits.  They
     * call this narrow service hook while waiting so SSIE frames continue to
     * reach the Edge Impulse pipeline.  It deliberately handles neither the
     * button nor networking, which keeps it safe to call from inside a W800
     * transaction without recursively starting another transaction.
     */
    if ((!g_audio_initialized) ||
        (AUDIO_VOICE_STATE_KWS_LISTEN != g_audio_state) ||
        g_capture_overflow)
    {
        return;
    }

    while ((AUDIO_VOICE_STATE_KWS_LISTEN == g_audio_state) &&
           audio_capture_frame_available() &&
           (!g_capture_overflow) &&
           (drained < AUDIO_CAPTURE_BUFFER_COUNT))
    {
        audio_process_kws_frame();
        drained++;
    }
}

void audio_voice_service_realtime_inputs(void)
{
    if (!g_audio_initialized)
    {
        return;
    }

    /* Poll and debounce only. State changes and networking stay in main. */
    audio_button_poll();
}

void audio_voice_process(void)
{
    bool pressed;
    audio_record_command_t record_command = AUDIO_RECORD_COMMAND_NONE;

    if (!g_audio_initialized)
    {
        return;
    }

    audio_button_poll();
    pressed = audio_button_take_pressed_event();
    if (pressed)
    {
        if (g_text_speech_active)
        {
            audio_preempt_text_speech_for_dialogue();
            return;
        }
        if (AUDIO_VOICE_STATE_KWS_LISTEN == g_audio_state)
        {
            audio_request_kws_stop(AUDIO_KWS_PENDING_DIALOGUE);
            return;
        }
        if ((AUDIO_VOICE_STATE_IDLE == g_audio_state) ||
            (AUDIO_VOICE_STATE_KWS_COOLDOWN == g_audio_state))
        {
            if (FSP_SUCCESS != audio_start_local_ack_playback(true))
            {
                audio_log("[AUDIO] local reply could not start.\r\n");
                deepseek_bridge_set_audio_active(false);
                audio_schedule_kws_cooldown();
            }
            return;
        }
        if (AUDIO_VOICE_STATE_RECORDING == g_audio_state)
        {
            audio_request_capture_stop();
        }
    }

    if (g_kws_health_mode)
    {
        if (audio_record_command_take_overflow())
        {
            audio_log("[AUDIO] C1/C2 command queue overflowed; stale commands discarded.\r\n");
        }
        record_command = audio_record_command_take_event();
    }
    if (AUDIO_RECORD_COMMAND_START == record_command)
    {
        if (AUDIO_VOICE_STATE_KWS_LISTEN == g_audio_state)
        {
            g_kws_armed = false;
            g_kws_enabled = false;
            audio_log("[AUDIO] 0xC1 start accepted; KWS armed=0 for this Flag=2 entry.\r\n");
            audio_request_kws_stop(AUDIO_KWS_PENDING_DIALOGUE);
            return;
        }
        if ((AUDIO_VOICE_STATE_IDLE == g_audio_state) ||
            (AUDIO_VOICE_STATE_KWS_COOLDOWN == g_audio_state))
        {
            g_kws_armed = false;
            g_kws_enabled = false;
            audio_log("[AUDIO] 0xC1 start accepted; playing local reply before recording.\r\n");
            if (FSP_SUCCESS != audio_start_local_ack_playback(true))
            {
                audio_log("[AUDIO] 0xC1 local reply could not start.\r\n");
                deepseek_bridge_set_audio_active(false);
                audio_schedule_kws_cooldown();
            }
            return;
        }
        /* Repeated C1 while recording or any other busy state is ignored. */
    }
    else if (AUDIO_RECORD_COMMAND_STOP == record_command)
    {
        if (AUDIO_VOICE_STATE_RECORDING == g_audio_state)
        {
            audio_log("[AUDIO] 0xC2 stop accepted; finishing current 100 ms frame.\r\n");
            audio_request_capture_stop();
        }
        /* C2 never starts a dialogue and is ignored outside RECORDING. */
    }

    switch (g_audio_state)
    {
        case AUDIO_VOICE_STATE_KWS_LISTEN:
            if (g_capture_overflow)
            {
                g_kws_capture_overflows++;
                audio_log("[KWS] SSIE capture queue overflow; listener will restart.\r\n");
                audio_request_kws_stop(AUDIO_KWS_PENDING_NONE);
                break;
            }
            audio_voice_service_kws_capture();
            break;

        case AUDIO_VOICE_STATE_KWS_STOPPING:
            if ((!g_capture_running) && g_ssie_idle)
            {
                audio_kws_pending_action_t action = g_kws_pending_action;
                g_kws_pending_action = AUDIO_KWS_PENDING_NONE;
                audio_clear_capture();
                g_kws_fifo_samples = 0U;

                if (AUDIO_KWS_PENDING_DIALOGUE == action)
                {
                    if (FSP_SUCCESS != audio_start_local_ack_playback(true))
                    {
                        audio_log("[AUDIO] local reply could not start.\r\n");
                        deepseek_bridge_set_audio_active(false);
                        audio_schedule_kws_cooldown();
                    }
                }
                else if (AUDIO_KWS_PENDING_ACK == action)
                {
                    if (FSP_SUCCESS != audio_start_local_ack_playback(false))
                    {
                        audio_log("[KWS] local reply could not start.\r\n");
                        deepseek_bridge_set_audio_active(false);
                        audio_schedule_kws_cooldown();
                    }
                }
                else if (AUDIO_KWS_PENDING_TEXT_SPEECH == action)
                {
                    if (FSP_SUCCESS != audio_begin_text_speech())
                    {
                        audio_finish_text_speech(AUDIO_VOICE_SPEECH_RESULT_FAILED);
                        deepseek_bridge_set_audio_active(false);
                        audio_schedule_kws_cooldown();
                    }
                }
                else if (AUDIO_KWS_PENDING_DISABLE == action)
                {
                    if (g_kws_enabled)
                    {
                        audio_schedule_kws_cooldown();
                    }
                    else
                    {
                        g_audio_state = AUDIO_VOICE_STATE_IDLE;
                        audio_log("[KWS] listener disabled.\r\n");
                    }
                }
                else
                {
                    audio_schedule_kws_cooldown();
                }
            }
            break;

        case AUDIO_VOICE_STATE_ACK_WAIT:
            if (system_tick_elapsed(g_ack_wait_start_ms,
                                    AUDIO_DIALOGUE_ACK_DELAY_MS))
            {
                audio_log("[AUDIO] acknowledgement delay complete; preparing recording.\r\n");
                audio_begin_session();
            }
            break;

        case AUDIO_VOICE_STATE_KWS_COOLDOWN:
            if (!g_kws_enabled)
            {
                g_audio_state = AUDIO_VOICE_STATE_IDLE;
            }
            else if (system_tick_elapsed(g_kws_cooldown_start_ms,
                                         AUDIO_KWS_COOLDOWN_MS))
            {
                wake_word_reset();
                if (FSP_SUCCESS == audio_start_kws_capture())
                {
                    audio_log("[KWS] listening resumed.\r\n");
                }
                else
                {
                    g_kws_enabled = false;
                    g_audio_state = AUDIO_VOICE_STATE_IDLE;
                    audio_log("[KWS] SSIE listener restart failed; local wake disabled.\r\n");
                }
            }
            break;

        case AUDIO_VOICE_STATE_RECORDING:
            if (g_capture_overflow)
            {
                audio_recover("[AUDIO] capture queue overflowed.\r\n", true);
                break;
            }

            if (audio_capture_frame_available())
            {
                audio_process_capture_frame();
            }

            if (g_capture_stop_requested &&
                (!g_capture_running) &&
                (!audio_capture_frame_available()))
            {
                g_audio_state = AUDIO_VOICE_STATE_FINISHING;
            }
            break;

        case AUDIO_VOICE_STATE_FINISHING:
            if (FSP_SUCCESS != audio_send_end())
            {
                audio_recover("[AUDIO] failed to send AUD1 END.\r\n", true);
                break;
            }
            OLED_ShowDeepseekPage();
            g_health_page_opened = true;
            audio_log("[AUDIO] upload complete; waiting for ASR/TTS.\r\n");
            g_last_server_activity_ms = system_tick_now();
            g_audio_state = AUDIO_VOICE_STATE_WAIT_RESULT;
            break;

        case AUDIO_VOICE_STATE_WAIT_RESULT:
        {
            int result = audio_receive_server_message(AUDIO_NETWORK_POLL_MS);
            if (result < 0)
            {
                audio_recover("[AUDIO] invalid or failed server audio response.\r\n", true);
                break;
            }
            if (result > 0)
            {
                g_last_server_activity_ms = system_tick_now();
            }
            else if (system_tick_elapsed(g_last_server_activity_ms,
                                         AUDIO_SERVER_IDLE_TIMEOUT_MS))
            {
                audio_recover("[AUDIO] server audio response timed out.\r\n", true);
                break;
            }

            if ((g_play_queue_count >= AUDIO_PREBUFFER_FRAMES) ||
                (g_server_done && (g_play_queue_count > 0U)))
            {
                if (!g_ssie_idle)
                {
                    break;
                }
                (void) audio_start_playback("[AUDIO] TTS playback started.\r\n");
            }
            else if (g_server_done && (0U == g_play_queue_count))
            {
                audio_log(g_text_received ?
                          "[AUDIO] recognition returned no playable PCM.\r\n" :
                          "[AUDIO] empty recognition result.\r\n");
                audio_finish_text_speech(AUDIO_VOICE_SPEECH_RESULT_FAILED);
                g_health_dialogue_active = false;
                deepseek_bridge_set_audio_active(false);
                audio_schedule_kws_cooldown();
            }
            break;
        }

        case AUDIO_VOICE_STATE_PLAYBACK_STARTING:
            if (system_tick_elapsed(g_playback_clock_ready_ms,
                                    AUDIO_AMP_CLOCK_SETTLE_MS))
            {
                audio_amp_set_enabled(true);
                if (NULL != g_playback_start_message)
                {
                    audio_log(g_playback_start_message);
                }
                g_playback_start_message = NULL;
                g_audio_state = AUDIO_VOICE_STATE_PLAYBACK;
            }
            break;

        case AUDIO_VOICE_STATE_PLAYBACK:
            audio_prepare_tx_buffers();
            if ((!g_server_done) && (g_play_queue_count < AUDIO_PLAY_QUEUE_COUNT))
            {
                int result = audio_receive_server_message(AUDIO_NETWORK_POLL_MS);
                if (result < 0)
                {
                    audio_recover("[AUDIO] TTS stream failed.\r\n", true);
                    break;
                }
                if (result > 0)
                {
                    g_last_server_activity_ms = system_tick_now();
                }
                else if (system_tick_elapsed(g_last_server_activity_ms,
                                             AUDIO_SERVER_IDLE_TIMEOUT_MS))
                {
                    audio_recover("[AUDIO] TTS stream timed out.\r\n", true);
                    break;
                }
            }
            audio_prepare_tx_buffers();

            if (g_playback_stop_requested && g_ssie_idle)
            {
                char log_line[96];
                fsp_err_t restore_err = FSP_SUCCESS;

                /* audio_ssie_callback shut down the amp before stopping SSIE. */
                audio_amp_set_enabled(false);
                (void) snprintf(log_line,
                                sizeof(log_line),
                                "[AUDIO] playback complete (underruns=%lu).\r\n",
                                (unsigned long) g_playback_underruns);
                audio_log(log_line);
                g_playback_running = false;

                if (g_internal_8k_test_active)
                {
                    g_internal_8k_test_active = false;
                    restore_err =
                        audio_reconfigure_internal_clock(AUDIO_NORMAL_MCLK_PERIOD_COUNTS);
                    if (FSP_SUCCESS == restore_err)
                    {
                        audio_log(
                            "[AUDIO] restored normal internal clock (LRCLK about 15.944 kHz).\r\n");
                    }
                    else
                    {
                        audio_log_init_error("restore normal audio clock", restore_err);
                        g_audio_initialized = false;
                        g_audio_state = AUDIO_VOICE_STATE_DISABLED;
                    }
                }

                if ((FSP_SUCCESS == restore_err) && g_internal_32bit_test_active)
                {
                    g_internal_32bit_test_active = false;
                    restore_err = audio_reconfigure_pcm_width(false);
                    if (FSP_SUCCESS == restore_err)
                    {
                        audio_log(
                            "[AUDIO] restored generated 24-bit PCM configuration.\r\n");
                    }
                    else
                    {
                        audio_log_init_error("restore 24-bit PCM width", restore_err);
                        g_audio_initialized = false;
                        g_audio_state = AUDIO_VOICE_STATE_DISABLED;
                    }
                }

                if ((FSP_SUCCESS == restore_err) && g_internal_left_justified_test_active)
                {
                    g_internal_left_justified_test_active = false;
                    restore_err = audio_reconfigure_pcm_width(false);
                    if (FSP_SUCCESS == restore_err)
                    {
                        audio_log(
                            "[AUDIO] restored standard I2S / generated 24-bit configuration.\r\n");
                    }
                    else
                    {
                        audio_log_init_error("restore standard I2S format", restore_err);
                        g_audio_initialized = false;
                        g_audio_state = AUDIO_VOICE_STATE_DISABLED;
                    }
                }

                if (FSP_SUCCESS == restore_err)
                {
                    if (g_local_ack_playback && g_ack_starts_dialogue)
                    {
                        g_local_ack_playback = false;
                        g_ack_starts_dialogue = false;
                        g_ack_wait_start_ms = system_tick_now();
                        g_audio_state = AUDIO_VOICE_STATE_ACK_WAIT;
                        audio_log("[AUDIO] wo zai complete; waiting 500 ms before recording.\r\n");
                    }
                    else
                    {
                        g_health_dialogue_active = false;
                        audio_finish_text_speech(AUDIO_VOICE_SPEECH_RESULT_SUCCESS);
                        deepseek_bridge_set_audio_active(false);
                        audio_schedule_kws_cooldown();
                    }
                }
                else
                {
                    deepseek_bridge_set_audio_active(false);
                }
            }
            break;

        case AUDIO_VOICE_STATE_RECOVERING:
            if (g_ssie_idle)
            {
                audio_clear_capture();
                audio_clear_playback();
                audio_network_reset();
                deepseek_bridge_set_audio_active(false);
                audio_schedule_kws_cooldown();
                audio_log("[AUDIO] recovered; local listener will resume if enabled.\r\n");
            }
            break;

        case AUDIO_VOICE_STATE_DISABLED:
        case AUDIO_VOICE_STATE_IDLE:
        case AUDIO_VOICE_STATE_STARTING:
        default:
            break;
    }
}

void audio_ssie_callback(i2s_callback_args_t * p_args)
{
    if (NULL == p_args)
    {
        return;
    }

    if (I2S_EVENT_IDLE == p_args->event)
    {
        g_ssie_idle = true;
        g_capture_running = false;
        return;
    }

    if ((I2S_EVENT_RX_FULL == p_args->event) && g_capture_running)
    {
        uint8_t completed = g_capture_current;
        uint8_t next = (uint8_t) ((completed + 1U) % AUDIO_CAPTURE_BUFFER_COUNT);
        g_capture_ready[completed] = true;

        if (g_capture_stop_requested || g_capture_ready[next])
        {
            if (g_capture_ready[next] &&
                (!g_capture_stop_requested) &&
                g_kws_capture_mode)
            {
                /*
                 * Continuous KWS must follow real time.  Edge Impulse can
                 * occasionally take slightly longer than the 250 ms slice
                 * period, especially while another peripheral is active.
                 * Dropping the just-completed newest 100 ms frame is safe:
                 * the consumer can still finish every older buffer and the
                 * DTC never overwrites a buffer being read by the CPU.  A
                 * short discontinuity is preferable to stopping SSIE and
                 * resetting the entire MFE window every few seconds.
                 */
                g_capture_ready[completed] = false;
                g_kws_capture_dropped_frames++;
                if (FSP_SUCCESS != R_SSI_Read(&g_i2s0_ctrl,
                                              g_capture_raw[completed],
                                              AUDIO_RAW_BYTES_PER_FRAME))
                {
                    g_capture_overflow = true;
                    g_capture_running = false;
                    (void) R_SSI_Stop(&g_i2s0_ctrl);
                }
                return;
            }

            if (g_capture_ready[next] && (!g_capture_stop_requested))
            {
                g_capture_overflow = true;
            }
            g_capture_running = false;
            (void) R_SSI_Stop(&g_i2s0_ctrl);
            return;
        }

        g_capture_current = next;
        if (FSP_SUCCESS != R_SSI_Read(&g_i2s0_ctrl,
                                      g_capture_raw[next],
                                      AUDIO_RAW_BYTES_PER_FRAME))
        {
            g_capture_overflow = true;
            g_capture_running = false;
            (void) R_SSI_Stop(&g_i2s0_ctrl);
        }
        return;
    }

    if ((I2S_EVENT_TX_EMPTY == p_args->event) && g_playback_running)
    {
        int next = -1;

        if (g_tx_current >= 0)
        {
            g_tx_ready[(uint8_t) g_tx_current] = false;
            g_tx_current = -1;
        }

        for (uint8_t i = 0U; i < AUDIO_TX_BUFFER_COUNT; i++)
        {
            if (g_tx_ready[i])
            {
                next = (int) i;
                break;
            }
        }

        if (next >= 0)
        {
            g_tx_current = (int8_t) next;
            if (FSP_SUCCESS != R_SSI_Write(&g_i2s0_ctrl,
                                           g_tx_raw[next],
                                           AUDIO_RAW_BYTES_PER_FRAME))
            {
                g_playback_stop_requested = true;
                audio_stop_ssie();
            }
            return;
        }

        if (g_server_done && (0U == g_play_queue_count))
        {
            if (!g_suffix_silence_inflight)
            {
                g_suffix_silence_inflight = true;
                if (FSP_SUCCESS != R_SSI_Write(&g_i2s0_ctrl,
                                               g_tx_silence,
                                               AUDIO_RAW_BYTES_PER_FRAME))
                {
                    g_playback_stop_requested = true;
                    audio_stop_ssie();
                }
            }
            else
            {
                g_playback_stop_requested = true;
                audio_stop_ssie();
            }
            return;
        }

        g_playback_underruns++;
        if (FSP_SUCCESS != R_SSI_Write(&g_i2s0_ctrl,
                                       g_tx_silence,
                                       AUDIO_RAW_BYTES_PER_FRAME))
        {
            g_playback_stop_requested = true;
            audio_stop_ssie();
        }
    }
}
