#include "backend.h"
#include "drm.h"
#include "drm_mode.h"
#include <ctype.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wayland-server.h>
#include <poll.h>

//DRM 
#include <xf86drm.h>
#include <xf86drmMode.h>


//evdev 
#include <libevdev-1.0/libevdev/libevdev.h>

//Close and Open 
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

//Print
#include <stdio.h>
#include <evdev.h>

static struct scwl_drm_backend backend;


int scwl_drm_open_device(const char *path) {
	int fd = open(path, O_RDWR | O_CLOEXEC);

	drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

	//TODO: set DRM Client caps 
	return fd;
}

void scwl_drm_draw_frame(); 

void scwl_drm_first_frame_handle(int fd, unsigned int frame, unsigned int sec, unsigned int usec, void *data) {
	printf("VBLANK EVENT:\n");

	srand(time(NULL));
	uint32_t pixel = 0x282a36;

	for(uint32_t x = 0; x < backend.buffers[backend.front_bfr].width; ++x) {
		for(uint32_t y = 0; y < backend.buffers[backend.front_bfr].height; ++y) {
			backend.buffers[backend.front_bfr].data[x + (y * backend.buffers[backend.front_bfr].width)] = pixel;
		}
	}	

	
	drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.buffers[backend.front_bfr].fb_id, 0, 0, &backend.connector->connector_id, 1, &backend.mode);
	scwl_drm_draw_frame();
}

void scwl_drm_page_flip_handler(int fd, unsigned int seq,
		unsigned int tv_sec, unsigned int tv_usec,
		void *user_data) {
	struct scwl_drm_backend *drm = user_data;

	drm->pending_flip = 0;
	if(!drm->closing) {
		scwl_drm_draw_frame();
	}
}

/*HACK: TEMPORAY FUNCTION TO GENERATE 
 * SOME SCREEN TEARING
 * NOTE: THe old code was worse than this in terms
 * of flashing but we will still just be careful 
 * and leave the warning.
 */
void scwl_drm_draw_frame() {
	int ret = 0;
	srand(time(NULL));
	uint32_t pixel = 0x282a36;

		for(uint32_t x = 0; x < backend.buffers[backend.front_bfr].width; ++x) {
			for(uint32_t y = 0; y < backend.buffers[backend.front_bfr].height; ++y) {
				backend.buffers[backend.front_bfr].data[x + (y * backend.buffers[backend.front_bfr].width)] = pixel;
			}
		}	


		if(drmModePageFlip(backend.fd, backend.crtc->crtc_id, backend.buffers[backend.front_bfr].fb_id, DRM_MODE_PAGE_FLIP_EVENT, &backend)) {
			printf("Cannot flip CRTC: %m\n");
		}

		/*NOTE: Change Pixel Data A little*/
		pixel += 0x140000;
		pixel += 0x0a;	
		pixel += 0x1000;
		
		backend.front_bfr ^= 1;
		backend.pending_flip = 1;
}

void scwl_drm_destroy_buffer(struct scwl_drm_buffer buffer) {
	//Unmap the memory 
	munmap(buffer.data, buffer.size);
	
	//Remove the scanout framebuffer 
	drmModeRmFB(backend.fd, buffer.fb_id);

	//Destroy the dumb buffer 
	drmModeDestroyDumbBuffer(backend.fd, buffer.buffer_handle);
}
	
int scwl_drm_create_buffer(struct scwl_drm_buffer *buffer) {
	//TODO: Make this an allocation
	if(drmModeCreateDumbBuffer(backend.fd, backend.mode.hdisplay, backend.mode.vdisplay, 32, 0, &buffer->buffer_handle, &buffer->pitch, &buffer->size) != 0) {
		printf("DRM failed to create dumb buffer: %m\n");
		return -1;
	}

	buffer->width = backend.mode.hdisplay;
	buffer->height = backend.mode.vdisplay; 
	buffer->depth = 24;
	buffer->bpp = 32;

	if(drmModeAddFB(backend.fd, buffer->width, buffer->height, buffer->depth, buffer->bpp, buffer->pitch, buffer->buffer_handle, &buffer->fb_id) != 0) {
		printf("DRM Failed to add FB: %m\n");
		return -1;
	}
	

	if(drmModeMapDumbBuffer(backend.fd, buffer->buffer_handle, &buffer->offset) != 0) {
		printf("DRM FAiled to Map buffer\n");
		return -1;
	}
	
	buffer->data = mmap(NULL, buffer->size, PROT_READ | PROT_WRITE, MAP_SHARED, backend.fd, buffer->offset);

	return 0;
}

