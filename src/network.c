/*
 * network.c - Network isolation and setup for minibox
 *
 * Implements container networking by:
 * 1. Creating virtual ethernet (veth) pairs to bridge host and container
 * 2. Moving one end of the veth into the container's network namespace
 * 3. Assigning IP addresses and setting up routes
 * 4. Configuring NAT/masquerade for outbound traffic
 * 5. Setting up DNAT rules for port forwarding
 *
 * Network topology:
 *
 *   +-------------+           +------------------+
 *   |    Host     |           |    Container     |
 *   |             |           |                  |
 *   | veth-<id>   |<--------->|   eth0           |
 *   | 10.10.10.1  |   veth    |   10.10.10.2     |
 *   |    /24      |   pair    |      /24          |
 *   +-------------+           +------------------+
 *
 * This approach is similar to Docker's default bridge networking,
 * but simplified for a single container (no shared bridge).
 *
 * We use the `ip` and `iptables` commands rather than raw netlink
 * sockets for clarity and reliability. A production runtime would
 * use libnetlink or direct netlink socket communication.
 *
 * References:
 *   veth(4)         - Virtual ethernet pair devices
 *   ip-link(8)      - Network device configuration
 *   ip-address(8)   - Protocol address management
 *   ip-route(8)     - Routing table management
 *   iptables(8)     - IP packet filter administration
 *   namespaces(7)   - Network namespace isolation
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include "network.h"
#include "utils.h"

/*
 * run_cmd - Execute a shell command and check the result
 *
 * Helper that logs the command at DEBUG level and returns
 * 0 on success, -1 on failure.
 */
static int run_cmd(const char *cmd)
{
    log_message(LOG_DEBUG, "Running: %s", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        log_message(LOG_DEBUG, "Command failed (exit %d): %s", ret, cmd);
        return -1;
    }
    return 0;
}

/*
 * create_veth_pair - Create a virtual ethernet pair
 *
 * A veth pair is two virtual network interfaces connected to each other.
 * Whatever is sent on one end comes out the other. This is the fundamental
 * building block for container networking.
 *
 * After creation, both interfaces exist in the host namespace.
 * One will be moved into the container's namespace later.
 */
int create_veth_pair(const char *veth_host, const char *veth_guest)
{
    if (!veth_host || !veth_guest) return -1;

    char cmd[512];

    /* Delete any existing veth with the same name (cleanup from previous runs) */
    snprintf(cmd, sizeof(cmd), "ip link delete %s 2>/dev/null", veth_host);
    run_cmd(cmd);  /* Ignore errors - interface may not exist */

    /*
     * Create the veth pair:
     *   ip link add <host_end> type veth peer name <guest_end>
     *
     * This creates two interfaces connected back-to-back.
     * They start in DOWN state.
     */
    snprintf(cmd, sizeof(cmd),
             "ip link add %s type veth peer name %s",
             veth_host, veth_guest);

    if (run_cmd(cmd) != 0) {
        log_message(LOG_ERROR, "Failed to create veth pair (%s, %s)",
                   veth_host, veth_guest);
        return -1;
    }

    log_message(LOG_DEBUG, "Created veth pair: %s <-> %s", veth_host, veth_guest);
    return 0;
}

/*
 * move_if_to_netns - Move an interface to another network namespace
 *
 * Moving an interface into a network namespace makes it invisible
 * from the host and only accessible from within that namespace.
 * The container will see it as a regular network interface.
 *
 * We specify the target namespace by PID - the kernel looks up
 * /proc/<pid>/ns/net to find the namespace.
 */
int move_if_to_netns(const char *ifname, pid_t pid)
{
    if (!ifname || pid <= 0) return -1;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip link set %s netns %d", ifname, pid);

    if (run_cmd(cmd) != 0) {
        log_message(LOG_ERROR, "Failed to move interface %s to netns of PID %d",
                   ifname, pid);
        return -1;
    }

    log_message(LOG_DEBUG, "Moved interface %s to netns of PID %d", ifname, pid);
    return 0;
}

