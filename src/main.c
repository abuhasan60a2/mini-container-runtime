/*
 * main.c - CLI entry point for minibox container runtime
 *
 * Provides a Docker-like command line interface for managing containers:
 *   minibox run <image> <command>   - Create and start a container
 *   minibox ps                      - List all containers
 *   minibox exec <id> <command>     - Execute command in running container
 *   minibox stop <id>               - Stop a running container
 *   minibox rm <id>                 - Remove a stopped container
 *   minibox logs <id>               - Show container logs
 *   minibox --help                  - Display usage information
 *   minibox --version               - Show version
 *
 * Arguments are parsed using getopt_long(3) for GNU-style flags.
 *
 * All container operations require root privileges (euid == 0).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>

#include "container.h"
#include "cgroup.h"
#include "network.h"
#include "utils.h"

#define MINIBOX_VERSION "0.1.0"

/* Global container reference for signal handler cleanup */
static container_t *g_running_container = NULL;

/*
 * print_usage - Display help text for all commands and flags
 */
static void print_usage(const char *prog)
{
    printf("minibox - Minimal Container Runtime v%s\n\n", MINIBOX_VERSION);
    printf("Usage:\n");
    printf("  %s run [options] <image> <command> [args...]\n", prog);
    printf("  %s ps\n", prog);
    printf("  %s exec <container-id> <command> [args...]\n", prog);
    printf("  %s stop <container-id>\n", prog);
    printf("  %s rm <container-id>\n", prog);
    printf("  %s logs <container-id>\n", prog);
    printf("\n");
    printf("Options for 'run':\n");
    printf("  -d, --detach              Run container in background\n");
    printf("  -p, --port <host:guest>   Port mapping (repeatable)\n");
    printf("  -v, --volume <host:guest> Volume mount (repeatable)\n");
    printf("  -e, --env <KEY=VALUE>     Environment variable (repeatable)\n");
    printf("  --memory <size>           Memory limit (e.g., 256m, 1g)\n");
    printf("  --cpu <percentage>        CPU limit (1-100)\n");
    printf("  --name <name>             Container name\n");
    printf("\n");
    printf("General options:\n");
    printf("  -h, --help                Show this help message\n");
    printf("  -V, --version             Show version\n");
    printf("  --debug                   Enable debug logging\n");
    printf("\n");
    printf("Examples:\n");
    printf("  # Run an Alpine container\n");
    printf("  sudo %s run alpine-rootfs.tar.gz /bin/sh\n\n", prog);
    printf("  # Run with resource limits\n");
    printf("  sudo %s run --memory 128m --cpu 25 alpine-rootfs.tar.gz /bin/echo hello\n\n", prog);
    printf("  # Run in background with port forwarding\n");
    printf("  sudo %s run -d -p 8080:80 --name web alpine-rootfs.tar.gz /usr/sbin/httpd -f\n\n", prog);
    printf("  # List containers\n");
    printf("  sudo %s ps\n\n", prog);
    printf("  # Execute command in running container\n");
    printf("  sudo %s exec <id> /bin/ps aux\n\n", prog);
    printf("  # Stop and remove container\n");
    printf("  sudo %s stop <id>\n", prog);
    printf("  sudo %s rm <id>\n", prog);
}

/*
 * print_version - Display version information
 */
static void print_version(void)
{
    printf("minibox version %s\n", MINIBOX_VERSION);
    printf("A minimal container runtime demonstrating Linux namespaces,\n");
    printf("cgroups, network isolation, and pivot_root.\n");
}

/*
 * sigint_handler - Handle Ctrl-C for graceful cleanup
 *
 * When the user presses Ctrl-C while a container is running,
 * we want to stop the container cleanly rather than leaving
 * orphaned processes and cgroup entries.
 */
static void sigint_handler(int sig)
{
    (void)sig;
    fprintf(stderr, "\nReceived interrupt signal, cleaning up...\n");

    if (g_running_container) {
        container_stop(g_running_container->id);
    }

    exit(0);
}

