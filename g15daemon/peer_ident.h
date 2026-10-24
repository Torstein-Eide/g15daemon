#ifndef G15DAEMON_PEER_IDENT_H
#define G15DAEMON_PEER_IDENT_H

#include <stddef.h>
#include <sys/types.h>

/* Best-effort identification of the process on the other end of an
 * accepted AF_INET (loopback) TCP socket, e.g. for the sd_notify STATUS=
 * foreground field and the registered-clients log line.
 *
 * There's no SO_PEERCRED for AF_INET (that's Unix-socket only), so this
 * matches the connection's 4-tuple against /proc/net/tcp to find the
 * peer's own socket inode, then scans /proc/<pid>/fd/ for every process
 * looking for a symlink to that inode to find the owning PID, then reads
 * /proc/<pid>/comm.
 *
 * Never fails the caller: always writes a printable, NUL-terminated
 * string into `out` (the process comm on success, "peer <ip>:<port>" as a
 * fallback when resolution isn't possible - e.g. no permission to read
 * another uid's /proc/<pid>/fd/, which needs CAP_SYS_PTRACE once
 * g15daemon has dropped root - see the AmbientCapabilities note in
 * contrib/init/g15daemon.service). *pid_out (if non-NULL) gets the
 * owning PID, or 0 when it couldn't be resolved. */
void g15_resolve_peer_name(int sockfd, char *out, size_t out_len, pid_t *pid_out);

#endif /* G15DAEMON_PEER_IDENT_H */
