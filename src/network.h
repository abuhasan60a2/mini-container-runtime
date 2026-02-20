/*
 * network.h - Network isolation and setup for minibox
 *
 * Creates virtual ethernet (veth) pairs to connect containers to
 * the host, configures IP addresses, routes, and NAT rules for
 * port forwarding.
 *
 * Network topology per container:
 *   Host:      veth-<container_id>  10.10.10.1/24
 *   Container: eth0                 10.10.10.2/24
 *   Gateway:   10.10.10.1
 *
 * Uses netlink sockets (NETLINK_ROUTE) for interface creation and
 * configuration, and iptables for NAT/port forwarding.
 *
 * References:
 *   netlink(7)       - Netlink socket protocol
 *   rtnetlink(7)     - Routing netlink
 *   veth(4)          - Virtual ethernet pair devices
 *   ip-link(8)       - Network device configuration
 *   iptables(8)      - IP packet filter rules
 */

#ifndef MINIBOX_NETWORK_H
#define MINIBOX_NETWORK_H

#include <sys/types.h>

/* Network configuration constants */
#define CONTAINER_IP       "10.10.10.2"
#define HOST_IP            "10.10.10.1"
#define NETMASK            "255.255.255.0"
#define SUBNET_PREFIX_LEN  24

/*
 * setup_network - Main network setup orchestration
 *
 * @container_pid: PID of the container process
 * @container_id:  Container ID (for naming the veth pair)
 *
 * Returns: 0 on success, -1 on failure
 *
 * Steps:
 * 1. Create veth pair: veth-<id> (host) and eth0 (guest)
 * 2. Move guest veth into container's network namespace
 * 3. Configure host veth with IP 10.10.10.1/24
 * 4. Bring up host veth
 * 5. Configure container veth with IP 10.10.10.2/24
 * 6. Enable IP forwarding on host
 * 7. Setup MASQUERADE NAT for container traffic
 */
int setup_network(pid_t container_pid, const char *container_id);

/*
 * create_veth_pair - Create a virtual ethernet pair
 *
 * @veth_host:  Name for the host-side interface
 * @veth_guest: Name for the guest-side interface
 *
 * Returns: 0 on success, -1 on failure
 *
 * Uses the `ip` command to create the veth pair.
 * Both interfaces start in the host's network namespace.
 */
int create_veth_pair(const char *veth_host, const char *veth_guest);

/*
 * move_if_to_netns - Move a network interface to a process's network namespace
 *
 * @ifname: Interface name to move
 * @pid:    Target process PID (whose netns to enter)
 *
 * Returns: 0 on success, -1 on failure
 *
 * Uses `ip link set <ifname> netns <pid>`.
 */
int move_if_to_netns(const char *ifname, pid_t pid);

/*
 * setup_container_network - Configure networking inside the container
 *
 * @container_pid: PID of the container
 *
 * Returns: 0 on success, -1 on failure
 *
 * Uses nsenter to configure from the host side:
 * - Set interface up
 * - Assign IP address
 * - Set default route
 * - Write /etc/resolv.conf
 */
int setup_container_network(pid_t container_pid);

/*
 * configure_nat_rules - Setup iptables NAT rules for port forwarding
 *
 * @port_mapping: Port mapping string "host_port:container_port[,...]"
 *
 * Returns: 0 on success, -1 on failure
 *
 * Creates DNAT rules to forward traffic from the host port
 * to the container's IP and port.
 */
int configure_nat_rules(const char *port_mapping);

/*
 * cleanup_network - Remove network resources for a container
 *
 * @container_id: Container ID
 *
 * Returns: 0 on success, -1 on failure
 *
 * Deletes the veth pair (deleting one end automatically
 * deletes the other) and removes iptables rules.
 */
int cleanup_network(const char *container_id);

#endif /* MINIBOX_NETWORK_H */
