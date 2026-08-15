#include "wake_word.h"

#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

#include <cstring>
#include <new>

namespace
{
const char * const kWakeLabel = "nihao_xiaou";
const char * const kNoiseLabel = "noise";
const char * const kUnknownLabel = "unknown";

const int16_t * g_signal_pcm = nullptr;
wake_word_status_t g_status = {};

uint16_t score_to_permille(float value)
{
    if (value <= 0.0f)
    {
        return 0U;
    }
    if (value >= 1.0f)
    {
        return 1000U;
    }
    return static_cast<uint16_t>((value * 1000.0f) + 0.5f);
}

int signal_get_data(size_t offset, size_t length, float * p_out)
{
    if ((nullptr == g_signal_pcm) || (nullptr == p_out) ||
        ((offset + length) > WAKE_WORD_SLICE_SAMPLES))
    {
        return EIDSP_PARAMETER_INVALID;
    }

    for (size_t i = 0U; i < length; i++)
    {
        p_out[i] = static_cast<float>(g_signal_pcm[offset + i]);
    }
    return EIDSP_OK;
}

bool labels_are_present(void)
{
    bool wake_found = false;
    bool noise_found = false;
    bool unknown_found = false;

    for (size_t i = 0U; i < EI_CLASSIFIER_LABEL_COUNT; i++)
    {
        const char * label = ei_classifier_inferencing_categories[i];
        wake_found |= (0 == std::strcmp(label, kWakeLabel));
        noise_found |= (0 == std::strcmp(label, kNoiseLabel));
        unknown_found |= (0 == std::strcmp(label, kUnknownLabel));
    }

    return wake_found && noise_found && unknown_found;
}
} // namespace

extern "C" bool wake_word_init(void)
{
    std::memset(&g_status, 0, sizeof(g_status));

    if ((EI_CLASSIFIER_FREQUENCY != WAKE_WORD_SAMPLE_RATE_HZ) ||
        (EI_CLASSIFIER_SLICE_SIZE != WAKE_WORD_SLICE_SAMPLES) ||
        (EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW != WAKE_WORD_WARMUP_SLICES) ||
        (!labels_are_present()))
    {
        g_status.last_error = -1;
        return false;
    }

    run_classifier_init();
    g_status.initialized = true;
    return true;
}

extern "C" void wake_word_reset(void)
{
    if (!g_status.initialized)
    {
        return;
    }

    run_classifier_deinit();
    run_classifier_init();
    g_status.wake_permille = 0U;
    g_status.noise_permille = 0U;
    g_status.unknown_permille = 0U;
    g_status.consecutive_hits = 0U;
    g_status.slices_processed = 0U;
    g_status.last_error = 0;
}

extern "C" bool wake_word_process(const int16_t * p_pcm, size_t samples)
{
    ei::signal_t signal;
    ei_impulse_result_t result = {};
    EI_IMPULSE_ERROR error;

    if ((!g_status.initialized) || (nullptr == p_pcm) ||
        (WAKE_WORD_SLICE_SAMPLES != samples))
    {
        g_status.last_error = -2;
        return false;
    }

    g_signal_pcm = p_pcm;
    signal.total_length = samples;
    signal.get_data = signal_get_data;
    try
    {
        error = run_classifier_continuous(&signal, &result, false, false);
    }
    catch (const std::bad_alloc &)
    {
        g_signal_pcm = nullptr;
        g_status.last_error = -3;
        g_status.consecutive_hits = 0U;
        return false;
    }
    catch (...)
    {
        g_signal_pcm = nullptr;
        g_status.last_error = -4;
        g_status.consecutive_hits = 0U;
        return false;
    }
    g_signal_pcm = nullptr;

    if (EI_IMPULSE_OK != error)
    {
        g_status.last_error = static_cast<int32_t>(error);
        g_status.consecutive_hits = 0U;
        return false;
    }

    g_status.last_error = 0;
    g_status.slices_processed++;
    for (size_t i = 0U; i < EI_CLASSIFIER_LABEL_COUNT; i++)
    {
        const char * label = result.classification[i].label;
        uint16_t score = score_to_permille(result.classification[i].value);

        if (0 == std::strcmp(label, kWakeLabel))
        {
            g_status.wake_permille = score;
        }
        else if (0 == std::strcmp(label, kNoiseLabel))
        {
            g_status.noise_permille = score;
        }
        else if (0 == std::strcmp(label, kUnknownLabel))
        {
            g_status.unknown_permille = score;
        }
    }

    if (g_status.slices_processed < WAKE_WORD_WARMUP_SLICES)
    {
        g_status.consecutive_hits = 0U;
        return false;
    }

    if (g_status.wake_permille >= WAKE_WORD_THRESHOLD_PERMILLE)
    {
        if (g_status.consecutive_hits < WAKE_WORD_REQUIRED_HITS)
        {
            g_status.consecutive_hits++;
        }
    }
    else
    {
        g_status.consecutive_hits = 0U;
    }

    return (g_status.consecutive_hits >= WAKE_WORD_REQUIRED_HITS);
}

extern "C" float wake_word_last_score(void)
{
    return static_cast<float>(g_status.wake_permille) / 1000.0f;
}

extern "C" void wake_word_get_status(wake_word_status_t * p_status)
{
    if (nullptr != p_status)
    {
        *p_status = g_status;
    }
}
