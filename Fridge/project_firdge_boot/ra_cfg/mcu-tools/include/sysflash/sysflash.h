/* generated configuration header file - do not edit */
#ifndef SYSFLASH_H_
#define SYSFLASH_H_
#ifndef __SYSFLASH_H__
#define __SYSFLASH_H__

#include "mcuboot_config/mcuboot_config.h"

#ifdef RM_MCUBOOT_PORT_CFG_SECONDARY_USE_OSPI_B
#include "mcuboot_config/mcuboot_ospi_b_config.h"
#endif

#include "bsp_api.h"

/* Common macro for FSP header files. There is also a corresponding FSP_FOOTER macro at the end of this file. */
FSP_HEADER

#define FLASH_DEVICE_INTERNAL_FLASH    (0x7F)
#define FLASH_DEVICE_EXTERNAL_FLASH    (0x80)

#define FLASH_AREA_BOOTLOADER          0
#define FLASH_AREA_IMAGE_0             1
#define FLASH_AREA_IMAGE_1             2
#define FLASH_AREA_IMAGE_SCRATCH       3
#define FLASH_AREA_IMAGE_2             5
#define FLASH_AREA_IMAGE_3             6

#if (MCUBOOT_IMAGE_NUMBER == 1)
 #define FLASH_AREA_IMAGE_PRIMARY(x)      (((x) == 0) ?         \
                                           FLASH_AREA_IMAGE_0 : \
                                           FLASH_AREA_IMAGE_0)
 #define FLASH_AREA_IMAGE_SECONDARY(x)    (((x) == 0) ?         \
                                           FLASH_AREA_IMAGE_1 : \
                                           FLASH_AREA_IMAGE_1)

#elif (MCUBOOT_IMAGE_NUMBER == 2)

 #define FLASH_AREA_IMAGE_PRIMARY(x)      (((x) == 0) ?         \
                                           FLASH_AREA_IMAGE_0 : \
                                           ((x) == 1) ?         \
                                           FLASH_AREA_IMAGE_1 : \
                                           255)
 #define FLASH_AREA_IMAGE_SECONDARY(x)    (((x) == 0) ?         \
                                           FLASH_AREA_IMAGE_2 : \
                                           ((x) == 1) ?         \
                                           FLASH_AREA_IMAGE_3 : \
                                           255)

#else
#warning "Image slot and flash area mapping is not defined"
#endif

#define FLASH_AREA_MCUBOOT_OFFSET     BSP_FEATURE_FLASH_CODE_FLASH_START

/* Define an offset if placing image at an address other than the start of the XSPI region. */
#ifndef XSPI_AREA_MCUBOOT_OFFSET
#define XSPI_AREA_MCUBOOT_OFFSET      (0x0)
#endif

#if BSP_FEATURE_FLASH_HP_SUPPORTS_DUAL_BANK
 #define RM_MCUBOOT_DUAL_BANK_ENABLED    (!(0x7U & (BSP_CFG_ROM_REG_DUALSEL)))
#elif BSP_FEATURE_FLASH_LP_SUPPORTS_DUAL_BANK
 #define RM_MCUBOOT_DUAL_BANK_ENABLED    MCUBOOT_DUALBANK_FLASH_LP
#endif

#if (MCUBOOT_IMAGE_NUMBER == 1)

/* Secure + Non-Secure image primary slot */
 #ifndef FLASH_AREA_0_ID
  #define FLASH_AREA_0_ID               (FLASH_AREA_IMAGE_0)
 #endif
 #ifndef FLASH_AREA_0_OFFSET
  #define FLASH_AREA_0_OFFSET           (FLASH_AREA_MCUBOOT_OFFSET + RM_MCUBOOT_PORT_CFG_MCUBOOT_SIZE)
 #endif
 #ifndef FLASH_AREA_0_SIZE
  #define FLASH_AREA_0_SIZE             (RM_MCUBOOT_PORT_CFG_PARTITION_SIZE)
 #endif

