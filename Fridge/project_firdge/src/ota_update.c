#include "ota_update.h"

#include "audio_voice.h"
#include "deepseek_bridge.h"
#include "ota_boot_record.h"
#include "ota_sha256.h"
#include "ota_update_config.h"
#include "variable.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OTA_LINE_BYTES             (192U)
#define OTA_STREAM_BUFFER_BYTES    (OTA_UPDATE_CHUNK_BYTES + OTA_LINE_BYTES)
#define OTA_SHA256_HEX_BYTES       (64U)
#define OTA_FLASH_WRITE_BYTES      (128U)
#define OTA_NETWORK_POLL_MS        (50U)
#define OTA_BANK_COUNT             (2U)

#if OTA_UPDATE_FSP_READY

typedef struct st_ota_offer
{
    char deployment_id[OTA_BOOT_RECORD_DEPLOYMENT_ID_MAX];
    char version[OTA_BOOT_RECORD_VERSION_MAX];
    uint32_t size;
    uint8_t sha256[32];
} ota_offer_t;

typedef struct st_ota_stream
{
    uint8_t data[OTA_STREAM_BUFFER_BYTES];
    uint32_t cursor;
    uint32_t length;
} ota_stream_t;

static flash_ctrl_t * const gp_ota_flash_ctrl = &g_ota_flash_ctrl;
static flash_cfg_t const * const gp_ota_flash_cfg = &g_ota_flash_cfg;
static wdt_ctrl_t * const gp_ota_iwdt_ctrl = &g_iwdt0_ctrl;
static wdt_cfg_t const * const gp_ota_iwdt_cfg = &g_iwdt0_cfg;

static uint8_t g_ota_chunk[OTA_UPDATE_CHUNK_BYTES + OTA_FLASH_WRITE_BYTES]
    BSP_ALIGN_VARIABLE(OTA_FLASH_WRITE_BYTES);
static ota_stream_t g_ota_stream;
static ota_boot_record_t g_ota_record;
static bool g_ota_record_valid = false;
static bool g_ota_exclusive = false;
static bool g_ota_flash_open = false;
static bool g_ota_iwdt_open = false;
static bool g_ota_configured = false;
static bool g_ota_result_pending = false;
static uint32_t g_ota_last_check_ms = 0U;
static uint32_t g_ota_trial_start_ms = 0U;
static uint32_t g_ota_record_slot_address = OTA_UPDATE_METADATA_SLOT0_ADDRESS;