/*
 * setup_host_veth - Configure the host side of the veth pair
 *
 * Sets the IP address and brings the interface up.
 * This interface acts as the gateway for the container.
 */
static int setup_host_veth(const char *veth_host)
{
    char cmd[256];

    /* Assign IP address to the host veth */
    snprintf(cmd, sizeof(cmd),
             "ip addr add %s/%d dev %s",
             HOST_IP, SUBNET_PREFIX_LEN, veth_host);
    if (run_cmd(cmd) != 0) {
        log_message(LOG_ERROR, "Failed to assign IP to host veth");
        return -1;
    }

    /* Bring the interface up */
    snprintf(cmd, sizeof(cmd), "ip link set %s up", veth_host);
    if (run_cmd(cmd) != 0) {
        log_message(LOG_ERROR, "Failed to bring up host veth");
        return -1;
    }

    log_message(LOG_DEBUG, "Host veth %s configured: %s/%d",
               veth_host, HOST_IP, SUBNET_PREFIX_LEN);
    return 0;
}

/*
 * setup_container_network - Configure networking inside the container namespace
 *
 * Uses nsenter to execute commands in the container's network namespace.
 * This is done from the parent (host) process because the container
 * child process has already called execvp and can't run additional
 * network setup commands.
 *
 * Configures:
 * 1. Loopback interface (lo) - needed for localhost communication
 * 2. eth0 with the container's IP address
 * 3. Default route via the host's veth IP
 */
int setup_container_network(pid_t container_pid)
{
    char cmd[512];

    /* Bring up loopback interface
     *
     * Without this, localhost (127.0.0.1) won't work inside the container.
     * Many programs expect loopback to be available.
     */
    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip link set lo up", container_pid);
    if (run_cmd(cmd) != 0) {
        log_message(LOG_WARN, "Failed to bring up loopback in container");
    }

    /* Assign IP to container's eth0 */
    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip addr add %s/%d dev eth0",
             container_pid, CONTAINER_IP, SUBNET_PREFIX_LEN);
    if (run_cmd(cmd) != 0) {
        log_message(LOG_ERROR, "Failed to assign IP to container eth0");
        return -1;
    }

    /* Bring eth0 up */
    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip link set eth0 up", container_pid);
    if (run_cmd(cmd) != 0) {
        log_message(LOG_ERROR, "Failed to bring up container eth0");
        return -1;
    }

    /* Set default route via host veth
     *
     * All traffic from the container that isn't for the local subnet
     * (10.10.10.0/24) will be routed through the host's veth IP.
     * The host then needs IP forwarding and MASQUERADE to forward
     * this traffic to the outside world.
     */
    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip route add default via %s",
             container_pid, HOST_IP);
    if (run_cmd(cmd) != 0) {
        log_message(LOG_WARN, "Failed to set default route in container");
    }

    log_message(LOG_DEBUG, "Container network configured: eth0=%s/%d, gw=%s",
               CONTAINER_IP, SUBNET_PREFIX_LEN, HOST_IP);
    return 0;
}

/*
 * enable_ip_forwarding - Enable IP forwarding on the host
 *
 * Required for the host to forward packets between the container
 * network and the external network.
 *
 * Without this, the container can only communicate with the host,
 * not with the outside world.
 */
static int enable_ip_forwarding(void)
{
    if (write_file("/proc/sys/net/ipv4/ip_forward", "1") != 0) {
        log_message(LOG_WARN, "Failed to enable IP forwarding");
        return -1;
    }
    return 0;
}

/*
 * setup_masquerade - Setup NAT masquerading for outbound container traffic
 *
 * MASQUERADE does source NAT (SNAT): when a packet from the container
 * (10.10.10.2) goes to the external network, its source IP is rewritten
 * to the host's external IP. Reply packets are then translated back.
 *
 * This is the same technique Docker uses for its bridge networking.
 */
