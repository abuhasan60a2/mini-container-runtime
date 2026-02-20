/*
 * rootfs.c - Filesystem isolation for minibox
 *
 * Handles the complete filesystem setup for a container:
 * - Extracting a rootfs tarball (e.g., Alpine Linux minirootfs)
 * - Creating mount points for essential filesystems
 * - Using pivot_root(2) to change the root directory
 * - Mounting /proc, /sys, and /dev/pts inside the container
 *
 * The key insight: pivot_root changes the root for the entire mount
 * namespace, while chroot only changes it for the current process.
 * This means after pivot_root, even a process that escapes its jail
 * still can't access the host filesystem.
 *
 * References:
 *   pivot_root(2)       - Change the root filesystem
 *   mount(2)            - Mount filesystems
 *   mount_namespaces(7) - Mount namespace isolation
 *   proc(5)             - /proc filesystem
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <fcntl.h>

#include "rootfs.h"
#include "container.h"
#include "utils.h"

/*
 * pivot_root syscall wrapper
 *
 * glibc doesn't provide a wrapper for pivot_root(2),
 * so we call it directly via syscall(). This is standard
 * practice for container runtimes.
 */
static int pivot_root(const char *new_root, const char *put_old)
{
    return (int)syscall(SYS_pivot_root, new_root, put_old);
}

/*
 * setup_rootfs - Orchestrate complete rootfs preparation
 *
 * This is called by the parent process BEFORE clone().
 * It extracts the rootfs and creates mount points that
 * the child will need.
 */
int setup_rootfs(const char *image_path, const char *container_id)
{
    if (!image_path || !container_id) {
        log_message(LOG_ERROR, "setup_rootfs: NULL argument");
        return -1;
    }

    /* Verify the image tarball exists */
    if (!file_exists(image_path)) {
        log_message(LOG_ERROR, "Image not found: %s", image_path);
        return -1;
    }

    /* Build the rootfs extraction path */
    char rootfs_path[256];
    snprintf(rootfs_path, sizeof(rootfs_path),
             "%s/%s/rootfs", MINIBOX_IMAGE_DIR, container_id);

    /* Create the rootfs directory if it doesn't exist */
    if (create_directory_recursive(rootfs_path) != 0) {
        log_message(LOG_ERROR, "Failed to create rootfs directory: %s", rootfs_path);
        return -1;
    }

    /* Extract the tarball */
    if (extract_rootfs_tarball(image_path, rootfs_path) != 0) {
        log_message(LOG_ERROR, "Failed to extract rootfs tarball");
        return -1;
    }

    /* Create mount points inside rootfs */
    if (prepare_mount_points(rootfs_path) != 0) {
        log_message(LOG_ERROR, "Failed to prepare mount points");
        return -1;
    }

    /* Bind mount /dev from host into rootfs
     *
     * We need device nodes for the container to function.
     * Bind mounting /dev provides access to /dev/null, /dev/zero,
     * /dev/random, etc. without needing to create them manually.
     */
    char dev_path[512];
    snprintf(dev_path, sizeof(dev_path), "%s/dev", rootfs_path);
    if (mount("/dev", dev_path, NULL, MS_BIND | MS_REC, NULL) != 0) {
        log_message(LOG_WARN, "Failed to bind mount /dev: %s (container may have limited device access)",
                   strerror(errno));
    }

    log_message(LOG_INFO, "Rootfs prepared at %s", rootfs_path);
    return 0;
}

/*
 * extract_rootfs_tarball - Extract a .tar.gz to a destination directory
 *
 * Uses system() to call tar. While not the most elegant approach,
 * it's reliable and handles all tar formats (gz, bz2, xz).
 *
 * A production runtime would use libarchive for this.
 */
int extract_rootfs_tarball(const char *tarball, const char *dest)
{
    if (!tarball || !dest) return -1;

    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "tar xf '%s' -C '%s' 2>/dev/null", tarball, dest);

    log_message(LOG_DEBUG, "Extracting: %s", cmd);

    int ret = system(cmd);
    if (ret != 0) {
        log_message(LOG_ERROR, "tar extraction failed (exit code %d)", ret);
        return -1;
    }

    return 0;
}

/*
 * prepare_mount_points - Create directories needed for filesystem mounts
 *
 * These directories must exist in the rootfs BEFORE we pivot_root,
 * because after pivot_root the host filesystem isn't accessible.
 */