/* Checks if a CRTC is linked to an encoder/connector pair 
 * if the CRTC is in use the id of the connector using this 
 * crtc will be returned. Else if the CRTC is currently free 
 * 0 will be returned
 */
uint32_t scwl_drm_crtc_in_use(int fd, int crtc_id, drmModeResPtr resources) {
	for(int i = 0; i < resources->count_connectors; i++) {
		drmModeConnectorPtr connector = drmModeGetConnector(fd, resources->connectors[i]);
		
		if(connector->encoder_id) {
			drmModeEncoderPtr encoder = drmModeGetEncoder(fd, connector->encoder_id);

			if(encoder->crtc_id == crtc_id) {
				drmModeFreeEncoder(encoder);
				drmModeFreeConnector(connector);
				return connector->connector_id; //CRTC is in use 
			}

			drmModeFreeEncoder(encoder);
		}

		drmModeFreeConnector(connector);
	}

	return 0;
}

int scwl_drm_event(int fd, uint32_t mask, void *data) {
	drmEventContext ev_ctx = { 
		.version = 2,
		.vblank_handler = scwl_drm_first_frame_handle, 
		.page_flip_handler = scwl_drm_page_flip_handler,
	};
	drmHandleEvent(fd, &ev_ctx);
	
	return 0;
}

int scwl_drm_backend_init(struct wl_display *display) {
	backend.fd = scwl_drm_open_device("/dev/dri/card0");

	if(drmIsKMS(backend.fd) == 0) {
		printf("Error Device is not a KMS device\n");
		return -1;
	}
	if(!drmIsMaster(backend.fd)) {
		printf("Error We are not the DRM Master lock holder\n"
				"Is another graphics setting running?\n");
		return -1;
	}

	if(backend.fd < 0) {
		printf("Error: Unable to open DRM device: %m\n");
		return -1;
	}
	backend.curx = 0;
	backend.cury = 0;
	backend.res = drmModeGetResources(backend.fd);
	backend.plane_res = drmModeGetPlaneResources(backend.fd);

	for(int i = 0; i < backend.res->count_connectors; i++) {
		backend.connector = drmModeGetConnector(backend.fd, backend.res->connectors[i]);
		if(!backend.connector) {
			continue;
		}
		
		if(backend.connector->connection == DRM_MODE_CONNECTED) {
			break;
		}

		drmModeFreeConnector(backend.connector);
	}
	
	if(backend.connector == NULL) {
		printf("Error: Unable to find a connected connector\n");
		return -1;
	}

	uint32_t possible = drmModeConnectorGetPossibleCrtcs(backend.fd, backend.connector);

	
	
	//Attempt to pair CRTC/Encoder pair as atm we just use 
	//whatever pair was already present which there is no gurantee
	//that any pair will already exist
	/*
	for(int i = 0; i < backend.res->count_crtcs; i++) {
		drmModeCrtcPtr crtc; 
		if(i & possible) {
			crtc = drmModeGetCrtc(backend.fd, backend.res->crtcs[i]);
			int in_use = scwl_drm_crtc_in_use(backend.fd, crtc->crtc_id, backend.res);
			if(!in_use) {
				backend.crtc = crtc;
				break;
			}
			drmModeFreeCrtc(crtc);
		}
	}
	
	for(int i = 0; i < backend.res->count_encoders; i++) {
		drmModeEncoderPtr encoder;
		
	}
	*/
	if(backend.connector->encoder_id) {
		backend.encoder = drmModeGetEncoder(backend.fd, backend.connector->encoder_id);
	}
	
	if(backend.encoder->crtc_id) {
		backend.crtc = drmModeGetCrtc(backend.fd, backend.encoder->crtc_id);
	}
	int ind = 0;
	for(ind = 0; ind < backend.res->count_crtcs; ind++) {
		if(backend.crtc->crtc_id == backend.res->crtcs[ind]) {
			break;
		}
		
	}

	backend.mode = backend.connector->modes[0];

	scwl_drm_create_buffer(&backend.buffers[0]);
	scwl_drm_create_buffer(&backend.buffers[1]);
	backend.cursor[0].width = 64;
	backend.cursor[0].height = 64;

	drmModeCreateDumbBuffer(backend.fd, 64, 64, 32, 0, 
			&backend.cursor[0].buffer_handle, 
			&backend.cursor[0].pitch, 
			&backend.cursor[0].size);

	//FIX: First frame isn't vsync'd drmWaitVBlank or drmCrtcQueueSequence
	//TODO: Use atomic API
	errno = 0;
	
	drmModeMapDumbBuffer(backend.fd, backend.cursor[0].buffer_handle, &backend.cursor[0].offset);

	backend.cursor[0].data = mmap(NULL, backend.cursor[0].size, PROT_READ | PROT_WRITE, MAP_SHARED, backend.fd, backend.cursor[0].offset);

	for(int x = 0; x < 64; x++) {
		for(int y = 0; y < 64; y++) {
			backend.cursor[0].data[x + (y * 64)] = 0xfffffff;
		}
	}

	drmModeSetCursor(backend.fd, backend.crtc->crtc_id, 
			backend.cursor[0].buffer_handle, 
			backend.cursor[0].width, 
			backend.cursor[0].height);
	
	printf("set Plane: %m\n");
	
	backend.display = display;
	backend.ev_loop = wl_display_get_event_loop(display);
	backend.ev_source = wl_event_loop_add_fd(backend.ev_loop, backend.fd, WL_EVENT_READABLE, scwl_drm_event, &backend);

	drmVBlank vbl = { 0 };

	//TODO: Implement Sequence handlers 
	//TODO: Implement V2 page flip and change draw func
	drmEventContext ev_ctx = { 
		.version = 2,
		.vblank_handler = scwl_drm_first_frame_handle, 
		.page_flip_handler = scwl_drm_page_flip_handler,
	};
		
	//Trigger an event for the Next vblank to set the first frame 
	vbl.request.sequence = 1;
	vbl.request.type = DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT | DRM_VBLANK_NEXTONMISS;
	drmWaitVBlank(backend.fd, &vbl);
	

	return 0;
}

