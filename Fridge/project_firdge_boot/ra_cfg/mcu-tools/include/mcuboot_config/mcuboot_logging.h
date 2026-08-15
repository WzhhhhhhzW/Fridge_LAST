/* generated configuration header file - do not edit */
#ifndef MCUBOOT_LOGGING_H_
#define MCUBOOT_LOGGING_H_
#ifndef __MCUBOOT_LOGGING_H__
#define __MCUBOOT_LOGGING_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <mcuboot_config/mcuboot_config.h>

#define MCUBOOT_LOG_LEVEL_OFF        0
#define MCUBOOT_LOG_LEVEL_ERROR      1
#define MCUBOOT_LOG_LEVEL_WARNING    2
#define MCUBOOT_LOG_LEVEL_INFO       3
#define MCUBOOT_LOG_LEVEL_DEBUG      4

/*
 * The compiled log level determines the maximum level that can be
 * printed.
 */
#ifndef MCUBOOT_LOG_LEVEL
#define MCUBOOT_LOG_LEVEL           MCUBOOT_LOG_LEVEL_OFF
#else
 #include <stdio.h>
#endif

#define MCUBOOT_LOG_MODULE_DECLARE(domain)  /* ignore */
#define MCUBOOT_LOG_MODULE_REGISTER(domain) /* ignore */

#ifndef MCUBOOT_LOG_ERR
#if MCUBOOT_LOG_LEVEL >= MCUBOOT_LOG_LEVEL_ERROR
  #define MCUBOOT_LOG_ERR(_fmt, ...)           printf("[ERR] " _fmt "\n", ## __VA_ARGS__)
 #else
#define MCUBOOT_LOG_ERR(...)                 IGNORE(__VA_ARGS__)
#endif
#endif

#ifndef MCUBOOT_LOG_WRN
#if MCUBOOT_LOG_LEVEL >= MCUBOOT_LOG_LEVEL_WARNING
  #define MCUBOOT_LOG_WRN(_fmt, ...)    printf("[WRN] " _fmt "\n", ## __VA_ARGS__)
 #else
#define MCUBOOT_LOG_WRN(...)          IGNORE(__VA_ARGS__)
#endif
#endif

#ifndef MCUBOOT_LOG_INF
#if MCUBOOT_LOG_LEVEL >= MCUBOOT_LOG_LEVEL_INFO
  #define MCUBOOT_LOG_INF(_fmt, ...)    printf("[INF] " _fmt "\n", ## __VA_ARGS__)
 #else
#define MCUBOOT_LOG_INF(...)          IGNORE(__VA_ARGS__)
#endif
#endif

#ifndef MCUBOOT_LOG_DBG
#if MCUBOOT_LOG_LEVEL >= MCUBOOT_LOG_LEVEL_DEBUG
  #define MCUBOOT_LOG_DBG(_fmt, ...)    printf("[DBG] " _fmt "\n", ## __VA_ARGS__)
 #else
#define MCUBOOT_LOG_DBG(...)          IGNORE(__VA_ARGS__)
#endif
#endif

#ifdef __cplusplus
}
#endif

#endif                                 /* __MCUBOOT_LOGGING_H__ */
#endif /* MCUBOOT_LOGGING_H_ */
