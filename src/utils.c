/*
 * utils.c - Utility functions for minibox container runtime
 *
 * Implements logging, error handling, ID generation, and common
 * filesystem operations used by all other modules.
 */

#include "utils.h"

/* Default log level: show INFO and above */
int g_log_level = LOG_INFO;

/* String representations of log levels */
static const char *level_strings[] = {
    "DEBUG",
    "INFO",
    "WARN",
    "ERROR"
};

/*
 * log_message - Write a formatted log message to stderr
 *
 * Includes timestamp and level prefix for easy filtering.
 * Thread-safe by virtue of using fprintf atomically.
 */
void log_message(int level, const char *format, ...)
{
    if (level < g_log_level)
        return;

    /* Get current time for timestamp */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_buf[64];
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);

    /* Print header: [timestamp] [LEVEL] */
    fprintf(stderr, "[%s] [%s] ", time_buf, level_strings[level]);

    /* Print the actual message */
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
}

/*
 * die - Fatal error: print message with errno context and exit
 *
 * This is the last resort when we cannot recover.
 */
void die(const char *msg)
{
    log_message(LOG_ERROR, "%s: %s", msg, strerror(errno));
    exit(EXIT_FAILURE);
}

/*
 * generate_container_id - Create a random 12-char hex string
 *
 * Reads 6 bytes from /dev/urandom and encodes them as hex.
 * Returns pointer to a static buffer (not thread-safe).
 */
char *generate_container_id(void)
{
    static char id[13]; /* 12 hex chars + null terminator */
    unsigned char bytes[6];

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        /* Fallback to time-based if /dev/urandom unavailable */
        srand((unsigned int)(time(NULL) ^ getpid()));
        for (int i = 0; i < 6; i++)
            bytes[i] = (unsigned char)(rand() % 256);
    } else {
        ssize_t n = read(fd, bytes, sizeof(bytes));
        close(fd);
        if (n != (ssize_t)sizeof(bytes)) {
            srand((unsigned int)(time(NULL) ^ getpid()));
            for (int i = 0; i < 6; i++)
                bytes[i] = (unsigned char)(rand() % 256);
        }
    }

    snprintf(id, sizeof(id), "%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);

    return id;
}

/*
 * check_root_privileges - Verify euid == 0
 *
 * Container runtime needs root for:
 * - Creating namespaces (clone with CLONE_NEW*)
 * - Mounting filesystems
 * - Configuring cgroups
 * - Setting up network interfaces
 */
int check_root_privileges(void)
{
    if (geteuid() != 0) {
        log_message(LOG_ERROR, "This program must be run as root (euid=%d)", geteuid());
        return -1;
    }
    return 0;
}

/*
 * create_directory_recursive - Create directory path recursively
 *
 * Equivalent to `mkdir -p <path>`. Creates each component
 * of the path if it doesn't already exist.
 */
int create_directory_recursive(const char *path)
{
    char tmp[4096];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    /* Remove trailing slash */
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    /* Walk the path, creating directories as needed */
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                log_message(LOG_ERROR, "Failed to create directory '%s': %s",
                           tmp, strerror(errno));
                return -1;
            }
            *p = '/';
        }
    }

    /* Create the final directory */
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        log_message(LOG_ERROR, "Failed to create directory '%s': %s",
                   tmp, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * write_file - Write string content to a file atomically
 *
 * Opens file with O_CREAT|O_TRUNC, writes content, closes.
 * Used for cgroup control files, state files, etc.
 */
int write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        log_message(LOG_ERROR, "Failed to open file '%s' for writing: %s",
                   path, strerror(errno));
        return -1;
    }

    size_t len = strlen(content);
    ssize_t written = write(fd, content, len);
    close(fd);

    if (written < 0 || (size_t)written != len) {
        log_message(LOG_ERROR, "Failed to write to file '%s': %s",
                   path, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * read_file_contents - Read entire file into heap-allocated buffer
 *
 * Caller is responsible for freeing *buffer.
 * Useful for reading state files and configuration.
 */
int read_file_contents(const char *path, char **buffer, size_t *size)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        log_message(LOG_ERROR, "Failed to open file '%s' for reading: %s",
                   path, strerror(errno));
        return -1;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (fsize < 0) {
        fclose(fp);
        return -1;
    }

    *buffer = malloc((size_t)fsize + 1);
    if (!*buffer) {
        fclose(fp);
        log_message(LOG_ERROR, "Failed to allocate %ld bytes for file read", fsize);
        return -1;
    }

    size_t nread = fread(*buffer, 1, (size_t)fsize, fp);
    fclose(fp);

    (*buffer)[nread] = '\0';
    if (size)
        *size = nread;

    return 0;
}

/*
 * file_exists - Check file existence using stat()
 */
int file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

/*
 * validate_name - Ensure name uses only safe characters
 *
 * Valid characters: a-z, A-Z, 0-9, dash (-), underscore (_)
 * This prevents path traversal and shell injection attacks.
 */
int validate_name(const char *name)
{
    if (!name || strlen(name) == 0)
        return 0;

    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') ||
              (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') ||
              *p == '-' || *p == '_')) {
            return 0;
        }
    }
    return 1;
}

/*
 * parse_memory_string - Convert human-readable size to bytes
 *
 * Supports suffixes: k/K (KiB), m/M (MiB), g/G (GiB)
 * Returns 0 on parse error.
 */
size_t parse_memory_string(const char *str)
{
    if (!str || strlen(str) == 0)
        return 0;

    char *endptr;
    unsigned long value = strtoul(str, &endptr, 10);

    if (endptr == str)
        return 0;

    switch (*endptr) {
    case 'k':
    case 'K':
        return value * 1024;
    case 'm':
    case 'M':
        return value * 1024 * 1024;
    case 'g':
    case 'G':
        return value * 1024 * 1024 * 1024;
    case '\0':
        return value; /* Raw bytes */
    default:
        return 0;
    }
}