static uint32_t ota_crc32(const uint8_t * p_data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;

    for (size_t i = 0U; i < length; i++)
    {
        crc ^= p_data[i];
        for (uint32_t bit = 0U; bit < 8U; bit++)
        {
            uint32_t mask = (uint32_t) (-(int32_t) (crc & 1U));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }

    return ~crc;
}

static bool ota_record_is_valid(const ota_boot_record_t * p_record)
{
    uint32_t expected_crc;

    if ((NULL == p_record) ||
        (OTA_BOOT_RECORD_MAGIC != p_record->magic) ||
        (OTA_BOOT_RECORD_SCHEMA != p_record->schema) ||
        (p_record->state > OTA_BOOT_STATE_ROLLBACK) ||
        (p_record->target_bank >= OTA_BANK_COUNT))
    {
        return false;
    }

    expected_crc = ota_crc32((const uint8_t *) p_record,
                             offsetof(ota_boot_record_t, crc32));
    return (expected_crc == p_record->crc32);
}

static void ota_record_load(void)
{
    const ota_boot_record_t * p_slot0 =
        (const ota_boot_record_t *) OTA_UPDATE_METADATA_SLOT0_ADDRESS;
    const ota_boot_record_t * p_slot1 =
        (const ota_boot_record_t *) OTA_UPDATE_METADATA_SLOT1_ADDRESS;
    bool slot0_valid = ota_record_is_valid(p_slot0);
    bool slot1_valid = ota_record_is_valid(p_slot1);

    memset(&g_ota_record, 0, sizeof(g_ota_record));
    g_ota_record_valid = false;
    g_ota_record_slot_address = OTA_UPDATE_METADATA_SLOT0_ADDRESS;

    if (slot0_valid && slot1_valid)
    {
        if ((int32_t) (p_slot1->generation - p_slot0->generation) > 0)
        {
            memcpy(&g_ota_record, p_slot1, sizeof(g_ota_record));
            g_ota_record_slot_address = OTA_UPDATE_METADATA_SLOT1_ADDRESS;
        }
        else
        {
            memcpy(&g_ota_record, p_slot0, sizeof(g_ota_record));
        }
        g_ota_record_valid = true;
    }
    else if (slot0_valid)
    {
        memcpy(&g_ota_record, p_slot0, sizeof(g_ota_record));
        g_ota_record_valid = true;
    }
    else if (slot1_valid)
    {
        memcpy(&g_ota_record, p_slot1, sizeof(g_ota_record));
        g_ota_record_slot_address = OTA_UPDATE_METADATA_SLOT1_ADDRESS;
        g_ota_record_valid = true;
    }
}

void ota_update_watchdog_refresh(void)
{
    if (!g_ota_iwdt_open)
    {
        fsp_err_t open_error = R_IWDT_Open(gp_ota_iwdt_ctrl, gp_ota_iwdt_cfg);
        if ((FSP_SUCCESS == open_error) || (FSP_ERR_ALREADY_OPEN == open_error))
        {
            g_ota_iwdt_open = true;
        }
    }

    if (g_ota_iwdt_open)
    {
        (void) R_IWDT_Refresh(gp_ota_iwdt_ctrl);
    }
}

static bool ota_device_token_configured(void)
{
    return ('\0' != OTA_UPDATE_DEVICE_TOKEN[0]) &&
           (0 != strcmp(OTA_UPDATE_DEVICE_TOKEN, "replace-with-fridge-ota-token"));
}

static fsp_err_t ota_record_save(void)
{
    uint32_t next_slot;
    fsp_err_t flash_error;

    if (!g_ota_flash_open)
    {
        return FSP_ERR_NOT_OPEN;
    }

    next_slot = (OTA_UPDATE_METADATA_SLOT0_ADDRESS == g_ota_record_slot_address) ?
                OTA_UPDATE_METADATA_SLOT1_ADDRESS :
                OTA_UPDATE_METADATA_SLOT0_ADDRESS;
    g_ota_record.magic = OTA_BOOT_RECORD_MAGIC;
    g_ota_record.schema = OTA_BOOT_RECORD_SCHEMA;
    g_ota_record.generation++;
    g_ota_record.crc32 = ota_crc32((const uint8_t *) &g_ota_record,
                                   offsetof(ota_boot_record_t, crc32));

    ota_update_watchdog_refresh();
    flash_error = R_FLASH_HP_Erase(gp_ota_flash_ctrl,
                                   next_slot,
                                   OTA_UPDATE_METADATA_ERASE_BLOCKS);
    if (FSP_SUCCESS != flash_error)
    {
        return flash_error;
    }

    ota_update_watchdog_refresh();
    flash_error = R_FLASH_HP_Write(gp_ota_flash_ctrl,
                                   (uint32_t) &g_ota_record,
                                   next_slot,
                                   sizeof(g_ota_record));
    if (FSP_SUCCESS == flash_error)
    {
        g_ota_record_slot_address = next_slot;
        g_ota_record_valid = true;
    }
    return flash_error;
}

static uint8_t ota_current_bank(void)
{
    if (g_ota_record_valid &&
        ((OTA_BOOT_STATE_PENDING_TRIAL == g_ota_record.state) ||
         (OTA_BOOT_STATE_CONFIRMED == g_ota_record.state)))
    {
        return g_ota_record.target_bank;
    }
    if (g_ota_record_valid && (OTA_BOOT_STATE_ROLLBACK == g_ota_record.state))
    {
        return (uint8_t) (g_ota_record.target_bank ^ 1U);
    }
    return OTA_UPDATE_INITIAL_ACTIVE_BANK;
}

static bool ota_hex_to_sha256(const char * p_hex, uint8_t digest[32])
{
    if ((NULL == p_hex) || (strlen(p_hex) != OTA_SHA256_HEX_BYTES))
    {
        return false;
    }

    for (uint32_t i = 0U; i < 32U; i++)
    {
        char pair[3] = {p_hex[i * 2U], p_hex[i * 2U + 1U], '\0'};
        char * p_end = NULL;
        unsigned long value = strtoul(pair, &p_end, 16);
        if ((NULL == p_end) || ('\0' != *p_end) || (value > 0xFFUL))
        {
            return false;
        }
        digest[i] = (uint8_t) value;
    }
    return true;
}

static bool ota_sha256_equal(const uint8_t left[32], const uint8_t right[32])
{
    uint8_t difference = 0U;

    for (uint32_t i = 0U; i < 32U; i++)
    {
        difference |= (uint8_t) (left[i] ^ right[i]);
    }
    return (0U == difference);
}

static fsp_err_t ota_send_line(const char * p_line)
{
    uint8_t newline = (uint8_t) '\n';
    deepseek_bridge_iovec_t vectors[2];

    if (NULL == p_line)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    vectors[0].p_data = (const uint8_t *) p_line;
    vectors[0].length = (uint32_t) strlen(p_line);
    vectors[1].p_data = &newline;
    vectors[1].length = 1U;
    return deepseek_bridge_socket_sendv_all(vectors, 2U);
}

static void ota_send_progress(const ota_offer_t * p_offer,
                              uint32_t received,
                              const char * p_state)
{
    char line[OTA_LINE_BYTES];
    int length;

    length = snprintf(line,
                      sizeof(line),
                      "FW1 PROGRESS %s %lu %s %s",
                      p_offer->deployment_id,
                      (unsigned long) received,
                      p_state,
                      OTA_UPDATE_DEVICE_TOKEN);
    if ((length > 0) && ((size_t) length < sizeof(line)))
    {
        (void) ota_send_line(line);
    }
}

static fsp_err_t ota_send_result(const char * p_deployment_id,
                                 const char * p_version,
                                 const char * p_result,
                                 const char * p_error)
{
    char line[OTA_LINE_BYTES];
    int length;

    length = snprintf(line,
                      sizeof(line),
                      "FW1 RESULT %s %s %u %s %s %s",
                      p_deployment_id,
                      p_version,
                      (unsigned int) ota_current_bank(),
                      p_result,
                      p_error,
                      OTA_UPDATE_DEVICE_TOKEN);
    if ((length > 0) && ((size_t) length < sizeof(line)))
    {
        return ota_send_line(line);
    }
    return FSP_ERR_INVALID_SIZE;
}

static fsp_err_t ota_send_saved_result(void)
{
    const char * p_result;
    const char * p_error;

    if ((!g_ota_record_valid) || ('\0' == g_ota_record.deployment_id[0]))
    {
        return FSP_ERR_NOT_FOUND;
    }

    if (OTA_BOOT_STATE_CONFIRMED == g_ota_record.state)
    {
        p_result = "success";
        p_error = "-";
    }
    else if (OTA_BOOT_STATE_ROLLBACK == g_ota_record.state)
    {
        p_result = "rolled_back";
        p_error = (g_ota_record.trial_boots >= OTA_UPDATE_MAX_TRIAL_BOOTS) ?
                  "trial_timeout" :
                  "bootloader_rejected";
    }
    else
    {
        return FSP_ERR_INVALID_STATE;
    }

    return ota_send_result(g_ota_record.deployment_id,
                           OTA_UPDATE_APP_VERSION,
                           p_result,
                           p_error);
}

static void ota_result_delivered(void)
{
    char saved_deployment_id[OTA_BOOT_RECORD_DEPLOYMENT_ID_MAX];

    (void) snprintf(saved_deployment_id,
                    sizeof(saved_deployment_id),
                    "%s",
                    g_ota_record.deployment_id);
    memset(g_ota_record.deployment_id, 0, sizeof(g_ota_record.deployment_id));
    if (FSP_SUCCESS == ota_record_save())
    {
        g_ota_result_pending = false;
        return;
    }

    (void) snprintf(g_ota_record.deployment_id,
                    sizeof(g_ota_record.deployment_id),
                    "%s",
                    saved_deployment_id);
}

static int32_t ota_stream_fill(ota_stream_t * p_stream, uint32_t deadline_start)
{
    while (!system_tick_elapsed(deadline_start, OTA_UPDATE_NETWORK_TIMEOUT_MS))
    {
        int32_t received;

        ota_update_watchdog_refresh();
        received = deepseek_bridge_socket_receive(p_stream->data,
                                                  sizeof(p_stream->data),
                                                  OTA_NETWORK_POLL_MS);
        if (received > 0)
        {
            p_stream->cursor = 0U;
            p_stream->length = (uint32_t) received;
            return received;
        }
        if (received < 0)
        {
            return -1;
        }
        R_BSP_SoftwareDelay(OTA_NETWORK_POLL_MS, BSP_DELAY_UNITS_MILLISECONDS);
    }
    return 0;
}

static fsp_err_t ota_stream_read_byte(ota_stream_t * p_stream,
                                      uint8_t * p_byte,
                                      uint32_t deadline_start)
{
    if ((NULL == p_stream) || (NULL == p_byte))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    if (p_stream->cursor >= p_stream->length)
    {
        int32_t received = ota_stream_fill(p_stream, deadline_start);
        if (received < 0)
        {
            return FSP_ERR_ABORTED;
        }
        if (0 == received)
        {
            return FSP_ERR_TIMEOUT;
        }
    }

    *p_byte = p_stream->data[p_stream->cursor++];
    return FSP_SUCCESS;
}

static fsp_err_t ota_stream_read_line(ota_stream_t * p_stream,
                                      char * p_line,
                                      size_t line_size)
{
    uint32_t start_ms = system_tick_now();
    size_t length = 0U;

    if ((NULL == p_line) || (line_size < 2U))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    while (!system_tick_elapsed(start_ms, OTA_UPDATE_NETWORK_TIMEOUT_MS))
    {
        uint8_t byte;
        fsp_err_t read_error = ota_stream_read_byte(p_stream, &byte, start_ms);
        if (FSP_SUCCESS != read_error)
        {
            return read_error;
        }
        if ('\n' == (char) byte)
        {
            p_line[length] = '\0';
            return FSP_SUCCESS;
        }
        if ('\r' == (char) byte)
        {
            continue;
        }
        if (length >= (line_size - 1U))
        {
            return FSP_ERR_OVERFLOW;
        }
        p_line[length++] = (char) byte;
    }
    return FSP_ERR_TIMEOUT;
}

static fsp_err_t ota_stream_read_exact(ota_stream_t * p_stream,
                                       uint8_t * p_data,
                                       uint32_t length)
{
    uint32_t start_ms = system_tick_now();
    uint32_t copied = 0U;

    while (copied < length)
    {
        uint32_t available;
        uint32_t copy_length;

        if (p_stream->cursor >= p_stream->length)
        {
            int32_t received = ota_stream_fill(p_stream, start_ms);
            if (received < 0)
            {
                return FSP_ERR_ABORTED;
            }
            if (0 == received)
            {
                return FSP_ERR_TIMEOUT;
            }
        }

        available = p_stream->length - p_stream->cursor;
        copy_length = length - copied;
        if (copy_length > available)
        {
            copy_length = available;
        }
        memcpy(&p_data[copied], &p_stream->data[p_stream->cursor], copy_length);
        copied += copy_length;
        p_stream->cursor += copy_length;
        ota_update_watchdog_refresh();
    }
    return FSP_SUCCESS;
}

static bool ota_offer_parse(const char * p_response, ota_offer_t * p_offer)
{
    char sha_hex[OTA_SHA256_HEX_BYTES + 1U];
    unsigned long size = 0UL;
    unsigned long chunk = 0UL;
    int fields;

    if ((NULL == p_response) || (NULL == p_offer))
    {
        return false;
    }

    memset(p_offer, 0, sizeof(*p_offer));
    fields = sscanf(p_response,
                    "FW1 OFFER %31s %31s %lu %64s %lu",
                    p_offer->deployment_id,
                    p_offer->version,
                    &size,
                    sha_hex,
                    &chunk);
    if ((5 != fields) ||
        (OTA_UPDATE_CHUNK_BYTES != chunk) ||
        (0UL == size) ||
        (size > OTA_UPDATE_INACTIVE_SLOT_SIZE) ||
        (!ota_hex_to_sha256(sha_hex, p_offer->sha256)))
    {
        return false;
    }
    p_offer->size = (uint32_t) size;
    return true;
}

static bool ota_record_matches_offer(const ota_offer_t * p_offer)
{
    return g_ota_record_valid &&
           (OTA_BOOT_STATE_DOWNLOADING == g_ota_record.state) &&
           (g_ota_record.total_length == p_offer->size) &&
           (0 == strcmp(g_ota_record.deployment_id, p_offer->deployment_id)) &&
           (0 == strcmp(g_ota_record.target_version, p_offer->version)) &&
           ota_sha256_equal(g_ota_record.image_sha256, p_offer->sha256) &&
           (g_ota_record.received_length <= p_offer->size) &&
           ((g_ota_record.received_length == p_offer->size) ||
            (0U == (g_ota_record.received_length % OTA_UPDATE_CHECKPOINT_BYTES)));
}

static void ota_safe_outputs(void)
{
    audio_voice_abort();
    cooling_interlock_abort();
    black_fan_set_speed(0U);
    (void) water_cooling_set_enabled(false);
    door_state = false;
    door_close();
}

static fsp_err_t ota_erase_inactive_tail(uint32_t resume_offset)
{
    uint32_t skipped_blocks;
    uint32_t erase_blocks;

    if ((resume_offset > OTA_UPDATE_INACTIVE_SLOT_SIZE) ||
        (0U != (resume_offset % OTA_UPDATE_CODE_FLASH_ERASE_BYTES)))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }

    skipped_blocks = resume_offset / OTA_UPDATE_CODE_FLASH_ERASE_BYTES;
    if (skipped_blocks > OTA_UPDATE_INACTIVE_SLOT_ERASE_BLOCKS)
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    erase_blocks = OTA_UPDATE_INACTIVE_SLOT_ERASE_BLOCKS - skipped_blocks;
    if (0U == erase_blocks)
    {
        return FSP_SUCCESS;
    }

    ota_update_watchdog_refresh();
    return R_FLASH_HP_Erase(gp_ota_flash_ctrl,
                            OTA_UPDATE_INACTIVE_SLOT_ADDRESS + resume_offset,
                            erase_blocks);
}

