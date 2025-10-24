#ifndef _X_JSON_
#define _X_JSON_

#include <json-c/json.h>

#include "xed.h"

extern void perfed_json(const int perfed_pid);

extern void json_enter_call(const call_stack_t* const frame);
extern void json_exit_call(const call_stack_t* const frame);
extern void json_reset_call(const double tsc);

extern void json_close(void);

#endif
