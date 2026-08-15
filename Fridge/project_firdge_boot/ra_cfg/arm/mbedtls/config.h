/* generated configuration header file - do not edit */
#ifndef CONFIG_H_
#define CONFIG_H_
/*-----------------------------------------------------------
 * mbedCrypto Configuration file.
 *----------------------------------------------------------*/

#ifndef RM_PSA_CRYPTO_CFG_H
#define RM_PSA_CRYPTO_CFG_H

#include <stdbool.h>
#include "bsp_api.h"

/* Common macro for FSP header files. There is also a corresponding FSP_FOOTER macro at the end of this file. */
FSP_HEADER

/*** rm_psa_crypto module configurations **/

#define PSA_CRYPTO_CFG_PLAINTEXT_KEY_SUPPORT                (0x01)
#define PSA_CRYPTO_CFG_WRAPPED_KEY_SUPPORT                  (0x02)
#define PSA_CRYPTO_CFG_WRAPPED_AND_PLAINTEXT_KEY_SUPPORT    ((PSA_CRYPTO_CFG_PLAINTEXT_KEY_SUPPORT) | \
                                                                        (PSA_CRYPTO_CFG_WRAPPED_KEY_SUPPORT))

#define PSA_CRYPTO_IS_WRAPPED_SUPPORT_REQUIRED(type)      ((type) & PSA_CRYPTO_CFG_WRAPPED_KEY_SUPPORT)
#define PSA_CRYPTO_IS_PLAINTEXT_SUPPORT_REQUIRED(type)    ((type) & PSA_CRYPTO_CFG_PLAINTEXT_KEY_SUPPORT)

#define PSA_CRYPTO_CFG_AES_FORMAT 3
#define PSA_CRYPTO_CFG_ECC_FORMAT 3
#define PSA_CRYPTO_CFG_RSA_FORMAT 3

#define RM_PSA_CRYPTO_CFG_RSA3K_KEYGEN_ENABLED 0
#define RM_PSA_CRYPTO_CFG_RSA3K_SIGNING_ENABLED 0
#define RM_PSA_CRYPTO_CFG_RSA3K_VERIFICATION_ENABLED 0
#define RM_PSA_CRYPTO_CFG_RSA4K_KEYGEN_ENABLED 0
#define RM_PSA_CRYPTO_CFG_RSA4K_SIGNING_ENABLED 0
#define RM_PSA_CRYPTO_CFG_RSA4K_VERIFICATION_ENABLED 0

/*** end-rm_psa_crypto module configurations **/

#if (defined(_MSC_VER))&&(!defined(_CRT_SECURE_NO_DEPRECATE))
            #define _CRT_SECURE_NO_DEPRECATE 1
            #endif

#if RA_NOT_DEFINED
    #include "mbedtls/tls-config.h"
#endif

#if RA_NOT_DEFINED && RA_NOT_DEFINED
 #define CONFIG_MEDTLS_USE_AFR_MEMORY
#endif

/**
 * \name SECTION: System support
 *
 * This section sets system specific settings.
 * \{
 */

/**
 * \def MBEDTLS_HAVE_ASM
 *
 * The compiler has support for asm().
 *
 * Requires support for asm() in compiler.
 *
 * Used in:
 *      library/aesni.h
 *      library/aria.c
 *      library/bn_mul.h
 *      library/constant_time.c
 *      library/padlock.h
 *
 * Required by:
 *      MBEDTLS_AESCE_C
 *      MBEDTLS_AESNI_C (on some platforms)
 *      MBEDTLS_PADLOCK_C
 *
 * Comment to disable the use of assembly code.
 */
#if 0
            #define MBEDTLS_HAVE_ASM
            #endif

/**
 * \def MBEDTLS_NO_UDBL_DIVISION
 *
 * The platform lacks support for double-width integer division (64-bit
 * division on a 32-bit platform, 128-bit division on a 64-bit platform).
 *
 * Used in:
 *      include/mbedtls/bignum.h
 *      library/bignum.c
 *
 * The bignum code uses double-width division to speed up some operations.
 * Double-width division is often implemented in software that needs to
 * be linked with the program. The presence of a double-width integer
 * type is usually detected automatically through preprocessor macros,
 * but the automatic detection cannot know whether the code needs to
 * and can be linked with an implementation of division for that type.
 * By default division is assumed to be usable if the type is present.
 * Uncomment this option to prevent the use of double-width division.
 *
 * Note that division for the native integer type is always required.
 * Furthermore, a 64-bit type is always required even on a 32-bit
 * platform, but it need not support multiplication or division. In some
 * cases it is also desirable to disable some double-width operations. For
 * example, if double-width division is implemented in software, disabling
 * it can reduce code size in some embedded targets.
 */
#if 0
            #define MBEDTLS_NO_UDBL_DIVISION
            #endif

/**
 * \def MBEDTLS_NO_64BIT_MULTIPLICATION
 *
 * The platform lacks support for 32x32 -> 64-bit multiplication.
 *
 * Used in:
 *      library/poly1305.c
 *
 * Some parts of the library may use multiplication of two unsigned 32-bit
 * operands with a 64-bit result in order to speed up computations. On some
 * platforms, this is not available in hardware and has to be implemented in
 * software, usually in a library provided by the toolchain.
 *
 * Sometimes it is not desirable to have to link to that library. This option
 * removes the dependency of that library on platforms that lack a hardware
 * 64-bit multiplier by embedding a software implementation in Mbed TLS.
 *
 * Note that depending on the compiler, this may decrease performance compared
 * to using the library function provided by the toolchain.
 */
#if 0
            #define MBEDTLS_NO_64BIT_MULTIPLICATION
            #endif

/**
 * \def MBEDTLS_HAVE_SSE2
 *
 * CPU supports SSE2 instruction set.
 *
 * Uncomment if the CPU supports SSE2 (IA-32 specific).
 */
#if 0
            #define MBEDTLS_HAVE_SSE2
            #endif

/**
 * \def MBEDTLS_HAVE_TIME
 *
 * System has time.h and time().
 * The time does not need to be correct, only time differences are used,
 * by contrast with MBEDTLS_HAVE_TIME_DATE
 *
 * Defining MBEDTLS_HAVE_TIME allows you to specify MBEDTLS_PLATFORM_TIME_ALT,
 * MBEDTLS_PLATFORM_TIME_MACRO, MBEDTLS_PLATFORM_TIME_TYPE_MACRO and
 * MBEDTLS_PLATFORM_STD_TIME.
 *
 * Comment if your system does not support time functions.
 *
 * \note If MBEDTLS_TIMING_C is set - to enable the semi-portable timing
 *       interface - timing.c will include time.h on suitable platforms
 *       regardless of the setting of MBEDTLS_HAVE_TIME, unless
 *       MBEDTLS_TIMING_ALT is used. See timing.c for more information.
 */
#if 0
            #define MBEDTLS_HAVE_TIME
            #endif

/**
 * \def MBEDTLS_HAVE_TIME_DATE
 *
 * System has time.h, time(), and an implementation for
 * mbedtls_platform_gmtime_r() (see below).
 * The time needs to be correct (not necessarily very accurate, but at least
 * the date should be correct). This is used to verify the validity period of
 * X.509 certificates.
 *
 * Comment if your system does not have a correct clock.
 *
 * \note mbedtls_platform_gmtime_r() is an abstraction in platform_util.h that
 * behaves similarly to the gmtime_r() function from the C standard. Refer to
 * the documentation for mbedtls_platform_gmtime_r() for more information.
 *
 * \note It is possible to configure an implementation for
 * mbedtls_platform_gmtime_r() at compile-time by using the macro
 * MBEDTLS_PLATFORM_GMTIME_R_ALT.
 */
#if 0
            #define MBEDTLS_HAVE_TIME_DATE
            #endif

/**
 * \def MBEDTLS_PLATFORM_MEMORY
 *
 * Enable the memory allocation layer.
 *
 * By default Mbed TLS uses the system-provided calloc() and free().
 * This allows different allocators (self-implemented or provided) to be
 * provided to the platform abstraction layer.
 *
 * Enabling #MBEDTLS_PLATFORM_MEMORY without the
 * MBEDTLS_PLATFORM_{FREE,CALLOC}_MACROs will provide
 * "mbedtls_platform_set_calloc_free()" allowing you to set an alternative calloc() and
 * free() function pointer at runtime.
 *
 * Enabling #MBEDTLS_PLATFORM_MEMORY and specifying
 * MBEDTLS_PLATFORM_{CALLOC,FREE}_MACROs will allow you to specify the
 * alternate function at compile time.
 *
 * An overview of how the value of mbedtls_calloc is determined:
 *
 * - if !MBEDTLS_PLATFORM_MEMORY
 *     - mbedtls_calloc = calloc
 * - if MBEDTLS_PLATFORM_MEMORY
 *     - if (MBEDTLS_PLATFORM_CALLOC_MACRO AND MBEDTLS_PLATFORM_FREE_MACRO):
 *         - mbedtls_calloc = MBEDTLS_PLATFORM_CALLOC_MACRO
 *     - if !(MBEDTLS_PLATFORM_CALLOC_MACRO AND MBEDTLS_PLATFORM_FREE_MACRO):
 *         - Dynamic setup via mbedtls_platform_set_calloc_free is now possible with a default value MBEDTLS_PLATFORM_STD_CALLOC.
 *         - How is MBEDTLS_PLATFORM_STD_CALLOC handled?
 *         - if MBEDTLS_PLATFORM_NO_STD_FUNCTIONS:
 *             - MBEDTLS_PLATFORM_STD_CALLOC is not set to anything;
 *             - MBEDTLS_PLATFORM_STD_MEM_HDR can be included if present;
 *         - if !MBEDTLS_PLATFORM_NO_STD_FUNCTIONS:
 *             - if MBEDTLS_PLATFORM_STD_CALLOC is present:
 *                 - User-defined MBEDTLS_PLATFORM_STD_CALLOC is respected;
 *             - if !MBEDTLS_PLATFORM_STD_CALLOC:
 *                 - MBEDTLS_PLATFORM_STD_CALLOC = calloc
 *
 *         - At this point the presence of MBEDTLS_PLATFORM_STD_CALLOC is checked.
 *         - if !MBEDTLS_PLATFORM_STD_CALLOC
 *             - MBEDTLS_PLATFORM_STD_CALLOC = uninitialized_calloc
 *
 *         - mbedtls_calloc = MBEDTLS_PLATFORM_STD_CALLOC.
 *
 * Defining MBEDTLS_PLATFORM_CALLOC_MACRO and #MBEDTLS_PLATFORM_STD_CALLOC at the same time is not possible.
 * MBEDTLS_PLATFORM_CALLOC_MACRO and MBEDTLS_PLATFORM_FREE_MACRO must both be defined or undefined at the same time.
 * #MBEDTLS_PLATFORM_STD_CALLOC and #MBEDTLS_PLATFORM_STD_FREE do not have to be defined at the same time, as, if they are used,
 * dynamic setup of these functions is possible. See the tree above to see how are they handled in all cases.
 * An uninitialized #MBEDTLS_PLATFORM_STD_CALLOC always fails, returning a null pointer.
 * An uninitialized #MBEDTLS_PLATFORM_STD_FREE does not do anything.
 *
 * Requires: MBEDTLS_PLATFORM_C
 *
 * Enable this layer to allow use of alternative memory allocators.
 */
#if 1
#define MBEDTLS_PLATFORM_MEMORY
#endif

/**
 * \def MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
 *
 * Do not assign standard functions in the platform layer (e.g. calloc() to
 * MBEDTLS_PLATFORM_STD_CALLOC and printf() to MBEDTLS_PLATFORM_STD_PRINTF)
 *
 * This makes sure there are no linking errors on platforms that do not support
 * these functions. You will HAVE to provide alternatives, either at runtime
 * via the platform_set_xxx() functions or at compile time by setting
 * the MBEDTLS_PLATFORM_STD_XXX defines, or enabling a
 * MBEDTLS_PLATFORM_XXX_MACRO.
 *
 * Requires: MBEDTLS_PLATFORM_C
 *
 * Uncomment to prevent default assignment of standard functions in the
 * platform layer.
 */
#if 0
            #define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
            #endif

/**
 * \def MBEDTLS_PLATFORM_EXIT_ALT
 *
 * MBEDTLS_PLATFORM_XXX_ALT: Uncomment a macro to let Mbed TLS support the
 * function in the platform abstraction layer.
 *
 * Example: In case you uncomment MBEDTLS_PLATFORM_PRINTF_ALT, Mbed TLS will
 * provide a function "mbedtls_platform_set_printf()" that allows you to set an
 * alternative printf function pointer.
 *
 * All these define require MBEDTLS_PLATFORM_C to be defined!
 *
 * \note MBEDTLS_PLATFORM_SNPRINTF_ALT is required on Windows;
 * it will be enabled automatically by check_config.h
 *
 * \warning MBEDTLS_PLATFORM_XXX_ALT cannot be defined at the same time as
 * MBEDTLS_PLATFORM_XXX_MACRO!
 *
 * Requires: MBEDTLS_PLATFORM_TIME_ALT requires MBEDTLS_HAVE_TIME
 *
 * Uncomment a macro to enable alternate implementation of specific base
 * platform function
 */
