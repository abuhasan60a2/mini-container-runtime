/*
 * cgroup.c - cgroup v2 resource management for minibox
 *
 * Implements resource limiting via the Linux cgroup v2 unified hierarchy.
 * Each container gets its own cgroup under /sys/fs/cgroup/minibox/<id>/
 * with memory and CPU limits applied.
 *
 * How cgroup v2 works:
 * - Single unified hierarchy (unlike v1's multiple hierarchies)
 * - Controllers enabled per-subtree via cgroup.subtree_control
 * - Processes assigned by writing PID to cgroup.procs
 * - Limits set by writing to controller-specific files (memory.max, cpu.max)
 *
 * References:
 *   cgroups(7)            - Linux control groups overview
 *   cgroup_namespaces(7)  - cgroup namespace isolation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#include <dirent.h>

#include "cgroup.h"
#include "utils.h"

/*
 * verify_cgroup_v2 - Verify that cgroup v2 is available
 *
 * cgroup v2 is identified by the presence of cgroup.controllers
 * in the root cgroup directory. cgroup v1 uses separate hierarchies
 * per controller (memory, cpu, etc.), while v2 uses a single tree.
 */
int verify_cgroup_v2(void)
{
    if (!file_exists("/sys/fs/cgroup/cgroup.controllers")) {
        log_message(LOG_ERROR, "cgroup v2 not available. "
                   "Ensure your kernel supports cgroup v2 and it's mounted at /sys/fs/cgroup/");
        return -1;
    }

    log_message(LOG_DEBUG, "cgroup v2 verified at /sys/fs/cgroup/");
    return 0;
}

/*
 * enable_controllers - Enable cpu and memory controllers for the minibox subtree
 *
 * For cgroup v2, controllers must be explicitly enabled for child cgroups
 * by writing to cgroup.subtree_control in the parent.
 */
static int enable_controllers(void)
{
    /* Enable controllers in root cgroup for minibox subtree */
    const char *subtree_control = "/sys/fs/cgroup/cgroup.subtree_control";

    /* Try to enable memory and cpu controllers
     * This may fail if controllers are already enabled or not available,
     * which is acceptable - we'll still try to use them */
    write_file(subtree_control, "+memory +cpu");

    /* Also enable in the minibox parent cgroup */
    char minibox_subtree[256];
    snprintf(minibox_subtree, sizeof(minibox_subtree),
             "%s/cgroup.subtree_control", CGROUP_BASE_PATH);
    write_file(minibox_subtree, "+memory +cpu");

    return 0;
}

/*
 * setup_cgroup - Create the cgroup directory for a container
 *
 * Creates /sys/fs/cgroup/minibox/<container_id>/ and enables
 * the memory and cpu controllers for it.
 */
int setup_cgroup(const char *container_id)
{
    if (!container_id) return -1;

    /* Step 1: Verify cgroup v2 is available */
    if (verify_cgroup_v2() != 0) {
        return -1;
    }

    /* Step 2: Create the minibox parent cgroup if needed */
    if (create_directory_recursive(CGROUP_BASE_PATH) != 0) {
        log_message(LOG_ERROR, "Failed to create cgroup base directory: %s",
                   CGROUP_BASE_PATH);
        return -1;
    }

    /* Step 3: Enable controllers */
    enable_controllers();

    /* Step 4: Create the container-specific cgroup directory
     *
     * mkdir() creates the cgroup - the kernel automatically populates
     * it with control files (cgroup.procs, memory.max, cpu.max, etc.)
     */
    char cgroup_path[256];
    snprintf(cgroup_path, sizeof(cgroup_path),
             "%s/%s", CGROUP_BASE_PATH, container_id);

    if (mkdir(cgroup_path, 0755) != 0 && errno != EEXIST) {
        log_message(LOG_ERROR, "Failed to create cgroup directory '%s': %s",
                   cgroup_path, strerror(errno));
        return -1;
    }

    log_message(LOG_INFO, "Created cgroup at %s", cgroup_path);
    return 0;
}

/*
 * add_pid_to_cgroup - Move a process into the container's cgroup
 *
 * Writing a PID to cgroup.procs moves that process (and all its
 * threads) into the cgroup. All resource limits then apply to it.
 *
 * This is called by the parent AFTER clone() returns the child PID.
 */
int add_pid_to_cgroup(const char *container_id, pid_t pid)
{
    if (!container_id || pid <= 0) return -1;

    char path[512];
    snprintf(path, sizeof(path),
             "%s/%s/cgroup.procs", CGROUP_BASE_PATH, container_id);

    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", pid);

    if (write_file(path, pid_str) != 0) {
        log_message(LOG_ERROR, "Failed to add PID %d to cgroup %s",
                   pid, container_id);
        return -1;
    }

    log_message(LOG_DEBUG, "Added PID %d to cgroup %s", pid, container_id);
    return 0;
}

