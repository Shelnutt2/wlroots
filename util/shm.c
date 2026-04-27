#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wlr/config.h>
#include "util/shm.h"

#ifdef __ANDROID__
/* Android bionic doesn't provide shm_open/shm_unlink at API < 30.
 * Use memfd_create via syscall (available since Android 8 / API 26). */
#include <stdio.h>
#include <sys/syscall.h>
#include <linux/memfd.h>

static int android_memfd_create(const char *name, unsigned int flags) {
	return (int)syscall(SYS_memfd_create, name, flags);
}

int allocate_shm_file(size_t size) {
	int fd = android_memfd_create("wlroots", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		return -1;
	}
	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

bool allocate_shm_file_pair(size_t size, int *rw_fd_ptr, int *ro_fd_ptr) {
	int rw_fd = android_memfd_create("wlroots", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (rw_fd < 0) {
		return false;
	}
	char path[64];
	snprintf(path, sizeof(path), "/proc/self/fd/%d", rw_fd);
	int ro_fd = open(path, O_RDONLY | O_CLOEXEC);
	if (ro_fd < 0) {
		/* /proc/self/fd reopen blocked by SELinux — fall back to dup().
		 * Both FDs will be read-write, which is acceptable on Android
		 * where compositor and clients share the same UID. */
		ro_fd = dup(rw_fd);
		if (ro_fd < 0) {
			close(rw_fd);
			return false;
		}
		fcntl(ro_fd, F_SETFD, FD_CLOEXEC);
	}
	int ret;
	do {
		ret = ftruncate(rw_fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(rw_fd);
		close(ro_fd);
		return false;
	}
	*rw_fd_ptr = rw_fd;
	*ro_fd_ptr = ro_fd;
	return true;
}

#else /* !__ANDROID__ */

#define RANDNAME_PATTERN "/wlroots-XXXXXX"

static void randname(char *buf) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	long r = ts.tv_nsec;
	for (int i = 0; i < 6; ++i) {
		buf[i] = 'A'+(r&15)+(r&16)*2;
		r >>= 5;
	}
}

static int excl_shm_open(char *name) {
	int retries = 100;
	do {
		randname(name + strlen(RANDNAME_PATTERN) - 6);

		--retries;
		// CLOEXEC is guaranteed to be set by shm_open
		int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd >= 0) {
			return fd;
		}
	} while (retries > 0 && errno == EEXIST);

	return -1;
}

int allocate_shm_file(size_t size) {
	char name[] = RANDNAME_PATTERN;
	int fd = excl_shm_open(name);
	if (fd < 0) {
		return -1;
	}
	shm_unlink(name);

	int ret;
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}

	return fd;
}

bool allocate_shm_file_pair(size_t size, int *rw_fd_ptr, int *ro_fd_ptr) {
	char name[] = RANDNAME_PATTERN;
	int rw_fd = excl_shm_open(name);
	if (rw_fd < 0) {
		return false;
	}

	// CLOEXEC is guaranteed to be set by shm_open
	int ro_fd = shm_open(name, O_RDONLY, 0);
	if (ro_fd < 0) {
		shm_unlink(name);
		close(rw_fd);
		return false;
	}

	shm_unlink(name);

	// Make sure the file cannot be re-opened in read-write mode (e.g. via
	// "/proc/self/fd/" on Linux)
	if (fchmod(rw_fd, 0) != 0) {
		close(rw_fd);
		close(ro_fd);
		return false;
	}

	int ret;
	do {
		ret = ftruncate(rw_fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(rw_fd);
		close(ro_fd);
		return false;
	}

	*rw_fd_ptr = rw_fd;
	*ro_fd_ptr = ro_fd;
	return true;
}

#endif /* __ANDROID__ */