static fsp_err_t ota_rebuild_hash(ota_sha256_t * p_sha, uint32_t resume_offset)
{
    uint32_t offset = 0U;

    ota_sha256_init(p_sha);
    while (offset < resume_offset)
    {
        uint32_t length = resume_offset - offset;
        if (length > OTA_UPDATE_CHUNK_BYTES)
        {
            length = OTA_UPDATE_CHUNK_BYTES;
        }
        ota_sha256_update(p_sha,
                          (const uint8_t *) (OTA_UPDATE_INACTIVE_SLOT_ADDRESS + offset),
                          length);
        offset += length;
        ota_update_watchdog_refresh();
    }
    return FSP_SUCCESS;
}

static fsp_err_t ota_flash_write_chunk(uint32_t offset,
                                       uint8_t * p_data,
                                       uint32_t data_length)
{
    uint32_t write_length =
        (data_length + OTA_FLASH_WRITE_BYTES - 1U) &
        ~(OTA_FLASH_WRITE_BYTES - 1U);

    if ((0U == data_length) ||
        (write_length > sizeof(g_ota_chunk)) ||
        ((offset + write_length) > OTA_UPDATE_INACTIVE_SLOT_SIZE))
    {
        return FSP_ERR_INVALID_ARGUMENT;
    }
    if (write_length > data_length)
    {
        memset(&p_data[data_length], 0xFF, write_length - data_length);
    }

    ota_update_watchdog_refresh();
    return R_FLASH_HP_Write(gp_ota_flash_ctrl,
                            (uint32_t) p_data,
                            OTA_UPDATE_INACTIVE_SLOT_ADDRESS + offset,
                            write_length);
}

