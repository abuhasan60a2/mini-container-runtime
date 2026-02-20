/*
 * cgroup.h - cgroup v2 resource management for minibox
 *
 * Controls CPU and memory resource limits for containers using
 * the Linux cgroup v2 unified hierarchy.
 *
 * Cgroup hierarchy: /sys/fs/cgroup/minibox/<container-id>/
 *
 * Supported controllers:
 * - memory: Limits RAM usage via memory.max
 * - cpu: Limits CPU usage via cpu.max (quota/period)
 *
 * References: cgroups(7), cgroup_namespaces(7)
 */

#ifndef MINIBOX_CGROUP_H
#define MINIBOX_CGROUP_H

#include <sys/types.h>

/* Base cgroup path for minibox containers */
#define CGROUP_BASE_PATH "/sys/fs/cgroup/minibox"

/*
 * verify_cgroup_v2 - Check if cgroup v2 is available
 *
 * Returns: 0 if cgroup v2 is mounted, -1 if not
 *
 * Checks for the existence of /sys/fs/cgroup/cgroup.controllers,
 * which is only present in cgroup v2 (unified hierarchy).
 */
int verify_cgroup_v2(void);

/*
 * setup_cgroup - Create a cgroup for a container
 *
 * @container_id: Container ID (used as cgroup name)
 *
 * Returns: 0 on success, -1 on failure
 *
 * Creates /sys/fs/cgroup/minibox/<container_id>/
 * and enables cpu and memory controllers.
 */
int setup_cgroup(const char *container_id);

/*
 * add_pid_to_cgroup - Add a process to the container's cgroup
 *
 * @container_id: Container ID
 * @pid:          Process ID to add
 *
 * Returns: 0 on success, -1 on failure
 *
 * Writes the PID to cgroup.procs, which moves the process
 * and all its threads into the cgroup.
 */
int add_pid_to_cgroup(const char *container_id, pid_t pid);

/*
 * set_memory_limit - Set the memory limit for a container
 *
 * @container_id: Container ID
 * @bytes:        Memory limit in bytes
 *
 * Returns: 0 on success, -1 on failure
 *
 * Writes to memory.max. When the container exceeds this limit,
 * the OOM killer will terminate processes inside the cgroup.
 */
int set_memory_limit(const char *container_id, size_t bytes);

/*
 * set_cpu_limit - Set the CPU limit for a container
 *
 * @container_id: Container ID
 * @percentage:   CPU limit as percentage of one core (1-100)
 *
 * Returns: 0 on success, -1 on failure
 *
 * Writes to cpu.max as "quota period":
 * - period is fixed at 100000 (100ms)
 * - quota = percentage * 1000 (e.g., 50% = "50000 100000")
 *
 * This means the container gets at most <percentage>% of one CPU core.
 */
int set_cpu_limit(const char *container_id, int percentage);

/*
 * cleanup_cgroup - Remove a container's cgroup
 *
 * @container_id: Container ID
 *
 * Returns: 0 on success, -1 on failure
 *
 * First kills all processes in the cgroup (if any),
 * then removes the cgroup directory with rmdir().
 * The directory must be empty (no processes) to be removed.
 */
int cleanup_cgroup(const char *container_id);

#endif /* MINIBOX_CGROUP_H */