#if 0
            #define MBEDTLS_PLATFORM_SETBUF_ALT
            #endif
#if 0
            #define MBEDTLS_PLATFORM_EXIT_ALT
            #endif
#if 0
            #define MBEDTLS_PLATFORM_TIME_ALT
            #endif
#if 0
            #define MBEDTLS_PLATFORM_FPRINTF_ALT
            #endif
#if 0
            #define MBEDTLS_PLATFORM_PRINTF_ALT
            #endif
#if 0
            #define MBEDTLS_PLATFORM_SNPRINTF_ALT
            #endif
#if 0
            #define MBEDTLS_PLATFORM_VSNPRINTF_ALT
            #endif
#if 0
            #define MBEDTLS_PLATFORM_NV_SEED_ALT
            #endif
#if 1
#define MBEDTLS_PLATFORM_SETUP_TEARDOWN_ALT
#endif

/** \def MBEDTLS_PSA_CRYPTO_DRIVERS
 *
 * Enable support for the experimental PSA crypto driver interface.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 * \warning This interface is experimental. We intend to maintain backward
 *          compatibility with application code that relies on drivers,
 *          but the driver interfaces may change without notice.
 */
#if 0
            #define MBEDTLS_PSA_CRYPTO_DRIVERS
            #endif

#if 0
            #define MBEDTLS_PLATFORM_MS_TIME_ALT
            #endif

/**
 * \def MBEDTLS_DEPRECATED_WARNING
 *
 * Mark deprecated functions and features so that they generate a warning if
 * used. Functionality deprecated in one version will usually be removed in the
 * next version. You can enable this to help you prepare the transition to a
 * new major version by making sure your code is not using this functionality.
 *
 * This only works with GCC and Clang. With other compilers, you may want to
 * use MBEDTLS_DEPRECATED_REMOVED
 *
 * Uncomment to get warnings on using deprecated functions and features.
 */
#if 0
            #define MBEDTLS_DEPRECATED_WARNING
            #endif

/**
 * \def MBEDTLS_DEPRECATED_REMOVED
 *
 * Remove deprecated functions and features so that they generate an error if
 * used. Functionality deprecated in one version will usually be removed in the
 * next version. You can enable this to help you prepare the transition to a
 * new major version by making sure your code is not using this functionality.
 *
 * Uncomment to get errors on using deprecated functions and features.
 */
#if 1
#define MBEDTLS_DEPRECATED_REMOVED
#endif

/* \} name SECTION: System support */

/**
 * \name SECTION: Mbed TLS feature support
 *
 * This section sets support for features that are or are not needed
 * within the modules that are enabled.
 * \{
 */

/**
 * \def MBEDTLS_TIMING_ALT
 *
 * Uncomment to provide your own alternate implementation for
 * mbedtls_timing_get_timer(), mbedtls_set_alarm(), mbedtls_set/get_delay()
 *
 * Only works if you have MBEDTLS_TIMING_C enabled.
 *
 * You will need to provide a header "timing_alt.h" and an implementation at
 * compile time.
 */
#if 0
            #define MBEDTLS_TIMING_ALT
            #endif

/**
 * \def MBEDTLS_AES_ALT
 *
 * MBEDTLS__MODULE_NAME__ALT: Uncomment a macro to let Mbed TLS use your
 * alternate core implementation of a symmetric crypto, an arithmetic or hash
 * module (e.g. platform specific assembly optimized implementations). Keep
 * in mind that the function prototypes should remain the same.
 *
 * This replaces the whole module. If you only want to replace one of the
 * functions, use one of the MBEDTLS__FUNCTION_NAME__ALT flags.
 *
 * Example: In case you uncomment MBEDTLS_AES_ALT, Mbed TLS will no longer
 * provide the "struct mbedtls_aes_context" definition and omit the base
 * function declarations and implementations. "aes_alt.h" will be included from
 * "aes.h" to include the new function definitions.
 *
 * Uncomment a macro to enable alternate implementation of the corresponding
 * module.
 *
 * \warning   MD5, DES and SHA-1 are considered weak and their
 *            use constitutes a security risk. If possible, we recommend
 *            avoiding dependencies on them, and considering stronger message
 *            digests and ciphers instead.
 *
 */
#if 1
#define MBEDTLS_AES_ALT
#endif
#if 0
            #define MBEDTLS_ARIA_ALT
            #endif
#if 0
            #define MBEDTLS_CAMELLIA_ALT
            #endif
#if 1
#define MBEDTLS_CCM_ALT
#endif
#if 0
            #define MBEDTLS_CHACHA20_ALT
            #endif
#if 0
            #define MBEDTLS_CHACHAPOLY_ALT
            #endif
#if 1
#define MBEDTLS_CMAC_ALT
#endif
#if 0
            #define MBEDTLS_DES_ALT
            #endif
#if 0
            #define MBEDTLS_DHM_ALT
            #endif
#if 0
            #define MBEDTLS_ECJPAKE_ALT
            #endif
#if 1
#define MBEDTLS_GCM_ALT
#endif
#if 0
            #define MBEDTLS_NIST_KW_ALT
            #endif
#if 0
            #define MBEDTLS_MD5_ALT
            #endif
#if 0
            #define MBEDTLS_POLY1305_ALT
            #endif
#if 0
            #define MBEDTLS_RIPEMD160_ALT
            #endif
#if 0
            #define MBEDTLS_RSA_1024_ALT
            #endif
#if 1
#define MBEDTLS_RSA_ALT
#endif
#if 0
            #define MBEDTLS_SHA1_ALT
            #endif
#if 1
#define MBEDTLS_SHA256_ALT
#endif
#if 0
            #define MBEDTLS_SHA512_ALT
            #endif

/*
 * When replacing the elliptic curve module, please consider, that it is
 * implemented with two .c files:
 *      - ecp.c
 *      - ecp_curves.c
 * You can replace them very much like all the other MBEDTLS__MODULE_NAME__ALT
 * macros as described above. The only difference is that you have to make sure
 * that you provide functionality for both .c files.
 */
#if 1
#define MBEDTLS_ECP_ALT
#endif

/**
 * \def MBEDTLS_SHA256_PROCESS_ALT
 *
 * MBEDTLS__FUNCTION_NAME__ALT: Uncomment a macro to let Mbed TLS use you
 * alternate core implementation of symmetric crypto or hash function. Keep in
 * mind that function prototypes should remain the same.
 *
 * This replaces only one function. The header file from Mbed TLS is still
 * used, in contrast to the MBEDTLS__MODULE_NAME__ALT flags.
 *
 * Example: In case you uncomment MBEDTLS_SHA256_PROCESS_ALT, Mbed TLS will
 * no longer provide the mbedtls_sha1_process() function, but it will still provide
 * the other function (using your mbedtls_sha1_process() function) and the definition
 * of mbedtls_sha1_context, so your implementation of mbedtls_sha1_process must be compatible
 * with this definition.
 *
 * \note If you use the AES_xxx_ALT macros, then it is recommended to also set
 *       MBEDTLS_AES_ROM_TABLES in order to help the linker garbage-collect the AES
 *       tables.
 *
 * Uncomment a macro to enable alternate implementation of the corresponding
 * function.
 *
 * \warning   MD5, DES and SHA-1 are considered weak and their use
 *            constitutes a security risk. If possible, we recommend avoiding
 *            dependencies on them, and considering stronger message digests
 *            and ciphers instead.
 *
 * \warning   If both MBEDTLS_ECDSA_SIGN_ALT and MBEDTLS_ECDSA_DETERMINISTIC are
 *            enabled, then the deterministic ECDH signature functions pass the
 *            the static HMAC-DRBG as RNG to mbedtls_ecdsa_sign(). Therefore
 *            alternative implementations should use the RNG only for generating
 *            the ephemeral key and nothing else. If this is not possible, then
 *            MBEDTLS_ECDSA_DETERMINISTIC should be disabled and an alternative
 *            implementation should be provided for mbedtls_ecdsa_sign_det_ext().
 *
 */
#if 0
            #define MBEDTLS_MD5_PROCESS_ALT
            #endif
#if 0
            #define MBEDTLS_RIPEMD160_PROCESS_ALT
            #endif
#if 0
            #define MBEDTLS_SHA1_PROCESS_ALT
            #endif
#if (1)
#define MBEDTLS_SHA256_PROCESS_ALT
#endif
#if 0
            #define MBEDTLS_SHA512_PROCESS_ALT
            #endif
#if 0
            #define MBEDTLS_DES_SETKEY_ALT
            #endif
#if 0
            #define MBEDTLS_DES_CRYPT_ECB_ALT
            #endif
#if 0
            #define MBEDTLS_DES3_CRYPT_ECB_ALT
            #endif
#if 1
#define MBEDTLS_AES_SETKEY_ENC_ALT
#endif
#if 1
#define MBEDTLS_AES_SETKEY_DEC_ALT
#endif
#if 1
#define MBEDTLS_AES_ENCRYPT_ALT
#endif
#if 1
#define MBEDTLS_AES_DECRYPT_ALT
#endif
#if 0
            #define MBEDTLS_ECDH_GEN_PUBLIC_ALT
            #endif
#if 0
            #define MBEDTLS_ECDH_COMPUTE_SHARED_ALT
            #endif
#if 1
#define MBEDTLS_ECDSA_VERIFY_ALT
#endif
#if 1
#define MBEDTLS_ECDSA_SIGN_ALT
#endif
#if 0
            #define MBEDTLS_ECDSA_GENKEY_ALT
            #endif

/**
 * \def MBEDTLS_ECP_INTERNAL_ALT
 *
 * Expose a part of the internal interface of the Elliptic Curve Point module.
 *
 * MBEDTLS_ECP__FUNCTION_NAME__ALT: Uncomment a macro to let Mbed TLS use your
 * alternative core implementation of elliptic curve arithmetic. Keep in mind
 * that function prototypes should remain the same.
 *
 * This partially replaces one function. The header file from Mbed TLS is still
 * used, in contrast to the MBEDTLS_ECP_ALT flag. The original implementation
 * is still present and it is used for group structures not supported by the
 * alternative.
 *
 * The original implementation can in addition be removed by setting the
 * MBEDTLS_ECP_NO_FALLBACK option, in which case any function for which the
 * corresponding MBEDTLS_ECP__FUNCTION_NAME__ALT macro is defined will not be
 * able to fallback to curves not supported by the alternative implementation.
 *
 * Any of these options become available by defining MBEDTLS_ECP_INTERNAL_ALT
 * and implementing the following functions:
 *      unsigned char mbedtls_internal_ecp_grp_capable(
 *          const mbedtls_ecp_group *grp )
 *      int  mbedtls_internal_ecp_init( const mbedtls_ecp_group *grp )
 *      void mbedtls_internal_ecp_free( const mbedtls_ecp_group *grp )
 * The mbedtls_internal_ecp_grp_capable function should return 1 if the
 * replacement functions implement arithmetic for the given group and 0
 * otherwise.
 * The functions mbedtls_internal_ecp_init and mbedtls_internal_ecp_free are
 * called before and after each point operation and provide an opportunity to
 * implement optimized set up and tear down instructions.
 *
 * Example: In case you set MBEDTLS_ECP_INTERNAL_ALT and
 * MBEDTLS_ECP_DOUBLE_JAC_ALT, Mbed TLS will still provide the ecp_double_jac()
 * function, but will use your mbedtls_internal_ecp_double_jac() if the group
 * for the operation is supported by your implementation (i.e. your
 * mbedtls_internal_ecp_grp_capable() function returns 1 for this group). If the
 * group is not supported by your implementation, then the original Mbed TLS
 * implementation of ecp_double_jac() is used instead, unless this fallback
 * behaviour is disabled by setting MBEDTLS_ECP_NO_FALLBACK (in which case
 * ecp_double_jac() will return MBEDTLS_ERR_ECP_FEATURE_UNAVAILABLE).
 *
 * The function prototypes and the definition of mbedtls_ecp_group and
 * mbedtls_ecp_point will not change based on MBEDTLS_ECP_INTERNAL_ALT, so your
 * implementation of mbedtls_internal_ecp__function_name__ must be compatible
 * with their definitions.
 *
 * Uncomment a macro to enable alternate implementation of the corresponding
 * function.
 */
/* Required for all the functions in this section */
#if 0
            #define MBEDTLS_ECP_INTERNAL_ALT
            #endif
/* Turn off software fallback for curves not supported in hardware */
#if 0
            #define MBEDTLS_ECP_NO_FALLBACK
            #endif
/* Support for Weierstrass curves with Jacobi representation */
#if 0
            #define MBEDTLS_ECP_RANDOMIZE_JAC_ALT
            #endif
#if 0
            #define MBEDTLS_ECP_ADD_MIXED_ALT
            #endif
#if 0
            #define MBEDTLS_ECP_DOUBLE_JAC_ALT
            #endif
