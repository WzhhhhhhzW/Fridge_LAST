#ifndef AUDIO_VOICE_H_
#define AUDIO_VOICE_H_

#include "hal_data.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum e_audio_voice_speech_result
{
    AUDIO_VOICE_SPEECH_RESULT_NONE = 0,
    AUDIO_VOICE_SPEECH_RESULT_SUCCESS,
    AUDIO_VOICE_SPEECH_RESULT_FAILED,
} audio_voice_speech_result_t;

fsp_err_t audio_voice_init(void);
void audio_voice_process(void);
void audio_voice_service_realtime_inputs(void);
void audio_voice_service_kws_capture(void);
void audio_voice_submit_start_command(void);
void audio_voice_submit_stop_command(void);
bool audio_voice_busy(void);
void audio_voice_abort(void);
fsp_err_t audio_voice_play_ack(void);
fsp_err_t audio_voice_speak_text(const char * utf8_text);
void audio_voice_cancel_text_speech(void);
bool audio_voice_text_speech_active(void);
audio_voice_speech_result_t audio_voice_take_speech_result(void);
fsp_err_t audio_voice_kws_set_enabled(bool enabled);
fsp_err_t audio_voice_set_health_mode(bool active);
bool audio_voice_kws_enabled(void);
void audio_voice_kws_log_status(void);
fsp_err_t audio_voice_play_test_tone(void);
fsp_err_t audio_voice_play_test_silence(void);
fsp_err_t audio_voice_play_test_tone_8k(void);
fsp_err_t audio_voice_play_test_tone_32bit(void);
fsp_err_t audio_voice_play_test_tone_left_justified(void);
void audio_ssie_callback(i2s_callback_args_t * p_args);

#endif /* AUDIO_VOICE_H_ */