int prepare_mount_points(const char *rootfs_path)
{
    if (!rootfs_path) return -1;

    /* Directories to create */
    const char *dirs[] = {
        "proc",
        "sys",
        "dev",
        "dev/pts",
        "dev/shm",
        "tmp",
        "root",
        "etc",
        NULL
    };

    for (int i = 0; dirs[i]; i++) {
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", rootfs_path, dirs[i]);

        if (mkdir(full_path, 0755) != 0 && errno != EEXIST) {
            log_message(LOG_ERROR, "Failed to create mount point %s: %s",
                       full_path, strerror(errno));
            return -1;
        }
    }

    return 0;
}

/*
 * mount_essential_filesystems - Mount proc, sysfs, devpts
 *
 * Called INSIDE the container (after pivot_root) so paths
 * are relative to the new root.
 *
 * /proc is essential for:
 *   - Process listing (ps, top)
 *   - Reading /proc/self/... info
 *   - cgroup visibility
 *
 * /sys is essential for:
 *   - Device information
 *   - Kernel parameters
 *
 * /dev/pts is essential for:
 *   - Pseudo-terminal allocation (interactive shells)
 */
int mount_essential_filesystems(const char *rootfs_path)
{
    char path[512];

    /* Mount /proc - process information filesystem
     *
     * In a PID namespace, /proc only shows the container's processes.
     * The container's PID 1 is the only directly visible process.
     */
    snprintf(path, sizeof(path), "%s/proc", rootfs_path);
    if (mount("proc", path, "proc", 0, NULL) != 0) {
        /* proc mount can fail if already mounted; try remount */
        if (errno != EBUSY) {
            log_message(LOG_ERROR, "Failed to mount /proc: %s", strerror(errno));
            return -1;
        }
    }

    /* Mount /sys - sysfs
     *
     * Provides a view of the kernel's device model.
     * Mount read-only for security (container shouldn't modify
     * kernel parameters).
     */
    snprintf(path, sizeof(path), "%s/sys", rootfs_path);
    if (mount("sysfs", path, "sysfs", MS_RDONLY, NULL) != 0) {
        if (errno != EBUSY) {
            log_message(LOG_WARN, "Failed to mount /sys: %s", strerror(errno));
            /* Non-fatal: some containers work without /sys */
        }
    }

    /* Mount /dev/pts - pseudo-terminal devices
     *
     * Required for interactive shells. Without this, commands
     * like `su`, `login`, and `ssh` won't work properly.
     */
    snprintf(path, sizeof(path), "%s/dev/pts", rootfs_path);
    if (mount("devpts", path, "devpts", 0, NULL) != 0) {
        if (errno != EBUSY) {
            log_message(LOG_WARN, "Failed to mount /dev/pts: %s", strerror(errno));
            /* Non-fatal: only needed for interactive use */
        }
    }

    return 0;
}

/*
 * do_pivot_root - Change the root filesystem using pivot_root(2)
 *
 * This is the critical filesystem isolation step. After this:
 * - The container sees new_root as "/"
 * - The host filesystem is completely unmounted
 * - There's no way to access host files from the container
 *
 * The technique used here (pivot_root(".", ".")) is the same
 * approach used by runc and other production container runtimes.
 * It was documented by the pivot_root(2) man page.
 *
 * Steps:
 * 1. Bind mount new_root to itself
 *    - Required because pivot_root needs new_root to be a mount point
 * 2. chdir into new_root
 * 3. pivot_root(".", ".")
 *    - Swaps the root mount with the current directory
 *    - Old root is now mounted "on top of" new root
 * 4. umount2(".", MNT_DETACH)
 *    - Lazily unmount the old root
 *    - It will be fully removed once no processes reference it
 * 5. chdir("/")
 *    - Move to the new root directory
 */