#if 0
            #define MBEDTLS_ECP_NORMALIZE_JAC_MANY_ALT
            #endif
#if 0
            #define MBEDTLS_ECP_NORMALIZE_JAC_ALT
            #endif
/* Support for curves with Montgomery arithmetic */
#if 0
            #define MBEDTLS_ECP_DOUBLE_ADD_MXZ_ALT
            #endif
#if 0
            #define MBEDTLS_ECP_RANDOMIZE_MXZ_ALT
            #endif
#if 0
            #define MBEDTLS_ECP_NORMALIZE_MXZ_ALT
            #endif

/**
 * \def MBEDTLS_ENTROPY_HARDWARE_ALT
 *
 * Uncomment this macro to let Mbed TLS use your own implementation of a
 * hardware entropy collector.
 *
 * Your function must be called \c mbedtls_hardware_poll(), have the same
 * prototype as declared in library/entropy_poll.h, and accept NULL as first
 * argument.
 *
 * Uncomment to use your own hardware entropy collector.
 */
#if 1
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#endif

/**
 * \def MBEDTLS_AES_ROM_TABLES
 *
 * Use precomputed AES tables stored in ROM.
 *
 * Uncomment this macro to use precomputed AES tables stored in ROM.
 * Comment this macro to generate AES tables in RAM at runtime.
 *
 * Tradeoff: Using precomputed ROM tables reduces RAM usage by ~8kb
 * (or ~2kb if \c MBEDTLS_AES_FEWER_TABLES is used) and reduces the
 * initialization time before the first AES operation can be performed.
 * It comes at the cost of additional ~8kb ROM use (resp. ~2kb if \c
 * MBEDTLS_AES_FEWER_TABLES below is used), and potentially degraded
 * performance if ROM access is slower than RAM access.
 *
 * This option is independent of \c MBEDTLS_AES_FEWER_TABLES.
 */
#if 0
            #define MBEDTLS_AES_ROM_TABLES
            #endif

/**
 * \def MBEDTLS_AES_FEWER_TABLES
 *
 * Use less ROM/RAM for AES tables.
 *
 * Uncommenting this macro omits 75% of the AES tables from
 * ROM / RAM (depending on the value of \c MBEDTLS_AES_ROM_TABLES)
 * by computing their values on the fly during operations
 * (the tables are entry-wise rotations of one another).
 *
 * Tradeoff: Uncommenting this reduces the RAM / ROM footprint
 * by ~6kb but at the cost of more arithmetic operations during
 * runtime. Specifically, one has to compare 4 accesses within
 * different tables to 4 accesses with additional arithmetic
 * operations within the same table. The performance gain/loss
 * depends on the system and memory details.
 *
 * This option is independent of \c MBEDTLS_AES_ROM_TABLES.
 *
 */
#if 0
            #define MBEDTLS_AES_FEWER_TABLES
            #endif

/**
 * \def MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH
 *
 * Use only 128-bit keys in AES operations to save ROM.
 *
 * Uncomment this macro to remove support for AES operations that use 192-
 * or 256-bit keys.
 *
 * Uncommenting this macro reduces the size of AES code by ~300 bytes
 * on v8-M/Thumb2.
 *
 * Module:  library/aes.c
 *
 * Requires: MBEDTLS_AES_C
 */
#if 0
            #define MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH
            #endif

/**
 * \def MBEDTLS_CAMELLIA_SMALL_MEMORY
 *
 * Use less ROM for the Camellia implementation (saves about 768 bytes).
 *
 * Uncomment this macro to use less memory for Camellia.
 */
#if 0
            #define MBEDTLS_CAMELLIA_SMALL_MEMORY
            #endif

/**
 * \def MBEDTLS_CHECK_RETURN_WARNING
 *
 * If this macro is defined, emit a compile-time warning if application code
 * calls a function without checking its return value, but the return value
 * should generally be checked in portable applications.
 *
 * This is only supported on platforms where #MBEDTLS_CHECK_RETURN is
 * implemented. Otherwise this option has no effect.
 *
 * Uncomment to get warnings on using fallible functions without checking
 * their return value.
 *
 * \note  This feature is a work in progress.
 *        Warnings will be added to more functions in the future.
 *
 * \note  A few functions are considered critical, and ignoring the return
 *        value of these functions will trigger a warning even if this
 *        macro is not defined. To completely disable return value check
 *        warnings, define #MBEDTLS_CHECK_RETURN with an empty expansion.
 */
#if 0
            #define MBEDTLS_CHECK_RETURN_WARNING
            #endif

/**
 * \def MBEDTLS_CIPHER_MODE_CBC
 *
 * Enable Cipher Block Chaining mode (CBC) for symmetric ciphers.
 */
#if 1
#define MBEDTLS_CIPHER_MODE_CBC
#endif

/**
 * \def MBEDTLS_CIPHER_MODE_CFB
 *
 * Enable Cipher Feedback mode (CFB) for symmetric ciphers.
 */
#if 1
#define MBEDTLS_CIPHER_MODE_CFB
#endif

/**
 * \def MBEDTLS_CIPHER_MODE_CTR
 *
 * Enable Counter Block Cipher mode (CTR) for symmetric ciphers.
 */
#if 1
#define MBEDTLS_CIPHER_MODE_CTR
#endif

/**
 * \def MBEDTLS_CIPHER_MODE_OFB
 *
 * Enable Output Feedback mode (OFB) for symmetric ciphers.
 */
#if 0
            #define MBEDTLS_CIPHER_MODE_OFB
            #endif

/**
 * \def MBEDTLS_CIPHER_MODE_XTS
 *
 * Enable Xor-encrypt-xor with ciphertext stealing mode (XTS) for AES.
 */
#if 0
            #define MBEDTLS_CIPHER_MODE_XTS
            #endif

/**
 * \def MBEDTLS_CIPHER_NULL_CIPHER
 *
 * Enable NULL cipher.
 * Warning: Only do so when you know what you are doing. This allows for
 * encryption or channels without any security!
 *
 * This module is required to support the TLS ciphersuites that use the NULL
 * cipher.
 *
 * Uncomment this macro to enable the NULL cipher and ciphersuites
 */
#if 0
            #define MBEDTLS_CIPHER_NULL_CIPHER
            #endif

/**
 * \def MBEDTLS_CIPHER_PADDING_PKCS7
 *
 * MBEDTLS_CIPHER_PADDING_XXX: Uncomment or comment macros to add support for
 * specific padding modes in the cipher layer with cipher modes that support
 * padding (e.g. CBC)
 *
 * If you disable all padding modes, only full blocks can be used with CBC.
 *
 * Enable padding modes in the cipher layer.
 */
#if 1
#define MBEDTLS_CIPHER_PADDING_PKCS7
#endif
#if 1
#define MBEDTLS_CIPHER_PADDING_ONE_AND_ZEROS
#endif
#if 1
#define MBEDTLS_CIPHER_PADDING_ZEROS_AND_LEN
#endif
#if 1
#define MBEDTLS_CIPHER_PADDING_ZEROS
#endif
/** \def MBEDTLS_CTR_DRBG_USE_128_BIT_KEY
 *
 * Uncomment this macro to use a 128-bit key in the CTR_DRBG module.
 * Without this, CTR_DRBG uses a 256-bit key
 * unless \c MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH is set.
 */
#if 0
            #define MBEDTLS_CTR_DRBG_USE_128_BIT_KEY
            #endif

/**
 * \def MBEDTLS_ECP_DP_SECP192R1_ENABLED
 *
 * MBEDTLS_ECP_XXXX_ENABLED: Enables specific curves within the Elliptic Curve
 * module.  By default all supported curves are enabled.
 *
 * Comment macros to disable the curve and functions for it
 */
#if 0
            #define MBEDTLS_ECP_DP_SECP192R1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_SECP224R1_ENABLED
            #endif
#if 1
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#endif
#if 0
            #define MBEDTLS_ECP_DP_SECP384R1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_SECP521R1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_SECP192K1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_SECP224K1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_SECP256K1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_BP256R1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_BP384R1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_BP512R1_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_CURVE25519_ENABLED
            #endif
#if 0
            #define MBEDTLS_ECP_DP_CURVE448_ENABLED
            #endif

/**
 * \def MBEDTLS_ECP_NIST_OPTIM
 *
 * Enable specific 'modulo p' routines for each NIST prime.
 * Depending on the prime and architecture, makes operations 4 to 8 times
 * faster on the corresponding curve.
 *
 * Comment this macro to disable NIST curves optimisation.
 */
#if 0
            #define MBEDTLS_ECP_NIST_OPTIM
            #endif

/**
 * \def MBEDTLS_ECP_RESTARTABLE
 *
 * Enable "non-blocking" ECC operations that can return early and be resumed.
 *
 * This allows various functions to pause by returning
 * #MBEDTLS_ERR_ECP_IN_PROGRESS (or, for functions in the SSL module,
 * MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS) and then be called later again in order
 * to further progress and eventually complete their operation. This is
 * controlled through mbedtls_ecp_set_max_ops() which limits the maximum number
 * of ECC operations a function may perform before pausing; see
 * mbedtls_ecp_set_max_ops() for more information.
 *
 * This is useful in non-threaded environments if you want to avoid blocking
 * for too long on ECC (and, hence, X.509 or SSL/TLS) operations.
 *
 * This option:
 * - Adds xxx_restartable() variants of existing operations in the
 *   following modules, with corresponding restart context types:
 *   - ECP (for Short Weierstrass curves only): scalar multiplication (mul),
 *     linear combination (muladd);
 *   - ECDSA: signature generation and verification;
 *   - PK: signature generation and verification;
 *   - X509: certificate chain verification.
 * - Adds mbedtls_ecdh_enable_restart() in the ECDH module.
 * - Changes the behaviour of TLS 1.2 clients (not servers) when using the
 *   ECDHE-ECDSA key exchange (not other key exchanges) to make all ECC
 *   computations restartable:
 *   - ECDH operations from the key exchange, only for Short Weierstrass
 *     curves, only when MBEDTLS_USE_PSA_CRYPTO is not enabled.
 *   - verification of the server's key exchange signature;
 *   - verification of the server's certificate chain;
 *   - generation of the client's signature if client authentication is used,
 *     with an ECC key/certificate.
 *
 * \note  In the cases above, the usual SSL/TLS functions, such as
 *        mbedtls_ssl_handshake(), can now return
 *        MBEDTLS_ERR_SSL_CRYPTO_IN_PROGRESS.
 *
 * \note  When this option and MBEDTLS_USE_PSA_CRYPTO are both enabled,
 *        restartable operations in PK, X.509 and TLS (see above) are not
 *        using PSA. On the other hand, ECDH computations in TLS are using
 *        PSA, and are not restartable. These are temporary limitations that
 *        should be lifted in the future.
 *
 * \note  This option only works with the default software implementation of
 *        elliptic curve functionality. It is incompatible with
 *        MBEDTLS_ECP_ALT, MBEDTLS_ECDH_XXX_ALT, MBEDTLS_ECDSA_XXX_ALT.
 *
 * Requires: MBEDTLS_ECP_C
 *
 * Uncomment this macro to enable restartable ECC computations.
 */
#if 0
            #define MBEDTLS_ECP_RESTARTABLE
            #endif

/**
 * \def MBEDTLS_ECDSA_DETERMINISTIC
 *
 * Enable deterministic ECDSA (RFC 6979).
 * Standard ECDSA is "fragile" in the sense that lack of entropy when signing
 * may result in a compromise of the long-term signing key. This is avoided by
 * the deterministic variant.
 *
 * Requires: MBEDTLS_HMAC_DRBG_C
 *
 * Comment this macro to disable deterministic ECDSA.
 */
#if 0
            #define MBEDTLS_ECDSA_DETERMINISTIC
            #endif

/**
 * \def MBEDTLS_PK_PARSE_EC_EXTENDED
 *
 * Enhance support for reading EC keys using variants of SEC1 not allowed by
 * RFC 5915 and RFC 5480.
 *
 * Currently this means parsing the SpecifiedECDomain choice of EC
 * parameters (only known groups are supported, not arbitrary domains, to
 * avoid validation issues).
 *
 * Disable if you only need to support RFC 5915 + 5480 key formats.
 */
#if 0
            #define MBEDTLS_PK_PARSE_EC_EXTENDED
            #endif

/**
 * \def MBEDTLS_PK_PARSE_EC_COMPRESSED
 *
 * Enable the support for parsing public keys of type Short Weierstrass
 * (MBEDTLS_ECP_DP_SECP_XXX and MBEDTLS_ECP_DP_BP_XXX) which are using the
 * compressed point format. This parsing is done through ECP module's functions.
 *
 * \note As explained in the description of MBEDTLS_ECP_PF_COMPRESSED (in ecp.h)
 *       the only unsupported curves are MBEDTLS_ECP_DP_SECP224R1 and
 *       MBEDTLS_ECP_DP_SECP224K1.
 */
#if 0
            #define MBEDTLS_PK_PARSE_EC_COMPRESSED
            #endif
