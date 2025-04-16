#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

#define ARGS_DELIMITER " "
#define ARGUMENT_MAX 36
#define SIZE_MAX 120
typedef int pid_t;

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



void connector_ini(void);
int connector_read(char *pipe_name, int ticket);
void connector_write(char *pipe_name, int ticket, int message);

#endif
