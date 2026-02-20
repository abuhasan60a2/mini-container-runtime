/*
 * namespace.c - Linux namespace isolation for minibox
 *
 * Creates isolated namespaces using clone(2). The child process
 * runs in its own PID, mount, network, UTS, and IPC namespaces,
 * providing the core isolation that makes containers work.
 *
 * References:
 *   clone(2)          - Create child in new namespaces
 *   namespaces(7)     - Overview of Linux namespaces
 *   pid_namespaces(7) - PID namespace details
 *   sethostname(2)    - Set UTS hostname
 */

#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "namespace.h"
#include "rootfs.h"
#include "utils.h"

/*
 * child_function - Entry point for the container process
 *
 * This function executes inside the new namespaces created by clone().
 * At this point, the child has:
 *   - Its own PID namespace (it sees itself as PID 1)
 *   - Its own mount namespace (mount changes are private)
 *   - Its own network namespace (empty, no interfaces)
 *   - Its own UTS namespace (can set hostname)
 *   - Its own IPC namespace (isolated shared memory)
 *
 * Steps:
 * 1. Set the container hostname via sethostname()
 * 2. Call do_pivot_root() to change the root filesystem
 * 3. Mount essential filesystems (/proc, /sys, /dev/pts)
 * 4. Parse command and execute with execvp()
 */
int child_function(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    /* Step 1: Set the hostname for this UTS namespace
     * This is what the container sees when it runs `hostname` */
    if (sethostname(config->hostname, strlen(config->hostname)) != 0) {
        fprintf(stderr, "ERROR: sethostname failed: %s\n", strerror(errno));
        return 1;
    }

    log_message(LOG_DEBUG, "Child: hostname set to '%s'", config->hostname);

    /* Step 2: Perform pivot_root to change the root filesystem
     *
     * do_pivot_root does:
     *   1. Make rootfs a mount point (bind mount to self)
     *   2. chdir into rootfs
     *   3. pivot_root(".", ".") to swap root
     *   4. umount old root (MNT_DETACH)
     *
     * After this, '/' points to the container's rootfs and
     * the host filesystem is no longer accessible.
     */
    if (do_pivot_root(config->rootfs_path) != 0) {
        fprintf(stderr, "ERROR: pivot_root failed\n");
        return 1;
    }

    log_message(LOG_DEBUG, "Child: pivot_root completed");

    /* Step 3: Mount essential filesystems inside container
     *
     * /proc    - Process information (needed for ps, top, etc.)
     * /sys     - Kernel/device information
     * /dev/pts - Pseudo-terminal devices
     */
    if (mount_essential_filesystems("/") != 0) {
        fprintf(stderr, "ERROR: Failed to mount essential filesystems\n");
        return 1;
    }

    log_message(LOG_DEBUG, "Child: essential filesystems mounted");

    /* Step 4: Set environment variables */
    clearenv();  /* Start with a clean environment */

    /* Set basic environment */
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("HOME", "/root", 1);
    setenv("TERM", "xterm", 1);
    setenv("HOSTNAME", config->hostname, 1);

    /* Set user-defined environment variables */
    if (config->env_vars) {
        for (int i = 0; i < config->env_count; i++) {
            if (config->env_vars[i]) {
                /* env_vars[i] is in "KEY=VALUE" format */
                putenv(config->env_vars[i]);
            }
        }
    }

    /* Step 5: Parse command string into argv array
     *
     * Split "cmd arg1 arg2" into {"cmd", "arg1", "arg2", NULL}
     * This allows the user to pass commands like "/bin/sh -c 'echo hello'"
     */
    char *cmd_copy = strdup(config->command);
    if (!cmd_copy) {
        fprintf(stderr, "ERROR: strdup failed\n");
        return 1;
    }

    char *argv[64];
    int argc = 0;
    char *token = strtok(cmd_copy, " ");
    while (token && argc < 63) {
        argv[argc++] = token;
        token = strtok(NULL, " ");
    }
    argv[argc] = NULL;

    if (argc == 0) {
        fprintf(stderr, "ERROR: No command specified\n");
        free(cmd_copy);
        return 1;
    }

    /* Step 6: Replace this process with the container command
     *
     * execvp() searches PATH for the command, so /bin/sh works.
     * If execvp succeeds, this code never continues.
     * If it fails, we print an error and exit.
     */
    log_message(LOG_DEBUG, "Child: executing '%s'", config->command);

    execvp(argv[0], argv);

    /* If we get here, execvp failed */
    fprintf(stderr, "ERROR: Failed to execute '%s': %s\n",
           argv[0], strerror(errno));
    free(cmd_copy);
    return 1;
}