static void ota_prepare_new_record(const ota_offer_t * p_offer)
{
    uint8_t current_bank = ota_current_bank();
    uint32_t next_generation = g_ota_record_valid ? g_ota_record.generation : 0U;

    memset(&g_ota_record, 0, sizeof(g_ota_record));
    g_ota_record.magic = OTA_BOOT_RECORD_MAGIC;
    g_ota_record.schema = OTA_BOOT_RECORD_SCHEMA;
    g_ota_record.state = OTA_BOOT_STATE_DOWNLOADING;
    g_ota_record.target_bank = (uint8_t) (current_bank ^ 1U);
    g_ota_record.generation = next_generation;
    g_ota_record.total_length = p_offer->size;
    (void) snprintf(g_ota_record.deployment_id,
                    sizeof(g_ota_record.deployment_id),
                    "%s",
                    p_offer->deployment_id);
    (void) snprintf(g_ota_record.target_version,
                    sizeof(g_ota_record.target_version),
                    "%s",
                    p_offer->version);
    memcpy(g_ota_record.image_sha256, p_offer->sha256, sizeof(g_ota_record.image_sha256));
}

static fsp_err_t ota_download(const ota_offer_t * p_offer, bool * p_permanent_failure)
{
    char line[OTA_LINE_BYTES];
    ota_sha256_t sha;
    uint8_t digest[32];
    uint32_t resume_offset;
    uint32_t received;
    uint32_t expected_sequence;
    fsp_err_t operation_error;

    *p_permanent_failure = false;
    if (ota_record_matches_offer(p_offer))
    {
        resume_offset = g_ota_record.received_length;
    }
    else
    {
        ota_prepare_new_record(p_offer);
        resume_offset = 0U;
    }

    ota_send_progress(p_offer, resume_offset, "erasing");
    operation_error = ota_erase_inactive_tail(resume_offset);
    if (FSP_SUCCESS != operation_error)
    {
        *p_permanent_failure = true;
        return operation_error;
    }

    operation_error = ota_rebuild_hash(&sha, resume_offset);
    if (FSP_SUCCESS != operation_error)
    {
        *p_permanent_failure = true;
        return operation_error;
    }

    g_ota_record.received_length = resume_offset;
    operation_error = ota_record_save();
    if (FSP_SUCCESS != operation_error)
    {
        *p_permanent_failure = true;
        return operation_error;
    }

    (void) snprintf(line,
                    sizeof(line),
                    "FW1 START %s %lu %s",
                    p_offer->deployment_id,
                    (unsigned long) resume_offset,
                    OTA_UPDATE_DEVICE_TOKEN);
    operation_error = ota_send_line(line);
    if (FSP_SUCCESS != operation_error)
    {
        return operation_error;
    }

    memset(&g_ota_stream, 0, sizeof(g_ota_stream));
    received = resume_offset;
    expected_sequence = resume_offset / OTA_UPDATE_CHUNK_BYTES;
    for (;;)
    {
        char deployment_id[OTA_BOOT_RECORD_DEPLOYMENT_ID_MAX];
        char crc_text[16];
        unsigned long sequence;
        unsigned long offset;
        unsigned long bytes;
        int fields;

        operation_error = ota_stream_read_line(&g_ota_stream, line, sizeof(line));
        if (FSP_SUCCESS != operation_error)
        {
            return operation_error;
        }

        if (0 == strncmp(line, "FW1 DATA ", 9U))
        {
            char * p_end = NULL;
            unsigned long expected_crc;
            uint32_t actual_crc;

            fields = sscanf(line,
                            "FW1 DATA %31s %lu %lu %lu %15s",
                            deployment_id,
                            &sequence,
                            &offset,
                            &bytes,
                            crc_text);
            if ((5 != fields) ||
                (0 != strcmp(deployment_id, p_offer->deployment_id)) ||
                (sequence != expected_sequence) ||
                (offset != received) ||
                (0UL == bytes) ||
                (bytes > OTA_UPDATE_CHUNK_BYTES) ||
                ((received + bytes) > p_offer->size))
            {
                *p_permanent_failure = true;
                return FSP_ERR_INVALID_DATA;
            }

            expected_crc = strtoul(crc_text, &p_end, 16);
            if ((NULL == p_end) || ('\0' != *p_end) || (expected_crc > 0xFFFFFFFFUL))
            {
                *p_permanent_failure = true;
                return FSP_ERR_INVALID_DATA;
            }
            operation_error = ota_stream_read_exact(&g_ota_stream,
                                                     g_ota_chunk,
                                                     (uint32_t) bytes);
            if (FSP_SUCCESS != operation_error)
            {
                return operation_error;
            }
            actual_crc = ota_crc32(g_ota_chunk, (uint32_t) bytes);
            if (actual_crc != (uint32_t) expected_crc)
            {
                return FSP_ERR_INVALID_DATA;
            }

            operation_error = ota_flash_write_chunk(received,
                                                     g_ota_chunk,
                                                     (uint32_t) bytes);
            if (FSP_SUCCESS != operation_error)
            {
                *p_permanent_failure = true;
                return operation_error;
            }
            ota_sha256_update(&sha, g_ota_chunk, (uint32_t) bytes);
            received += (uint32_t) bytes;
            expected_sequence++;

            if ((received == p_offer->size) ||
                (0U == (received % OTA_UPDATE_CHECKPOINT_BYTES)))
            {
                g_ota_record.received_length = received;
                operation_error = ota_record_save();
                if (FSP_SUCCESS != operation_error)
                {
                    *p_permanent_failure = true;
                    return operation_error;
                }
                ota_send_progress(p_offer, received, "writing");
            }
            continue;
        }

        if (0 == strncmp(line, "FW1 END ", 8U))
        {
            char end_sha_hex[OTA_SHA256_HEX_BYTES + 1U];
            uint8_t end_sha[32];
            unsigned long total;

            fields = sscanf(line,
                            "FW1 END %31s %lu %64s",
                            deployment_id,
                            &total,
                            end_sha_hex);
            if ((3 != fields) ||
                (0 != strcmp(deployment_id, p_offer->deployment_id)) ||
                (total != p_offer->size) ||
                (received != p_offer->size) ||
                (!ota_hex_to_sha256(end_sha_hex, end_sha)) ||
                (!ota_sha256_equal(end_sha, p_offer->sha256)))
            {
                *p_permanent_failure = true;
                return FSP_ERR_INVALID_DATA;
            }
            break;
        }

        if (0 == strncmp(line, "FW1 ERROR ", 10U))
        {
            *p_permanent_failure = true;
            return FSP_ERR_ABORTED;
        }

        *p_permanent_failure = true;
        return FSP_ERR_INVALID_DATA;
    }

    ota_send_progress(p_offer, received, "verifying");
    ota_sha256_final(&sha, digest);
    if (!ota_sha256_equal(digest, p_offer->sha256))
    {
        *p_permanent_failure = true;
        return FSP_ERR_INVALID_DATA;
    }

    g_ota_record.state = OTA_BOOT_STATE_PENDING_TRIAL;
    g_ota_record.received_length = received;
    g_ota_record.trial_boots = 0U;
    operation_error = ota_record_save();
    if (FSP_SUCCESS != operation_error)
    {
        *p_permanent_failure = true;
        return operation_error;
    }

    ota_send_progress(p_offer, received, "waiting_reboot");
    return FSP_SUCCESS;
}

