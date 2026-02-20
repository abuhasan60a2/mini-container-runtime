/*
 * rootfs.h - Filesystem isolation for minibox
 *
 * Handles extraction of rootfs tarballs, mounting essential filesystems,
 * and performing pivot_root to change the container's root directory.
 *
 * Why pivot_root instead of chroot:
 * - pivot_root changes root for the entire mount namespace
 * - chroot only affects the current process and can be escaped
 * - pivot_root allows unmounting the old root for better security
 *
 * References: pivot_root(2), mount(2), mount_namespaces(7)
 */

#ifndef MINIBOX_ROOTFS_H
#define MINIBOX_ROOTFS_H

/*
 * setup_rootfs - Main orchestration function for rootfs preparation
 *
 * @image_path:   Path to the rootfs tarball (e.g., alpine-minirootfs.tar.gz)
 * @container_id: Container ID (used for extraction path)
 *
 * Returns: 0 on success, -1 on failure
 *
 * Steps:
 * 1. Create extraction directory /var/lib/minibox/images/<id>/rootfs/
 * 2. Extract tarball to that directory
 * 3. Create mount point directories (proc, sys, dev, dev/pts)
 */
int setup_rootfs(const char *image_path, const char *container_id);

/*
 * extract_rootfs_tarball - Extract a tar.gz archive to destination
 *
 * @tarball: Path to .tar.gz file
 * @dest:    Destination directory (must exist)
 *
 * Returns: 0 on success, -1 on failure
 *
 * Uses system("tar xzf ...") for extraction.
 */
int extract_rootfs_tarball(const char *tarball, const char *dest);

/*
 * prepare_mount_points - Create directories for essential mounts
 *
 * @rootfs_path: Path to the extracted rootfs
 *
 * Returns: 0 on success, -1 on failure
 *
 * Creates: /proc, /sys, /dev, /dev/pts, /tmp
 */
int prepare_mount_points(const char *rootfs_path);

/*
 * mount_essential_filesystems - Mount proc, sysfs, devpts inside container
 *
 * @rootfs_path: Root path (usually "/" after pivot_root)
 *
 * Returns: 0 on success, -1 on failure
 *
 * Mounts:
 * - proc at /proc (process information filesystem)
 * - sysfs at /sys (kernel/device information)
 * - devpts at /dev/pts (pseudo-terminal devices)
 */
int mount_essential_filesystems(const char *rootfs_path);

/*
 * do_pivot_root - Change root filesystem using pivot_root(2)
 *
 * @new_root: Path to the new root directory
 *
 * Returns: 0 on success, -1 on failure
 *
 * Implementation:
 * 1. Bind mount new_root to itself (required by pivot_root)
 * 2. chdir(new_root)
 * 3. pivot_root(".", ".") - swap old and new root
 * 4. umount2(".", MNT_DETACH) - detach old root
 * 5. chdir("/") - move to new root
 *
 * After this call, the old root filesystem is inaccessible.
 */
int do_pivot_root(const char *new_root);

/*
 * setup_volume_mounts - Mount host directories into container
 *
 * @rootfs_path:  Path to container rootfs
 * @volume_spec:  Volume specification "host_path:container_path"
 *
 * Returns: 0 on success, -1 on failure
 *
 * Uses bind mounts to share host directories with the container.
 */
int setup_volume_mounts(const char *rootfs_path, const char *volume_spec);

#endif /* MINIBOX_ROOTFS_H */
