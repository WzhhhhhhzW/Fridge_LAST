#ifndef ACCESS_CONTROL_H
#define ACCESS_CONTROL_H

#include "hal_data.h"

fsp_err_t access_control_init(void);
void access_control_process(void);
void access_control_log_status(void);
void access_control_log_list(void);
void access_control_enroll_start(void);
void access_control_enroll_cancel(void);

#endif /* ACCESS_CONTROL_H */
