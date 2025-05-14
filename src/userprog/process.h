#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#define ARGS_DELIMITER " "
#define ARGUMENT_MAX 36
#define SIZE_MAX 120
typedef int pid_t;
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);

#endif /* userprog/process.h */