/*
 * allocate_stack - Allocate stack for clone() using mmap
 *
 * Returns a pointer to the BASE of the allocated memory region.
 * For clone(), you need to pass (base + STACK_SIZE) because the
 * stack grows downward on x86/x86_64.
 *
 * Using mmap instead of malloc because:
 * - MAP_STACK hints to the kernel about stack usage
 * - Better alignment guarantees
 * - Easier to set up guard pages (not done here for simplicity)
 */
void *allocate_stack(void)
{
    void *stack = mmap(NULL, STACK_SIZE,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK,
                       -1, 0);

    if (stack == MAP_FAILED) {
        log_message(LOG_ERROR, "Failed to allocate stack (%d bytes): %s",
                   STACK_SIZE, strerror(errno));
        return NULL;
    }

    return stack;
}

/*
 * create_namespaces - Create child process in isolated namespaces
 *
 * This is the core of container creation. We:
 * 1. Allocate an 8MiB stack for the child
 * 2. Call clone() with CLONE_NEW* flags to create a new process
 *    in its own namespaces
 * 3. Return the child PID to the parent
 *
 * The clone() flags:
 *   CLONE_NEWPID - Child gets PID 1 in its namespace
 *   CLONE_NEWNS  - Mount changes are isolated
 *   CLONE_NEWNET - No network access initially (set up later)
 *   CLONE_NEWUTS - Can change hostname without affecting host
 *   CLONE_NEWIPC - Isolated System V IPC and POSIX message queues
 *   SIGCHLD      - Send SIGCHLD to parent when child exits
 */
pid_t create_namespaces(child_config_t *config)
{
    if (!config) {
        log_message(LOG_ERROR, "create_namespaces: NULL config");
        return -1;
    }

    /* Step 1: Allocate stack for the child process */
    void *stack = allocate_stack();
    if (!stack) {
        return -1;
    }

    /* Step 2: Define clone flags
     *
     * Each CLONE_NEW* flag creates a new namespace of that type.
     * The child process will have its own isolated view of:
     * - Process IDs (PID)
     * - Mount points (NS/MNT)
     * - Network interfaces (NET)
     * - Hostname (UTS)
     * - IPC resources (IPC)
     */
    int clone_flags = CLONE_NEWPID  |  /* New PID namespace */
                      CLONE_NEWNS   |  /* New mount namespace */
                      CLONE_NEWNET  |  /* New network namespace */
                      CLONE_NEWUTS  |  /* New UTS namespace */
                      CLONE_NEWIPC  |  /* New IPC namespace */
                      SIGCHLD;         /* Notify parent on exit */

    /* Step 3: Create the child process
     *
     * clone() is like fork() but allows specifying which resources
     * to share or isolate. The child starts executing child_function()
     * with config as its argument.
     *
     * The stack pointer needs to point to the TOP of the allocated
     * memory because the stack grows downward on x86/x86_64.
     */
    pid_t child_pid = clone(child_function,
                            (char *)stack + STACK_SIZE,  /* Top of stack */
                            clone_flags,
                            config);                     /* Argument to child_function */

    if (child_pid < 0) {
        log_message(LOG_ERROR, "clone() failed: %s", strerror(errno));
        munmap(stack, STACK_SIZE);
        return -1;
    }

    log_message(LOG_INFO, "Created child process with PID %d in new namespaces", child_pid);

    /*
     * Note: We intentionally don't free the stack here because the child
     * process is still using it. The stack will be reclaimed when the
     * child exits and the parent calls waitpid(). In practice, a small
     * memory leak on the parent side (8MiB per container), which is
     * acceptable for this educational implementation.
     *
     * A production runtime would track the stack and free it in a
     * SIGCHLD handler or after waitpid().
     */

    return child_pid;
}
