#include "x_json.h"

#include <stdio.h>

static json_object* root;
static json_object* events;

static int tid;

static unsigned int no_tabs;

void perfed_json(const int perfed_pid) {
  tid = perfed_pid;

  root   = json_object_new_object();
  events = json_object_new_array();

  json_object_object_add(root, "displayTimeUnit", json_object_new_string("ns"));
}

void json_enter_call(const call_stack_t* const frame) {
  if (frame->tsc_c != 0.0f) {
    for (unsigned int i = 0u; i < no_tabs; i++) {
      fprintf(stdout, "_");
    }
    fprintf(stdout, "C %16llx %20.2lf %16llx\n", frame->ret->addr, frame->tsc_c, frame->call->addr);
    no_tabs++;
  }
}

void json_exit_call(const call_stack_t* const frame) {
  if (frame->tsc_c != 0.0f) {
    no_tabs--;
    for (unsigned int i = 0u; i < no_tabs; i++) {
      fprintf(stdout, "_");
    }
    fprintf(stdout, "R %16llx %20.2lf \n", frame->ret->addr, frame->tsc_r);
  }
}

void json_reset_call(const double tsc __attribute__((unused))) {
  no_tabs = 0u;
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
