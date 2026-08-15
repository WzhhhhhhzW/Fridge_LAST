#ifndef DEEPSEEK_BRIDGE_H_
#define DEEPSEEK_BRIDGE_H_

#include "hal_data.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct st_deepseek_bridge_iovec
{
    const uint8_t * p_data;
    uint32_t length;
} deepseek_bridge_iovec_t;

typedef void (*deepseek_bridge_cooperative_hook_t)(void);

void deepseek_bridge_init(void);
void deepseek_bridge_set_uart_availability(bool pc_uart_ready, bool w800_uart_ready);
void deepseek_bridge_set_cooperative_hook(deepseek_bridge_cooperative_hook_t hook);
void deepseek_bridge_process(void);
void deepseek_bridge_pc_write(const char * text);
fsp_err_t deepseek_bridge_server_request(const char * request,
                                         char * response,
                                         size_t response_size,
                                         uint32_t timeout_ms);
bool deepseek_bridge_online(void);
void deepseek_bridge_set_audio_active(bool active);
void deepseek_bridge_set_ota_active(bool active);
fsp_err_t deepseek_bridge_audio_prepare(void);
fsp_err_t deepseek_bridge_ota_prepare(void);
fsp_err_t deepseek_bridge_socket_send_all(const uint8_t * p_data, uint32_t length);
fsp_err_t deepseek_bridge_socket_sendv_all(const deepseek_bridge_iovec_t * p_vectors,
                                           size_t vector_count);
int32_t deepseek_bridge_socket_receive(uint8_t * p_data,
                                       uint32_t length,
                                       uint32_t timeout_ms);
void deepseek_bridge_force_reconnect(void);
void deepseek_bridge_set_vision_slot(uint8_t slot);
void deepseek_bridge_uart7_callback(uart_callback_args_t * p_args);
void deepseek_bridge_w800_callback(uart_callback_args_t * p_args);

#endif /* DEEPSEEK_BRIDGE_H_ */
