#include "access_control.h"

#include "DOOR.h"
#include "deepseek_bridge.h"
#include "ota_update.h"
#include "pn532.h"
#include "system_tick.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define ACCESS_RECORD_MAGIC          (0x3143464EU) /* "NFC1" */
#define ACCESS_RECORD_SCHEMA         (1U)
#define ACCESS_MAX_CARDS             (10U)
#define ACCESS_RECORD_SLOT0_ADDRESS  (0x08001C00UL)
#define ACCESS_RECORD_SLOT1_ADDRESS  (0x08001D00UL)
#define ACCESS_RECORD_ERASE_BLOCKS   (4U)
#define ACCESS_ENROLL_TIMEOUT_MS     (30000U)
#define ACCESS_UNLOCK_COOLDOWN_MS    (3000U)
#define ACCESS_CARD_REMOVED_SCANS    (3U)

typedef struct st_access_uid
{
    uint8_t length;
    uint8_t bytes[PN532_UID_MAX_BYTES];
} access_uid_t;

typedef struct st_access_record
{
    uint32_t magic;
    uint16_t schema;
    uint8_t card_count;
    uint8_t reserved0;
    uint32_t generation;
    access_uid_t cards[ACCESS_MAX_CARDS];
    uint8_t reserved1[160];
    uint32_t crc32;
} __attribute__((packed, aligned(4))) access_record_t;

typedef enum e_access_enroll_state
{
    ACCESS_ENROLL_IDLE = 0,
    ACCESS_ENROLL_AUTH,
    ACCESS_ENROLL_WAIT_REMOVE,
    ACCESS_ENROLL_NEW,
} access_enroll_state_t;

_Static_assert(sizeof(access_record_t) == 256U,
               "NFC whitelist record must occupy exactly four data-flash blocks");

static uint8_t const g_initial_uid[] = {0x2BU, 0xDDU, 0x28U, 0x54U};
/* Cards that must remain usable even when an older valid Data Flash record
 * predates them.  The second UID is the physical card reported by this unit. */
static uint8_t const g_builtin_uids[][4] =
{
    {0x2BU, 0xDDU, 0x28U, 0x54U},
    {0x5CU, 0x24U, 0x75U, 0x8EU},
};
#define ACCESS_BUILTIN_UID_COUNT ((uint8_t) (sizeof(g_builtin_uids) / sizeof(g_builtin_uids[0])))
static access_record_t g_access_record;
static uint32_t g_access_record_slot = 0U;
static bool g_access_persistence_available = false;
static bool g_access_initialized = false;
static bool g_access_ota_suspended = false;
static bool g_access_card_latched = false;
static uint8_t g_access_no_card_scans = 0U;
static uint32_t g_access_last_scan_generation = 0U;
static bool g_access_unlock_ever = false;
static uint32_t g_access_last_unlock_ms = 0U;
static access_enroll_state_t g_access_enroll_state = ACCESS_ENROLL_IDLE;
static uint32_t g_access_enroll_started_ms = 0U;

static void access_log(const char * text)
{
    deepseek_bridge_pc_write(text);
}

static uint32_t access_crc32(uint8_t const * data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;

    for (size_t i = 0U; i < length; i++)
    {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; bit++)
        {
            crc = (crc >> 1U) ^ ((0U != (crc & 1U)) ? 0xEDB88320UL : 0U);
        }
    }
    return ~crc;
}

static bool access_uid_valid(access_uid_t const * uid)
{
    return (NULL != uid) && ((4U == uid->length) || (7U == uid->length));
}

static bool access_record_valid(access_record_t const * record)
{
    if ((NULL == record) ||
        (ACCESS_RECORD_MAGIC != record->magic) ||
        (ACCESS_RECORD_SCHEMA != record->schema) ||
        (0U == record->card_count) ||
        (record->card_count > ACCESS_MAX_CARDS) ||
        (record->crc32 != access_crc32((uint8_t const *) record,
                                       offsetof(access_record_t, crc32))))
    {
        return false;
    }

    for (uint8_t i = 0U; i < record->card_count; i++)
    {
        if (!access_uid_valid(&record->cards[i]))
        {
            return false;
        }
    }
    return true;
}

static bool access_generation_newer(uint32_t candidate, uint32_t reference)
{
    return ((int32_t) (candidate - reference) > 0);
}

static void access_use_factory_default(void)
{
    memset(&g_access_record, 0, sizeof(g_access_record));
    g_access_record.magic = ACCESS_RECORD_MAGIC;
    g_access_record.schema = ACCESS_RECORD_SCHEMA;
    g_access_record.card_count = 1U;
    g_access_record.cards[0].length = sizeof(g_initial_uid);
    memcpy(g_access_record.cards[0].bytes, g_initial_uid, sizeof(g_initial_uid));
    g_access_record.crc32 = access_crc32((uint8_t const *) &g_access_record,
                                         offsetof(access_record_t, crc32));
    g_access_record_slot = 0U;
}

