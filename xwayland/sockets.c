#undef _POSIX_C_SOURCE
#define _XOPEN_SOURCE 700 // for S_ISVTX
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "sockets.h"

static const char default_tmpdir[] = "/tmp";

/**
 * Return the base directory for XWayland sockets.
 *
 * Checks the WLR_XWAYLAND_TMPDIR environment variable first,
 * falling back to /tmp.  This allows Android apps (which cannot
 * write to /tmp) to redirect XWayland sockets into an app-private
 * cache directory.
 */
static const char *get_tmpdir(void) {
	const char *dir = getenv("WLR_XWAYLAND_TMPDIR");
	if (dir && dir[0]) {
		return dir;
	}
	return default_tmpdir;
}

/* Format helpers — write into caller-supplied buffer using the
 * runtime tmpdir.  Buffer must be at least 128 bytes. */
static void fmt_lock(char *buf, size_t len, int display) {
	snprintf(buf, len, "%s/.X%d-lock", get_tmpdir(), display);
}
static void fmt_socket_dir(char *buf, size_t len) {
	snprintf(buf, len, "%s/.X11-unix", get_tmpdir());
}
static void fmt_socket(char *buf, size_t len, int display) {
	snprintf(buf, len, "%s/.X11-unix/X%d", get_tmpdir(), display);
}
#ifndef __linux__
static void fmt_socket2(char *buf, size_t len, int display) {
	snprintf(buf, len, "%s/.X11-unix/X%d_", get_tmpdir(), display);
}
#endif

bool set_cloexec(int fd, bool cloexec) {
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1) {
		wlr_log_errno(WLR_ERROR, "fcntl failed");
		return false;
	}
	if (cloexec) {
		flags = flags | FD_CLOEXEC;
	} else {
		flags = flags & ~FD_CLOEXEC;
	}
	if (fcntl(fd, F_SETFD, flags) == -1) {
		wlr_log_errno(WLR_ERROR, "fcntl failed");
		return false;
	}
	return true;
}

static int open_socket(struct sockaddr_un *addr, size_t path_size) {
	int fd, rc;
	socklen_t size = offsetof(struct sockaddr_un, sun_path) + path_size + 1;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		wlr_log_errno(WLR_ERROR, "Failed to create socket %c%s",
			addr->sun_path[0] ? addr->sun_path[0] : '@',
			addr->sun_path + 1);
		return -1;
	}
	if (!set_cloexec(fd, true)) {
		close(fd);
		return -1;
	}

	if (addr->sun_path[0]) {
		unlink(addr->sun_path);
	}
	if (bind(fd, (struct sockaddr*)addr, size) < 0) {
		rc = errno;
		wlr_log_errno(WLR_ERROR, "Failed to bind socket %c%s",
			addr->sun_path[0] ? addr->sun_path[0] : '@',
			addr->sun_path + 1);
		goto cleanup;
	}
	if (listen(fd, 1) < 0) {
		rc = errno;
		wlr_log_errno(WLR_ERROR, "Failed to listen to socket %c%s",
			addr->sun_path[0] ? addr->sun_path[0] : '@',
			addr->sun_path + 1);
		goto cleanup;
	}

	return fd;

cleanup:
	close(fd);
	if (addr->sun_path[0]) {
		unlink(addr->sun_path);
	}
	errno = rc;
	return -1;
}

static bool check_socket_dir(const char *dir) {
	struct stat buf;

	if (lstat(dir, &buf)) {
		wlr_log_errno(WLR_ERROR, "Failed to stat %s", dir);
		return false;
	}
	if (!(buf.st_mode & S_IFDIR)) {
		wlr_log(WLR_ERROR, "%s is not a directory", dir);
		return false;
	}
	if (!((buf.st_uid == 0) || (buf.st_uid == getuid()))) {
		wlr_log(WLR_ERROR, "%s not owned by root or us", dir);
		return false;
	}
	if (!(buf.st_mode & S_ISVTX)) {
		/* we can deal with no sticky bit... */
		if ((buf.st_mode & (S_IWGRP | S_IWOTH))) {
			/* but not if other users can mess with our sockets */
			wlr_log(WLR_ERROR, "sticky bit not set on %s", dir);
			return false;
		}
	}
	return true;
}