/* Secure + Non-secure secondary slot */
 #ifndef FLASH_AREA_2_ID
  #define FLASH_AREA_2_ID               (FLASH_AREA_IMAGE_1)
 #endif
 #ifndef FLASH_AREA_2_OFFSET
  #ifdef RM_MCUBOOT_PORT_CFG_SECONDARY_USE_QSPI
   #define FLASH_AREA_2_OFFSET           (BSP_FEATURE_QSPI_DEVICE_START_ADDRESS + XSPI_AREA_MCUBOOT_OFFSET)
  #elif defined(RM_MCUBOOT_PORT_CFG_SECONDARY_USE_OSPI_B)
   #if (RM_MCUBOOT_PORT_CFG_OSPI_B_CHANNEL == 1)
    #define FLASH_AREA_2_OFFSET           (BSP_FEATURE_OSPI_B_DEVICE_1_START_ADDRESS + XSPI_AREA_MCUBOOT_OFFSET)
   #elif (RM_MCUBOOT_PORT_CFG_OSPI_B_CHANNEL == 0)
    #define FLASH_AREA_2_OFFSET           (BSP_FEATURE_OSPI_B_DEVICE_0_START_ADDRESS + XSPI_AREA_MCUBOOT_OFFSET)
   #endif
  #elif RM_MCUBOOT_DUAL_BANK_ENABLED
   #if BSP_FEATURE_FLASH_HP_VERSION > 0
    #define FLASH_AREA_2_OFFSET          (BSP_FEATURE_FLASH_HP_CF_DUAL_BANK_START + \
                                        RM_MCUBOOT_PORT_CFG_MCUBOOT_SIZE)
   #else
    #define FLASH_AREA_2_OFFSET          (BSP_FEATURE_FLASH_LP_CF_DUAL_BANK_START + \
                                        RM_MCUBOOT_PORT_CFG_MCUBOOT_SIZE)
   #endif
  #else
   #define FLASH_AREA_2_OFFSET           (FLASH_AREA_0_OFFSET + FLASH_AREA_0_SIZE)
  #endif
 #endif
 #ifndef FLASH_AREA_2_SIZE
  #define FLASH_AREA_2_SIZE             (RM_MCUBOOT_PORT_CFG_PARTITION_SIZE)
 #endif

/* Swap space.  */
 #ifndef FLASH_AREA_SCRATCH_ID
  #define FLASH_AREA_SCRATCH_ID         (FLASH_AREA_IMAGE_SCRATCH)
 #endif
 #ifndef FLASH_AREA_SCRATCH_OFFSET
  #ifdef RM_MCUBOOT_PORT_CFG_SECONDARY_USE_XSPI
   #define FLASH_AREA_SCRATCH_OFFSET     (FLASH_AREA_0_OFFSET + FLASH_AREA_0_SIZE)
  #else
   #define FLASH_AREA_SCRATCH_OFFSET     (FLASH_AREA_2_OFFSET + FLASH_AREA_2_SIZE)
  #endif
 #endif
 #ifndef FLASH_AREA_SCRATCH_SIZE
  #define FLASH_AREA_SCRATCH_SIZE       (RM_MCUBOOT_PORT_CFG_SCRATCH_SIZE)
 #endif

#elif (MCUBOOT_IMAGE_NUMBER == 2)

/* Swap space.  */
 #ifndef FLASH_AREA_SCRATCH_ID
  #define FLASH_AREA_SCRATCH_ID         (FLASH_AREA_IMAGE_SCRATCH)
 #endif
 #ifndef FLASH_AREA_SCRATCH_OFFSET
  #define FLASH_AREA_SCRATCH_OFFSET     (FLASH_AREA_MCUBOOT_OFFSET + RM_MCUBOOT_PORT_CFG_MCUBOOT_SIZE)
 #endif
 #ifndef FLASH_AREA_SCRATCH_SIZE
  #define FLASH_AREA_SCRATCH_SIZE       (RM_MCUBOOT_PORT_CFG_SCRATCH_SIZE)
 #endif

