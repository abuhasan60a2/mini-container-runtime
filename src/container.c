/*
 * container.c - Container lifecycle management for minibox
 *
 * Implements the core orchestration logic: creating containers from
 * configuration, starting them (namespace + cgroup + network setup),
 * stopping, removing, and listing containers.
 *
 * State is persisted as JSON files in /var/run/minibox/<id>.json
 * using the jansson library.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <jansson.h>
#include <sched.h>

#include "container.h"
#include "namespace.h"
#include "cgroup.h"
#include "network.h"
#include "rootfs.h"
#include "utils.h"

/* Status string lookup table */
static const char *status_strings[] = {
    "created",
    "running",
    "stopped",
    "removing"
};

/*
 * container_status_string - Convert status enum to human-readable string
 */
const char *container_status_string(container_status_t status)
{
    if (status > CONTAINER_REMOVING)
        return "unknown";
    return status_strings[status];
}

/*
 * container_create - Build a new container from configuration
 *
 * Allocates a container_t, generates an ID, sets defaults,
 * creates runtime directories, and persists initial state.
 */
container_t *container_create(container_config_t *config)
{
    if (!config) {
        log_message(LOG_ERROR, "container_create: NULL config");
        return NULL;
    }

    /* Validate image path */
    if (strlen(config->image) == 0) {
        log_message(LOG_ERROR, "container_create: No image specified");
        return NULL;
    }

    /* Validate command */
    if (strlen(config->command) == 0) {
        log_message(LOG_ERROR, "container_create: No command specified");
        return NULL;
    }

    /* Allocate container structure */
    container_t *c = calloc(1, sizeof(container_t));
    if (!c) {
        log_message(LOG_ERROR, "container_create: Failed to allocate container");
        return NULL;
    }

    /* Generate unique ID */
    char *id = generate_container_id();
    strncpy(c->id, id, sizeof(c->id) - 1);

    /* Set name: use provided name or default to ID */
    if (strlen(config->name) > 0) {
        if (!validate_name(config->name)) {
            log_message(LOG_ERROR, "Invalid container name: '%s' (use alphanumeric, dash, underscore)",
                       config->name);
            free(c);
            return NULL;
        }
        strncpy(c->name, config->name, sizeof(c->name) - 1);
    } else {
        strncpy(c->name, c->id, sizeof(c->name) - 1);
    }

    /* Copy configuration */
    strncpy(c->image, config->image, sizeof(c->image) - 1);
    strncpy(c->command, config->command, sizeof(c->command) - 1);

    /* Apply resource limits with defaults */
    c->resources.memory_bytes = (config->memory_limit > 0) ?
        config->memory_limit : DEFAULT_MEMORY_LIMIT;
    c->resources.cpu_percentage = (config->cpu_limit > 0) ?
        config->cpu_limit : DEFAULT_CPU_LIMIT;

    /* Copy port mappings */
    if (strlen(config->port_mappings) > 0)
        strncpy(c->network.ports, config->port_mappings, sizeof(c->network.ports) - 1);

    /* Set initial state */
    c->status = CONTAINER_CREATED;
    c->created_at = time(NULL);
    c->pid = 0;

    /* Setup paths */
    snprintf(c->rootfs_path, sizeof(c->rootfs_path),
             "%s/%s/rootfs", MINIBOX_IMAGE_DIR, c->id);
    snprintf(c->log_path, sizeof(c->log_path),
             "%s/%s.log", MINIBOX_LOG_DIR, c->id);
    snprintf(c->state_path, sizeof(c->state_path),
             "%s/%s.json", MINIBOX_STATE_DIR, c->id);

    /* Create runtime directories */
    if (create_directory_recursive(MINIBOX_STATE_DIR) != 0 ||
        create_directory_recursive(MINIBOX_LOG_DIR) != 0 ||
        create_directory_recursive(c->rootfs_path) != 0) {
        log_message(LOG_ERROR, "Failed to create runtime directories");
        free(c);
        return NULL;
    }

    /* Persist initial state */
    if (container_save_state(c) != 0) {
        log_message(LOG_ERROR, "Failed to save initial container state");
        free(c);
        return NULL;
    }

    log_message(LOG_INFO, "Container %s created (name=%s, image=%s)",
               c->id, c->name, c->image);

    return c;
}

/*
 * container_start - Start the container process
 *
 * This is the main orchestration function. It:
 * 1. Extracts the rootfs from the image tarball
 * 2. Creates isolated namespaces via clone()
 * 3. Sets up cgroup resource limits
 * 4. Configures network (veth pair, IP, NAT)
 * 5. Waits for the child or returns (if detached)
 */
