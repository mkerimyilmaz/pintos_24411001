#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

#define COMMAND_ARGS_DELIMITER " "
#define COMMAND_ARGS_MAX 30
#define COMMAND_LENGTH_MAX 100

typedef int pid_t;

/* Process management functions. */
pid_t process_execute (const char *file_name);
int process_wait (pid_t child_tid);
void process_exit (int status);
void process_activate (void);
void process_init(void);

int process_open (const char *file_name);
int process_write(int fd, const void *buffer, unsigned size);
int process_read (int fd, void *buffer, unsigned length);
void process_close (int fd);
void process_seek (int fd, unsigned position);
int process_file_length (int fd);
int process_file_position (int fd);
void process_close_all(void);

#endif /* USERPROG_PROCESS_H */