static int setup_masquerade(void)
{
    char cmd[512];

    /* POSTROUTING MASQUERADE rule
     *
     * -t nat: Operate on the NAT table
     * -A POSTROUTING: Append to the POSTROUTING chain (after routing decision)
     * -s 10.10.10.0/24: Only for packets from the container subnet
     * ! -o lo: Not for loopback traffic
     * -j MASQUERADE: Replace source IP with outgoing interface's IP
     */
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -A POSTROUTING -s 10.10.10.0/24 ! -o lo -j MASQUERADE 2>/dev/null");
    if (run_cmd(cmd) != 0) {
        log_message(LOG_WARN, "Failed to setup MASQUERADE (iptables may not be available)");
        return -1;
    }

    /* Also allow forwarding for the container subnet */
    snprintf(cmd, sizeof(cmd),
             "iptables -A FORWARD -s 10.10.10.0/24 -j ACCEPT 2>/dev/null");
    run_cmd(cmd);

    snprintf(cmd, sizeof(cmd),
             "iptables -A FORWARD -d 10.10.10.0/24 -j ACCEPT 2>/dev/null");
    run_cmd(cmd);

    return 0;
}

/*
 * setup_network - Main network setup orchestration
 *
 * This is called by the parent process AFTER clone() creates
 * the container process. The container is running in its own
 * network namespace, which starts empty (no interfaces at all).
 *
 * We create a veth pair in the host namespace, move one end
 * into the container, and configure both sides.
 */
int setup_network(pid_t container_pid, const char *container_id)
{
    if (container_pid <= 0 || !container_id) return -1;

    /* Build interface names
     * Host side: "veth-" + first 7 chars of container ID
     * Guest side: "veth-g" + first 5 chars (will be renamed to eth0 inside container)
     */
    char veth_host[32], veth_guest[32];
    snprintf(veth_host, sizeof(veth_host), "veth-%.7s", container_id);
    snprintf(veth_guest, sizeof(veth_guest), "vethg%.7s", container_id);

    log_message(LOG_INFO, "Setting up network: %s <-> %s (PID %d)",
               veth_host, veth_guest, container_pid);

    /* Step 1: Create the veth pair in host namespace */
    if (create_veth_pair(veth_host, veth_guest) != 0) {
        return -1;
    }

    /* Step 2: Move guest end into container's network namespace
     *
     * After this, the guest veth is only visible inside the container
     * and disappears from the host.
     */
    if (move_if_to_netns(veth_guest, container_pid) != 0) {
        /* Cleanup host veth on failure */
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ip link delete %s 2>/dev/null", veth_host);
        run_cmd(cmd);
        return -1;
    }

    /* Step 3: Rename guest interface to eth0 inside container
     *
     * Standard convention: the primary network interface in a
     * container is called eth0.
     */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "nsenter -t %d -n ip link set %s name eth0",
             container_pid, veth_guest);
    if (run_cmd(cmd) != 0) {
        log_message(LOG_WARN, "Failed to rename interface to eth0 (using original name)");
    }

    /* Step 4: Configure host side */
    if (setup_host_veth(veth_host) != 0) {
        return -1;
    }

    /* Step 5: Configure container side */
    if (setup_container_network(container_pid) != 0) {
        return -1;
    }

    /* Step 6: Enable IP forwarding for host-to-container routing */
    enable_ip_forwarding();

    /* Step 7: Setup NAT masquerading for outbound traffic */
    setup_masquerade();

    log_message(LOG_INFO, "Network setup complete for container %s", container_id);
    return 0;
}

/*
 * configure_nat_rules - Setup DNAT rules for port forwarding
 *
 * Port forwarding allows external traffic to reach container services.
 * For example, mapping host:8080 to container:80 means:
 *   curl http://host:8080 -> forwarded to container:80
 *
 * This uses iptables DNAT (Destination NAT):
 * - Incoming packets to host:8080 have their destination rewritten
 *   to 10.10.10.2:80
 * - Reply packets are automatically reverse-translated
 *
 * Port mapping format: "host_port:container_port[,host_port:container_port,...]"
 */