int container_start(container_t *container)
{
    if (!container) {
        log_message(LOG_ERROR, "container_start: NULL container");
        return -1;
    }

    if (container->status != CONTAINER_CREATED) {
        log_message(LOG_ERROR, "Container %s is not in 'created' state (status=%s)",
                   container->id, container_status_string(container->status));
        return -1;
    }

    log_message(LOG_INFO, "Starting container %s...", container->id);

    /* Step 1: Extract rootfs from image tarball */
    log_message(LOG_INFO, "Extracting rootfs from %s...", container->image);
    if (setup_rootfs(container->image, container->id) != 0) {
        log_message(LOG_ERROR, "Failed to setup rootfs for container %s", container->id);
        return -1;
    }

    /* Step 2: Setup cgroup (create directory, set limits) */
    log_message(LOG_INFO, "Setting up cgroups...");
    if (setup_cgroup(container->id) != 0) {
        log_message(LOG_ERROR, "Failed to setup cgroup for container %s", container->id);
        return -1;
    }

    if (set_memory_limit(container->id, container->resources.memory_bytes) != 0) {
        log_message(LOG_WARN, "Failed to set memory limit (non-fatal)");
    }

    if (set_cpu_limit(container->id, container->resources.cpu_percentage) != 0) {
        log_message(LOG_WARN, "Failed to set CPU limit (non-fatal)");
    }

    /* Step 3: Create namespaces and start child process */
    log_message(LOG_INFO, "Creating namespaces and starting process...");

    /* Build child configuration */
    child_config_t child_cfg;
    memset(&child_cfg, 0, sizeof(child_cfg));
    strncpy(child_cfg.hostname, container->name, sizeof(child_cfg.hostname) - 1);
    snprintf(child_cfg.rootfs_path, sizeof(child_cfg.rootfs_path),
             "%s/%s/rootfs", MINIBOX_IMAGE_DIR, container->id);
    strncpy(child_cfg.command, container->command, sizeof(child_cfg.command) - 1);
    strncpy(child_cfg.container_id, container->id, sizeof(child_cfg.container_id) - 1);

    /* Parse volume mounts into child config (if any) */
    /* Volume mounts are handled during rootfs setup */

    /* Create the container process with isolated namespaces */
    pid_t child_pid = create_namespaces(&child_cfg);
    if (child_pid < 0) {
        log_message(LOG_ERROR, "Failed to create namespaces for container %s", container->id);
        cleanup_cgroup(container->id);
        return -1;
    }

    container->pid = child_pid;
    log_message(LOG_INFO, "Container process started with PID %d", child_pid);

    /* Step 4: Add child PID to cgroup */
    if (add_pid_to_cgroup(container->id, child_pid) != 0) {
        log_message(LOG_WARN, "Failed to add PID to cgroup (non-fatal)");
    }

    /* Step 5: Setup network for container */
    log_message(LOG_INFO, "Setting up network...");
    if (setup_network(child_pid, container->id) != 0) {
        log_message(LOG_WARN, "Failed to setup network (non-fatal, container will run without network)");
    } else {
        strncpy(container->network.ip, "10.10.10.2", sizeof(container->network.ip) - 1);
        strncpy(container->network.gateway, "10.10.10.1", sizeof(container->network.gateway) - 1);
        snprintf(container->network.veth_host, sizeof(container->network.veth_host),
                 "veth-%.12s", container->id);
        strncpy(container->network.veth_guest, "eth0", sizeof(container->network.veth_guest) - 1);
    }

    /* Step 6: Setup port forwarding if requested */
    if (strlen(container->network.ports) > 0) {
        if (configure_nat_rules(container->network.ports) != 0) {
            log_message(LOG_WARN, "Failed to configure port forwarding (non-fatal)");
        }
    }

    /* Update container state */
    container->status = CONTAINER_RUNNING;
    container_save_state(container);

    log_message(LOG_INFO, "Container %s is running (PID=%d, IP=%s)",
               container->id, container->pid, container->network.ip);

    return 0;
}

/*
 * container_stop - Stop a running container gracefully
 *
 * Sends SIGTERM first, waits up to 10 seconds for the process
 * to exit. If it doesn't, sends SIGKILL. Then cleans up
 * cgroups and network resources.
 */