int do_pivot_root(const char *new_root)
{
    if (!new_root) {
        log_message(LOG_ERROR, "do_pivot_root: NULL new_root");
        return -1;
    }

    log_message(LOG_DEBUG, "Performing pivot_root to '%s'", new_root);

    /*
     * Step 1: Ensure new_root is a mount point
     *
     * pivot_root(2) requires that new_root is a mount point.
     * The simplest way to ensure this is to bind mount it
     * to itself. This is a standard technique.
     */
    if (mount(new_root, new_root, NULL, MS_BIND | MS_REC, NULL) != 0) {
        log_message(LOG_ERROR, "Failed to bind mount new root '%s': %s",
                   new_root, strerror(errno));
        return -1;
    }

    /*
     * Step 2: Change directory into the new root
     *
     * We need to be inside new_root for the pivot_root(".", ".") trick.
     */
    if (chdir(new_root) != 0) {
        log_message(LOG_ERROR, "Failed to chdir to new root '%s': %s",
                   new_root, strerror(errno));
        return -1;
    }

    /*
     * Step 3: pivot_root(".", ".")
     *
     * This clever technique works because:
     * - new_root = "." (current directory = new_root)
     * - put_old = "." (old root is placed at "." of new root)
     * - After pivot, old root is mounted at "/" of new root
     * - We then unmount it to remove access to host filesystem
     *
     * This avoids needing a separate put_old directory.
     */
    if (pivot_root(".", ".") != 0) {
        log_message(LOG_ERROR, "pivot_root failed: %s", strerror(errno));
        return -1;
    }

    /*
     * Step 4: Unmount the old root filesystem
     *
     * MNT_DETACH performs a lazy unmount - the filesystem is removed
     * from the mount hierarchy immediately, and actual cleanup happens
     * when all references are dropped. This is safer than a regular
     * unmount because it won't fail if something is still using a file.
     */
    if (umount2(".", MNT_DETACH) != 0) {
        log_message(LOG_ERROR, "Failed to unmount old root: %s", strerror(errno));
        return -1;
    }

    /*
     * Step 5: Move to the new root
     *
     * After pivot_root and unmount, we need to explicitly
     * chdir("/") to be in the new root directory.
     */
    if (chdir("/") != 0) {
        log_message(LOG_ERROR, "Failed to chdir to /: %s", strerror(errno));
        return -1;
    }

    log_message(LOG_DEBUG, "pivot_root completed successfully");
    return 0;
}

/*
 * setup_volume_mounts - Bind mount host directories into container
 *
 * Parses a volume specification like "/host/path:/container/path"
 * and creates a bind mount. This allows the container to access
 * specific host directories.
 *
 * The bind mount is created BEFORE pivot_root, so the host
 * path is accessible. After pivot_root, only the mount point
 * inside the container remains visible.
 */
int setup_volume_mounts(const char *rootfs_path, const char *volume_spec)
{
    if (!rootfs_path || !volume_spec || strlen(volume_spec) == 0)
        return 0;  /* Nothing to mount */

    /* Parse "host_path:container_path" */
    char spec_copy[1024];
    strncpy(spec_copy, volume_spec, sizeof(spec_copy) - 1);
    spec_copy[sizeof(spec_copy) - 1] = '\0';

    /* Handle multiple volume mounts separated by commas */
    char *saveptr;
    char *mount_entry = strtok_r(spec_copy, ",", &saveptr);

    while (mount_entry) {
        /* Find the colon separator */
        char *colon = strchr(mount_entry, ':');
        if (!colon) {
            log_message(LOG_ERROR, "Invalid volume spec: '%s' (expected host:container)",
                       mount_entry);
            mount_entry = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        *colon = '\0';
        char *host_path = mount_entry;
        char *container_path = colon + 1;

        /* Validate paths - prevent path traversal */
        if (strstr(host_path, "..") || strstr(container_path, "..")) {
            log_message(LOG_ERROR, "Path traversal detected in volume mount");
            return -1;
        }

        /* Build the full container-side mount path */
        char full_mount_path[1024];
        snprintf(full_mount_path, sizeof(full_mount_path),
                 "%s%s", rootfs_path, container_path);

        /* Create the mount point directory */
        if (create_directory_recursive(full_mount_path) != 0) {
            log_message(LOG_ERROR, "Failed to create mount point: %s", full_mount_path);
            return -1;
        }

        /* Create the bind mount */
        if (mount(host_path, full_mount_path, NULL, MS_BIND | MS_REC, NULL) != 0) {
            log_message(LOG_ERROR, "Failed to bind mount %s -> %s: %s",
                       host_path, full_mount_path, strerror(errno));
            return -1;
        }

        log_message(LOG_INFO, "Mounted volume: %s -> %s", host_path, container_path);
        mount_entry = strtok_r(NULL, ",", &saveptr);
    }

    return 0;
}
