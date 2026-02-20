/*
 * container.h - Container lifecycle management for minibox
 *
 * Defines the core data structures for container state and configuration,
 * and provides functions for creating, starting, stopping, and removing
 * containers. State is persisted to /var/run/minibox/<id>.json.
 */

#ifndef MINIBOX_CONTAINER_H
#define MINIBOX_CONTAINER_H

#include <sys/types.h>
#include <time.h>

/* Container status enum */
typedef enum {
    CONTAINER_CREATED  = 0,
    CONTAINER_RUNNING  = 1,
    CONTAINER_STOPPED  = 2,
    CONTAINER_REMOVING = 3
} container_status_t;

/* Runtime state of a container */
typedef struct {
    char id[64];             /* 12-char hex ID + null */
    char name[256];          /* Human-readable name */
    char image[256];         /* Image/tarball path */
    char command[256];       /* Command to execute inside container */
    pid_t pid;               /* PID of container init process (in host PID ns) */
    container_status_t status;
    time_t created_at;

    /* Network configuration */
    struct {
        char ip[16];         /* Container IP address */
        char gateway[16];    /* Gateway IP */
        char veth_host[32];  /* Host-side veth name */
        char veth_guest[32]; /* Container-side veth name */
        char ports[1024];    /* Port mapping string */
    } network;

    /* Resource limits */
    struct {
        size_t memory_bytes;  /* Memory limit in bytes */
        int cpu_percentage;   /* CPU limit as percentage of one core */
    } resources;

    /* Paths */
    char rootfs_path[512];   /* Path to extracted rootfs */
    char log_path[512];      /* Path to container log file */
    char state_path[512];    /* Path to state JSON file */
} container_t;

/* Configuration passed when creating a new container */
typedef struct {
    char image[256];           /* Image tarball path */
    char command[256];         /* Command to run */
    char name[256];            /* Optional container name */
    int detach;                /* Run in background */
    char port_mappings[1024];  /* "8080:80,9090:90" */
    char volume_mounts[1024];  /* "/host:/container" */
    char env_vars[2048];       /* "KEY1=VAL1,KEY2=VAL2" */
    size_t memory_limit;       /* Memory limit in bytes (0 = default) */
    int cpu_limit;             /* CPU percentage (0 = default) */
} container_config_t;

/* State directory for container runtime */
#define MINIBOX_STATE_DIR  "/var/run/minibox"
#define MINIBOX_LOG_DIR    "/var/log/minibox"
#define MINIBOX_IMAGE_DIR  "/var/lib/minibox/images"

/* Default resource limits */
#define DEFAULT_MEMORY_LIMIT  (256 * 1024 * 1024)  /* 256 MiB */
#define DEFAULT_CPU_LIMIT     50                     /* 50% of one core */

/*
 * container_create - Initialize a new container from configuration
 *
 * @config: Container configuration (image, command, limits, etc.)
 *
 * Returns: Pointer to allocated container_t on success, NULL on failure.
 *          Caller must free with container_free().
 *
 * Steps:
 * 1. Generate unique container ID
 * 2. Apply default resource limits if not specified
 * 3. Create runtime directories
 * 4. Save initial state to disk
 */
container_t *container_create(container_config_t *config);

/*
 * container_start - Start a previously created container
 *
 * @container: Container to start (must be in CREATED state)
 *
 * Returns: 0 on success, -1 on failure
 *
 * Orchestrates the full container startup:
 * 1. Extract rootfs from image tarball
 * 2. Create namespaces (clone)
 * 3. Setup cgroups
 * 4. Setup network
 * 5. Execute the container command
 */
int container_start(container_t *container);

/*
 * container_stop - Stop a running container
 *
 * @id: Container ID to stop
 *
 * Returns: 0 on success, -1 on failure
 *
 * Sends SIGTERM, waits 10s, then SIGKILL if still running.
 * Cleans up cgroups and network resources.
 */
int container_stop(const char *id);

/*
 * container_remove - Remove a stopped container
 *
 * @id: Container ID to remove
 *
 * Returns: 0 on success, -1 on failure
 *
 * Removes rootfs, state file, and log files.
 */
int container_remove(const char *id);

/*
 * container_list - List all known containers
 *
 * @count: Output: number of containers found
 *
 * Returns: Array of container_t pointers. Caller must free each
 *          element and the array itself.
 */
container_t **container_list(int *count);

/*
 * container_save_state - Persist container state to disk as JSON
 *
 * @container: Container whose state to save
 *
 * Returns: 0 on success, -1 on failure
 *
 * Writes to /var/run/minibox/<id>.json
 */
int container_save_state(container_t *container);

/*
 * container_load_state - Load container state from disk
 *
 * @id: Container ID to load
 *
 * Returns: Pointer to allocated container_t, or NULL on failure.
 *          Caller must free with container_free().
 */
container_t *container_load_state(const char *id);

/*
 * container_free - Free a container_t structure
 *
 * @container: Container to free (may be NULL)
 */
void container_free(container_t *container);

/*
 * container_exec - Execute a command in a running container
 *
 * @id:      Container ID
 * @command: Command to execute
 *
 * Returns: 0 on success, -1 on failure
 *
 * Uses setns() to enter the container's namespaces, then exec.
 */
int container_exec(const char *id, const char *command);

/*
 * container_logs - Display logs for a container
 *
 * @id: Container ID
 *
 * Returns: 0 on success, -1 on failure
 */
int container_logs(const char *id);

/*
 * container_status_string - Get human-readable status string
 *
 * @status: Container status enum value
 *
 * Returns: Static string like "created", "running", "stopped", "removing"
 */
const char *container_status_string(container_status_t status);

#endif /* MINIBOX_CONTAINER_H */
