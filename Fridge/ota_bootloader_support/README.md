# RA6M5 MCUboot trial/rollback user hook

This folder contains only non-generated Bootloader user logic. It does not
modify an FSP `configuration.xml`, generated MCUboot layout, or `ra_gen`.

Use it only after `project_firdge_boot` has been created from the FSP 5.9.0
RA6M5 MCUboot Direct XIP, dual-bank, ECDSA-P256/SCE9 template.

## Integration point

1. Add `ota_boot_trial.c` to the Bootloader project as a linked source file.
   Add `ota_boot_watchdog.c` the same way.
2. Add `Fridge/project_firdge/src` to the Bootloader include paths so the
   application and Bootloader use the exact same `ota_boot_record.h`.
3. In Bootloader user code, load the newest valid record from Data Flash
   `0x08001E00` and `0x08001F00`.
4. After MCUboot has authenticated its candidate with SCE9, but before it
   jumps to the image, call:

   ```c
   action = ota_boot_trial_before_launch(&record, selected_bank, 3U);
   ```

5. On `OTA_BOOT_TRIAL_ACTION_STORE_AND_BOOT`, write the updated record to the
   other 256-byte journal slot, then launch the candidate.
6. On `OTA_BOOT_TRIAL_ACTION_STORE_AND_ROLL_BACK`, write the record, use the
   generated MCUboot Direct XIP API to invalidate the failed image header,
   select the other bank, and reset.

Set the MCUboot stack's **Watchdog Feed** property to
`ota_boot_watchdog_refresh();`. The function is implemented in
`ota_boot_watchdog.c` and refreshes the generated `g_iwdt0` instance.

Do not guess the MCUboot image addresses or internal function names. They vary
with the generated `.sbd` and FSP template. Once the generated Bootloader
project exists, inspect its layout and public port API, then add the small
adapter at its actual pre-jump hook.

The application confirms a healthy trial after ten seconds without depending
on Wi-Fi or cloud services. Confirmation changes the shared record from
`PENDING_TRIAL` to `CONFIRMED`; a confirmed record is left unchanged by this
hook.