/*
 * sigchld_handler - Reap child processes
 *
 * When a container process exits, the parent needs to call
 * waitpid() to prevent zombie processes.
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    /* Reap all terminated children (non-blocking) */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/*
 * cmd_run - Handle the 'minibox run' command
 *
 * Parses run-specific flags, creates a container, starts it,
 * and either waits for it (foreground) or returns (detached).
 */
static int cmd_run(int argc, char *argv[])
{
    container_config_t config;
    memset(&config, 0, sizeof(config));

    /* Long options for the run command */
    static struct option long_options[] = {
        {"detach",  no_argument,       0, 'd'},
        {"port",    required_argument, 0, 'p'},
        {"volume",  required_argument, 0, 'v'},
        {"env",     required_argument, 0, 'e'},
        {"memory",  required_argument, 0, 'm'},
        {"cpu",     required_argument, 0, 'c'},
        {"name",    required_argument, 0, 'n'},
        {"help",    no_argument,       0, 'h'},
        {"debug",   no_argument,       0, 'D'},
        {0, 0, 0, 0}
    };

    /* Temporary buffers for accumulating repeatable flags */
    char port_buf[1024] = "";
    char vol_buf[1024] = "";
    char env_buf[2048] = "";

    /* Reset getopt */
    optind = 1;

    int opt;
    while ((opt = getopt_long(argc, argv, "dp:v:e:m:c:n:hD", long_options, NULL)) != -1) {
        switch (opt) {
        case 'd':
            config.detach = 1;
            break;

        case 'p':
            /* Accumulate port mappings: "8080:80,9090:90" */
            if (strlen(port_buf) > 0) {
                strncat(port_buf, ",", sizeof(port_buf) - strlen(port_buf) - 1);
            }
            strncat(port_buf, optarg, sizeof(port_buf) - strlen(port_buf) - 1);
            break;

        case 'v':
            /* Accumulate volume mounts */
            if (strlen(vol_buf) > 0) {
                strncat(vol_buf, ",", sizeof(vol_buf) - strlen(vol_buf) - 1);
            }
            strncat(vol_buf, optarg, sizeof(vol_buf) - strlen(vol_buf) - 1);
            break;

        case 'e':
            /* Accumulate environment variables */
            if (strlen(env_buf) > 0) {
                strncat(env_buf, ",", sizeof(env_buf) - strlen(env_buf) - 1);
            }
            strncat(env_buf, optarg, sizeof(env_buf) - strlen(env_buf) - 1);
            break;

        case 'm':
            config.memory_limit = parse_memory_string(optarg);
            if (config.memory_limit == 0) {
                fprintf(stderr, "Invalid memory limit: %s\n", optarg);
                return 1;
            }
            break;

        case 'c':
            config.cpu_limit = atoi(optarg);
            if (config.cpu_limit < 1 || config.cpu_limit > 100) {
                fprintf(stderr, "CPU limit must be 1-100 (got %s)\n", optarg);
                return 1;
            }
            break;

        case 'n':
            strncpy(config.name, optarg, sizeof(config.name) - 1);
            break;

        case 'D':
            g_log_level = LOG_DEBUG;
            break;

        case 'h':
            print_usage(argv[0]);
            return 0;

        default:
            fprintf(stderr, "Unknown option. Use --help for usage.\n");
            return 1;
        }
    }

    /* After flags, we need at least: image command */
    if (optind >= argc) {
        fprintf(stderr, "Error: Missing image and command.\n");
        fprintf(stderr, "Usage: minibox run [options] <image> <command> [args...]\n");
        return 1;
    }

    /* Image is the first non-option argument */
    strncpy(config.image, argv[optind], sizeof(config.image) - 1);
    optind++;

    if (optind >= argc) {
        fprintf(stderr, "Error: Missing command.\n");
        fprintf(stderr, "Usage: minibox run [options] <image> <command> [args...]\n");
        return 1;
    }

    /* Command is all remaining arguments joined with spaces */
    char command_buf[256] = "";
    for (int i = optind; i < argc; i++) {
        if (i > optind) {
            strncat(command_buf, " ", sizeof(command_buf) - strlen(command_buf) - 1);
        }
        strncat(command_buf, argv[i], sizeof(command_buf) - strlen(command_buf) - 1);
    }
    strncpy(config.command, command_buf, sizeof(config.command) - 1);

    /* Copy accumulated flags */
    strncpy(config.port_mappings, port_buf, sizeof(config.port_mappings) - 1);
    strncpy(config.volume_mounts, vol_buf, sizeof(config.volume_mounts) - 1);
    strncpy(config.env_vars, env_buf, sizeof(config.env_vars) - 1);

    /* Validate image exists */
    if (!file_exists(config.image)) {
        fprintf(stderr, "Error: Image not found: %s\n", config.image);
        return 1;
    }

    log_message(LOG_INFO, "Creating container: image=%s, command=%s",
               config.image, config.command);

    /* Create the container */
    container_t *container = container_create(&config);
    if (!container) {
        fprintf(stderr, "Error: Failed to create container\n");
        return 1;
    }

    /* Setup signal handler for cleanup */
    g_running_container = container;
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Start the container */
    if (container_start(container) != 0) {
        fprintf(stderr, "Error: Failed to start container\n");
        container_remove(container->id);
        container_free(container);
        return 1;
    }

    printf("%s\n", container->id);

    if (config.detach) {
        /* Detached mode: print ID and return */
        log_message(LOG_INFO, "Container %s running in background (PID=%d)",
                   container->id, container->pid);
        container_free(container);
        return 0;
    }

    /* Foreground mode: wait for the container process to exit */
    log_message(LOG_INFO, "Waiting for container %s to exit...", container->id);

    int status;
    pid_t result = waitpid(container->pid, &status, 0);

    g_running_container = NULL;

    if (result > 0) {
        if (WIFEXITED(status)) {
            int exit_code = WEXITSTATUS(status);
            log_message(LOG_INFO, "Container exited with code %d", exit_code);
        } else if (WIFSIGNALED(status)) {
            log_message(LOG_INFO, "Container killed by signal %d", WTERMSIG(status));
        }
    }

    /* Clean up resources */
    container->status = CONTAINER_STOPPED;
    container->pid = 0;
    container_save_state(container);

    /* Cleanup cgroup and network */
    cleanup_cgroup(container->id);
    cleanup_network(container->id);

    container_free(container);
    return 0;
}