/**
 * \def MBEDTLS_ERROR_STRERROR_DUMMY
 *
 * Enable a dummy error function to make use of mbedtls_strerror() in
 * third party libraries easier when MBEDTLS_ERROR_C is disabled
 * (no effect when MBEDTLS_ERROR_C is enabled).
 *
 * You can safely disable this if MBEDTLS_ERROR_C is enabled, or if you're
 * not using mbedtls_strerror() or error_strerror() in your application.
 *
 * Disable if you run into name conflicts and want to really remove the
 * mbedtls_strerror()
 */
#if 1
#define MBEDTLS_ERROR_STRERROR_DUMMY
#endif

/**
 * \def MBEDTLS_GENPRIME
 *
 * Enable the prime-number generation code.
 *
 * Requires: MBEDTLS_BIGNUM_C
 */
#if 1
#define MBEDTLS_GENPRIME
#endif

/**
 * \def MBEDTLS_FS_IO
 *
 * Enable functions that use the filesystem.
 */
#if 0
            #define MBEDTLS_FS_IO
            #endif

/**
 * \def MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES
 *
 * Do not add default entropy sources in mbedtls_entropy_init().
 *
 * This is useful to have more control over the added entropy sources in an
 * application.
 *
 * Uncomment this macro to prevent loading of default entropy functions.
 */
#if 0
            #define MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES
            #endif

/**
 * \def MBEDTLS_NO_PLATFORM_ENTROPY
 *
 * Do not use built-in platform entropy functions.
 * This is useful if your platform does not support
 * standards like the /dev/urandom or Windows CryptoAPI.
 *
 * Uncomment this macro to disable the built-in platform entropy functions.
 */
#if 1
#define MBEDTLS_NO_PLATFORM_ENTROPY
#endif

/**
 * \def MBEDTLS_ENTROPY_FORCE_SHA256
 *
 * Force the entropy accumulator to use a SHA-256 accumulator instead of the
 * default SHA-512 based one (if both are available).
 *
 * Requires: MBEDTLS_SHA256_C
 *
 * On 32-bit systems SHA-256 can be much faster than SHA-512. Use this option
 * if you have performance concerns.
 *
 * This option is only useful if both MBEDTLS_SHA256_C and
 * MBEDTLS_SHA512_C are defined. Otherwise the available hash module is used.
 */
#if 0
            #define MBEDTLS_ENTROPY_FORCE_SHA256
            #endif

/**
 * \def MBEDTLS_ENTROPY_NV_SEED
 *
 * Enable the non-volatile (NV) seed file-based entropy source.
 * (Also enables the NV seed read/write functions in the platform layer)
 *
 * This is crucial (if not required) on systems that do not have a
 * cryptographic entropy source (in hardware or kernel) available.
 *
 * Requires: MBEDTLS_ENTROPY_C, MBEDTLS_PLATFORM_C
 *
 * \note The read/write functions that are used by the entropy source are
 *       determined in the platform layer, and can be modified at runtime and/or
 *       compile-time depending on the flags (MBEDTLS_PLATFORM_NV_SEED_*) used.
 *
 * \note If you use the default implementation functions that read a seedfile
 *       with regular fopen(), please make sure you make a seedfile with the
 *       proper name (defined in MBEDTLS_PLATFORM_STD_NV_SEED_FILE) and at
 *       least MBEDTLS_ENTROPY_BLOCK_SIZE bytes in size that can be read from
 *       and written to or you will get an entropy source error! The default
 *       implementation will only use the first MBEDTLS_ENTROPY_BLOCK_SIZE
 *       bytes from the file.
 *
 * \note The entropy collector will write to the seed file before entropy is
 *       given to an external source, to update it.
 */
#if 0
            #define MBEDTLS_ENTROPY_NV_SEED
            #endif

/* MBEDTLS_PSA_CRYPTO_KEY_ID_ENCODES_OWNER
 *
 * Enable key identifiers that encode a key owner identifier.
 *
 * The owner of a key is identified by a value of type ::mbedtls_key_owner_id_t
 * which is currently hard-coded to be int32_t.
 *
 * Note that this option is meant for internal use only and may be removed
 * without notice.
 */

#if 0
            #define MBEDTLS_PSA_CRYPTO_KEY_ID_ENCODES_OWNER
            #endif

/**
 * \def MBEDTLS_MEMORY_DEBUG
 *
 * Enable debugging of buffer allocator memory issues. Automatically prints
 * (to stderr) all (fatal) messages on memory allocation issues. Enables
 * function for 'debug output' of allocated memory.
 *
 * Requires: MBEDTLS_MEMORY_BUFFER_ALLOC_C
 *
 * Uncomment this macro to let the buffer allocator print out error messages.
 */
#if 0
            #define MBEDTLS_MEMORY_DEBUG
            #endif

/**
 * \def MBEDTLS_MEMORY_BACKTRACE
 *
 * Include backtrace information with each allocated block.
 *
 * Requires: MBEDTLS_MEMORY_BUFFER_ALLOC_C
 *           GLIBC-compatible backtrace() and backtrace_symbols() support
 *
 * Uncomment this macro to include backtrace information
 */
#if 0
            #define MBEDTLS_MEMORY_BACKTRACE
            #endif

/**
 * \def MBEDTLS_PK_RSA_ALT_SUPPORT
 *
 * Support external private RSA keys (eg from a HSM) in the PK layer.
 *
 * Comment this macro to disable support for external private RSA keys.
 */
#if 0
            #define MBEDTLS_PK_RSA_ALT_SUPPORT
            #endif

/**
 * \def MBEDTLS_PKCS1_V15
 *
 * Enable support for PKCS#1 v1.5 encoding.
 *
 * Requires: MBEDTLS_RSA_C
 *
 * This enables support for PKCS#1 v1.5 operations.
 */
#if 1
#define MBEDTLS_PKCS1_V15
#endif

/**
 * \def MBEDTLS_PKCS1_V21
 *
 * Enable support for PKCS#1 v2.1 encoding.
 *
 * Requires: MBEDTLS_RSA_C
 *
 * \warning If using a hash that is only provided by PSA drivers, you must
 * call psa_crypto_init() before doing any PKCS#1 v2.1 operation.
 *
 * This enables support for RSAES-OAEP and RSASSA-PSS operations.
 */
#if 1
#define MBEDTLS_PKCS1_V21
#endif

/** \def MBEDTLS_PSA_CRYPTO_BUILTIN_KEYS
 *
 * Enable support for platform built-in keys. If you enable this feature,
 * you must implement the function mbedtls_psa_platform_get_builtin_key().
 * See the documentation of that function for more information.
 *
 * Built-in keys are typically derived from a hardware unique key or
 * stored in a secure element.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C.
 *
 * \warning This interface is experimental and may change or be removed
 * without notice.
 */
#if 0
            #define MBEDTLS_PSA_CRYPTO_BUILTIN_KEYS
            #endif

/** \def PSA_CRYPTO_DRIVER_TFM_BUILTIN_KEY_LOADER
 *
 * Enable support for platform built-in keys for trusted firmware - m
 * bsaed applications.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C.
 */
#if 0
            #define PSA_CRYPTO_DRIVER_TFM_BUILTIN_KEY_LOADER
            #endif

/** \def MBEDTLS_PSA_CRYPTO_CLIENT
 *
 * Enable support for PSA crypto client.
 *
 * \note This option allows to include the code necessary for a PSA
 *       crypto client when the PSA crypto implementation is not included in
 *       the library (MBEDTLS_PSA_CRYPTO_C disabled). The code included is the
 *       code to set and get PSA key attributes.
 *       The development of PSA drivers partially relying on the library to
 *       fulfill the hardware gaps is another possible usage of this option.
 *
 * \warning This interface is experimental and may change or be removed
 * without notice.
 */
#if 0
            #define MBEDTLS_PSA_CRYPTO_CLIENT
            #endif

/** \def MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
 *
 * Make the PSA Crypto module use an external random generator provided
 * by a driver, instead of Mbed TLS's entropy and DRBG modules.
 *
 * \note This random generator must deliver random numbers with cryptographic
 *       quality and high performance. It must supply unpredictable numbers
 *       with a uniform distribution. The implementation of this function
 *       is responsible for ensuring that the random generator is seeded
 *       with sufficient entropy. If you have a hardware TRNG which is slow
 *       or delivers non-uniform output, declare it as an entropy source
 *       with mbedtls_entropy_add_source() instead of enabling this option.
 *
 * If you enable this option, you must configure the type
 * ::mbedtls_psa_external_random_context_t in psa/crypto_platform.h
 * and define a function called mbedtls_psa_external_get_random()
 * with the following prototype:
 * ```
 * psa_status_t mbedtls_psa_external_get_random(
 *     mbedtls_psa_external_random_context_t *context,
 *     uint8_t *output, size_t output_size, size_t *output_length);
 * );
 * ```
 * The \c context value is initialized to 0 before the first call.
 * The function must fill the \c output buffer with \c output_size bytes
 * of random data and set \c *output_length to \c output_size.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 * \warning If you enable this option, code that uses the PSA cryptography
 *          interface will not use any of the entropy sources set up for
 *          the entropy module, nor the NV seed that MBEDTLS_ENTROPY_NV_SEED
 *          enables.
 *
 * \note This option is experimental and may be removed without notice.
 */
#if 0
            #define MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG
            #endif

/**
 * \def MBEDTLS_PSA_CRYPTO_SPM
 *
 * When MBEDTLS_PSA_CRYPTO_SPM is defined, the code is built for SPM (Secure
 * Partition Manager) integration which separates the code into two parts: a
 * NSPE (Non-Secure Process Environment) and an SPE (Secure Process
 * Environment).
 *
 * If you enable this option, your build environment must include a header
 * file `"crypto_spe.h"` (either in the `psa` subdirectory of the Mbed TLS
 * header files, or in another directory on the compiler's include search
 * path). Alternatively, your platform may customize the header
 * `psa/crypto_platform.h`, in which case it can skip or replace the
 * inclusion of `"crypto_spe.h"`.
 *
 * Module:  library/psa_crypto.c
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 */
#if 0
            #define MBEDTLS_PSA_CRYPTO_SPM
            #endif

/**
 * \def MBEDTLS_PSA_KEY_STORE_DYNAMIC
 *
 * Dynamically resize the PSA key store to accommodate any number of
 * volatile keys (until the heap memory is exhausted).
 *
 * If this option is disabled, the key store has a fixed size
 * #MBEDTLS_PSA_KEY_SLOT_COUNT for volatile keys and loaded persistent keys
 * together.
 *
 * This option has no effect when #MBEDTLS_PSA_CRYPTO_C is disabled.
 *
 * Module:  library/psa_crypto.c
 * Requires: MBEDTLS_PSA_CRYPTO_C
 */
#if 0
            #define MBEDTLS_PSA_KEY_STORE_DYNAMIC
            #endif

/**
 * \def MBEDTLS_PSA_INJECT_ENTROPY
 *
 * Enable support for entropy injection at first boot. This feature is
 * required on systems that do not have a built-in entropy source (TRNG).
 * This feature is currently not supported on systems that have a built-in
 * entropy source.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_STORAGE_C, MBEDTLS_ENTROPY_NV_SEED
 *
 */
#if 0
            #define MBEDTLS_PSA_INJECT_ENTROPY
            #endif

/**
 * \def MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS
 *
 * Assume all buffers passed to PSA functions are owned exclusively by the
 * PSA function and are not stored in shared memory.
 *
 * This option may be enabled if all buffers passed to any PSA function reside
 * in memory that is accessible only to the PSA function during its execution.
 *
 * This option MUST be disabled whenever buffer arguments are in memory shared
 * with an untrusted party, for example where arguments to PSA calls are passed
 * across a trust boundary.
 *
 * \note Enabling this option reduces memory usage and code size.
 *
 * \note Enabling this option causes overlap of input and output buffers
 *       not to be supported by PSA functions.
 */
#if 0
            #define MBEDTLS_PSA_ASSUME_EXCLUSIVE_BUFFERS
            #endif

/**
 * \def MBEDTLS_RSA_NO_CRT
 *
 * Do not use the Chinese Remainder Theorem
 * for the RSA private operation.
 *
 * Uncomment this macro to disable the use of CRT in RSA.
 *
 */
#if 1
#define MBEDTLS_RSA_NO_CRT
#endif

/**
 * \def MBEDTLS_SELF_TEST
 *
 * Enable the checkup functions (*_self_test).
 */
#if 0
            #define MBEDTLS_SELF_TEST
            #endif

/**
 * \def MBEDTLS_SHA256_SMALLER
 *
 * Enable an implementation of SHA-256 that has lower ROM footprint but also
 * lower performance.
 *
 * The default implementation is meant to be a reasonnable compromise between
 * performance and size. This version optimizes more aggressively for size at
 * the expense of performance. Eg on Cortex-M4 it reduces the size of
 * mbedtls_sha256_process() from ~2KB to ~0.5KB for a performance hit of about
 * 30%.
 *
 * Uncomment to enable the smaller implementation of SHA256.
 */
#if 0
            #define MBEDTLS_SHA256_SMALLER
            #endif