int container_stop(const char *id)
{
    if (!id) return -1;

    container_t *container = container_load_state(id);
    if (!container) {
        log_message(LOG_ERROR, "Container %s not found", id);
        return -1;
    }

    if (container->status != CONTAINER_RUNNING) {
        log_message(LOG_WARN, "Container %s is not running (status=%s)",
                   id, container_status_string(container->status));
        container_free(container);
        return -1;
    }

    log_message(LOG_INFO, "Stopping container %s (PID=%d)...", id, container->pid);

    /* Step 1: Send SIGTERM for graceful shutdown */
    if (container->pid > 0) {
        if (kill(container->pid, SIGTERM) != 0 && errno != ESRCH) {
            log_message(LOG_WARN, "Failed to send SIGTERM to PID %d: %s",
                       container->pid, strerror(errno));
        }

        /* Wait up to 10 seconds for graceful exit */
        int waited = 0;
        while (waited < 10) {
            int status;
            pid_t result = waitpid(container->pid, &status, WNOHANG);
            if (result == container->pid || (result == -1 && errno == ECHILD)) {
                log_message(LOG_INFO, "Container process exited gracefully");
                break;
            }
            sleep(1);
            waited++;
        }

        /* Step 2: Force kill if still running */
        if (waited >= 10) {
            log_message(LOG_WARN, "Container did not stop gracefully, sending SIGKILL");
            kill(container->pid, SIGKILL);
            waitpid(container->pid, NULL, 0);
        }
    }

    /* Step 3: Cleanup cgroup */
    cleanup_cgroup(container->id);

    /* Step 4: Cleanup network */
    cleanup_network(container->id);

    /* Step 5: Update state */
    container->status = CONTAINER_STOPPED;
    container->pid = 0;
    container_save_state(container);

    log_message(LOG_INFO, "Container %s stopped", id);
    container_free(container);
    return 0;
}

/*
 * container_remove - Remove a stopped container and all its resources
 *
 * Removes: rootfs, cgroup directory, state file, log file
 */
int container_remove(const char *id)
{
    if (!id) return -1;

    container_t *container = container_load_state(id);
    if (!container) {
        log_message(LOG_ERROR, "Container %s not found", id);
        return -1;
    }

    if (container->status == CONTAINER_RUNNING) {
        log_message(LOG_ERROR, "Container %s is still running. Stop it first.", id);
        container_free(container);
        return -1;
    }

    log_message(LOG_INFO, "Removing container %s...", id);

    container->status = CONTAINER_REMOVING;
    container_save_state(container);

    /* Remove rootfs directory */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf %s/%s", MINIBOX_IMAGE_DIR, container->id);
    if (system(cmd) != 0) {
        log_message(LOG_WARN, "Failed to remove rootfs directory");
    }

    /* Remove log file */
    unlink(container->log_path);

    /* Remove state file */
    unlink(container->state_path);

    log_message(LOG_INFO, "Container %s removed", id);
    container_free(container);
    return 0;
}

/*
 * container_list - Enumerate all containers from state directory
 *
 * Scans /var/run/minibox/ for .json files and loads each one.
 */
container_t **container_list(int *count)
{
    *count = 0;

    DIR *dir = opendir(MINIBOX_STATE_DIR);
    if (!dir) {
        /* No state directory means no containers */
        return NULL;
    }

    /* First pass: count .json files */
    struct dirent *entry;
    int n = 0;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 5 && strcmp(entry->d_name + len - 5, ".json") == 0)
            n++;
    }

    if (n == 0) {
        closedir(dir);
        return NULL;
    }

    /* Allocate array */
    container_t **containers = calloc((size_t)n, sizeof(container_t *));
    if (!containers) {
        closedir(dir);
        return NULL;
    }

    /* Second pass: load each container */
    rewinddir(dir);
    int idx = 0;
    while ((entry = readdir(dir)) != NULL && idx < n) {
        size_t len = strlen(entry->d_name);
        if (len > 5 && strcmp(entry->d_name + len - 5, ".json") == 0) {
            /* Extract container ID from filename (remove .json) */
            char cid[64];
            snprintf(cid, sizeof(cid), "%.*s", (int)(len - 5), entry->d_name);

            container_t *c = container_load_state(cid);
            if (c) {
                /* Check if a running container's process is still alive */
                if (c->status == CONTAINER_RUNNING && c->pid > 0) {
                    if (kill(c->pid, 0) != 0 && errno == ESRCH) {
                        /* Process is gone, mark as stopped */
                        c->status = CONTAINER_STOPPED;
                        c->pid = 0;
                        container_save_state(c);
                    }
                }
                containers[idx++] = c;
            }
        }
    }

    closedir(dir);
    *count = idx;
    return containers;
}