/*
 * cmd_ps - Handle the 'minibox ps' command
 *
 * Lists all containers with their ID, name, image, status, and PID.
 */
static int cmd_ps(void)
{
    int count = 0;
    container_t **containers = container_list(&count);

    /* Print header */
    printf("%-14s %-20s %-30s %-10s %-8s %-16s\n",
           "CONTAINER ID", "NAME", "IMAGE", "STATUS", "PID", "IP");
    printf("%-14s %-20s %-30s %-10s %-8s %-16s\n",
           "------------", "----", "-----", "------", "---", "--");

    if (!containers || count == 0) {
        /* No containers to display */
        return 0;
    }

    for (int i = 0; i < count; i++) {
        container_t *c = containers[i];
        printf("%-14s %-20s %-30s %-10s %-8d %-16s\n",
               c->id,
               c->name,
               c->image,
               container_status_string(c->status),
               c->pid,
               c->network.ip);
        container_free(c);
    }

    free(containers);
    return 0;
}

/*
 * cmd_exec - Handle the 'minibox exec' command
 *
 * Executes a command inside a running container by entering
 * its namespaces.
 */
static int cmd_exec(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: minibox exec <container-id> <command> [args...]\n");
        return 1;
    }

    const char *container_id = argv[0];

    /* Build command from remaining args */
    char command[256] = "";
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            strncat(command, " ", sizeof(command) - strlen(command) - 1);
        }
        strncat(command, argv[i], sizeof(command) - strlen(command) - 1);
    }

    return container_exec(container_id, command);
}