/**
 * \def MBEDTLS_SHA512_SMALLER
 *
 * Enable an implementation of SHA-512 that has lower ROM footprint but also
 * lower performance.
 *
 * Uncomment to enable the smaller implementation of SHA512.
 */
#if 0
            #define MBEDTLS_SHA512_SMALLER
            #endif

/**
 * \def MBEDTLS_THREADING_ALT
 *
 * Provide your own alternate threading implementation.
 *
 * Requires: MBEDTLS_THREADING_C
 *
 * Uncomment this to allow your own alternate threading implementation.
 */
#if 0
            #define MBEDTLS_THREADING_ALT
            #endif

/**
 * \def MBEDTLS_THREADING_PTHREAD
 *
 * Enable the pthread wrapper layer for the threading layer.
 *
 * Requires: MBEDTLS_THREADING_C
 *
 * Uncomment this to enable pthread mutexes.
 */
#if 0
            #define MBEDTLS_THREADING_PTHREAD
            #endif

/**
 * \def MBEDTLS_USE_PSA_CRYPTO
 *
 * Make the X.509 and TLS libraries use PSA for cryptographic operations as
 * much as possible, and enable new APIs for using keys handled by PSA Crypto.
 *
 * \note Development of this option is currently in progress, and parts of Mbed
 * TLS's X.509 and TLS modules are not ported to PSA yet. However, these parts
 * will still continue to work as usual, so enabling this option should not
 * break backwards compatibility.
 *
 * \warning If you enable this option, you need to call `psa_crypto_init()`
 * before calling any function from the SSL/TLS, X.509 or PK modules, except
 * for the various mbedtls_xxx_init() functions which can be called at any time.
 *
 * \note An important and desirable effect of this option is that it allows
 * PK, X.509 and TLS to take advantage of PSA drivers. For example, enabling
 * this option is what allows use of drivers for ECDSA, ECDH and EC J-PAKE in
 * those modules. However, note that even with this option disabled, some code
 * in PK, X.509, TLS or the crypto library might still use PSA drivers, if it
 * can determine it's safe to do so; currently that's the case for hashes.
 *
 * \note See docs/use-psa-crypto.md for a complete description this option.
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C.
 *
 * Uncomment this to enable internal use of PSA Crypto and new associated APIs.
 */
#if 0
            #define MBEDTLS_USE_PSA_CRYPTO
            #endif

/**
 * \def MBEDTLS_PSA_CRYPTO_CONFIG
 *
 * This setting allows support for cryptographic mechanisms through the PSA
 * API to be configured separately from support through the mbedtls API.
 *
 * When this option is disabled, the PSA API exposes the cryptographic
 * mechanisms that can be implemented on top of the `mbedtls_xxx` API
 * configured with `MBEDTLS_XXX` symbols.
 *
 * When this option is enabled, the PSA API exposes the cryptographic
 * mechanisms requested by the `PSA_WANT_XXX` symbols defined in
 * include/psa/crypto_config.h. The corresponding `MBEDTLS_XXX` settings are
 * automatically enabled if required (i.e. if no PSA driver provides the
 * mechanism). You may still freely enable additional `MBEDTLS_XXX` symbols
 * in mbedtls_config.h.
 *
 * If the symbol #MBEDTLS_PSA_CRYPTO_CONFIG_FILE is defined, it specifies
 * an alternative header to include instead of include/psa/crypto_config.h.
 *
 * \warning This option is experimental, in that the set of `PSA_WANT_XXX`
 *          symbols is not completely finalized yet, and the configuration
 *          tooling is not ideally adapted to having two separate configuration
 *          files.
 *          Future minor releases of Mbed TLS may make minor changes to those
 *          symbols, but we will endeavor to provide a transition path.
 *          Nonetheless, this option is considered mature enough to use in
 *          production, as long as you accept that you may need to make
 *          minor changes to psa/crypto_config.h when upgrading Mbed TLS.
 */
#if 0
            #define MBEDTLS_PSA_CRYPTO_CONFIG
            #endif

/**
 * \def MBEDTLS_VERSION_FEATURES
 *
 * Allow run-time checking of compile-time enabled features. Thus allowing users
 * to check at run-time if the library is for instance compiled with threading
 * support via mbedtls_version_check_feature().
 *
 * Requires: MBEDTLS_VERSION_C
 *
 * Comment this to disable run-time checking and save ROM space
 */
#if 1
#define MBEDTLS_VERSION_FEATURES
#endif

/* \} name SECTION: Mbed TLS feature support */

/**
 * \name SECTION: Mbed TLS modules
 *
 * This section enables or disables entire modules in Mbed TLS
 * \{
 */

/**
 * \def MBEDTLS_AES_C
 *
 * Enable the AES block cipher.
 *
 * Module:  library/aes.c
 * Caller:  library/cipher.c
 *          library/pem.c
 *          library/ctr_drbg.c
 *
 * This module is required to support the TLS ciphersuites that use the AES
 * cipher.
 *
 * PEM_PARSE uses AES for decrypting encrypted keys.
 */
#if 1
#define MBEDTLS_AES_C
#endif

/**
 * \def MBEDTLS_ASN1_PARSE_C
 *
 * Enable the generic ASN1 parser.
 *
 * Module:  library/asn1.c
 * Caller:  library/x509.c
 *          library/dhm.c
 *          library/pkcs12.c
 *          library/pkcs5.c
 *          library/pkparse.c
 */
#if 1
#define MBEDTLS_ASN1_PARSE_C
#endif

/**
 * \def MBEDTLS_ASN1_WRITE_C
 *
 * Enable the generic ASN1 writer.
 *
 * Module:  library/asn1write.c
 * Caller:  library/ecdsa.c
 *          library/pkwrite.c
 *          library/x509_create.c
 *          library/x509write_crt.c
 *          library/x509write_csr.c
 */
#if 1
#define MBEDTLS_ASN1_WRITE_C
#endif

/**
 * \def MBEDTLS_BASE64_C
 *
 * Enable the Base64 module.
 *
 * Module:  library/base64.c
 * Caller:  library/pem.c
 *
 * This module is required for PEM support (required by X.509).
 */
#if 1
#define MBEDTLS_BASE64_C
#endif

/**
 * \def MBEDTLS_BLOCK_CIPHER_NO_DECRYPT
 *
 * Remove decryption operation for AES, ARIA and Camellia block cipher.
 *
 * \note  This feature is incompatible with insecure block cipher,
 *        MBEDTLS_DES_C, and cipher modes which always require decryption
 *        operation, MBEDTLS_CIPHER_MODE_CBC, MBEDTLS_CIPHER_MODE_XTS and
 *        MBEDTLS_NIST_KW_C. When #MBEDTLS_PSA_CRYPTO_CONFIG is enabled,
 *        this feature is incompatible with following supported PSA equivalence,
 *        PSA_WANT_ALG_ECB_NO_PADDING, PSA_WANT_ALG_CBC_NO_PADDING,
 *        PSA_WANT_ALG_CBC_PKCS7 and PSA_WANT_KEY_TYPE_DES.
 *
 * Module:  library/aes.c
 *          library/aesce.c
 *          library/aesni.c
 *          library/aria.c
 *          library/camellia.c
 *          library/cipher.c
 */
#if 0
             #define MBEDTLS_BLOCK_CIPHER_NO_DECRYPT
            #endif

/**
 * \def MBEDTLS_BIGNUM_C
 *
 * Enable the multi-precision integer library.
 *
 * Module:  library/bignum.c
 *          library/bignum_core.c
 *          library/bignum_mod.c
 *          library/bignum_mod_raw.c
 * Caller:  library/dhm.c
 *          library/ecp.c
 *          library/ecdsa.c
 *          library/rsa.c
 *          library/rsa_alt_helpers.c
 *          library/ssl_tls.c
 *
 * This module is required for RSA, DHM and ECC (ECDH, ECDSA) support.
 */
#if 1
#define MBEDTLS_BIGNUM_C
#endif

/**
 * \def MBEDTLS_CAMELLIA_C
 *
 * Enable the Camellia block cipher.
 *
 * Module:  library/camellia.c
 * Caller:  library/cipher.c
 *
 * This module enables the following ciphersuites (if other requisites are
 * enabled as well):
 */
#if 0
            #define MBEDTLS_CAMELLIA_C
            #endif

/**
 * \def MBEDTLS_ARIA_C
 *
 * Enable the ARIA block cipher.
 *
 * Module:  library/aria.c
 * Caller:  library/cipher.c
 *
 * This module enables the following ciphersuites (if other requisites are
 * enabled as well):
 */
#if 0
            #define MBEDTLS_ARIA_C
            #endif

/**
 * \def MBEDTLS_CCM_C
 *
 * Enable the Counter with CBC-MAC (CCM) mode for 128-bit block cipher.
 *
 * Module:  library/ccm.c
 *
 * Requires: MBEDTLS_CIPHER_C, MBEDTLS_AES_C or MBEDTLS_CAMELLIA_C or
 *                             MBEDTLS_ARIA_C
 *
 * This module enables the AES-CCM ciphersuites, if other requisites are
 * enabled as well.
 */
#if 1
#define MBEDTLS_CCM_C
#endif

/**
 * \def MBEDTLS_CHACHA20_C
 *
 * Enable the ChaCha20 stream cipher.
 *
 * Module:  library/chacha20.c
 */
#if 0
            #define MBEDTLS_CHACHA20_C
            #endif

/**
 * \def MBEDTLS_CHACHAPOLY_C
 *
 * Enable the ChaCha20-Poly1305 AEAD algorithm.
 *
 * Module:  library/chachapoly.c
 *
 * This module requires: MBEDTLS_CHACHA20_C, MBEDTLS_POLY1305_C
 */
#if 0
            #define MBEDTLS_CHACHAPOLY_C
            #endif

/**
 * \def MBEDTLS_CIPHER_C
 *
 * Enable the generic cipher layer.
 *
 * Module:  library/cipher.c
 * Caller:  library/ccm.c
 *          library/cmac.c
 *          library/gcm.c
 *          library/nist_kw.c
 *          library/pkcs12.c
 *          library/pkcs5.c
 *          library/psa_crypto_aead.c
 *          library/psa_crypto_mac.c
 *          library/ssl_ciphersuites.c
 *          library/ssl_msg.c
 *          library/ssl_ticket.c (unless MBEDTLS_USE_PSA_CRYPTO is enabled)
 *
 * Uncomment to enable generic cipher wrappers.
 */
#if 1
#define MBEDTLS_CIPHER_C
#endif

#if defined(MBEDTLS_AES_ALT)
#define MBEDTLS_CIPHER_ALT
#endif

/**
 * \def MBEDTLS_CMAC_C
 *
 * Enable the CMAC (Cipher-based Message Authentication Code) mode for block
 * ciphers.
 *
 * \note When #MBEDTLS_CMAC_ALT is active, meaning that the underlying
 *       implementation of the CMAC algorithm is provided by an alternate
 *       implementation, that alternate implementation may opt to not support
 *       AES-192 or 3DES as underlying block ciphers for the CMAC operation.
 *
 * Module:  library/cmac.c
 *
 * Requires: MBEDTLS_CIPHER_C, MBEDTLS_AES_C or MBEDTLS_DES_C
 *
 */
#if 0
            #define MBEDTLS_CMAC_C
            #endif

/**
 * \def MBEDTLS_CTR_DRBG_C
 *
 * Enable the CTR_DRBG AES-based random generator.
 * The CTR_DRBG generator uses AES-256 by default.
 * To use AES-128 instead, enable \c MBEDTLS_CTR_DRBG_USE_128_BIT_KEY above.
 *
 * \note AES-128 will be used if \c MBEDTLS_AES_ONLY_128_BIT_KEY_LENGTH is set.
 *
 * \note To achieve a 256-bit security strength with CTR_DRBG,
 *       you must use AES-256 *and* use sufficient entropy.
 *       See ctr_drbg.h for more details.
 *
 * Module:  library/ctr_drbg.c
 * Caller:
 *
 * Requires: MBEDTLS_AES_C
 *
 * This module provides the CTR_DRBG AES random number generator.
 */
#if 1
#define MBEDTLS_CTR_DRBG_C
#endif

/**
 * \def MBEDTLS_CTR_DRBG_C_ALT
 *
 * Enable the CTR_DRBG AES-based random generator.
 * The CTR_DRBG generator uses software implementation of AES-256.
 *
 * \note To achieve a 256-bit security strength with CTR_DRBG,
 *       you must use AES-256 *and* use sufficient entropy.
 *
 * Module:  ctr_drbg_alt.c
 * Caller:
 *
 *
 * This module provides the CTR_DRBG AES random number generator.
 */
#if 1
#define MBEDTLS_CTR_DRBG_C_ALT
#endif

/**
 * \def MBEDTLS_DES_C
 *
 * Enable the DES block cipher.
 *
 * Module:  library/des.c
 * Caller:  library/pem.c
 *          library/cipher.c
 *
 * PEM_PARSE uses DES/3DES for decrypting encrypted keys.
 *
 * \warning   DES is considered a weak cipher and its use constitutes a
 *            security risk. We recommend considering stronger ciphers instead.
 */
