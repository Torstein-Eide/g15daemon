#include <config.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "peer_ident.h"

static void fallback_label(int sockfd, char *out, size_t out_len) {
	struct sockaddr_in peer;
	socklen_t len = sizeof(peer);

	if (getpeername(sockfd, (struct sockaddr *)&peer, &len) == 0) {
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
		snprintf(out, out_len, "peer %s:%u", ip, ntohs(peer.sin_port));
	} else {
		snprintf(out, out_len, "peer (unknown)");
	}
}

/* Address field in /proc/net/tcp is the raw in-memory bytes of
 * sin_addr.s_addr (network byte order) printed as hex - compares equal to
 * s_addr directly, no ntohl(). Port field is printed as the plain human
 * port number in hex - compares equal to ntohs(sin_port). Verified live
 * against a real loopback connection before writing this. */
static int parse_addr_port(const char *field, unsigned int *addr, unsigned int *port) {
	return sscanf(field, "%x:%x", addr, port) == 2;
}

/* Find the /proc/net/tcp row that is the *peer's own* outbound socket -
 * local=peer's address:port, remote=our own address:port as seen from
 * their side - and return its inode, or 0 if not found. */
static unsigned long find_peer_inode(int sockfd) {
	struct sockaddr_in us, peer;
	socklen_t ulen = sizeof(us), plen = sizeof(peer);
	FILE *f;
	char line[512];
	unsigned long inode = 0;

	if (getsockname(sockfd, (struct sockaddr *)&us, &ulen) != 0)
		return 0;
	if (getpeername(sockfd, (struct sockaddr *)&peer, &plen) != 0)
		return 0;

	f = fopen("/proc/net/tcp", "r");
	if (!f)
		return 0;

	if (!fgets(line, sizeof(line), f)) { /* header */
		fclose(f);
		return 0;
	}

	while (fgets(line, sizeof(line), f)) {
		char local_field[32], rem_field[32];
		unsigned int local_addr, local_port, rem_addr, rem_port;
		unsigned long row_inode;

		if (sscanf(line, "%*d: %31s %31s %*x %*x:%*x %*x:%*x %*x %*d %*d %lu",
			   local_field, rem_field, &row_inode) != 3)
			continue;
		if (!parse_addr_port(local_field, &local_addr, &local_port))
			continue;
		if (!parse_addr_port(rem_field, &rem_addr, &rem_port))
			continue;

		if (local_addr == peer.sin_addr.s_addr && local_port == ntohs(peer.sin_port) &&
		    rem_addr == us.sin_addr.s_addr && rem_port == ntohs(us.sin_port)) {
			inode = row_inode;
			break;
		}
	}
	fclose(f);
	return inode;
}

/* Scan /proc/<pid>/fd/ of every process for a symlink to socket:[inode],
 * returning the owning pid, or 0 if not found / not permitted. */
static pid_t find_inode_owner(unsigned long inode) {
	char target[64];
	DIR *proc_dir;
	struct dirent *pentry;

	snprintf(target, sizeof(target), "socket:[%lu]", inode);

	proc_dir = opendir("/proc");
	if (!proc_dir)
		return 0;

	while ((pentry = readdir(proc_dir)) != NULL) {
		pid_t pid;
		char fd_dir_path[64];
		DIR *fd_dir;
		struct dirent *fentry;

		if (pentry->d_name[0] < '0' || pentry->d_name[0] > '9')
			continue;
		pid = (pid_t)atoi(pentry->d_name);

		snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);
		fd_dir = opendir(fd_dir_path);
		if (!fd_dir) /* not permitted to read this pid's fds, or it exited - skip */
			continue;

		while ((fentry = readdir(fd_dir)) != NULL) {
			char link_path[320], link_target[80];
			ssize_t n;

			if (fentry->d_name[0] == '.')
				continue;
			snprintf(link_path, sizeof(link_path), "%s/%s", fd_dir_path, fentry->d_name);
			n = readlink(link_path, link_target, sizeof(link_target) - 1);
			if (n <= 0)
				continue;
			link_target[n] = '\0';
			if (strcmp(link_target, target) == 0) {
				closedir(fd_dir);
				closedir(proc_dir);
				return pid;
			}
		}
		closedir(fd_dir);
	}
	closedir(proc_dir);
	return 0;
}

void g15_resolve_peer_name(int sockfd, char *out, size_t out_len) {
	unsigned long inode;
	pid_t pid;
	char comm_path[32];
	FILE *f;

	inode = find_peer_inode(sockfd);
	if (!inode) {
		fallback_label(sockfd, out, out_len);
		return;
	}

	pid = find_inode_owner(inode);
	if (!pid) {
		fallback_label(sockfd, out, out_len);
		return;
	}

	snprintf(comm_path, sizeof(comm_path), "/proc/%d/comm", pid);
	f = fopen(comm_path, "r");
	if (!f) {
		fallback_label(sockfd, out, out_len);
		return;
	}
	if (!fgets(out, out_len, f)) {
		fclose(f);
		fallback_label(sockfd, out, out_len);
		return;
	}
	fclose(f);

	/* strip trailing newline from /proc/<pid>/comm */
	size_t len = strlen(out);
	if (len && out[len - 1] == '\n')
		out[len - 1] = '\0';
}