int configure_nat_rules(const char *port_mapping)
{
    if (!port_mapping || strlen(port_mapping) == 0)
        return 0;

    char mapping_copy[1024];
    strncpy(mapping_copy, port_mapping, sizeof(mapping_copy) - 1);
    mapping_copy[sizeof(mapping_copy) - 1] = '\0';

    /* Parse each port mapping */
    char *saveptr;
    char *entry = strtok_r(mapping_copy, ",", &saveptr);

    while (entry) {
        /* Parse "host_port:container_port" */
        char *colon = strchr(entry, ':');
        if (!colon) {
            log_message(LOG_ERROR, "Invalid port mapping: '%s' (expected host_port:container_port)",
                       entry);
            entry = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        *colon = '\0';
        int host_port = atoi(entry);
        int container_port = atoi(colon + 1);

        /* Validate port numbers */
        if (host_port < 1 || host_port > 65535 ||
            container_port < 1 || container_port > 65535) {
            log_message(LOG_ERROR, "Invalid port numbers: %d:%d (must be 1-65535)",
                       host_port, container_port);
            entry = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        /* DNAT rule: forward host_port to container_ip:container_port
         *
         * -t nat: NAT table
         * -A PREROUTING: Process before routing decision
         * -p tcp: TCP protocol only
         * --dport: Match destination port
         * -j DNAT --to-destination: Rewrite destination
         */
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "iptables -t nat -A PREROUTING -p tcp --dport %d "
                 "-j DNAT --to-destination %s:%d 2>/dev/null",
                 host_port, CONTAINER_IP, container_port);

        if (run_cmd(cmd) != 0) {
            log_message(LOG_WARN, "Failed to setup port forwarding %d -> %d",
                       host_port, container_port);
        } else {
            log_message(LOG_INFO, "Port forwarding: host:%d -> container:%d",
                       host_port, container_port);
        }

        /* Also need a rule for locally-generated traffic (OUTPUT chain)
         *
         * PREROUTING only handles external traffic. For traffic from
         * the host itself (e.g., curl localhost:8080), we need an
         * OUTPUT rule.
         */
        snprintf(cmd, sizeof(cmd),
                 "iptables -t nat -A OUTPUT -p tcp --dport %d "
                 "-j DNAT --to-destination %s:%d 2>/dev/null",
                 host_port, CONTAINER_IP, container_port);
        run_cmd(cmd);

        entry = strtok_r(NULL, ",", &saveptr);
    }

    return 0;
}

/*
 * cleanup_network - Remove all network resources for a container
 *
 * Deleting the host-side veth automatically deletes the guest side too
 * (they're a pair). We also clean up iptables rules to avoid leaking
 * NAT rules across container restarts.
 */
int cleanup_network(const char *container_id)
{
    if (!container_id) return -1;

    char veth_host[32];
    snprintf(veth_host, sizeof(veth_host), "veth-%.7s", container_id);

    /* Delete the veth pair
     *
     * Deleting one end of a veth pair automatically deletes the other.
     * This is a kernel-guaranteed behavior.
     */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "ip link delete %s 2>/dev/null", veth_host);
    run_cmd(cmd);  /* Don't check error - interface may already be gone */

    /* Clean up NAT rules
     *
     * Flush the entire PREROUTING chain of rules matching our container subnet.
     * A more sophisticated approach would track individual rules.
     */
    snprintf(cmd, sizeof(cmd),
             "iptables -t nat -D POSTROUTING -s 10.10.10.0/24 ! -o lo -j MASQUERADE 2>/dev/null");
    run_cmd(cmd);

    log_message(LOG_DEBUG, "Network cleanup complete for %s", container_id);
    return 0;
}