static bool access_uid_equal(access_uid_t const * card,
                             uint8_t const * uid,
                             uint8_t uid_length)
{
    return access_uid_valid(card) &&
           (card->length == uid_length) &&
           (0 == memcmp(card->bytes, uid, uid_length));
}

static int access_find_uid(uint8_t const * uid, uint8_t uid_length)
{
    for (uint8_t i = 0U; i < g_access_record.card_count; i++)
    {
        if (access_uid_equal(&g_access_record.cards[i], uid, uid_length))
        {
            return (int) i;
        }
    }

    if (4U == uid_length)
    {
        for (uint8_t i = 0U; i < ACCESS_BUILTIN_UID_COUNT; i++)
        {
            if (0 == memcmp(g_builtin_uids[i], uid, 4U))
            {
                /* Keep built-in matches non-negative so normal authorization
                 * and enrollment authorization use the same path. */
                return (int) ACCESS_MAX_CARDS + (int) i;
            }
        }
    }
    return -1;
}

static void access_format_uid(char * output,
                              size_t output_size,
                              uint8_t const * uid,
                              uint8_t uid_length)
{
    size_t used = 0U;

    if ((NULL == output) || (0U == output_size))
    {
        return;
    }
    output[0] = '\0';
    for (uint8_t i = 0U; i < uid_length; i++)
    {
        int written = snprintf(&output[used],
                               output_size - used,
                               (0U == i) ? "%02X" : " %02X",
                               uid[i]);
        if ((written <= 0) || ((size_t) written >= (output_size - used)))
        {
            output[output_size - 1U] = '\0';
            return;
        }
        used += (size_t) written;
    }
}

static fsp_err_t access_record_save(void)
{
    access_record_t write_record;
    uint32_t target_slot = (ACCESS_RECORD_SLOT0_ADDRESS == g_access_record_slot) ?
                           ACCESS_RECORD_SLOT1_ADDRESS : ACCESS_RECORD_SLOT0_ADDRESS;
    fsp_err_t err;

    if (ota_update_active())
    {
        return FSP_ERR_IN_USE;
    }

    write_record = g_access_record;
    write_record.magic = ACCESS_RECORD_MAGIC;
    write_record.schema = ACCESS_RECORD_SCHEMA;
    write_record.generation++;
    write_record.crc32 = access_crc32((uint8_t const *) &write_record,
                                      offsetof(access_record_t, crc32));

    ota_update_watchdog_refresh();
    err = R_FLASH_HP_Erase(&g_ota_flash_ctrl,
                           target_slot,
                           ACCESS_RECORD_ERASE_BLOCKS);
    if (FSP_SUCCESS != err)
    {
        return err;
    }

    ota_update_watchdog_refresh();
    err = R_FLASH_HP_Write(&g_ota_flash_ctrl,
                           (uint32_t) &write_record,
                           target_slot,
                           sizeof(write_record));
    if (FSP_SUCCESS != err)
    {
        return err;
    }
    if ((0 != memcmp((void const *) target_slot,
                     &write_record,
                     sizeof(write_record))) ||
        (!access_record_valid((access_record_t const *) target_slot)))
    {
        return FSP_ERR_WRITE_FAILED;
    }

    g_access_record = write_record;
    g_access_record_slot = target_slot;
    return FSP_SUCCESS;
}

static void access_cancel_enrollment(const char * reason)
{
    if (ACCESS_ENROLL_IDLE != g_access_enroll_state)
    {
        g_access_enroll_state = ACCESS_ENROLL_IDLE;
        g_access_card_latched = false;
        g_access_no_card_scans = 0U;
        access_log(reason);
    }
}