fsp_err_t ota_update_init(void)
{
    fsp_err_t open_error;

    ota_update_watchdog_refresh();
    if (!g_ota_iwdt_open)
    {
        return FSP_ERR_NOT_OPEN;
    }

    open_error = R_FLASH_HP_Open(gp_ota_flash_ctrl, gp_ota_flash_cfg);
    if ((FSP_SUCCESS == open_error) || (FSP_ERR_ALREADY_OPEN == open_error))
    {
        g_ota_flash_open = true;
    }
    else
    {
        return open_error;
    }

    g_ota_configured = ota_device_token_configured();
    ota_record_load();
    if (g_ota_record_valid && (OTA_BOOT_STATE_PENDING_TRIAL == g_ota_record.state))
    {
        if (0 == strcmp(g_ota_record.target_version, OTA_UPDATE_APP_VERSION))
        {
            g_ota_trial_start_ms = system_tick_now();
            deepseek_bridge_pc_write("[OTA] trial image running; local confirmation starts in 10 seconds.\r\n");
        }
        else
        {
            /*
             * MCUboot selected the previous image.  Do not let that previous
             * image confirm a candidate that was rejected by signature,
             * header, or version validation.
             */
            g_ota_record.state = OTA_BOOT_STATE_ROLLBACK;
            if (FSP_SUCCESS == ota_record_save())
            {
                g_ota_result_pending = ('\0' != g_ota_record.deployment_id[0]);
            }
            deepseek_bridge_pc_write("[OTA] MCUboot did not launch the target image; rollback recorded.\r\n");
        }
    }
    else if (g_ota_record_valid && (OTA_BOOT_STATE_CONFIRMED == g_ota_record.state))
    {
        g_ota_result_pending = ('\0' != g_ota_record.deployment_id[0]);
    }
    else if (g_ota_record_valid && (OTA_BOOT_STATE_ROLLBACK == g_ota_record.state))
    {
        g_ota_result_pending = ('\0' != g_ota_record.deployment_id[0]);
    }

    if (!g_ota_configured)
    {
        deepseek_bridge_pc_write(
            "[OTA] Set OTA_UPDATE_DEVICE_TOKEN to the server FRIDGE_OTA_TOKEN before flashing.\r\n");
    }
    g_ota_last_check_ms = system_tick_now() - OTA_UPDATE_CHECK_INTERVAL_MS;
    return FSP_SUCCESS;
}

