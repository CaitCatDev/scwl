#pragma once

#include <stdint.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <wayland-server.h>

struct scwl_drm_buffer {
	uint32_t *data;
	uint64_t offset;
	uint64_t size;
	uint32_t fb_id;
	uint32_t height;
	uint32_t width;
	uint32_t pitch;
	uint32_t bpp;
	uint32_t depth;
	uint32_t buffer_handle;
};

struct scwl_drm_backend {
	struct wl_display *display;
	int fd;
	drmModeResPtr res;
	drmModePlaneResPtr plane_res;

	drmModeConnectorPtr connector;
	drmModeEncoderPtr encoder;
	drmModeCrtcPtr crtc; 
	drmModeModeInfo mode;
	int pending_flip;	
	int front_bfr;
	int closing;
	struct scwl_drm_buffer buffers[2];
};