static bool open_sockets(int socks[2], int display) {
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	size_t path_size;
	char dir_buf[128], sock_buf[128];

	fmt_socket_dir(dir_buf, sizeof(dir_buf));
	if (mkdir(dir_buf, 0755) == 0) {
		wlr_log(WLR_INFO, "Created %s ourselves -- other users will "
			"be unable to create X11 UNIX sockets of their own",
			dir_buf);
	} else if (errno != EEXIST) {
		wlr_log_errno(WLR_ERROR, "Unable to mkdir %s", dir_buf);
		return false;
	} else if (!check_socket_dir(dir_buf)) {
		return false;
	}

#ifdef __linux__
	addr.sun_path[0] = 0;
	fmt_socket(sock_buf, sizeof(sock_buf), display);
	path_size = strlen(sock_buf);
	memcpy(addr.sun_path + 1, sock_buf, path_size);
#else
	fmt_socket2(sock_buf, sizeof(sock_buf), display);
	path_size = strlen(sock_buf);
	memcpy(addr.sun_path, sock_buf, path_size + 1);
#endif
	socks[0] = open_socket(&addr, path_size);
	if (socks[0] < 0) {
		return false;
	}

	fmt_socket(sock_buf, sizeof(sock_buf), display);
	path_size = strlen(sock_buf);
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_buf);
	socks[1] = open_socket(&addr, path_size);
	if (socks[1] < 0) {
		close(socks[0]);
		socks[0] = -1;
		return false;
	}

	return true;
}

void unlink_display_sockets(int display) {
	char sun_path[128];

	fmt_socket(sun_path, sizeof(sun_path), display);
	unlink(sun_path);

#ifndef __linux__
	fmt_socket2(sun_path, sizeof(sun_path), display);
	unlink(sun_path);
#endif

	fmt_lock(sun_path, sizeof(sun_path), display);
	unlink(sun_path);
}

int open_display_sockets(int socks[2]) {
	int lock_fd, display;
	char lock_name[128];

	for (display = 0; display <= 32; display++) {
		fmt_lock(lock_name, sizeof(lock_name), display);
		if ((lock_fd = open(lock_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0444)) >= 0) {
			if (!open_sockets(socks, display)) {
				unlink(lock_name);
				close(lock_fd);
				continue;
			}
			char pid[12];
			snprintf(pid, sizeof(pid), "%10d", getpid());
			if (write(lock_fd, pid, sizeof(pid) - 1) != sizeof(pid) - 1) {
				unlink(lock_name);
				close(lock_fd);
				continue;
			}
			close(lock_fd);
			break;
		}

		if ((lock_fd = open(lock_name, O_RDONLY | O_CLOEXEC)) < 0) {
			continue;
		}

		char pid[12] = { 0 }, *end_pid;
		ssize_t bytes = read(lock_fd, pid, sizeof(pid) - 1);
		close(lock_fd);

		if (bytes != sizeof(pid) - 1) {
			continue;
		}
		long int read_pid;
		read_pid = strtol(pid, &end_pid, 10);
		if (read_pid < 0 || read_pid > INT32_MAX || end_pid != pid + sizeof(pid) - 2) {
			continue;
		}
		errno = 0;
		if (kill((pid_t)read_pid, 0) != 0 && errno == ESRCH) {
			if (unlink(lock_name) != 0) {
				continue;
			}
			// retry
			display--;
			continue;
		}
	}

	if (display > 32) {
		wlr_log(WLR_ERROR, "No display available in the first 33");
		return -1;
	}

	return display;
}
