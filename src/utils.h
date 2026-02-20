/*
 * utils.h - Utility functions for minibox container runtime
 *
 * Provides logging, error handling, and common helper functions
 * used throughout the container runtime.
 */

#ifndef MINIBOX_UTILS_H
#define MINIBOX_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

/* Log levels */
enum log_level {
    LOG_DEBUG = 0,
    LOG_INFO  = 1,
    LOG_WARN  = 2,
    LOG_ERROR = 3
};

/* Global log level - can be set by CLI flags */
extern int g_log_level;

/*
 * log_message - Write a log message with timestamp, level, and format string
 *
 * @level:  Log level (LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR)
 * @format: printf-style format string
 * @...:    Variable arguments for format string
 *
 * Messages below g_log_level are suppressed.
 * Format: [YYYY-MM-DD HH:MM:SS] [LEVEL] Message
 */
void log_message(int level, const char *format, ...);

/*
 * die - Fatal error handler
 *
 * @msg: Descriptive error message
 *
 * Prints error with perror context and exits with EXIT_FAILURE.
 * Use when an unrecoverable error occurs.
 */
void die(const char *msg);

/*
 * generate_container_id - Generate a random 12-character hex container ID
 *
 * Returns: Pointer to static buffer containing the hex ID string.
 *          Caller must copy the result if persistence is needed.
 *
 * Uses /dev/urandom for randomness.
 */
char *generate_container_id(void);

/*
 * check_root_privileges - Verify the process is running as root
 *
 * Returns: 0 if running as root, -1 otherwise
 *
 * Container operations require root for namespace, cgroup,
 * and network manipulation.
 */
int check_root_privileges(void);

/*
 * create_directory_recursive - Create directory and all parents (like mkdir -p)
 *
 * @path: Directory path to create
 *
 * Returns: 0 on success, -1 on failure
 */
int create_directory_recursive(const char *path);

/*
 * write_file - Safely write content to a file
 *
 * @path:    File path to write to
 * @content: Null-terminated string to write
 *
 * Returns: 0 on success, -1 on failure
 *
 * Creates the file if it doesn't exist, truncates if it does.
 */
int write_file(const char *path, const char *content);

/*
 * read_file - Read entire file contents into a buffer
 *
 * @path:   File path to read
 * @buffer: Output buffer (caller must free)
 * @size:   Output: number of bytes read
 *
 * Returns: 0 on success, -1 on failure
 */
int read_file_contents(const char *path, char **buffer, size_t *size);

/*
 * file_exists - Check if a file exists
 *
 * @path: File path to check
 *
 * Returns: 1 if file exists, 0 otherwise
 */
int file_exists(const char *path);

/*
 * validate_name - Check if a name contains only valid characters
 *
 * @name: Name string to validate
 *
 * Returns: 1 if valid (alphanumeric + dash + underscore), 0 otherwise
 */
int validate_name(const char *name);

/*
 * parse_memory_string - Parse human-readable memory size to bytes
 *
 * @str: Memory string (e.g., "256m", "1g", "512k")
 *
 * Returns: Size in bytes, or 0 on parse error
 */
size_t parse_memory_string(const char *str);

#endif /* MINIBOX_UTILS_H */
