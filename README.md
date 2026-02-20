# Minibox - Minimal Container Runtime

A minimal but production-quality container runtime written in C that demonstrates the core Linux technologies behind Docker: **namespaces**, **cgroups**, **network isolation**, and **filesystem isolation with pivot_root**.

## Features

- **PID Namespace Isolation** - Container processes see themselves as PID 1
- **Mount Namespace Isolation** - Container has its own filesystem view
- **Network Namespace Isolation** - Virtual ethernet (veth) pairs with IP assignment
- **UTS Namespace Isolation** - Container gets its own hostname
- **IPC Namespace Isolation** - Isolated shared memory and semaphores
- **Filesystem Isolation** - `pivot_root(2)` for secure root filesystem change
- **cgroup v2 Resource Limits** - Memory and CPU limits
- **Port Forwarding** - NAT-based host-to-container port mapping
- **Container Lifecycle** - Create, start, stop, remove, list, exec, logs
- **State Persistence** - Container state saved as JSON files
- **Docker-like CLI** - Familiar command interface

## Architecture

```
                    +---------------------+
                    |   CLI (main.c)      |
                    |  Argument Parsing   |
                    +----------+----------+
                               |
                    +----------v----------+
                    | Container Orchestrator|
                    |   (container.c)      |
                    +--+---+---+---+---+--+
                       |   |   |   |   |
          +------------+   |   |   |   +-------------+
          |                |   |   |                  |
  +-------v-------+ +-----v---v-----+ +------v------+
  | Namespaces    | | Rootfs        | | Utils       |
  | (namespace.c) | | (rootfs.c)    | | (utils.c)   |
  |               | |               | |             |
  | clone()       | | pivot_root()  | | Logging     |
  | PID/MNT/NET/  | | tar extract   | | ID gen      |
  | UTS/IPC       | | mount proc/   | | File I/O    |
  +---------------+ | sys/dev       | +-------------+
                     +-------+-------+
                             |
                    +--------+--------+
                    |                 |
              +-----v-----+   +------v------+
              | cgroups    |   | Network     |
              | (cgroup.c) |   | (network.c) |
              |            |   |             |
              | memory.max |   | veth pairs  |
              | cpu.max    |   | IP config   |
              | cgroup.procs|  | NAT/iptables|
              +------------+   +-------------+
```

## Prerequisites

- **Linux kernel 4.6+** (for cgroup v2 support)
- **Root access** (required for namespaces, cgroups, network)
- **GCC** with C11 support
- **make**
- **libjansson-dev** (JSON parsing library)
- **iproute2** (`ip` command)
- **iptables** (port forwarding)
- **tar** (rootfs extraction)

## Building

```bash
# Clone the repository
git clone <repo-url> minibox
cd minibox

# Build (development, with debug symbols)
make

# Build with optimizations
make release

# Build with debug mode
make debug

# Clean build artifacts
make clean
```

### Using Docker (recommended for macOS/Windows)

```bash
# Build the test environment
docker build -t minibox-dev .

# Run with required privileges
docker run -it --privileged --rm -v $(pwd):/minibox minibox-dev bash

# Inside the container
cd /minibox && make clean && make
```

## Installation

```bash
# Build and install to /usr/local/bin
sudo make install

# Uninstall
sudo make uninstall
```

## Usage

### Run a Container

```bash
# Basic: run a command in an Alpine Linux container
sudo ./minibox run alpine-minirootfs-3.19.0-x86_64.tar.gz /bin/echo "Hello from minibox"

# Interactive shell
sudo ./minibox run alpine-minirootfs-3.19.0-x86_64.tar.gz /bin/sh

# With resource limits
sudo ./minibox run --memory 128m --cpu 25 alpine-rootfs.tar.gz /bin/sh

# Detached (background) mode
sudo ./minibox run -d --name mycontainer alpine-rootfs.tar.gz /bin/sleep 3600

# With port forwarding
sudo ./minibox run -d -p 8080:80 --name web alpine-rootfs.tar.gz /bin/sh

# With environment variables
sudo ./minibox run -e MY_VAR=hello -e DEBUG=1 alpine-rootfs.tar.gz /bin/env

# With volume mounts
sudo ./minibox run -v /host/path:/container/path alpine-rootfs.tar.gz /bin/ls /container/path
```