/*
 * cmd_stop - Handle the 'minibox stop' command
 */
static int cmd_stop(const char *container_id)
{
    if (!container_id) {
        fprintf(stderr, "Usage: minibox stop <container-id>\n");
        return 1;
    }

    return container_stop(container_id);
}

/*
 * cmd_rm - Handle the 'minibox rm' command
 */
static int cmd_rm(const char *container_id)
{
    if (!container_id) {
        fprintf(stderr, "Usage: minibox rm <container-id>\n");
        return 1;
    }

    return container_remove(container_id);
}

/*
 * cmd_logs - Handle the 'minibox logs' command
 */
static int cmd_logs(const char *container_id)
{
    if (!container_id) {
        fprintf(stderr, "Usage: minibox logs <container-id>\n");
        return 1;
    }

    return container_logs(container_id);
}

/*
 * find_command - Find the subcommand in argv
 *
 * Skips global options (--debug, etc.) to find the actual command
 * (run, ps, exec, stop, rm, logs).
 *
 * Returns the index of the command in argv, or -1 if not found.
 */
static int find_command(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        /* Skip global flags */
        if (strcmp(argv[i], "--debug") == 0 || strcmp(argv[i], "-D") == 0) {
            g_log_level = LOG_DEBUG;
            continue;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            print_version();
            exit(0);
        }
        /* Found the command */
        if (argv[i][0] != '-') {
            return i;
        }
    }
    return -1;
}

/*
 * main - Entry point
 *
 * Dispatches to the appropriate command handler based on argv.
 */
int main(int argc, char *argv[])
{
    /* Install signal handlers */
    signal(SIGCHLD, sigchld_handler);

    /* Need at least one argument (the command) */
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    /* Find the subcommand */
    int cmd_idx = find_command(argc, argv);
    if (cmd_idx < 0) {
        print_usage(argv[0]);
        return 1;
    }

    const char *command = argv[cmd_idx];

    /* Most commands require root privileges */
    if (strcmp(command, "run") == 0 ||
        strcmp(command, "exec") == 0 ||
        strcmp(command, "stop") == 0 ||
        strcmp(command, "rm") == 0) {
        if (check_root_privileges() != 0) {
            fprintf(stderr, "Error: This command requires root privileges.\n");
            fprintf(stderr, "Try: sudo %s %s ...\n", argv[0], command);
            return 1;
        }
    }

    /* Dispatch to command handler */
    if (strcmp(command, "run") == 0) {
        /* Pass remaining args (after "run") to cmd_run */
        return cmd_run(argc - cmd_idx, argv + cmd_idx);

    } else if (strcmp(command, "ps") == 0) {
        return cmd_ps();

    } else if (strcmp(command, "exec") == 0) {
        /* Pass args after "exec" */
        return cmd_exec(argc - cmd_idx - 1, argv + cmd_idx + 1);

    } else if (strcmp(command, "stop") == 0) {
        const char *id = (cmd_idx + 1 < argc) ? argv[cmd_idx + 1] : NULL;
        return cmd_stop(id);

    } else if (strcmp(command, "rm") == 0) {
        const char *id = (cmd_idx + 1 < argc) ? argv[cmd_idx + 1] : NULL;
        return cmd_rm(id);

    } else if (strcmp(command, "logs") == 0) {
        const char *id = (cmd_idx + 1 < argc) ? argv[cmd_idx + 1] : NULL;
        return cmd_logs(id);

    } else {
        fprintf(stderr, "Unknown command: '%s'\n", command);
        fprintf(stderr, "Available commands: run, ps, exec, stop, rm, logs\n");
        fprintf(stderr, "Use '%s --help' for more information.\n", argv[0]);
        return 1;
    }
}
