#include "backend.h"
#include "drm_mode.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server.h>

//DRM 
#include <libdrm/drm.h>

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

	//TODO: set DRM Client caps 
	return fd;
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

	for(int i = 0; i < 30; i++) {
		for(uint32_t x = 0; x < backend.buffers[backend.front_bfr].width; ++x) {
			for(uint32_t y = 0; y < backend.buffers[backend.front_bfr].height; ++y) {
				backend.buffers[backend.front_bfr].data[x + (y * backend.buffers[backend.front_bfr].width)] = pixel;
			}
		}	

		if(drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.buffers[backend.front_bfr].fb_id, 0, 0, &backend.connector->connector_id, 1, &backend.mode) != 0) {
			printf("Error setting CRTC %m\n");
		}
	

		/*NOTE: Change Pixel Data A little*/
		pixel += 0x140000;
		pixel += 0x0a;	
		pixel += 0x1000;
		
		backend.front_bfr ^= 1;

		usleep(100000);
	}
}

int scwl_drm_create_buffer(struct scwl_drm_buffer *buffer) {
	//TODO: Make this an allocation

	if(drmModeCreateDumbBuffer(backend.fd, backend.mode.hdisplay, backend.mode.vdisplay, 
			32, 0, &buffer->buffer_handle, &buffer->pitch, &buffer->size) != 0) {
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
	
	backend.mode = backend.connector->modes[0];

	scwl_drm_create_buffer(&backend.buffers[0]);
	scwl_drm_create_buffer(&backend.buffers[1]);

	//TODO: Remove sleep and exit on input 
	//TODO: Implement Page Flip
	//TODO: Implement Double Buffer
	//TODO: Use the ATOMIC API

	

	//Set the CRTC to our fb 
	drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.buffers[backend.front_bfr].fb_id, 
			0, 0, &backend.connector->connector_id, 1, &backend.mode);
	
	scwl_drm_draw_frame();

	//Reset the CRTC 
	drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.crtc->buffer_id, 
			0, 0, &backend.connector->connector_id, 1, &backend.crtc->mode);
	
	return 0;
}

void scwl_drm_backend_cleanup() {
	//Unmap the memory 
	//munmap(backend.data, backend.size);
	
	//Remove the scanout framebuffer 
	//drmModeRmFB(backend.fd, backend.fb_id);

	//Destroy the dumb buffer 
	//drmModeDestroyDumbBuffer(backend.fd, backend.buffer_handle);
	
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
	printf("Press c and hit enter to continue if you accept this risk\n"
			"Or Press q and hit enter to quit the application early\n");
	
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
