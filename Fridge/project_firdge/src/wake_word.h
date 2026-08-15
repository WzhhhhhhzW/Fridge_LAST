#ifndef WAKE_WORD_H_
#define WAKE_WORD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WAKE_WORD_SAMPLE_RATE_HZ       (16000U)
#define WAKE_WORD_SLICE_SAMPLES        (4000U)
#define WAKE_WORD_THRESHOLD_PERMILLE   (650U)
#define WAKE_WORD_REQUIRED_HITS        (2U)
#define WAKE_WORD_WARMUP_SLICES        (4U)

typedef struct st_wake_word_status
{
    bool initialized;
    uint16_t wake_permille;
    uint16_t noise_permille;
    uint16_t unknown_permille;
    uint8_t consecutive_hits;
    uint32_t slices_processed;
    int32_t last_error;
} wake_word_status_t;

bool wake_word_init(void);
void wake_word_reset(void);
bool wake_word_process(const int16_t * p_pcm, size_t samples);
float wake_word_last_score(void);
void wake_word_get_status(wake_word_status_t * p_status);

#ifdef __cplusplus
}
#endif

#endif /* WAKE_WORD_H_ */
