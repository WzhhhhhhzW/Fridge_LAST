#include "LM35.h"
#include "system_tick.h"

/*
 * The public lm35_* API names are retained for compatibility with the rest of
 * the project. The active analog temperature sensor is selected in LM35.h.
 */
#define TEMPERATURE_ADC_MAX_COUNTS               (4095.0f)
#define TEMPERATURE_ADC_REF_VOLTAGE              (3.3f)

#define LM35_MIN_VALID_OUTPUT_VOLTAGE             (0.0f)
#define LM35_MAX_VALID_OUTPUT_VOLTAGE             (1.50f)
#define LM35_GAIN_V_PER_C                         (0.0100f)
#define LM35_CALIBRATION_OFFSET_C                 (0.0f)

#define TMP235_MIN_VALID_OUTPUT_VOLTAGE           (0.05f)
#define TMP235_MAX_VALID_OUTPUT_VOLTAGE           (2.10f)
#define TMP235_CALIBRATION_OFFSET_C               (0.0f)
#define TMP235_LOW_RANGE_OFFSET_VOLTAGE           (0.5000f)
#define TMP235_LOW_RANGE_GAIN_V_PER_C             (0.0100f)
#define TMP235_MID_RANGE_START_C                  (100.0f)
#define TMP235_MID_RANGE_OFFSET_VOLTAGE           (1.5000f)
#define TMP235_MID_RANGE_GAIN_V_PER_C             (0.0101f)
#define TMP235_HIGH_RANGE_START_C                 (125.0f)
#define TMP235_HIGH_RANGE_OFFSET_VOLTAGE          (1.7525f)
#define TMP235_HIGH_RANGE_GAIN_V_PER_C            (0.0106f)

#define TEMPERATURE_SAMPLE_BATCH_SIZE            (10U)
#define TEMPERATURE_TRIMMED_SAMPLE_COUNT         (TEMPERATURE_SAMPLE_BATCH_SIZE - 2U)
#define TEMPERATURE_SAMPLE_INTERVAL_MS           (100U)
#define TEMPERATURE_CONVERSION_WAIT_MS           (1U)

#if TEMPERATURE_SENSOR_ACTIVE == TEMPERATURE_SENSOR_LM35
 #define TEMPERATURE_MIN_VALID_OUTPUT_VOLTAGE    LM35_MIN_VALID_OUTPUT_VOLTAGE
 #define TEMPERATURE_MAX_VALID_OUTPUT_VOLTAGE    LM35_MAX_VALID_OUTPUT_VOLTAGE
 #define TEMPERATURE_CALIBRATION_OFFSET_C        LM35_CALIBRATION_OFFSET_C
#elif TEMPERATURE_SENSOR_ACTIVE == TEMPERATURE_SENSOR_TMP235
 #define TEMPERATURE_MIN_VALID_OUTPUT_VOLTAGE    TMP235_MIN_VALID_OUTPUT_VOLTAGE
 #define TEMPERATURE_MAX_VALID_OUTPUT_VOLTAGE    TMP235_MAX_VALID_OUTPUT_VOLTAGE
 #define TEMPERATURE_CALIBRATION_OFFSET_C        TMP235_CALIBRATION_OFFSET_C
#else
 #error "TEMPERATURE_SENSOR_ACTIVE must be TEMPERATURE_SENSOR_LM35 or TEMPERATURE_SENSOR_TMP235"
#endif

typedef enum e_temperature_state
{
    TEMPERATURE_STATE_DISABLED = 0,
    TEMPERATURE_STATE_IDLE,
    TEMPERATURE_STATE_CONVERTING,
} temperature_state_t;

static uint16_t g_temperature_adc_data;
static float g_temperature_raw_samples[TEMPERATURE_SAMPLE_BATCH_SIZE];
static uint8_t g_temperature_sample_count = 0U;
static float g_temperature_last_raw_average = 0.0f;
static bool g_temperature_has_sample = false;
static temperature_state_t g_temperature_state = TEMPERATURE_STATE_DISABLED;
static uint32_t g_temperature_last_start_ms = 0U;

static float temperature_voltage_to_raw_temperature(float voltage)
{
#if TEMPERATURE_SENSOR_ACTIVE == TEMPERATURE_SENSOR_LM35
    return voltage / LM35_GAIN_V_PER_C;
#else
    float temperature_c;

    if (voltage < TMP235_MID_RANGE_OFFSET_VOLTAGE)
    {
        temperature_c = (voltage - TMP235_LOW_RANGE_OFFSET_VOLTAGE) /
                        TMP235_LOW_RANGE_GAIN_V_PER_C;
    }
    else if (voltage <= TMP235_HIGH_RANGE_OFFSET_VOLTAGE)
    {
        temperature_c = TMP235_MID_RANGE_START_C +
                        ((voltage - TMP235_MID_RANGE_OFFSET_VOLTAGE) /
                         TMP235_MID_RANGE_GAIN_V_PER_C);
    }
    else
    {
        temperature_c = TMP235_HIGH_RANGE_START_C +
                        ((voltage - TMP235_HIGH_RANGE_OFFSET_VOLTAGE) /
                         TMP235_HIGH_RANGE_GAIN_V_PER_C);
    }

    return temperature_c;
#endif
}

