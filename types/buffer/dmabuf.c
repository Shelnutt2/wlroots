#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
#include "types/wlr_buffer.h"

static const struct wlr_buffer_impl dmabuf_buffer_impl;

static struct wlr_dmabuf_buffer *dmabuf_buffer_from_buffer(
		struct wlr_buffer *wlr_buffer) {
	assert(wlr_buffer->impl == &dmabuf_buffer_impl);
	struct wlr_dmabuf_buffer *buffer = wl_container_of(wlr_buffer, buffer, base);
	return buffer;
}

static void dmabuf_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct wlr_dmabuf_buffer *buffer = dmabuf_buffer_from_buffer(wlr_buffer);
	wlr_buffer_finish(wlr_buffer);
	if (buffer->saved) {
		wlr_dmabuf_attributes_finish(&buffer->dmabuf);
	}
	free(buffer);
}

static bool dmabuf_buffer_get_dmabuf(struct wlr_buffer *wlr_buffer,
		struct wlr_dmabuf_attributes *dmabuf) {
	struct wlr_dmabuf_buffer *buffer = dmabuf_buffer_from_buffer(wlr_buffer);
	if (buffer->dmabuf.n_planes == 0) {
		return false;
	}
	*dmabuf = buffer->dmabuf;
	return true;
}

static bool dmabuf_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct wlr_dmabuf_buffer *buffer = dmabuf_buffer_from_buffer(wlr_buffer);

	wlr_log(WLR_INFO, "dmabuf CPU fallback: enter flags=0x%x n_planes=%d format=0x%x fd[0]=%d %dx%d stride=%u",
			flags, buffer->dmabuf.n_planes, buffer->dmabuf.format,
			buffer->dmabuf.fd[0], wlr_buffer->width, wlr_buffer->height,
			buffer->dmabuf.stride[0]);

	// Only support read access on single-plane buffers
	if (flags != WLR_BUFFER_DATA_PTR_ACCESS_READ ||
			buffer->dmabuf.n_planes != 1) {
		wlr_log(WLR_ERROR, "dmabuf CPU fallback: rejected flags=0x%x n_planes=%d",
				flags, buffer->dmabuf.n_planes);
		return false;
	}

	size_t size = (size_t)buffer->dmabuf.stride[0] * wlr_buffer->height;

	// Check if fd is large enough for the requested mapping
	struct stat st;
	if (fstat(buffer->dmabuf.fd[0], &st) == 0) {
		if ((off_t)(size + buffer->dmabuf.offset[0]) > st.st_size) {
			wlr_log(WLR_ERROR, "dmabuf CPU fallback: fd size %jd < needed %zu+%u",
					(intmax_t)st.st_size, size, buffer->dmabuf.offset[0]);
			// If offset is 0 and fd has data, try with actual fd size
			if (buffer->dmabuf.offset[0] == 0 && st.st_size > 0) {
				size = (size_t)st.st_size;
			} else {
				return false;
			}
		}
	}

	// Handle non-page-aligned offset
	long page_size = sysconf(_SC_PAGESIZE);
	off_t aligned_offset = buffer->dmabuf.offset[0] & ~(page_size - 1);
	size_t extra = buffer->dmabuf.offset[0] - aligned_offset;

	void *map = mmap(NULL, size + extra, PROT_READ, MAP_SHARED,
			buffer->dmabuf.fd[0], aligned_offset);
	if (map == MAP_FAILED) {
		wlr_log(WLR_ERROR, "dmabuf CPU fallback mmap failed: fd=%d size=%zu offset=%u errno=%d (%s)",
				buffer->dmabuf.fd[0], size, buffer->dmabuf.offset[0],
				errno, strerror(errno));
		return false;
	}

	wlr_log(WLR_DEBUG, "dmabuf CPU fallback mmap OK: fd=%d %zux%d stride=%u",
			buffer->dmabuf.fd[0], size, wlr_buffer->height, buffer->dmabuf.stride[0]);
	wlr_log(WLR_INFO, "dmabuf CPU fallback: format=0x%08x → will try gles2_texture_from_pixels",
			buffer->dmabuf.format);

	buffer->mapped_data = map;
	buffer->mapped_size = size + extra;
	*data = (char *)map + extra;
	*format = buffer->dmabuf.format;
	*stride = buffer->dmabuf.stride[0];
	return true;
}

static void dmabuf_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
	struct wlr_dmabuf_buffer *buffer = dmabuf_buffer_from_buffer(wlr_buffer);
	if (buffer->mapped_data) {
		munmap(buffer->mapped_data, buffer->mapped_size);
		buffer->mapped_data = NULL;
		buffer->mapped_size = 0;
	}
}

static const struct wlr_buffer_impl dmabuf_buffer_impl = {
	.destroy = dmabuf_buffer_destroy,
	.get_dmabuf = dmabuf_buffer_get_dmabuf,
	.begin_data_ptr_access = dmabuf_buffer_begin_data_ptr_access,
	.end_data_ptr_access = dmabuf_buffer_end_data_ptr_access,
};

struct wlr_dmabuf_buffer *dmabuf_buffer_create(
		struct wlr_dmabuf_attributes *dmabuf) {
	struct wlr_dmabuf_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	wlr_buffer_init(&buffer->base, &dmabuf_buffer_impl,
		dmabuf->width, dmabuf->height);

	buffer->dmabuf = *dmabuf;

	return buffer;
}

bool dmabuf_buffer_drop(struct wlr_dmabuf_buffer *buffer) {
	bool ok = true;

	if (buffer->base.n_locks > 0) {
		struct wlr_dmabuf_attributes saved_dmabuf = {0};
		if (!wlr_dmabuf_attributes_copy(&saved_dmabuf, &buffer->dmabuf)) {
			wlr_log(WLR_ERROR, "Failed to save DMA-BUF");
			ok = false;
			buffer->dmabuf = (struct wlr_dmabuf_attributes){0};
		} else {
			buffer->dmabuf = saved_dmabuf;
			buffer->saved = true;
		}
	}

	wlr_buffer_drop(&buffer->base);
	return ok;
}
