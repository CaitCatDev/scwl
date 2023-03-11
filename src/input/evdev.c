#include <errno.h>
#include <linux/input-event-codes.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <wayland-server-core.h>
#include <wayland-server.h>
#include <string.h>
#include <sys/epoll.h>
#include <evdev.h>

typedef int fd_t;

void scwl_drm_move_cursour(int x, int y);


struct scwl_evdev_backend {
	//wayland stuff 
	struct wl_display *display;
	struct wl_event_loop *ev_loop;
	struct wl_event_source *ev_source;
	
	//Event stuff
	fd_t epoll_fd;
	int ev_count; 
};


#define EVDEV_DIR "/dev/input/"
#define EVDEV_ENT "event"

fd_t scwl_evdev_open_dev(const char *path) {
	return open(path, O_RDWR | O_CLOEXEC);
}

/* Filter the input directory to just the event files */
int scwl_evdev_filter_dev(const struct dirent *entry) {
	return strncmp("event", entry->d_name, 5) == 0;
}

int scwl_evdev_close_dev(fd_t fd) {
	int ret = 0; 
	while((ret = close(fd)) != 0 && errno == EINTR);
	
	return ret;
}

int scwl_evdev_event(int fd, uint32_t mask, void *data) {
	scwl_evdev_backend_t *backend = data;
	struct epoll_event ev = { 0 };
	struct input_event linux_ev = { 0 };
	
	epoll_wait(fd, &ev, backend->ev_count, -1);
	char buffer[128]  = { 0 };
	char fdbuffer[128] = { 0 };
	/*Resolve the FD to a file path 
	 *TODO: Remove this is just for debug
	 */
	snprintf(fdbuffer, 127, "/proc/self/fd/%d", ev.data.fd);
	readlink(fdbuffer, buffer, 127);
	printf("Event on File %s\n", buffer);

	struct input_absinfo absinfo = { 0 };

	read(ev.data.fd, &linux_ev, sizeof(linux_ev));
	if(linux_ev.code == KEY_Q) {
		wl_display_terminate(backend->display);
	} else if(linux_ev.type == EV_ABS) {
		if(linux_ev.code == ABS_X) {
			ioctl(ev.data.fd, EVIOCGABS(ABS_Y), &absinfo);
			scwl_drm_move_cursour(linux_ev.value, absinfo.value);
		}
		else if(linux_ev.code == ABS_Y) {
			ioctl(ev.data.fd, EVIOCGABS(ABS_X), &absinfo);
			scwl_drm_move_cursour(absinfo.value, linux_ev.value);
		}	
	}
	return 0;
}

/*HACK: This code is a huge fucking hack for me to test with.
 * It goes without saying it is by no means good nor should you use 
 * it as is I mean it's not even freeing resources but cheap and dirty 
 * make for easy testing 
 *TODO: CLEAN this up
 */
scwl_evdev_backend_t *scwl_evdev_backend_init(struct wl_display *display) {
	scwl_evdev_backend_t *backend = calloc(1, sizeof(*backend));
	backend->display = display;
	backend->ev_loop = wl_display_get_event_loop(display);
	struct dirent **entries = NULL;

	backend->ev_count = scandir(EVDEV_DIR, &entries, scwl_evdev_filter_dev, NULL);
	backend->epoll_fd = epoll_create1(0);

	for(int i = 0; i < backend->ev_count; ++i) {
		char buffer[128] = { 0 };
		struct epoll_event ev = { 0 };
		ev.data.ptr = 0;
		printf("entry: %s\n", entries[i]->d_name);
		snprintf(buffer, 127, "/dev/input/%s", entries[i]->d_name);
		printf("\tentry full path: %s\n", buffer);
	
		fd_t ev_fd = scwl_evdev_open_dev(buffer);
		ev.events = EPOLLIN;
		ev.data.fd = ev_fd;

		epoll_ctl(backend->epoll_fd, EPOLL_CTL_ADD, ev_fd, &ev);
	}
	
	backend->ev_source = wl_event_loop_add_fd(backend->ev_loop, backend->epoll_fd, WL_EVENT_READABLE, scwl_evdev_event, backend);


	return backend;
} 