static void access_handle_uid(uint8_t const * uid, uint8_t uid_length)
{
    char uid_text[32];
    char log_line[112];
    int card_index = access_find_uid(uid, uid_length);

    access_format_uid(uid_text, sizeof(uid_text), uid, uid_length);

    if (ACCESS_ENROLL_AUTH == g_access_enroll_state)
    {
        if (card_index < 0)
        {
            (void) snprintf(log_line,
                            sizeof(log_line),
                            "[NFC] enroll rejected: authorization card %s is unknown.\r\n",
                            uid_text);
            access_cancel_enrollment(log_line);
            return;
        }

        g_access_enroll_state = ACCESS_ENROLL_WAIT_REMOVE;
        g_access_enroll_started_ms = system_tick_now();
        g_access_no_card_scans = 0U;
        access_log("[NFC] authorization accepted; remove that card, then present the new card.\r\n");
        return;
    }

    if (ACCESS_ENROLL_NEW == g_access_enroll_state)
    {
        fsp_err_t save_error;

        if (card_index >= 0)
        {
            access_cancel_enrollment("[NFC] enroll cancelled: that card is already in the whitelist.\r\n");
            return;
        }
        if (g_access_record.card_count >= ACCESS_MAX_CARDS)
        {
            access_cancel_enrollment("[NFC] enroll cancelled: whitelist is full (10 cards).\r\n");
            return;
        }
        if (!g_access_persistence_available)
        {
            access_cancel_enrollment("[NFC] enroll cancelled: Data Flash persistence is unavailable.\r\n");
            return;
        }

        g_access_record.cards[g_access_record.card_count].length = uid_length;
        memcpy(g_access_record.cards[g_access_record.card_count].bytes, uid, uid_length);
        g_access_record.card_count++;
        save_error = access_record_save();
        if (FSP_SUCCESS != save_error)
        {
            g_access_record.card_count--;
            (void) snprintf(log_line,
                            sizeof(log_line),
                            "[NFC] enroll failed: Data Flash err=%d.\r\n",
                            (int) save_error);
            access_cancel_enrollment(log_line);
            return;
        }

        (void) snprintf(log_line,
                        sizeof(log_line),
                        "[NFC] new card saved: %s (count=%u).\r\n",
                        uid_text,
                        (unsigned int) g_access_record.card_count);
        access_cancel_enrollment(log_line);
        return;
    }

    if (ACCESS_ENROLL_WAIT_REMOVE == g_access_enroll_state)
    {
        return;
    }

    if (card_index >= 0)
    {
        uint32_t now = system_tick_now();
        if ((!g_access_unlock_ever) ||
            ((uint32_t) (now - g_access_last_unlock_ms) >= ACCESS_UNLOCK_COOLDOWN_MS))
        {
            if (door_unlock_request())
            {
                g_access_last_unlock_ms = now;
                g_access_unlock_ever = true;
                (void) snprintf(log_line,
                                sizeof(log_line),
                                "[NFC] authorized card %s; door unlocked for 500 ms.\r\n",
                                uid_text);
                access_log(log_line);
            }
            else if (!door_is_closed())
            {
                (void) snprintf(log_line,
                                sizeof(log_line),
                                "[NFC] authorized card ignored: door is already open.\r\n");
                access_log(log_line);
            }
        }
    }
    else
    {
        (void) snprintf(log_line,
                        sizeof(log_line),
                        "[NFC] unauthorized card rejected: %s.\r\n",
                        uid_text);
        access_log(log_line);
    }
}

fsp_err_t access_control_init(void)
{
    access_record_t const * slot0 = (access_record_t const *) ACCESS_RECORD_SLOT0_ADDRESS;
    access_record_t const * slot1 = (access_record_t const *) ACCESS_RECORD_SLOT1_ADDRESS;
    bool slot0_valid = access_record_valid(slot0);
    bool slot1_valid = access_record_valid(slot1);
    fsp_err_t pn532_error;

    if (slot0_valid && slot1_valid)
    {
        if (access_generation_newer(slot1->generation, slot0->generation))
        {
            g_access_record = *slot1;
            g_access_record_slot = ACCESS_RECORD_SLOT1_ADDRESS;
        }
        else
        {
            g_access_record = *slot0;
            g_access_record_slot = ACCESS_RECORD_SLOT0_ADDRESS;
        }
        g_access_persistence_available = true;
    }
    else if (slot0_valid)
    {
        g_access_record = *slot0;
        g_access_record_slot = ACCESS_RECORD_SLOT0_ADDRESS;
        g_access_persistence_available = true;
    }
    else if (slot1_valid)
    {
        g_access_record = *slot1;
        g_access_record_slot = ACCESS_RECORD_SLOT1_ADDRESS;
        g_access_persistence_available = true;
    }
    else
    {
        access_use_factory_default();
        if (FSP_SUCCESS == access_record_save())
        {
            g_access_persistence_available = true;
            access_log("[NFC] initialized Data Flash whitelist with factory card 2B DD 28 54.\r\n");
        }
        else
        {
            g_access_persistence_available = false;
            access_log("[NFC] Data Flash whitelist unavailable; factory card remains authorized in RAM only.\r\n");
        }
    }

    g_access_initialized = true;
    pn532_error = pn532_init();
    if (FSP_SUCCESS != pn532_error)
    {
        access_log("[NFC] PN532 disabled until the required FSP SPI/P405 settings are generated.\r\n");
    }
    return pn532_error;
}