### List Containers

```bash
sudo ./minibox ps
```

Output:
```
CONTAINER ID   NAME                 IMAGE                          STATUS     PID      IP
------------   ----                 -----                          ------     ---      --
a1b2c3d4e5f6   mycontainer          alpine-rootfs.tar.gz           running    12345    10.10.10.2
```

### Execute Command in Running Container

```bash
sudo ./minibox exec <container-id> /bin/ps aux
sudo ./minibox exec <container-id> /bin/sh
```

### Stop a Container

```bash
sudo ./minibox stop <container-id>
```

### Remove a Container

```bash
sudo ./minibox rm <container-id>
```

### View Container Logs

```bash
sudo ./minibox logs <container-id>
```

### Help and Version

```bash
./minibox --help
./minibox --version
```

### Run Options

| Flag | Description | Example |
|------|-------------|---------|
| `-d, --detach` | Run in background | `-d` |
| `-p, --port` | Port mapping (repeatable) | `-p 8080:80` |
| `-v, --volume` | Volume mount (repeatable) | `-v /host:/container` |
| `-e, --env` | Environment variable (repeatable) | `-e KEY=VALUE` |
| `--memory` | Memory limit | `--memory 256m` |
| `--cpu` | CPU limit (1-100%) | `--cpu 50` |
| `--name` | Container name | `--name myapp` |
| `--debug` | Enable debug logging | `--debug` |

## How It Works

### 1. Namespaces (namespace.c)

Minibox uses `clone(2)` with `CLONE_NEW*` flags to create isolated namespaces:

- **CLONE_NEWPID**: The container process becomes PID 1 in its own PID namespace. It can only see its own processes.
- **CLONE_NEWNS**: Mount changes inside the container don't affect the host.
- **CLONE_NEWNET**: The container starts with no network interfaces (empty network namespace).
- **CLONE_NEWUTS**: The container can have its own hostname.
- **CLONE_NEWIPC**: System V IPC and POSIX message queues are isolated.

### 2. Filesystem Isolation (rootfs.c)

The container gets its own root filesystem using `pivot_root(2)`:

1. Extract a rootfs tarball (e.g., Alpine Linux minirootfs)
2. Bind mount the rootfs to itself (required by `pivot_root`)
3. `chdir` into the new root
4. Call `pivot_root(".", ".")` to swap the root
5. Unmount the old root with `umount2(".", MNT_DETACH)`
6. Mount essential filesystems: `/proc`, `/sys`, `/dev/pts`

**Why pivot_root instead of chroot?**
- `pivot_root` changes root for the entire mount namespace, while `chroot` only affects the current process
- `chroot` can be escaped by a privileged process; `pivot_root` + unmounting old root cannot
- This is the same approach used by `runc` and other production runtimes

### 3. cgroup v2 Resource Limits (cgroup.c)

Each container gets a cgroup under `/sys/fs/cgroup/minibox/<container-id>/`:

- **Memory**: Written to `memory.max` (bytes). Exceeding triggers OOM killer.
- **CPU**: Written to `cpu.max` as `"quota period"`. For 50% of one core: `"50000 100000"` (50ms of every 100ms).

### 4. Network Isolation (network.c)

Container networking uses virtual ethernet (veth) pairs:

1. Create veth pair: `veth-<id>` (host) and `eth0` (container)
2. Move container end into the container's network namespace
3. Assign IPs: host=10.10.10.1/24, container=10.10.10.2/24
4. Set default route in container via 10.10.10.1
5. Enable IP forwarding on host
6. Setup MASQUERADE NAT for outbound traffic
7. Setup DNAT rules for port forwarding

## File Structure

