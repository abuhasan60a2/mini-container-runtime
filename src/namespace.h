/*
 * namespace.h - Linux namespace isolation for minibox
 *
 * Uses clone(2) with CLONE_NEW* flags to create isolated namespaces
 * for the container process. The child process gets its own PID,
 * mount, network, UTS (hostname), and IPC namespaces.
 *
 * References: clone(2), namespaces(7), pid_namespaces(7)
 */

#ifndef MINIBOX_NAMESPACE_H
#define MINIBOX_NAMESPACE_H

#include <sys/types.h>

/* Stack size for cloned child process: 8 MiB */
#define STACK_SIZE (8 * 1024 * 1024)

/*
 * Configuration passed to the child process after clone().
 * Contains everything the child needs to set itself up.
 */
typedef struct {
    char hostname[256];       /* UTS hostname for the container */
    char rootfs_path[512];    /* Path to the container's rootfs */
    char command[256];        /* Command to execute inside container */
    char container_id[64];    /* Container ID for cgroup/log paths */
    char **env_vars;          /* Environment variables (NULL-terminated array) */
    int env_count;            /* Number of environment variables */
    int log_fd;               /* File descriptor for logging output */
} child_config_t;

/*
 * create_namespaces - Create a new process in isolated namespaces
 *
 * @config: Configuration for the child process
 *
 * Returns: PID of the child process on success, -1 on failure
 *
 * Allocates an 8MiB stack using mmap() and calls clone() with:
 *   CLONE_NEWPID  - New PID namespace (child is PID 1)
 *   CLONE_NEWNS   - New mount namespace (isolated mounts)
 *   CLONE_NEWNET  - New network namespace (no network by default)
 *   CLONE_NEWUTS  - New UTS namespace (own hostname)
 *   CLONE_NEWIPC  - New IPC namespace (isolated shared memory/semaphores)
 *   SIGCHLD       - Parent receives SIGCHLD when child exits
 *
 * The child process runs child_function() which:
 * 1. Sets the hostname
 * 2. Performs pivot_root to change the root filesystem
 * 3. Mounts essential filesystems (proc, sys, dev)
 * 4. Executes the container command
 */
pid_t create_namespaces(child_config_t *config);

/*
 * child_function - Entry point for the cloned child process
 *
 * @arg: Pointer to child_config_t
 *
 * Returns: Exit code (passed back to parent via waitpid)
 *
 * This function runs in the new namespaces and is responsible for:
 * - Setting the container hostname
 * - Calling pivot_root to change root filesystem
 * - Mounting /proc, /sys, /dev
 * - Executing the container command with execvp()
 */
int child_function(void *arg);

/*
 * allocate_stack - Allocate stack memory for clone()
 *
 * Returns: Pointer to the base of the stack (mmap'd memory), or NULL on failure
 *
 * Uses mmap with MAP_ANONYMOUS|MAP_PRIVATE|MAP_STACK for a
 * STACK_SIZE (8MiB) region. The caller must munmap() when done.
 *
 * Note: clone() needs the TOP of the stack (stack + STACK_SIZE)
 * because the stack grows downward on x86/x86_64.
 */
void *allocate_stack(void);

#endif /* MINIBOX_NAMESPACE_H */