void access_control_process(void)
{
    uint8_t uid[PN532_UID_MAX_BYTES];
    uint8_t uid_length = 0U;
    uint32_t scan_generation;

    if (!g_access_initialized)
    {
        return;
    }

    if (ota_update_active())
    {
        if (!g_access_ota_suspended)
        {
            g_access_ota_suspended = true;
            pn532_suspend();
            door_close();
            access_cancel_enrollment("[NFC] enrollment cancelled for OTA.\r\n");
        }
        return;
    }
    if (g_access_ota_suspended)
    {
        g_access_ota_suspended = false;
        pn532_resume();
    }

    pn532_process();
    scan_generation = pn532_scan_generation();
    if (scan_generation != g_access_last_scan_generation)
    {
        g_access_last_scan_generation = scan_generation;
        if (pn532_card_present())
        {
            g_access_no_card_scans = 0U;
        }
        else
        {
            if (g_access_no_card_scans < ACCESS_CARD_REMOVED_SCANS)
            {
                g_access_no_card_scans++;
            }
            if (g_access_no_card_scans >= ACCESS_CARD_REMOVED_SCANS)
            {
                g_access_card_latched = false;
                if (ACCESS_ENROLL_WAIT_REMOVE == g_access_enroll_state)
                {
                    g_access_enroll_state = ACCESS_ENROLL_NEW;
                    g_access_enroll_started_ms = system_tick_now();
                    access_log("[NFC] authorization card removed; present the new card within 30 s.\r\n");
                }
            }
        }
    }

    if ((ACCESS_ENROLL_IDLE != g_access_enroll_state) &&
        system_tick_elapsed(g_access_enroll_started_ms, ACCESS_ENROLL_TIMEOUT_MS))
    {
        access_cancel_enrollment("[NFC] enrollment timed out; whitelist unchanged.\r\n");
    }

    if (pn532_take_uid(uid, &uid_length) && (!g_access_card_latched))
    {
        g_access_card_latched = true;
        access_handle_uid(uid, uid_length);
    }
}

void access_control_log_status(void)
{
    char line[160];
    char const * state = "idle";

    if (ACCESS_ENROLL_AUTH == g_access_enroll_state)
    {
        state = "waiting-authorized-card";
    }
    else if (ACCESS_ENROLL_WAIT_REMOVE == g_access_enroll_state)
    {
        state = "waiting-card-removal";
    }
    else if (ACCESS_ENROLL_NEW == g_access_enroll_state)
    {
        state = "waiting-new-card";
    }

    (void) snprintf(line,
                    sizeof(line),
                    "[NFC] ready=%u cards=%u persistence=%u enroll=%s door=%u.\r\n",
                    pn532_ready() ? 1U : 0U,
                    (unsigned int) g_access_record.card_count,
                    g_access_persistence_available ? 1U : 0U,
                    state,
                    door_unlock_active() ? 1U : 0U);
    access_log(line);
}

void access_control_log_list(void)
{
    char uid_text[32];
    char line[80];

    access_log("[NFC] whitelist:\r\n");
    for (uint8_t i = 0U; i < g_access_record.card_count; i++)
    {
        access_format_uid(uid_text,
                          sizeof(uid_text),
                          g_access_record.cards[i].bytes,
                          g_access_record.cards[i].length);
        (void) snprintf(line, sizeof(line), "  %u: %s\r\n", (unsigned int) (i + 1U), uid_text);
        access_log(line);
    }

    access_log("[NFC] built-in authorized cards:\r\n");
    for (uint8_t i = 0U; i < ACCESS_BUILTIN_UID_COUNT; i++)
    {
        access_format_uid(uid_text, sizeof(uid_text), g_builtin_uids[i], 4U);
        (void) snprintf(line,
                        sizeof(line),
                        "  B%u: %s\r\n",
                        (unsigned int) (i + 1U),
                        uid_text);
        access_log(line);
    }
}

void access_control_enroll_start(void)
{
    if ((!g_access_initialized) || (!pn532_ready()))
    {
        access_log("[NFC] enroll unavailable: PN532 is not ready.\r\n");
        return;
    }
    if (!g_access_persistence_available)
    {
        access_log("[NFC] enroll unavailable: Data Flash persistence is disabled.\r\n");
        return;
    }
    if (g_access_record.card_count >= ACCESS_MAX_CARDS)
    {
        access_log("[NFC] enroll unavailable: whitelist is full (10 cards).\r\n");
        return;
    }

    g_access_enroll_state = ACCESS_ENROLL_AUTH;
    g_access_enroll_started_ms = system_tick_now();
    g_access_card_latched = false;
    g_access_no_card_scans = 0U;
    access_log("[NFC] enroll started; present an existing authorized card within 30 s.\r\n");
}

void access_control_enroll_cancel(void)
{
    if (ACCESS_ENROLL_IDLE == g_access_enroll_state)
    {
        access_log("[NFC] no enrollment is active.\r\n");
        return;
    }
    access_cancel_enrollment("[NFC] enrollment cancelled; whitelist unchanged.\r\n");
}