/*
 * container_save_state - Serialize container state to JSON
 *
 * Uses jansson library for JSON serialization.
 * File is written atomically (write + close).
 */
int container_save_state(container_t *container)
{
    if (!container) return -1;

    json_t *root = json_object();
    if (!root) return -1;

    json_object_set_new(root, "id", json_string(container->id));
    json_object_set_new(root, "name", json_string(container->name));
    json_object_set_new(root, "image", json_string(container->image));
    json_object_set_new(root, "command", json_string(container->command));
    json_object_set_new(root, "pid", json_integer(container->pid));
    json_object_set_new(root, "status", json_integer(container->status));
    json_object_set_new(root, "created_at", json_integer(container->created_at));

    /* Network info */
    json_t *network = json_object();
    json_object_set_new(network, "ip", json_string(container->network.ip));
    json_object_set_new(network, "gateway", json_string(container->network.gateway));
    json_object_set_new(network, "veth_host", json_string(container->network.veth_host));
    json_object_set_new(network, "veth_guest", json_string(container->network.veth_guest));
    json_object_set_new(network, "ports", json_string(container->network.ports));
    json_object_set_new(root, "network", network);

    /* Resource limits */
    json_t *resources = json_object();
    json_object_set_new(resources, "memory_bytes", json_integer((json_int_t)container->resources.memory_bytes));
    json_object_set_new(resources, "cpu_percentage", json_integer(container->resources.cpu_percentage));
    json_object_set_new(root, "resources", resources);

    /* Paths */
    json_object_set_new(root, "rootfs_path", json_string(container->rootfs_path));
    json_object_set_new(root, "log_path", json_string(container->log_path));

    /* Write to file */
    int ret = json_dump_file(root, container->state_path, JSON_INDENT(2));
    json_decref(root);

    if (ret != 0) {
        log_message(LOG_ERROR, "Failed to write state file: %s", container->state_path);
        return -1;
    }

    return 0;
}

/*
 * container_load_state - Deserialize container state from JSON
 *
 * Reads /var/run/minibox/<id>.json and populates a container_t.
 */
container_t *container_load_state(const char *id)
{
    if (!id) return NULL;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", MINIBOX_STATE_DIR, id);

    json_error_t error;
    json_t *root = json_load_file(path, 0, &error);
    if (!root) {
        log_message(LOG_DEBUG, "Failed to load state for %s: %s", id, error.text);
        return NULL;
    }

    container_t *c = calloc(1, sizeof(container_t));
    if (!c) {
        json_decref(root);
        return NULL;
    }

    /* Extract fields */
    const char *str;

    str = json_string_value(json_object_get(root, "id"));
    if (str) strncpy(c->id, str, sizeof(c->id) - 1);

    str = json_string_value(json_object_get(root, "name"));
    if (str) strncpy(c->name, str, sizeof(c->name) - 1);

    str = json_string_value(json_object_get(root, "image"));
    if (str) strncpy(c->image, str, sizeof(c->image) - 1);

    str = json_string_value(json_object_get(root, "command"));
    if (str) strncpy(c->command, str, sizeof(c->command) - 1);

    c->pid = (pid_t)json_integer_value(json_object_get(root, "pid"));
    c->status = (container_status_t)json_integer_value(json_object_get(root, "status"));
    c->created_at = (time_t)json_integer_value(json_object_get(root, "created_at"));

    /* Network */
    json_t *network = json_object_get(root, "network");
    if (network) {
        str = json_string_value(json_object_get(network, "ip"));
        if (str) strncpy(c->network.ip, str, sizeof(c->network.ip) - 1);

        str = json_string_value(json_object_get(network, "gateway"));
        if (str) strncpy(c->network.gateway, str, sizeof(c->network.gateway) - 1);

        str = json_string_value(json_object_get(network, "veth_host"));
        if (str) strncpy(c->network.veth_host, str, sizeof(c->network.veth_host) - 1);

        str = json_string_value(json_object_get(network, "veth_guest"));
        if (str) strncpy(c->network.veth_guest, str, sizeof(c->network.veth_guest) - 1);

        str = json_string_value(json_object_get(network, "ports"));
        if (str) strncpy(c->network.ports, str, sizeof(c->network.ports) - 1);
    }

    /* Resources */
    json_t *resources = json_object_get(root, "resources");
    if (resources) {
        c->resources.memory_bytes = (size_t)json_integer_value(
            json_object_get(resources, "memory_bytes"));
        c->resources.cpu_percentage = (int)json_integer_value(
            json_object_get(resources, "cpu_percentage"));
    }

    /* Paths */
    str = json_string_value(json_object_get(root, "rootfs_path"));
    if (str) strncpy(c->rootfs_path, str, sizeof(c->rootfs_path) - 1);

    str = json_string_value(json_object_get(root, "log_path"));
    if (str) strncpy(c->log_path, str, sizeof(c->log_path) - 1);

    /* Reconstruct state path */
    snprintf(c->state_path, sizeof(c->state_path), "%s/%s.json", MINIBOX_STATE_DIR, c->id);

    json_decref(root);
    return c;
}