#if 0
            #define MBEDTLS_DES_C
            #endif

/**
 * \def MBEDTLS_DHM_C
 *
 * Enable the Diffie-Hellman-Merkle module.
 *
 * Module:  library/dhm.c
 * Caller:  library/ssl_tls.c
 *          library/ssl*_client.c
 *          library/ssl*_server.c
 *
 * This module is used by the following key exchanges:
 *      DHE-RSA, DHE-PSK
 *
 * \warning    Using DHE constitutes a security risk as it
 *             is not possible to validate custom DH parameters.
 *             If possible, it is recommended users should consider
 *             preferring other methods of key exchange.
 *             See dhm.h for more details.
 *
 */
#if 0
            #define MBEDTLS_DHM_C
            #endif

/**
 * \def MBEDTLS_ECDH_C
 *
 * Enable the elliptic curve Diffie-Hellman library.
 *
 * Module:  library/ecdh.c
 * Caller:  library/ssl_cli.c
 *          library/ssl_srv.c
 *
 * This module is used by the following key exchanges:
 *      ECDHE-ECDSA, ECDHE-RSA, DHE-PSK
 *
 * Requires: MBEDTLS_ECP_C
 */
#if 0
            #define MBEDTLS_ECDH_C
            #if 1
            #define MBEDTLS_ECDH_ALT
            #endif
            #endif

/**
 * \def MBEDTLS_ECDSA_C
 *
 * Enable the elliptic curve DSA library.
 *
 * Module:  library/ecdsa.c
 * Caller:
 *
 * This module is used by the following key exchanges:
 *      ECDHE-ECDSA
 *
 * Requires: MBEDTLS_ECP_C, MBEDTLS_ASN1_WRITE_C, MBEDTLS_ASN1_PARSE_C,
 *           and at least one MBEDTLS_ECP_DP_XXX_ENABLED for a
 *           short Weierstrass curve.
 */
#if 1
#define MBEDTLS_ECDSA_C
#endif

/**
 * \def MBEDTLS_ECJPAKE_C
 *
 * Enable the elliptic curve J-PAKE library.
 *
 * \note EC J-PAKE support is based on the Thread v1.0.0 specification.
 *       It has not been reviewed for compliance with newer standards such as
 *       Thread v1.1 or RFC 8236.
 *
 * Module:  library/ecjpake.c
 * Caller:
 *
 * This module is used by the following key exchanges:
 *      ECJPAKE
 *
 * Requires: MBEDTLS_ECP_C and either MBEDTLS_MD_C or MBEDTLS_PSA_CRYPTO_C
 *
 * \warning If using a hash that is only provided by PSA drivers, you must
 * call psa_crypto_init() before doing any EC J-PAKE operations.
 */
#if 0
            #define MBEDTLS_ECJPAKE_C
            #endif

/**
 * \def MBEDTLS_ECP_C
 *
 * Enable the elliptic curve over GF(p) library.
 *
 * Module:  library/ecp.c
 * Caller:  library/ecdh.c
 *          library/ecdsa.c
 *          library/ecjpake.c
 *
 * Requires: MBEDTLS_BIGNUM_C and at least one MBEDTLS_ECP_DP_XXX_ENABLED
 */
#if 1
#define MBEDTLS_ECP_C
#endif

/**
 * \def MBEDTLS_ENTROPY_C
 *
 * Enable the platform-specific entropy code.
 *
 * Module:  library/entropy.c
 * Caller:
 *
 * Requires: MBEDTLS_SHA512_C or MBEDTLS_SHA256_C
 *
 * This module provides a generic entropy pool
 */
#if 1
#define MBEDTLS_ENTROPY_C
#endif

/**
 * \def MBEDTLS_ERROR_C
 *
 * Enable error code to error string conversion.
 *
 * Module:  library/error.c
 * Caller:
 *
 * This module enables mbedtls_strerror().
 */
#if 1
#define MBEDTLS_ERROR_C
#endif

/**
 * \def MBEDTLS_GCM_C
 *
 * Enable the Galois/Counter Mode (GCM).
 *
 * Module:  library/gcm.c
 *
 * Requires: MBEDTLS_CIPHER_C, MBEDTLS_AES_C or MBEDTLS_CAMELLIA_C or
 *                             MBEDTLS_ARIA_C
 *
 * This module enables the AES-GCM and CAMELLIA-GCM ciphersuites, if other
 * requisites are enabled as well.
 */
#if 1
#define MBEDTLS_GCM_C
#endif

/**
 * \def MBEDTLS_GCM_LARGE_TABLE
 *
 * Enable large pre-computed tables for  Galois/Counter Mode (GCM).
 * Can significantly increase throughput on systems without GCM hardware
 * acceleration (e.g., AESNI, AESCE).
 *
 * The mbedtls_gcm_context size will increase by 3840 bytes.
 * The code size will increase by roughly 344 bytes.
 *
 * Module:  library/gcm.c
 *
 * Requires: MBEDTLS_GCM_C
 */
#if 0
            #define MBEDTLS_GCM_LARGE_TABLE
            #endif

/**
 * \def MBEDTLS_HKDF_C
 *
 * Enable the HKDF algorithm (RFC 5869).
 *
 * Module:  library/hkdf.c
 * Caller:
 *
 * Requires: MBEDTLS_MD_C
 *
 * This module adds support for the Hashed Message Authentication Code
 * (HMAC)-based key derivation function (HKDF).
 */
#if 1
#define MBEDTLS_HKDF_C
#endif

/**
 * \def MBEDTLS_HMAC_DRBG_C
 *
 * Enable the HMAC_DRBG random generator.
 *
 * Module:  library/hmac_drbg.c
 * Caller:
 *
 * Requires: MBEDTLS_MD_C
 *
 * Uncomment to enable the HMAC_DRBG random number generator.
 */
#if 0
            #define MBEDTLS_HMAC_DRBG_C
            #endif

/**
 * \def MBEDTLS_LMS_C
 *
 * Enable the LMS stateful-hash asymmetric signature algorithm.
 *
 * Module:  library/lms.c
 * Caller:
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 * Uncomment to enable the LMS verification algorithm and public key operations.
 */
#define MBEDTLS_LMS_C

/**
 * \def MBEDTLS_LMS_PRIVATE
 *
 * Enable LMS private-key operations and signing code. Functions enabled by this
 * option are experimental, and should not be used in production.
 *
 * Requires: MBEDTLS_LMS_C
 *
 * Uncomment to enable the LMS signature algorithm and private key operations.
 */
//#define MBEDTLS_LMS_PRIVATE
/**
 * \def MBEDTLS_LMS_C
 *
 * Enable the LMS stateful-hash asymmetric signature algorithm.
 *
 * Module:  library/lms.c
 * Caller:
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 * Uncomment to enable the LMS verification algorithm and public key operations.
 */
#if 1
#define MBEDTLS_LMS_C
#endif

/**
 * \def MBEDTLS_LMS_PRIVATE
 *
 * Enable LMS private-key operations and signing code. Functions enabled by this
 * option are experimental, and should not be used in production.
 *
 * Requires: MBEDTLS_LMS_C
 *
 * Uncomment to enable the LMS signature algorithm and private key operations.
 */
#if 0
            #define MBEDTLS_LMS_PRIVATE
            #endif

/**
 * \def MBEDTLS_NIST_KW_C
 *
 * Enable the Key Wrapping mode for 128-bit block ciphers,
 * as defined in NIST SP 800-38F. Only KW and KWP modes
 * are supported. At the moment, only AES is approved by NIST.
 *
 * Module:  library/nist_kw.c
 *
 * Requires: MBEDTLS_AES_C and MBEDTLS_CIPHER_C
 */
#if 0
            #define MBEDTLS_NIST_KW_C
            #endif

/**
 * \def MBEDTLS_MD_C
 *
 * Enable the generic layer for message digest (hashing) and HMAC.
 *
 * Requires: one of: MBEDTLS_MD5_C, MBEDTLS_RIPEMD160_C, MBEDTLS_SHA1_C,
 *                   MBEDTLS_SHA224_C, MBEDTLS_SHA256_C, MBEDTLS_SHA384_C,
 *                   MBEDTLS_SHA512_C, or MBEDTLS_PSA_CRYPTO_C with at least
 *                   one hash.
 * Module:  library/md.c
 * Caller:  library/constant_time.c
 *          library/ecdsa.c
 *          library/ecjpake.c
 *          library/hkdf.c
 *          library/hmac_drbg.c
 *          library/pk.c
 *          library/pkcs5.c
 *          library/pkcs12.c
 *          library/psa_crypto_ecp.c
 *          library/psa_crypto_rsa.c
 *          library/rsa.c
 *          library/ssl_cookie.c
 *          library/ssl_msg.c
 *          library/ssl_tls.c
 *          library/x509.c
 *          library/x509_crt.c
 *          library/x509write_crt.c
 *          library/x509write_csr.c
 *
 * Uncomment to enable generic message digest wrappers.
 */
#if 1
#define MBEDTLS_MD_C
#endif

/**
 * \def MBEDTLS_MD5_C
 *
 * Enable the MD5 hash algorithm.
 *
 * Module:  library/md5.c
 * Caller:  library/md.c
 *          library/pem.c
 *          library/ssl_tls.c
 *
 * This module is required for TLS 1.2 depending on the handshake parameters.
 * Further, it is used for checking MD5-signed certificates, and for PBKDF1
 * when decrypting PEM-encoded encrypted keys.
 *
 * \warning   MD5 is considered a weak message digest and its use constitutes a
 *            security risk. If possible, we recommend avoiding dependencies on
 *            it, and considering stronger message digests instead.
 *
 */
#if 1
#define MBEDTLS_MD5_C
#endif

/**
 * \def MBEDTLS_MEMORY_BUFFER_ALLOC_C
 *
 * Enable the buffer allocator implementation that makes use of a (stack)
 * based buffer to 'allocate' dynamic memory. (replaces calloc() and free()
 * calls)
 *
 * Module:  library/memory_buffer_alloc.c
 *
 * Requires: MBEDTLS_PLATFORM_C
 *           MBEDTLS_PLATFORM_MEMORY (to use it within Mbed TLS)
 *
 * Enable this module to enable the buffer memory allocator.
 */
#if 1
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C
#endif

/**
 * \def MBEDTLS_OID_C
 *
 * Enable the OID database.
 *
 * Module:  library/oid.c
 * Caller:  library/asn1write.c
 *          library/pkcs5.c
 *          library/pkparse.c
 *          library/pkwrite.c
 *          library/rsa.c
 *          library/x509.c
 *          library/x509_create.c
 *          library/x509_crl.c
 *          library/x509_crt.c
 *          library/x509_csr.c
 *          library/x509write_crt.c
 *          library/x509write_csr.c
 *
 * This modules translates between OIDs and internal values.
 */
#if 1
#define MBEDTLS_OID_C
#endif

/**
 * \def MBEDTLS_PEM_PARSE_C
 *
 * Enable PEM decoding / parsing.
 *
 * Module:  library/pem.c
 * Caller:  library/dhm.c
 *          library/pkparse.c
 *          library/x509_crl.c
 *          library/x509_crt.c
 *          library/x509_csr.c
 *
 * Requires: MBEDTLS_BASE64_C
 *           optionally MBEDTLS_MD5_C, or PSA Crypto with MD5 (see below)
 *
 * \warning When parsing password-protected files, if MD5 is provided only by
 * a PSA driver, you must call psa_crypto_init() before the first file.
 *
 * This modules adds support for decoding / parsing PEM files.
 */
#if 1
#define MBEDTLS_PEM_PARSE_C
#endif

/**
 * \def MBEDTLS_PEM_WRITE_C
 *
 * Enable PEM encoding / writing.
 *
 * Module:  library/pem.c
 * Caller:  library/pkwrite.c
 *          library/x509write_crt.c
 *          library/x509write_csr.c
 *
 * Requires: MBEDTLS_BASE64_C
 *
 * This modules adds support for encoding / writing PEM files.
 */
#if 1
#define MBEDTLS_PEM_WRITE_C
#endif

/**
 * \def MBEDTLS_PK_C
 *
 * Enable the generic public (asymmetric) key layer.
 *
 * Module:  library/pk.c
 * Caller:  library/psa_crypto_rsa.c
 *          library/ssl_tls.c
 *          library/ssl*_client.c
 *          library/ssl*_server.c
 *          library/x509.c
 *
 * Requires: MBEDTLS_MD_C, MBEDTLS_RSA_C or MBEDTLS_ECP_C
 *
 * Uncomment to enable generic public key wrappers.
 */
#if 1
#define MBEDTLS_PK_C
#endif

/**
 * \def MBEDTLS_PK_PARSE_C
 *
 * Enable the generic public (asymmetric) key parser.
 *
 * Module:  library/pkparse.c
 * Caller:  library/x509_crt.c
 *          library/x509_csr.c
 *
 * Requires: MBEDTLS_PK_C
 *
 * Uncomment to enable generic public key parse functions.
 */
#if 1
#define MBEDTLS_PK_PARSE_C
#endif