static float temperature_calculate_trimmed_average(void)
{
    float sum = 0.0f;
    float min_temperature = g_temperature_raw_samples[0];
    float max_temperature = g_temperature_raw_samples[0];

    for (uint8_t i = 0U; i < TEMPERATURE_SAMPLE_BATCH_SIZE; i++)
    {
        float sample = g_temperature_raw_samples[i];

        sum += sample;
        if (sample < min_temperature)
        {
            min_temperature = sample;
        }
        if (sample > max_temperature)
        {
            max_temperature = sample;
        }
    }

    return (sum - max_temperature - min_temperature) / (float) TEMPERATURE_TRIMMED_SAMPLE_COUNT;
}

static void temperature_accept_adc_sample(uint16_t adc_data)
{
    float voltage = ((float) adc_data / TEMPERATURE_ADC_MAX_COUNTS) * TEMPERATURE_ADC_REF_VOLTAGE;

    if ((voltage < TEMPERATURE_MIN_VALID_OUTPUT_VOLTAGE) ||
        (voltage > TEMPERATURE_MAX_VALID_OUTPUT_VOLTAGE))
    {
        return;
    }

    float raw_temperature = temperature_voltage_to_raw_temperature(voltage);
    g_temperature_raw_samples[g_temperature_sample_count++] = raw_temperature;

    if (!g_temperature_has_sample)
    {
        g_temperature_last_raw_average = raw_temperature;
        g_temperature_has_sample = true;
    }

    if (g_temperature_sample_count >= TEMPERATURE_SAMPLE_BATCH_SIZE)
    {
        g_temperature_last_raw_average = temperature_calculate_trimmed_average();
        g_temperature_sample_count = 0U;
    }
}

fsp_err_t lm35_init(void)
{
    fsp_err_t err = R_ADC_Open(&degree_ctrl, &degree_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    err = R_ADC_ScanCfg(&degree_ctrl, &degree_channel_cfg);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    g_temperature_state = TEMPERATURE_STATE_IDLE;
    g_temperature_last_start_ms = system_tick_now() - TEMPERATURE_SAMPLE_INTERVAL_MS;
    return FSP_SUCCESS;
}

void lm35_process(void)
{
    uint32_t now;
    fsp_err_t err;

    if (TEMPERATURE_STATE_DISABLED == g_temperature_state)
    {
        return;
    }

    now = system_tick_now();
    if (TEMPERATURE_STATE_IDLE == g_temperature_state)
    {
        if (!system_tick_elapsed(g_temperature_last_start_ms, TEMPERATURE_SAMPLE_INTERVAL_MS))
        {
            return;
        }

        err = R_ADC_ScanStart(&degree_ctrl);
        g_temperature_last_start_ms = now;
        if (FSP_SUCCESS == err)
        {
            g_temperature_state = TEMPERATURE_STATE_CONVERTING;
        }
        return;
    }

    if ((TEMPERATURE_STATE_CONVERTING == g_temperature_state) &&
        system_tick_elapsed(g_temperature_last_start_ms, TEMPERATURE_CONVERSION_WAIT_MS))
    {
        err = R_ADC_Read(&degree_ctrl, ADC_CHANNEL_10, &g_temperature_adc_data);
        g_temperature_state = TEMPERATURE_STATE_IDLE;
        if (FSP_SUCCESS == err)
        {
            temperature_accept_adc_sample(g_temperature_adc_data);
        }
    }
}

fsp_err_t lm35_read_temperature(float * temperature_c)
{
    if (NULL == temperature_c)
    {
        return FSP_ERR_ASSERTION;
    }
    if (!g_temperature_has_sample)
    {
        return FSP_ERR_NOT_INITIALIZED;
    }

    *temperature_c = g_temperature_last_raw_average + TEMPERATURE_CALIBRATION_OFFSET_C;
    return FSP_SUCCESS;
}

fsp_err_t lm35_read_raw_temperature(float * temperature_c)
{
    if (NULL == temperature_c)
    {
        return FSP_ERR_ASSERTION;
    }
    if (!g_temperature_has_sample)
    {
        return FSP_ERR_NOT_INITIALIZED;
    }

    *temperature_c = g_temperature_last_raw_average;
    return FSP_SUCCESS;
}

const char * lm35_sensor_name(void)
{
#if TEMPERATURE_SENSOR_ACTIVE == TEMPERATURE_SENSOR_LM35
    return "LM35";
#else
    return "TMP235";
#endif
}