/* Secure image secondary slot */
 #ifndef FLASH_AREA_2_ID
  #define FLASH_AREA_2_ID               (FLASH_AREA_IMAGE_2)
 #endif
 #ifndef FLASH_AREA_2_OFFSET
  #ifdef RM_MCUBOOT_PORT_CFG_SECONDARY_USE_QSPI
   #define FLASH_AREA_2_OFFSET           (BSP_FEATURE_QSPI_DEVICE_START_ADDRESS + XSPI_AREA_MCUBOOT_OFFSET)
  #elif defined(RM_MCUBOOT_PORT_CFG_SECONDARY_USE_OSPI_B)
   #if (RM_MCUBOOT_PORT_CFG_OSPI_B_CHANNEL == 1)
    #define FLASH_AREA_2_OFFSET           (BSP_FEATURE_OSPI_B_DEVICE_1_START_ADDRESS + XSPI_AREA_MCUBOOT_OFFSET)
   #elif (RM_MCUBOOT_PORT_CFG_OSPI_B_CHANNEL == 0)
    #define FLASH_AREA_2_OFFSET           (BSP_FEATURE_OSPI_B_DEVICE_0_START_ADDRESS + XSPI_AREA_MCUBOOT_OFFSET)
   #endif
  #else
   #define FLASH_AREA_2_OFFSET           (FLASH_AREA_SCRATCH_OFFSET + FLASH_AREA_SCRATCH_SIZE)
  #endif
 #endif
 #ifndef FLASH_AREA_2_SIZE
  #define FLASH_AREA_2_SIZE             (RM_MCUBOOT_PORT_CFG_S_PARTITION_SIZE)
 #endif

/* Secure image primary slot */
 #ifndef FLASH_AREA_0_ID
  #define FLASH_AREA_0_ID               (FLASH_AREA_IMAGE_0)
 #endif
 #ifndef FLASH_AREA_0_OFFSET
  #ifdef RM_MCUBOOT_PORT_CFG_SECONDARY_USE_QSPI
   #define FLASH_AREA_0_OFFSET           (FLASH_AREA_SCRATCH_OFFSET + FLASH_AREA_SCRATCH_SIZE)
  #else
   #define FLASH_AREA_0_OFFSET           (FLASH_AREA_2_OFFSET + FLASH_AREA_2_SIZE)
  #endif
 #endif
 #ifndef FLASH_AREA_0_SIZE
  #define FLASH_AREA_0_SIZE             (RM_MCUBOOT_PORT_CFG_S_PARTITION_SIZE)
 #endif

/* Non-Secure image primary slot */
 #ifndef FLASH_AREA_1_ID
  #define FLASH_AREA_1_ID               (FLASH_AREA_IMAGE_1)
 #endif
 #ifndef FLASH_AREA_1_OFFSET
  #define FLASH_AREA_1_OFFSET           (FLASH_AREA_0_OFFSET + FLASH_AREA_0_SIZE) + BSP_FEATURE_TZ_NS_OFFSET
 #endif
 #ifndef FLASH_AREA_1_SIZE
  #define FLASH_AREA_1_SIZE             (RM_MCUBOOT_PORT_CFG_NS_PARTITION_SIZE)
 #endif

/* Non-Secure image secondary slot */
 #ifndef FLASH_AREA_3_ID
  #define FLASH_AREA_3_ID               (FLASH_AREA_IMAGE_3)
 #endif
 #ifndef FLASH_AREA_3_OFFSET
   #ifdef RM_MCUBOOT_PORT_CFG_SECONDARY_USE_XSPI
   #define FLASH_AREA_3_OFFSET           (BSP_FEATURE_QSPI_DEVICE_START_ADDRESS + XSPI_AREA_MCUBOOT_OFFSET + FLASH_AREA_2_SIZE)
   #else
   #define FLASH_AREA_3_OFFSET           (FLASH_AREA_1_OFFSET + FLASH_AREA_1_SIZE)
  #endif
 #endif
 #ifndef FLASH_AREA_3_SIZE
  #define FLASH_AREA_3_SIZE             (RM_MCUBOOT_PORT_CFG_NS_PARTITION_SIZE)
 #endif
#else                                  /* MCUBOOT_IMAGE_NUMBER > 2 */
#error "Only MCUBOOT_IMAGE_NUMBER 1 and 2 are supported!"
#endif /* MCUBOOT_IMAGE_NUMBER */

/* Common macro for FSP header files. There is also a corresponding FSP_HEADER macro at the top of this file. */
FSP_FOOTER

#endif /* __SYSFLASH_H__ */
#endif /* SYSFLASH_H_ */