/*
 * container_free - Free a container_t allocated by container_create or container_load_state
 */
void container_free(container_t *container)
{
    free(container);
}

/*
 * container_exec - Execute a command inside a running container
 *
 * Opens the container's namespace file descriptors under /proc/<pid>/ns/
 * and uses setns() to enter each namespace. Then forks and execs
 * the requested command.
 */
int container_exec(const char *id, const char *command)
{
    if (!id || !command) return -1;

    container_t *container = container_load_state(id);
    if (!container) {
        log_message(LOG_ERROR, "Container %s not found", id);
        return -1;
    }

    if (container->status != CONTAINER_RUNNING || container->pid <= 0) {
        log_message(LOG_ERROR, "Container %s is not running", id);
        container_free(container);
        return -1;
    }

    pid_t target_pid = container->pid;
    container_free(container);

    /* Namespace types to enter */
    const char *ns_types[] = {"pid", "mnt", "net", "uts", "ipc", NULL};

    /* Open all namespace FDs first */
    int ns_fds[5];
    for (int i = 0; ns_types[i]; i++) {
        char ns_path[256];
        snprintf(ns_path, sizeof(ns_path), "/proc/%d/ns/%s", target_pid, ns_types[i]);

        ns_fds[i] = open(ns_path, O_RDONLY);
        if (ns_fds[i] < 0) {
            log_message(LOG_ERROR, "Failed to open namespace %s: %s",
                       ns_path, strerror(errno));
            /* Close already-opened FDs */
            for (int j = 0; j < i; j++)
                close(ns_fds[j]);
            return -1;
        }
    }

    /* Fork to enter namespaces (setns + exec replaces the process) */
    pid_t pid = fork();
    if (pid < 0) {
        log_message(LOG_ERROR, "Fork failed: %s", strerror(errno));
        for (int i = 0; ns_types[i]; i++)
            close(ns_fds[i]);
        return -1;
    }

    if (pid == 0) {
        /* Child: enter each namespace */
        for (int i = 0; ns_types[i]; i++) {
            if (setns(ns_fds[i], 0) != 0) {
                fprintf(stderr, "Failed to enter %s namespace: %s\n",
                       ns_types[i], strerror(errno));
                _exit(1);
            }
            close(ns_fds[i]);
        }

        /* Parse command into argv */
        char *cmd_copy = strdup(command);
        char *argv[64];
        int argc = 0;
        char *token = strtok(cmd_copy, " ");
        while (token && argc < 63) {
            argv[argc++] = token;
            token = strtok(NULL, " ");
        }
        argv[argc] = NULL;

        /* Execute the command */
        execvp(argv[0], argv);
        fprintf(stderr, "Failed to exec '%s': %s\n", command, strerror(errno));
        _exit(1);
    }

    /* Parent: close namespace FDs and wait for child */
    for (int i = 0; ns_types[i]; i++)
        close(ns_fds[i]);

    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status))
        return WEXITSTATUS(status);

    return -1;
}

/*
 * container_logs - Display the log file for a container
 *
 * Reads and prints the container's log file to stdout.
 */
int container_logs(const char *id)
{
    if (!id) return -1;

    container_t *container = container_load_state(id);
    if (!container) {
        log_message(LOG_ERROR, "Container %s not found", id);
        return -1;
    }

    if (!file_exists(container->log_path)) {
        log_message(LOG_INFO, "No logs available for container %s", id);
        container_free(container);
        return 0;
    }

    char *buffer = NULL;
    size_t size = 0;
    if (read_file_contents(container->log_path, &buffer, &size) != 0) {
        log_message(LOG_ERROR, "Failed to read log file for container %s", id);
        container_free(container);
        return -1;
    }

    /* Print log contents to stdout */
    if (buffer && size > 0) {
        fwrite(buffer, 1, size, stdout);
    } else {
        printf("(no logs)\n");
    }

    free(buffer);
    container_free(container);
    return 0;
}
