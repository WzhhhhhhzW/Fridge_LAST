#include "../project_firdge/src/ota_sha256.h"
#include "../ota_bootloader_support/ota_boot_trial.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void digest_to_hex(const uint8_t digest[32], char text[65])
{
    static const char digits[] = "0123456789abcdef";

    for (size_t i = 0U; i < 32U; i++)
    {
        text[i * 2U] = digits[digest[i] >> 4U];
        text[i * 2U + 1U] = digits[digest[i] & 0x0FU];
    }
    text[64] = '\0';
}

static void test_sha256(const char * input, const char * expected)
{
    ota_sha256_t context;
    uint8_t digest[32];
    char actual[65];

    ota_sha256_init(&context);
    ota_sha256_update(&context, (const uint8_t *) input, strlen(input));
    ota_sha256_final(&context, digest);
    digest_to_hex(digest, actual);
    assert(0 == strcmp(actual, expected));
}

static void test_trial_counter(void)
{
    ota_boot_record_t record;

    memset(&record, 0, sizeof(record));
    record.state = OTA_BOOT_STATE_PENDING_TRIAL;
    record.target_bank = 1U;
    ota_boot_trial_record_seal(&record);
    assert(ota_boot_trial_record_valid(&record));

    assert(OTA_BOOT_TRIAL_ACTION_STORE_AND_BOOT ==
           ota_boot_trial_before_launch(&record, 1U, 3U));
    assert(1U == record.trial_boots);
    assert(OTA_BOOT_TRIAL_ACTION_STORE_AND_BOOT ==
           ota_boot_trial_before_launch(&record, 1U, 3U));
    assert(2U == record.trial_boots);
    assert(OTA_BOOT_TRIAL_ACTION_STORE_AND_BOOT ==
           ota_boot_trial_before_launch(&record, 1U, 3U));
    assert(3U == record.trial_boots);
    assert(OTA_BOOT_TRIAL_ACTION_STORE_AND_ROLL_BACK ==
           ota_boot_trial_before_launch(&record, 1U, 3U));
    assert(OTA_BOOT_STATE_ROLLBACK == record.state);
    assert(ota_boot_trial_record_valid(&record));
}

int main(void)
{
    test_sha256("",
                "e3b0c44298fc1c149afbf4c8996fb924"
                "27ae41e4649b934ca495991b7852b855");
    test_sha256("abc",
                "ba7816bf8f01cfea414140de5dae2223"
                "b00361a396177a9cb410ff61f20015ad");
    test_trial_counter();
    puts("OTA core tests passed");
    return 0;
}