/*
 * set_memory_limit - Write memory limit to memory.max
 *
 * memory.max accepts:
 * - A number in bytes (e.g., "268435456" for 256MB)
 * - "max" for unlimited (the default)
 *
 * When a cgroup exceeds its memory limit:
 * 1. The kernel tries to reclaim memory (swap, drop caches)
 * 2. If that fails, the OOM killer selects and kills a process
 * 3. The killed process receives SIGKILL
 */
int set_memory_limit(const char *container_id, size_t bytes)
{
    if (!container_id) return -1;

    char path[512];
    snprintf(path, sizeof(path),
             "%s/%s/memory.max", CGROUP_BASE_PATH, container_id);

    char value[64];
    snprintf(value, sizeof(value), "%zu", bytes);

    if (write_file(path, value) != 0) {
        log_message(LOG_ERROR, "Failed to set memory limit for %s to %zu bytes",
                   container_id, bytes);
        return -1;
    }

    log_message(LOG_INFO, "Set memory limit for %s: %zu bytes (%.1f MiB)",
               container_id, bytes, (double)bytes / (1024.0 * 1024.0));
    return 0;
}

/*
 * set_cpu_limit - Write CPU limit to cpu.max
 *
 * cpu.max format: "quota period" (both in microseconds)
 * - period: How often the limit is enforced (typically 100ms = 100000us)
 * - quota: How many microseconds of CPU time per period
 *
 * Examples:
 * - 50% of one core: "50000 100000"  (50ms out of every 100ms)
 * - 100% of one core: "100000 100000" (the full period)
 * - 25% of one core: "25000 100000"  (25ms out of every 100ms)
 *
 * Note: This limits to a percentage of ONE core, not total CPU.
 */
int set_cpu_limit(const char *container_id, int percentage)
{
    if (!container_id) return -1;

    /* Clamp percentage to valid range */
    if (percentage < 1) percentage = 1;
    if (percentage > 100) percentage = 100;

    char path[512];
    snprintf(path, sizeof(path),
             "%s/%s/cpu.max", CGROUP_BASE_PATH, container_id);

    /* quota = percentage * 1000 microseconds
     * period = 100000 microseconds (100ms) */
    int quota = percentage * 1000;
    char value[64];
    snprintf(value, sizeof(value), "%d 100000", quota);

    if (write_file(path, value) != 0) {
        log_message(LOG_ERROR, "Failed to set CPU limit for %s to %d%%",
                   container_id, percentage);
        return -1;
    }

    log_message(LOG_INFO, "Set CPU limit for %s: %d%% (%d/100000 us)",
               container_id, percentage, quota);
    return 0;
}

/*
 * cleanup_cgroup - Remove a container's cgroup
 *
 * A cgroup directory can only be removed with rmdir() when it:
 * 1. Has no child cgroups
 * 2. Has no processes (cgroup.procs is empty)
 *
 * So we first kill all processes in the cgroup, wait for them
 * to exit, then remove the directory.
 */
int cleanup_cgroup(const char *container_id)
{
    if (!container_id) return -1;

    char cgroup_path[256];
    snprintf(cgroup_path, sizeof(cgroup_path),
             "%s/%s", CGROUP_BASE_PATH, container_id);

    /* Step 1: Kill all processes in the cgroup
     *
     * Read cgroup.procs and send SIGKILL to each PID.
     * We need to do this because rmdir() will fail if
     * the cgroup still has processes.
     */
    char procs_path[600];
    snprintf(procs_path, sizeof(procs_path), "%s/cgroup.procs", cgroup_path);

    char *buffer = NULL;
    size_t size = 0;
    if (read_file_contents(procs_path, &buffer, &size) == 0 && buffer) {
        char *line = strtok(buffer, "\n");
        while (line) {
            pid_t pid = (pid_t)atoi(line);
            if (pid > 0) {
                kill(pid, SIGKILL);
                log_message(LOG_DEBUG, "Killed cgroup process PID %d", pid);
            }
            line = strtok(NULL, "\n");
        }
        free(buffer);

        /* Brief wait for processes to be reaped */
        usleep(100000);  /* 100ms */
    }

    /* Step 2: Remove the cgroup directory
     *
     * rmdir() removes the cgroup. The kernel handles cleanup
     * of all the virtual control files inside it.
     */
    if (rmdir(cgroup_path) != 0 && errno != ENOENT) {
        log_message(LOG_WARN, "Failed to remove cgroup directory '%s': %s",
                   cgroup_path, strerror(errno));
        return -1;
    }

    log_message(LOG_DEBUG, "Cleaned up cgroup for %s", container_id);
    return 0;
}