/**
 * \def MBEDTLS_PK_WRITE_C
 *
 * Enable the generic public (asymmetric) key writer.
 *
 * Module:  library/pkwrite.c
 * Caller:  library/x509write.c
 *
 * Requires: MBEDTLS_PK_C
 *
 * Uncomment to enable generic public key write functions.
 */
#if 1
#define MBEDTLS_PK_WRITE_C
#endif

/**
 * \def MBEDTLS_PKCS5_C
 *
 * Enable PKCS#5 functions.
 *
 * Module:  library/pkcs5.c
 *
 * Requires: MBEDTLS_CIPHER_C
 * Auto-enables: MBEDTLS_MD_C
 *
 * \warning If using a hash that is only provided by PSA drivers, you must
 * call psa_crypto_init() before doing any PKCS5 operations.
 *
 * This module adds support for the PKCS#5 functions.
 */
#if 1
#define MBEDTLS_PKCS5_C
#endif

/**
 * \def MBEDTLS_PKCS7_C
 *
 * This feature is a work in progress and not ready for production. Testing and
 * validation is incomplete, and handling of malformed inputs may not be robust.
 * The API may change.
 *
 * Enable PKCS7 core for using PKCS7 formatted signatures.
 * RFC Link - https://tools.ietf.org/html/rfc2315
 *
 * Module:  library/pkcs7.c
 *
 * Requires: MBEDTLS_ASN1_PARSE_C, MBEDTLS_OID_C, MBEDTLS_PK_PARSE_C,
 *           MBEDTLS_X509_CRT_PARSE_C MBEDTLS_X509_CRL_PARSE_C,
 *           MBEDTLS_BIGNUM_C, MBEDTLS_MD_C
 *
 * This module is required for the PKCS7 parsing modules.
 */
#if 0
            #define MBEDTLS_PKCS7_C
            #endif

/**
 * \def MBEDTLS_PKCS12_C
 *
 * Enable PKCS#12 PBE functions.
 * Adds algorithms for parsing PKCS#8 encrypted private keys
 *
 * Module:  library/pkcs12.c
 * Caller:  library/pkparse.c
 *
 * Requires: MBEDTLS_ASN1_PARSE_C, MBEDTLS_CIPHER_C and either
 * MBEDTLS_MD_C or MBEDTLS_PSA_CRYPTO_C.
 *
 * \warning If using a hash that is only provided by PSA drivers, you must
 * call psa_crypto_init() before doing any PKCS12 operations.
 *
 * This module enables PKCS#12 functions.
 */
#if 1
#define MBEDTLS_PKCS12_C
#endif

/**
 * \def MBEDTLS_PLATFORM_C
 *
 * Enable the platform abstraction layer that allows you to re-assign
 * functions like calloc(), free(), snprintf(), printf(), fprintf(), exit().
 *
 * Enabling MBEDTLS_PLATFORM_C enables to use of MBEDTLS_PLATFORM_XXX_ALT
 * or MBEDTLS_PLATFORM_XXX_MACRO directives, allowing the functions mentioned
 * above to be specified at runtime or compile time respectively.
 *
 * \note This abstraction layer must be enabled on Windows (including MSYS2)
 * as other modules rely on it for a fixed snprintf implementation.
 *
 * Module:  library/platform.c
 * Caller:  Most other .c files
 *
 * This module enables abstraction of common (libc) functions.
 */
#if 1
#define MBEDTLS_PLATFORM_C
#endif

/**
 * \def MBEDTLS_POLY1305_C
 *
 * Enable the Poly1305 MAC algorithm.
 *
 * Module:  library/poly1305.c
 * Caller:  library/chachapoly.c
 */
#if 0
            #define MBEDTLS_POLY1305_C
            #endif

/**
 * \def MBEDTLS_PSA_CRYPTO_C
 *
 * Enable the Platform Security Architecture cryptography API.
 *
 * Module:  library/psa_crypto.c
 *
 * Requires: MBEDTLS_CIPHER_C,
 *           either MBEDTLS_CTR_DRBG_C and MBEDTLS_ENTROPY_C,
 *           or MBEDTLS_HMAC_DRBG_C and MBEDTLS_ENTROPY_C,
 *           or MBEDTLS_PSA_CRYPTO_EXTERNAL_RNG.
 *
 */
#if 1
#define MBEDTLS_PSA_CRYPTO_C
#endif

/**
 * \def MBEDTLS_PSA_CRYPTO_SE_C
 *
 * Enable dynamic secure element support in the Platform Security Architecture
 * cryptography API.
 *
 * \deprecated This feature is deprecated. Please switch to the PSA driver
 *             interface.
 *
 * Module:  library/psa_crypto_se.c
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C, MBEDTLS_PSA_CRYPTO_STORAGE_C
 *
 */
#if 0
            #define MBEDTLS_PSA_CRYPTO_SE_C
            #endif

/**
 * \def MBEDTLS_PSA_CRYPTO_ACCEL_DRV_C
 *
 * Enable the crypto accelerator support in the Platform Security Architecture
 * cryptography API.
 *
 * \warning This must be defined to use wrapped keys.
 * This must not be underfined even if PSA_CRYPTO_IS_WRAPPED_SUPPORT_REQUIRED()
 * is false for all keys because if the user then specifies vendor-type lifetime
 * the plaintext code will be invoked.
 *
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C
 *
 */

#define MBEDTLS_PSA_CRYPTO_ACCEL_DRV_C

/**
 * \def MBEDTLS_PSA_CRYPTO_STORAGE_C
 *
 * Enable the Platform Security Architecture persistent key storage.
 *
 * Module:  library/psa_crypto_storage.c
 *
 * Requires: MBEDTLS_PSA_CRYPTO_C,
 *           either MBEDTLS_PSA_ITS_FILE_C or a native implementation of
 *           the PSA ITS interface
 */
#if 0
            #define MBEDTLS_PSA_CRYPTO_STORAGE_C
            #endif

/**
 * \def MBEDTLS_PSA_ITS_FILE_C
 *
 * Enable the emulation of the Platform Security Architecture
 * Internal Trusted Storage (PSA ITS) over files.
 *
 * Module:  library/psa_its_file.c
 *
 * Requires: MBEDTLS_FS_IO
 */
#if 0
            #define MBEDTLS_PSA_ITS_FILE_C
            #endif

/**
 * \def MBEDTLS_RIPEMD160_C
 *
 * Enable the RIPEMD-160 hash algorithm.
 *
 * Module:  library/ripemd160.c
 * Caller:  library/md.c
 *
 */
#if 0
            #define MBEDTLS_RIPEMD160_C
            #endif

/**
 * \def MBEDTLS_RSA_C
 *
 * Enable the RSA public-key cryptosystem.
 *
 * Module:  library/rsa.c
 *          library/rsa_alt_helpers.c
 * Caller:  library/pk.c
 *          library/psa_crypto.c
 *          library/ssl_tls.c
 *          library/ssl*_client.c
 *          library/ssl*_server.c
 *
 * This module is used by the following key exchanges:
 *      RSA, DHE-RSA, ECDHE-RSA, RSA-PSK
 *
 * Requires: MBEDTLS_BIGNUM_C, MBEDTLS_OID_C
 */
#if 1
#define MBEDTLS_RSA_C
#endif

/**
 * \def MBEDTLS_SHA1_C
 *
 * Enable the SHA1 cryptographic hash algorithm.
 *
 * Module:  library/sha1.c
 * Caller:  library/md.c
 *          library/psa_crypto_hash.c
 *
 * This module is required for TLS 1.2 depending on the handshake parameters,
 * and for SHA1-signed certificates.
 *
 * \warning   SHA-1 is considered a weak message digest and its use constitutes
 *            a security risk. If possible, we recommend avoiding dependencies
 *            on it, and considering stronger message digests instead.
 *
 */
#if 1
#define MBEDTLS_SHA1_C
#endif

/**
 * \def MBEDTLS_SHA224_C
 *
 * Enable the SHA-224 cryptographic hash algorithm.
 *
 * Module:  library/sha256.c
 * Caller:  library/md.c
 *          library/ssl_cookie.c
 *
 * This module adds support for SHA-224.
 */
#if 1
#define MBEDTLS_SHA224_C
#endif

/**
 * \def MBEDTLS_SHA256_C
 *
 * Enable the SHA-256 cryptographic hash algorithm.
 *
 * Module:  library/sha256.c
 * Caller:  library/entropy.c
 *          library/md.c
 *          library/ssl_cli.c
 *          library/ssl_srv.c
 *          library/ssl_tls.c
 *          library/ssl*_client.c
 *          library/ssl*_server.c
 *
 * This module adds support for SHA-256.
 * This module is required for the SSL/TLS 1.2 PRF function.
 */
#if 1
#define MBEDTLS_SHA256_C
#endif

/**
 * \def MBEDTLS_SHA384_C
 *
 * Enable the SHA-384 cryptographic hash algorithm.
 *
 * Module:  library/sha512.c
 * Caller:  library/md.c
 *          library/psa_crypto_hash.c
 *          library/ssl_tls.c
 *          library/ssl*_client.c
 *          library/ssl*_server.c
 *
 * Comment to disable SHA-384
 */
#if 0
            #define MBEDTLS_SHA384_C
            #endif

/**
 * \def MBEDTLS_SHA512_C
 *
 * Enable SHA-512 cryptographic hash algorithms.
 *
 * Module:  library/sha512.c
 * Caller:  library/entropy.c
 *          library/md.c
 *          library/ssl_tls.c
 *          library/ssl_cookie.c
 *
 * This module adds support for SHA-512.
 */
#if 0
            #define MBEDTLS_SHA512_C
            #endif

/**
 * \def MBEDTLS_SHA3_C
 *
 * Enable the SHA3 cryptographic hash algorithm.
 *
 * Module:  library/sha3.c
 *
 * This module adds support for SHA3.
 */
#if 0
            #define MBEDTLS_SHA3_C
            #endif

/**
 * \def MBEDTLS_THREADING_C
 *
 * Enable the threading abstraction layer.
 * By default Mbed TLS assumes it is used in a non-threaded environment or that
 * contexts are not shared between threads. If you do intend to use contexts
 * between threads, you will need to enable this layer to prevent race
 * conditions. See also our Knowledge Base article about threading:
 * https://mbed-tls.readthedocs.io/en/latest/kb/development/thread-safety-and-multi-threading
 *
 * Module:  library/threading.c
 *
 * This allows different threading implementations (self-implemented or
 * provided).
 *
 * You will have to enable either MBEDTLS_THREADING_ALT or
 * MBEDTLS_THREADING_PTHREAD.
 *
 * Enable this layer to allow use of mutexes within Mbed TLS
 */
#if 0
            #define MBEDTLS_THREADING_C
            #endif

/**
 * \def MBEDTLS_TIMING_C
 *
 * Enable the semi-portable timing interface.
 *
 * \note The provided implementation only works on POSIX/Unix (including Linux,
 * BSD and OS X) and Windows. On other platforms, you can either disable that
 * module and provide your own implementations of the callbacks needed by
 * \c mbedtls_ssl_set_timer_cb() for DTLS, or leave it enabled and provide
 * your own implementation of the whole module by setting
 * \c MBEDTLS_TIMING_ALT in the current file.
 *
 * \note The timing module will include time.h on suitable platforms
 *       regardless of the setting of MBEDTLS_HAVE_TIME, unless
 *       MBEDTLS_TIMING_ALT is used. See timing.c for more information.
 *
 * \note See also our Knowledge Base article about porting to a new
 * environment:
 * https://mbed-tls.readthedocs.io/en/latest/kb/how-to/how-do-i-port-mbed-tls-to-a-new-environment-OS
 *
 * Module:  library/timing.c
 */
#if 0
            #define MBEDTLS_TIMING_C
            #endif

/**
 * \def MBEDTLS_VERSION_C
 *
 * Enable run-time version information.
 *
 * Module:  library/version.c
 *
 * This module provides run-time version information.
 */
#if 1
#define MBEDTLS_VERSION_C
#endif

/* \} name SECTION: Mbed TLS modules */

/**
 * \name SECTION: General configuration options
 *
 * This section contains Mbed TLS build settings that are not associated
 * with a particular module.
 *
 * \{
 */

/** \} name SECTION: General configuration options */

/**
 * \name SECTION: Module configuration options
 *
 * This section allows for the setting of module specific sizes and
 * configuration options. The default values are already present in the
 * relevant header files and should suffice for the regular use cases.
 *
 * Our advice is to enable options and change their values here
 * only if you have a good reason and know the consequences.
 * \{
 */
/* The Doxygen documentation here is used when a user comments out a
 * setting and runs doxygen themselves. On the other hand, when we typeset
 * the full documentation including disabled settings, the documentation
 * in specific modules' header files is used if present. When editing this
 * file, make sure that each option is documented in exactly one place,
 * plus optionally a same-line Doxygen comment here if there is a Doxygen
 * comment in the specific module. */

/* MPI / BIGNUM options */
#if 0
            #define MBEDTLS_MPI_WINDOW_SIZE            6/** Maximum windows size used. */
            #endif
