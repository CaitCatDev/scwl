#pragma once

typedef int fd_t;

/*Forward Private Declearation*/
typedef struct scwl_evdev_backend scwl_evdev_backend_t;

scwl_evdev_backend_t *scwl_evdev_backend_init(struct wl_display *display);