bool ota_update_active(void)
{
    return g_ota_exclusive;
}

void ota_update_confirm_current(void)
{
    if ((!g_ota_record_valid) ||
        (OTA_BOOT_STATE_PENDING_TRIAL != g_ota_record.state) ||
        (!g_ota_flash_open))
    {
        return;
    }

    g_ota_record.state = OTA_BOOT_STATE_CONFIRMED;
    if (FSP_SUCCESS != ota_record_save())
    {
        return;
    }
    g_ota_result_pending = true;

    if (g_ota_configured && (FSP_SUCCESS == deepseek_bridge_ota_prepare()))
    {
        deepseek_bridge_set_ota_active(true);
        if (FSP_SUCCESS == ota_send_saved_result())
        {
            ota_result_delivered();
        }
        deepseek_bridge_set_ota_active(false);
    }
    deepseek_bridge_pc_write("[OTA] trial image confirmed locally.\r\n");
}

void ota_update_process(void)
{
    char request[OTA_LINE_BYTES];
    char response[OTA_LINE_BYTES];
    ota_offer_t offer;
    bool permanent_failure = false;
    fsp_err_t process_error;

    ota_update_watchdog_refresh();
    if (g_ota_record_valid &&
        (OTA_BOOT_STATE_PENDING_TRIAL == g_ota_record.state) &&
        system_tick_elapsed(g_ota_trial_start_ms, OTA_UPDATE_TRIAL_CONFIRM_MS))
    {
        ota_update_confirm_current();
    }

    if (!g_ota_configured)
    {
        return;
    }

    if (g_ota_result_pending &&
        g_ota_record_valid &&
        ((OTA_BOOT_STATE_CONFIRMED == g_ota_record.state) ||
         (OTA_BOOT_STATE_ROLLBACK == g_ota_record.state)) &&
        (!audio_voice_busy()) &&
        deepseek_bridge_online())
    {
        deepseek_bridge_set_ota_active(true);
        if ((FSP_SUCCESS == deepseek_bridge_ota_prepare()) &&
            (FSP_SUCCESS == ota_send_saved_result()))
        {
            ota_result_delivered();
        }
        deepseek_bridge_set_ota_active(false);
    }

    if (g_ota_exclusive ||
        audio_voice_busy() ||
        (!system_tick_elapsed(g_ota_last_check_ms, OTA_UPDATE_CHECK_INTERVAL_MS)))
    {
        return;
    }
    g_ota_last_check_ms = system_tick_now();
    if (!deepseek_bridge_online())
    {
        return;
    }

    (void) snprintf(request,
                    sizeof(request),
                    "FW1 CHECK %s %s %s %s %u %s",
                    OTA_UPDATE_DEVICE_ID,
                    OTA_UPDATE_PART_NUMBER,
                    OTA_UPDATE_APP_VERSION,
                    OTA_UPDATE_BOOT_VERSION,
                    (unsigned int) ota_current_bank(),
                    OTA_UPDATE_DEVICE_TOKEN);
    process_error = deepseek_bridge_server_request(request,
                                                   response,
                                                   sizeof(response),
                                                   OTA_UPDATE_NETWORK_TIMEOUT_MS);
    if ((FSP_SUCCESS != process_error) || (0 == strcmp(response, "FW1 NONE")))
    {
        return;
    }
    if (!ota_offer_parse(response, &offer))
    {
        deepseek_bridge_pc_write("[OTA] server response rejected.\r\n");
        return;
    }

    g_ota_exclusive = true;
    ota_safe_outputs();
    deepseek_bridge_set_ota_active(true);
    process_error = deepseek_bridge_ota_prepare();
    if (FSP_SUCCESS == process_error)
    {
        process_error = ota_download(&offer, &permanent_failure);
    }
    if (FSP_SUCCESS == process_error)
    {
        deepseek_bridge_pc_write("[OTA] image complete; resetting for MCUboot validation.\r\n");
        ota_update_watchdog_refresh();
        R_BSP_SoftwareDelay(100U, BSP_DELAY_UNITS_MILLISECONDS);
        NVIC_SystemReset();
    }

    if (permanent_failure && deepseek_bridge_online())
    {
        (void) ota_send_result(offer.deployment_id,
                               OTA_UPDATE_APP_VERSION,
                               "failed",
                               "ota_write_or_verify");
    }
    deepseek_bridge_force_reconnect();
    deepseek_bridge_set_ota_active(false);
    g_ota_exclusive = false;
    deepseek_bridge_pc_write("[OTA] transfer stopped; checkpoint retained for retry.\r\n");
}

#else

static bool g_ota_disabled_notice_sent = false;

fsp_err_t ota_update_init(void)
{
    if (!g_ota_disabled_notice_sent)
    {
        deepseek_bridge_pc_write(
            "[OTA] disabled: generate dual-bank Flash/IWDT FSP instances, "
            "copy the MCUboot layout, then set OTA_UPDATE_FSP_READY=1.\r\n");
        g_ota_disabled_notice_sent = true;
    }
    return FSP_SUCCESS;
}

void ota_update_process(void)
{
}

bool ota_update_active(void)
{
    return false;
}

void ota_update_confirm_current(void)
{
}

void ota_update_watchdog_refresh(void)
{
}

#endif