void scwl_drm_move_cursour(int x, int y) {
	drmModeMoveCursor(backend.fd, backend.crtc->crtc_id, x, y);
}

void scwl_drm_backend_cleanup() {
	drmEventContext ev_ctx = { 
		.version = 2,
		.vblank_handler = scwl_drm_first_frame_handle, 
		.page_flip_handler = scwl_drm_page_flip_handler,
	};
	//Just handle the last POLL Page flip Event	
	while(backend.pending_flip) {
		drmHandleEvent(backend.fd, &ev_ctx);
	}
	//Reset the CRTC 
	drmModeSetCursor(backend.fd, backend.crtc->crtc_id, 0, 0, 0);
	printf("Reset Cursor %m\n");
	if(drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.crtc->buffer_id, 0, 0, &backend.connector->connector_id, 1, &backend.crtc->mode)) {
		printf("Failed to reset CRTC: %m\n");
	}


	//Clean up buffers 
	scwl_drm_destroy_buffer(backend.buffers[0]);
	scwl_drm_destroy_buffer(backend.buffers[1]);

	drmModeFreeCrtc(backend.crtc);
	drmModeFreeEncoder(backend.encoder);
	drmModeFreeConnector(backend.connector);
	drmModeFreePlaneResources(backend.plane_res);
	drmModeFreeResources(backend.res);

	close(backend.fd);
}

/* Temp main function to just test with 
 * HACK: Remove this once it's no longer needed
 *
 */

int main(int argc, char **argv) {
	int ch = 0;
	
	struct wl_display *display = wl_display_create();
	const char *socket = wl_display_add_socket_auto(display);

	scwl_evdev_backend_init(display);
	
	if(scwl_drm_backend_init(display) < 0) {
		return -1;
	}

	wl_display_run(display);
	backend.closing = 1;

	scwl_drm_backend_cleanup();

	return 0;
}
