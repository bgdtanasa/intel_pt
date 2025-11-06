#include "x_json.h"

#include <stdio.h>

static json_object* root;
static json_object* events;

static int tid;

static signed int no_tabs = -1;

void perfed_json(const int perfed_pid) {
  tid = perfed_pid;

  root   = json_object_new_object();
  events = json_object_new_array();

  json_object_object_add(root, "displayTimeUnit", json_object_new_string("ns"));
}

void json_enter_call(const call_stack_t* const frame) {
  if (frame->tsc_c != 0.0f) {
    no_tabs++;
  }
}

void json_exit_call(const call_stack_t* const frame) {
  if (frame->tsc_c != 0.0f) {
    if ((frame->tsc_r != 0.0f) && (no_tabs >= 0)) {
      for (signed int i = 0; i < no_tabs; i++) {
        fprintf(stdout, "_");
      }
      fprintf(stdout, "C %16llx %20.2lf %16llx\n", frame->ret->addr, frame->tsc_c, frame->call->addr);
      for (signed int i = 0; i < no_tabs; i++) {
        fprintf(stdout, "_");
      }
      fprintf(stdout,
              "R %16llx %20.2lf :: %20.2lf %6llu\n",
              frame->ret->addr,
              frame->tsc_r,
              frame->tsc_r - frame->tsc_c,
              frame->no_insts_r - frame->no_insts_c);
    }
    no_tabs--;
  }
}

void json_reset_call(void) {
  no_tabs = -1;
  fprintf(stdout, "\n");
}

void json_close(void) {
  if (root != NULL) {
    json_object_object_add(root, "traceEvents", events);
    if (json_object_to_file_ext("trace.json", root, JSON_C_TO_STRING_PRETTY) != -1) {

    }
    json_object_put(root);
  }
}
