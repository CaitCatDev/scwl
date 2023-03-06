#include "backend.h"
#include "drm.h"
#include "drm_mode.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server.h>
#include <poll.h>

//DRM 


//Close and Open 
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

//Print
#include <stdio.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

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
	uint32_t pixel = rand();

		for(uint32_t x = 0; x < backend.buffers[backend.front_bfr].width; ++x) {
			for(uint32_t y = 0; y < backend.buffers[backend.front_bfr].height; ++y) {
				backend.buffers[backend.front_bfr].data[x + (y * backend.buffers[backend.front_bfr].width)] = pixel;
			}
		}	


	drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.buffers[backend.front_bfr].fb_id, 0, 0, &backend.connector->connector_id, 1, &backend.mode);
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
 * WARN: LESS OF A CODE WARNING MORE OF A 
 * PLEASE BE CAREFUL IF YOU SUFFER FROM EPILEPSY 
 * OR ARE SENSETIVE TO FLASHING LIGHTS.
 * NOTE: THe old code was worse than this in terms
 * of flashing but we will still just be careful 
 * and leave the warning.
 */
void scwl_drm_draw_frame() {
	int ret = 0;
	srand(time(NULL));
	uint32_t pixel = rand();

		for(uint32_t x = 0; x < backend.buffers[backend.front_bfr].width; ++x) {
			for(uint32_t y = 0; y < backend.buffers[backend.front_bfr].height; ++y) {
				backend.buffers[backend.front_bfr].data[x + (y * backend.buffers[backend.front_bfr].width)] = pixel;
			}
		}	

		/*OLD CODE 
		if(drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.buffers[backend.front_bfr].fb_id, 0, 0, &backend.connector->connector_id, 1, &backend.mode) != 0) {
			printf("Error setting CRTC %m\n");
		}
		*/

		drmModeMoveCursor(backend.fd, backend.crtc->crtc_id, rand() % backend.mode.hdisplay, rand() % backend.mode.vdisplay);

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

int scwl_drm_backend_init() {
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
	drmModeMoveCursor(backend.fd, backend.crtc->crtc_id, 100, 100);
	struct pollfd fds[2] = { 0 };
	fds[0].events = POLLIN;
	fds[0].fd = backend.fd;
	fds[0].revents = 0;
	fds[1].events = POLLIN;
	fds[1].fd = STDIN_FILENO;
	fds[1].revents = 0;

	drmVBlank vbl = { 0 };

	//TODO: Implement Sequence and VBLank handlers 
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
	//Set the CRTC to our fb
	while(1) {
		poll(fds, 1, 100);
			if(fds[0].revents == POLLIN) {
				drmHandleEvent(backend.fd, &ev_ctx);
				break;
			}	
	}

	scwl_drm_draw_frame();

	
	
	for(int i = 0; i < 90; ++i) {
		poll(fds, 2, 100);
		if(fds[0].revents == POLLIN) {
			drmHandleEvent(backend.fd, &ev_ctx);
			fds[0].revents = 0;//reset 
		} 
		if(fds[1].revents == POLLIN) {
			break;
		}
	}
	backend.closing = 1;//Inform page flip handler not to 
						//send any more events 

	//Just handle the last POLL Page flip Event	
	while(backend.pending_flip) {
		poll(fds, 1, 100);
			if(fds[0].revents == POLLIN) {
				drmHandleEvent(backend.fd, &ev_ctx);
			}	
	}
	//Reset the CRTC 

	drmModeSetCursor(backend.fd, backend.crtc->crtc_id, 
			0, 0, 0);


	drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.crtc->buffer_id, 
			0, 0, &backend.connector->connector_id, 1, &backend.crtc->mode);
	
	return 0;
}

void scwl_drm_backend_cleanup() {

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
	printf("\033[31;1;4m"
			"We use a random function call to generate\n"
			"A color for the screen output and change it\n" 
			"Each render cycle to induce screen tearing\n"
			"it can sometimes be bright and/or quick to\n"
			"Flash so we recommend you don't run this\n"
			"Early Version if you are senstive to bright\n"
			"and/or flashing lights!\n"
			"\033[0m\n");
	printf("Press c and hit enter to continue if you accept this risk\nOr Press q and hit enter to quit the application early\n");
	
	while((ch = tolower(getchar())) != 'c') { 
		if(ch == 'q') {
			return -1;
		}
		//pause for character 
	}
	if(scwl_drm_backend_init() < 0) {
		return -1;
	}
	
	scwl_drm_backend_cleanup();

	return 0;
}