#if 0
            #define MBEDTLS_MPI_MAX_SIZE            1024/** Maximum number of bytes for usable MPIs. */
            #endif

/* CTR_DRBG options */
#if 0
            #define MBEDTLS_CTR_DRBG_ENTROPY_LEN               48/** Amount of entropy used per seed by default (48 with SHA-512, 32 with SHA-256) */
            #endif
#if 0
            #define MBEDTLS_CTR_DRBG_RESEED_INTERVAL        10000/** Interval before reseed is performed by default */
            #endif
#if 0
            #define MBEDTLS_CTR_DRBG_MAX_INPUT                256/** Maximum number of additional input bytes */
            #endif
#if 0
            #define MBEDTLS_CTR_DRBG_MAX_REQUEST             1024/** Maximum number of requested bytes per call */
            #endif
#if 0
            #define MBEDTLS_CTR_DRBG_MAX_SEED_INPUT           384/** Maximum size of (re)seed buffer */
            #endif

/* HMAC_DRBG options */
#if 0
            #define MBEDTLS_HMAC_DRBG_RESEED_INTERVAL   10000/** Interval before reseed is performed by default */
            #endif
#if 0
            #define MBEDTLS_HMAC_DRBG_MAX_INPUT           256/** Maximum number of additional input bytes */
            #endif
#if 0
            #define MBEDTLS_HMAC_DRBG_MAX_REQUEST        1024/** Maximum number of requested bytes per call */
            #endif
#if 0
            #define MBEDTLS_HMAC_DRBG_MAX_SEED_INPUT      384/** Maximum size of (re)seed buffer */
            #endif

/* ECP options */
#if 0
            #define MBEDTLS_ECP_WINDOW_SIZE            6/** Maximum window size used */
            #endif
#if 0
            #define MBEDTLS_ECP_FIXED_POINT_OPTIM      1/** Enable fixed-point speed-up */
            #endif

/* Entropy options */
#if 0
            #define MBEDTLS_ENTROPY_MAX_SOURCES                20/** Maximum number of sources supported */
            #endif
#if 0
            #define MBEDTLS_ENTROPY_MAX_GATHER                128/** Maximum amount requested from entropy sources */
            #endif
#if 0
            #define MBEDTLS_ENTROPY_MIN_HARDWARE               32/** Default minimum number of bytes required for the hardware entropy source mbedtls_hardware_poll() before entropy is released */
            #endif

/* Memory buffer allocator options */
#if 0
            #define MBEDTLS_MEMORY_ALIGN_MULTIPLE      4/** Align on multiples of this value */
            #endif

/* Platform options */
/** \def MBEDTLS_PLATFORM_STD_CALLOC
 *
 * Default allocator to use, can be undefined.
 * It must initialize the allocated buffer memory to zeroes.
 * The size of the buffer is the product of the two parameters.
 * The calloc function returns either a null pointer or a pointer to the allocated space.
 * If the product is 0, the function may either return NULL or a valid pointer to an array of size 0 which is a valid input to the deallocation function.
 * An uninitialized #MBEDTLS_PLATFORM_STD_CALLOC always fails, returning a null pointer.
 * See the description of #MBEDTLS_PLATFORM_MEMORY for more details.
 * The corresponding deallocation function is #MBEDTLS_PLATFORM_STD_FREE.
 */
#if 0
            #define MBEDTLS_PLATFORM_STD_CALLOC        calloc/** Default allocator to use, can be undefined */
            #endif
/** \def MBEDTLS_PLATFORM_STD_FREE
 *
 * Default free to use, can be undefined.
 * NULL is a valid parameter, and the function must do nothing.
 * A non-null parameter will always be a pointer previously returned by #MBEDTLS_PLATFORM_STD_CALLOC and not yet freed.
 * An uninitialized #MBEDTLS_PLATFORM_STD_FREE does not do anything.
 * See the description of #MBEDTLS_PLATFORM_MEMORY for more details (same principles as for MBEDTLS_PLATFORM_STD_CALLOC apply).
 */
#if 0
            #define MBEDTLS_PLATFORM_STD_FREE            free/** Default free to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_SETBUF            setbuf/** Default exit to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_EXIT            exit/** Default exit to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_TIME            time/** Default time to use, can be undefined. MBEDTLS_HAVE_TIME must be enabled */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_FPRINTF      fprintf/** Default fprintf to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_PRINTF        printf/** Default printf to use, can be undefined */
            #endif
/* Note: your snprintf must correctly zero-terminate the buffer! */
#if 0
            #define MBEDTLS_PLATFORM_STD_SNPRINTF    snprintf/** Default snprintf to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_EXIT_SUCCESS       0/** Default exit value to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_EXIT_FAILURE       1/** Default exit value to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_NV_SEED_READ   mbedtls_platform_std_nv_seed_read/** Default nv_seed_read function to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_NV_SEED_WRITE  mbedtls_platform_std_nv_seed_write/** Default nv_seed_write function to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_STD_NV_SEED_FILE  /** Seed file to read/write with default implementation */
            #endif

/* To use the following function macros, MBEDTLS_PLATFORM_C must be enabled. */
/* MBEDTLS_PLATFORM_XXX_MACRO and MBEDTLS_PLATFORM_XXX_ALT cannot both be defined */
#if 0
            #define MBEDTLS_PLATFORM_CALLOC_MACRO        calloc/** Default allocator macro to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_FREE_MACRO            free/** Default free macro to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_EXIT_MACRO            exit/** Default exit macro to use, can be undefined */
            #endif
#if 1
extern void dummy_setbuf(void *stream, char *buf);
#define MBEDTLS_PLATFORM_SETBUF_MACRO          dummy_setbuf/** Default exit macro to use, can be undefined */
#endif
#if 0
            #define MBEDTLS_PLATFORM_TIME_MACRO            time/** Default time macro to use, can be undefined. MBEDTLS_HAVE_TIME must be enabled */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_FPRINTF_MACRO      fprintf/** Default fprintf macro to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_PRINTF_MACRO        printf/** Default printf macro to use, can be undefined */
            #endif
/* Note: your snprintf must correctly zero-terminate the buffer! */
#if 0
            #define MBEDTLS_PLATFORM_SNPRINTF_MACRO    snprintf/** Default snprintf macro to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_VSNPRINTF_MACRO    vsnprintf/** Default vsnprintf macro to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_NV_SEED_READ_MACRO   mbedtls_platform_std_nv_seed_read/** Default nv_seed_read function to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_NV_SEED_WRITE_MACRO  mbedtls_platform_std_nv_seed_write/** Default nv_seed_write function to use, can be undefined */
            #endif
#if 0
            #define MBEDTLS_PLATFORM_MS_TIME_TYPE_MACRO       int64_t /** Default milliseconds time macro to use, can be undefined. MBEDTLS_HAVE_TIME must be enabled. It must be signed, and at least 64 bits. If it is changed from the default, MBEDTLS_PRINTF_MS_TIME must be updated to match.*/
            #endif
/** Default milliseconds time macro to use, can be undefined. MBEDTLS_HAVE_TIME must be enabled. It must be signed, and at least 64 bits. If it is changed from the default, MBEDTLS_PRINTF_MS_TIME must be updated to match.*/
#if 0
            #define MBEDTLS_PRINTF_MS_TIME       PRId64 /** Default fmt for printf. That's avoid compiler warning if mbedtls_ms_time_t is redefined */
            #endif

/** \def MBEDTLS_CHECK_RETURN
 *
 * This macro is used at the beginning of the declaration of a function
 * to indicate that its return value should be checked. It should
 * instruct the compiler to emit a warning or an error if the function
 * is called without checking its return value.
 *
 * There is a default implementation for popular compilers in platform_util.h.
 * You can override the default implementation by defining your own here.
 *
 * If the implementation here is empty, this will effectively disable the
 * checking of functions' return values.
 */
#if 1
#define MBEDTLS_CHECK_RETURN __attribute__((__warn_unused_result__))
#endif

/** \def MBEDTLS_IGNORE_RETURN
 *
 * This macro requires one argument, which should be a C function call.
 * If that function call would cause a #MBEDTLS_CHECK_RETURN warning, this
 * warning is suppressed.
 */
#if 0
            #define MBEDTLS_IGNORE_RETURN( result ) ((void) !(result))
            #endif

/* PSA options */
/**
 * Use HMAC_DRBG with the specified hash algorithm for HMAC_DRBG for the
 * PSA crypto subsystem.
 *
 * If this option is unset:
 * - If CTR_DRBG is available, the PSA subsystem uses it rather than HMAC_DRBG.
 * - Otherwise, the PSA subsystem uses HMAC_DRBG with either
 *   #MBEDTLS_MD_SHA512 or #MBEDTLS_MD_SHA256 based on availability and
 *   on unspecified heuristics.
 */
#if 1
#define MBEDTLS_PSA_HMAC_DRBG_MD_TYPE MBEDTLS_MD_SHA256
#endif

/** \def MBEDTLS_PSA_KEY_SLOT_COUNT
 * Restrict the PSA library to supporting a maximum amount of simultaneously
 * loaded keys. A loaded key is a key stored by the PSA Crypto core as a
 * volatile key, or a persistent key which is loaded temporarily by the
 * library as part of a crypto operation in flight.
 *
 * If this option is unset, the library will fall back to a default value of
 * 32 keys.
 */
#if 0
            #define MBEDTLS_PSA_KEY_SLOT_COUNT     32/** Maximum number of simultaneous loaded keys. */
            #endif

/**
 * Uncomment the macro to let mbed TLS use your alternate implementation of
 * mbedtls_platform_zeroize(). This replaces the default implementation in
 * platform_util.c.
 *
 * mbedtls_platform_zeroize() is a widely used function across the library to
 * zero a block of memory. The implementation is expected to be secure in the
 * sense that it has been written to prevent the compiler from removing calls
 * to mbedtls_platform_zeroize() as part of redundant code elimination
 * optimizations. However, it is difficult to guarantee that calls to
 * mbedtls_platform_zeroize() will not be optimized by the compiler as older
 * versions of the C language standards do not provide a secure implementation
 * of memset(). Therefore, MBEDTLS_PLATFORM_ZEROIZE_ALT enables users to
 * configure their own implementation of mbedtls_platform_zeroize(), for
 * example by using directives specific to their compiler, features from newer
 * C standards (e.g using memset_s() in C11) or calling a secure memset() from
 * their system (e.g explicit_bzero() in BSD).
 */
#if 0
            #define MBEDTLS_PLATFORM_ZEROIZE_ALT
            #endif

/**
 * Uncomment the macro to let Mbed TLS use your alternate implementation of
 * mbedtls_platform_gmtime_r(). This replaces the default implementation in
 * platform_util.c.
 *
 * gmtime() is not a thread-safe function as defined in the C standard. The
 * library will try to use safer implementations of this function, such as
 * gmtime_r() when available. However, if Mbed TLS cannot identify the target
 * system, the implementation of mbedtls_platform_gmtime_r() will default to
 * using the standard gmtime(). In this case, calls from the library to
 * gmtime() will be guarded by the global mutex mbedtls_threading_gmtime_mutex
 * if MBEDTLS_THREADING_C is enabled. We recommend that calls from outside the
 * library are also guarded with this mutex to avoid race conditions. However,
 * if the macro MBEDTLS_PLATFORM_GMTIME_R_ALT is defined, Mbed TLS will
 * unconditionally use the implementation for mbedtls_platform_gmtime_r()
 * supplied at compile time.
 */
#if 0
            #define MBEDTLS_PLATFORM_GMTIME_R_ALT
            #endif

/**
 * Enable the verified implementations of ECDH primitives from Project Everest
 * (currently only Curve25519). This feature changes the layout of ECDH
 * contexts and therefore is a compatibility break for applications that access
 * fields of a mbedtls_ecdh_context structure directly. See also
 * MBEDTLS_ECDH_LEGACY_CONTEXT in include/mbedtls/ecdh.h.
 */
#if 0
            #define MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED
            #endif

/* RSA OPTIONS */

#if 0
            #define MBEDTLS_RSA_GEN_KEY_MIN_BITS       1024 /** Minimum RSA key size that can be generated in bits (Minimum possible value is 128 bits) */
            #endif

/** \} name SECTION: Module configuration options */

#if defined (MBEDTLS_PSA_CRYPTO_STORAGE_C) && defined (MBEDTLS_PSA_ITS_FILE_C)
            #ifdef MBEDTLS_PSA_ITS_CUSTOM_HEADER
            #include MBEDTLS_PSA_ITS_CUSTOM_HEADER
            #elif defined (MBEDTLS_PSA_CRYPTO_STORAGE_C) && defined (MBEDTLS_PSA_ITS_FILE_C) && defined (MBEDTLS_FS_IO)
            #include "rm_stdio_littlefs.h"
            #endif
            #endif

/* Common macro for FSP header files. There is also a corresponding FSP_HEADER macro at the top of this file. */
FSP_FOOTER

#endif /* RM_PSA_CRYPTO_CFG_H */
#endif /* CONFIG_H_ */
