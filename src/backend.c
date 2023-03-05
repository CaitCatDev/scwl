#include "backend.h"
#include "drm_mode.h"
#include <wayland-server.h>

//DRM 
#include <libdrm/drm.h>

//Close and Open 
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

//Print
#include <stdio.h>
#include <xf86drmMode.h>

static struct scwl_drm_backend backend;

int scwl_drm_open_device(const char *path) {
	int fd = open(path, O_RDWR | O_CLOEXEC);

	//TODO: set DRM Client caps 
	return fd;
}

int scwl_drm_backend_init() {
	

	backend.fd = scwl_drm_open_device("/dev/dri/card0");

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

	if(drmModeCreateDumbBuffer(backend.fd, backend.mode.hdisplay, backend.mode.vdisplay, 
			32, 0, &backend.buffer_handle, &backend.pitch, &backend.size) != 0) {
		printf("DRM failed to create dumb buffer: %m\n");
		return -1;
	}
	backend.width = backend.mode.hdisplay;
	backend.height = backend.mode.vdisplay; 
	backend.depth = 24;
	backend.bpp = 32;

	if(drmModeAddFB(backend.fd, backend.width, backend.height, backend.depth, 
				backend.bpp, backend.pitch, backend.buffer_handle, &backend.fb_id) != 0) {
		printf("DRM Failed to add FB: %m\n");
		return -1;
	}

	if(drmModeMapDumbBuffer(backend.fd, backend.buffer_handle, &backend.offset) != 0) {
		printf("DRM FAiled to Map buffer\n");
		return -1;
	}
	
	backend.data = mmap(NULL, backend.size, PROT_READ | PROT_WRITE, MAP_SHARED, backend.fd, backend.offset);
	
	for(uint32_t x = 0; x < backend.width; ++x) {
		for(uint32_t y = 0; y < backend.height; ++y) {
			backend.data[x + (y * backend.width)] = 0xffff00ff;
		}
	}
	
	//Set the CRTC to our fb 
	drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.fb_id, 
			0, 0, &backend.connector->connector_id, 1, &backend.mode);
	
	sleep(3);

	//Reset the CRTC 
	drmModeSetCrtc(backend.fd, backend.crtc->crtc_id, backend.crtc->buffer_id, 
			0, 0, &backend.connector->connector_id, 1, &backend.crtc->mode);
	
	return 0;
}

void scwl_drm_backend_cleanup() {
	//Unmap the memory 
	munmap(backend.data, backend.size);
	
	//Remove the scanout framebuffer 
	drmModeRmFB(backend.fd, backend.fb_id);

	//Destroy the dumb buffer 
	drmModeDestroyDumbBuffer(backend.fd, backend.buffer_handle);
	
	drmModeFreeCrtc(backend.crtc);
	drmModeFreeEncoder(backend.encoder);
	drmModeFreeConnector(backend.connector);
	drmModeFreePlaneResources(backend.plane_res);
	drmModeFreeResources(backend.res);

	close(backend.fd);
}

/* Temp main function to just test with 
 * TODO: Remove this once it's no longer needed
 *
 */

int main(int argc, char **argv) {
	scwl_drm_backend_init();
	
	scwl_drm_backend_cleanup();

	return 0;
}