```
minibox/
├── src/
│   ├── main.c           # CLI entry point, argument parsing
│   ├── container.c      # Container lifecycle orchestration
│   ├── container.h      # Container data structures
│   ├── namespace.c      # Linux namespace creation (clone)
│   ├── namespace.h
│   ├── cgroup.c         # cgroup v2 resource management
│   ├── cgroup.h
│   ├── network.c        # veth pairs, IP config, NAT
│   ├── network.h
│   ├── rootfs.c         # Rootfs extraction, pivot_root
│   ├── rootfs.h
│   ├── utils.c          # Logging, error handling, helpers
│   └── utils.h
├── examples/
│   ├── test.sh          # Integration test suite
│   └── sample-config.json
├── Makefile             # Build system
├── Dockerfile           # Ubuntu 22.04 build/test environment
├── README.md            # This file
└── .gitignore
```

## Testing

### Quick Test

```bash
# Build
make

# Download Alpine rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/alpine-minirootfs-3.19.0-x86_64.tar.gz

# Run a basic container
sudo ./minibox run alpine-minirootfs-3.19.0-x86_64.tar.gz /bin/echo "Hello from minibox"
```

### Full Test Suite

```bash
sudo make test
# Or directly:
sudo ./examples/test.sh
```

### Docker-based Testing

```bash
docker build -t minibox-dev .
docker run -it --privileged --rm minibox-dev bash -c "cd /minibox && ./examples/test.sh"
```

## Limitations

This is an educational implementation. The following features are **not** implemented:

- Overlay filesystem (copy-on-write layers)
- Container image registry (pulling from Docker Hub)
- Bridge networking between multiple containers
- Docker-compatible REST API
- Image building (Dockerfile support)
- Volume management subsystem
- Health checks and auto-restart
- Resource usage statistics/metrics
- Rootless containers (user namespaces)
- Seccomp system call filtering
- AppArmor/SELinux security profiles
- Container checkpointing (CRIU)
- Log rotation

## Troubleshooting

### "Must run as root"
Container operations require root for namespace creation, cgroup manipulation, and network setup:
```bash
sudo ./minibox run ...
```

### "cgroup v2 not available"
Ensure your kernel supports cgroup v2 and it's mounted:
```bash
# Check cgroup v2
stat /sys/fs/cgroup/cgroup.controllers

# If using cgroup v1, you may need to add to kernel cmdline:
# systemd.unified_cgroup_hierarchy=1
```

### "Image not found"
The image argument must be a path to a rootfs tarball (`.tar.gz`):
```bash
# Download Alpine minirootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/alpine-minirootfs-3.19.0-x86_64.tar.gz
```

### Network not working
Ensure `iproute2` and `iptables` are installed:
```bash
apt-get install -y iproute2 iptables
```

### pivot_root fails
Ensure you're running on Linux (not macOS/Windows) and the kernel supports mount namespaces.

## Dependencies

### Build Dependencies
| Package | Purpose |
|---------|---------|
| gcc | C compiler (C11 support) |
| make | Build system |
| libjansson-dev | JSON parsing for state files |

### Runtime Dependencies
| Package | Purpose |
|---------|---------|
| iproute2 | `ip` command for network setup |
| iptables | NAT/port forwarding |
| tar | Rootfs extraction |

## References

- [clone(2)](https://man7.org/linux/man-pages/man2/clone.2.html) - Create child process in new namespaces
- [namespaces(7)](https://man7.org/linux/man-pages/man7/namespaces.7.html) - Overview of Linux namespaces
- [pivot_root(2)](https://man7.org/linux/man-pages/man2/pivot_root.2.html) - Change root filesystem
- [cgroups(7)](https://man7.org/linux/man-pages/man7/cgroups.7.html) - Linux control groups
- [veth(4)](https://man7.org/linux/man-pages/man4/veth.4.html) - Virtual ethernet pair devices
- [mount_namespaces(7)](https://man7.org/linux/man-pages/man7/mount_namespaces.7.html) - Mount namespace isolation
- [pid_namespaces(7)](https://man7.org/linux/man-pages/man7/pid_namespaces.7.html) - PID namespace details
- [setns(2)](https://man7.org/linux/man-pages/man2/setns.2.html) - Enter existing namespace

## License

This project is for educational purposes. Use at your own risk.
