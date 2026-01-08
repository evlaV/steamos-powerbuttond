// SPDX-License-Identifier: BSD-2-Clause
//
// Copyright (c) 2023-2025 Valve Software
// Maintainer: Vicki Pfau <vi@endrift.com>
#include <errno.h>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_DEVS 2

#define MOD_LSHIFT 0x01
#define MOD_RSHIFT 0x02
#define MOD_SHIFT 0x03
#define MOD_LCTRL 0x04
#define MOD_RCTRL 0x08
#define MOD_CTRL 0x0C
#define MOD_LALT 0x10
#define MOD_RALT 0x20
#define MOD_ALT 0x30
#define MOD_LMETA 0x40
#define MOD_RMETA 0x80
#define MOD_META 0xC0

extern char** environ;
static bool got_alarm = false;

struct evdev_context {
	struct libevdev* dev;
	int modifiers;
};

void signal_handler(int) {
	got_alarm = true;
}

bool open_dev(const char* path, struct evdev_context* ctx) {
	int fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0) {
		return false;
	}
	struct libevdev* dev;
	if (libevdev_new_from_fd(fd, &dev) < 0) {
		close(fd);
		return false;
	}
	bool has_meta = libevdev_has_event_code(dev, EV_KEY, KEY_LEFTMETA) && libevdev_has_event_code(dev, EV_KEY, KEY_F16);
	bool has_power = libevdev_has_event_code(dev, EV_KEY, KEY_POWER);
	bool has_lid = libevdev_has_event_code(dev, EV_SW, SW_LID);
	if (!has_meta && !has_power && !has_lid) {
		libevdev_free(dev);
		return false;
	}
	ctx->dev = dev;
	return true;
}

size_t find_devs(struct evdev_context* devs) {
	struct udev* udev = udev_new();
	size_t num_devs = 0;
	if (!udev) {
		return 0;
	}
	struct udev_enumerate* uenum = udev_enumerate_new(udev);
	if (!uenum) {
		goto out;
	}

	if (udev_enumerate_add_match_subsystem(uenum, "input") < 0) {
		goto out;
	}

	if (udev_enumerate_add_match_sysname(uenum, "event*") < 0) {
		goto out;
	}

	if (udev_enumerate_add_match_property(uenum, "STEAMOS_POWER_BUTTON", "1") < 0) {
		goto out;
	}

	if (udev_enumerate_scan_devices(uenum) < 0) {
		goto out;
	}

	struct udev_list_entry* devices = udev_enumerate_get_list_entry(uenum);

	for (; devices && num_devs < MAX_DEVS; devices = udev_list_entry_get_next(devices)) {
		const char* syspath = udev_list_entry_get_name(devices);
		struct udev_device* dev = udev_device_new_from_syspath(udev, syspath);
		if (!dev) {
			continue;
		}
		if (udev_device_get_property_value(dev, "STEAMOS_POWER_BUTTON_IGNORE")) {
			continue;
		}
		const char* devpath = udev_device_get_devnode(dev);
		if (devpath) {
			if (open_dev(devpath, &devs[num_devs])) {
				printf("Found power button device at %s\n", devpath);
				++num_devs;
			}
		}
		udev_device_unref(dev);
	}

out:
	if (uenum) {
		udev_enumerate_unref(uenum);
	}
	udev_unref(udev);

	return num_devs;
}

void do_press(const char* type) {
	char steam[PATH_MAX];
	char press[32];
	char* home = getenv("HOME");
	char* const args[] = {steam, "-ifrunning", press, NULL};

	alarm(0);
	got_alarm = false;

	snprintf(steam, sizeof(steam), "%s/.steam/root/ubuntu12_32/steam", home);
	snprintf(press, sizeof(press), "steam://%spowerpress", type);

	pid_t pid;
	if (posix_spawn(&pid, steam, NULL, NULL, args, environ) < 0) {
		return;
	}
	while (true) {
		if (waitpid(pid, NULL, 0) > 0) {
			break;
		}
		if (errno != EINTR && errno != EAGAIN) {
			break;
		}
	}
}

int main(int argc, char* argv[]) {
	struct sigaction sa = {
		.sa_handler = signal_handler,
		.sa_flags = SA_NOCLDSTOP,
	};
	sigemptyset(&sa.sa_mask);
	sigaction(SIGALRM, &sa, NULL);

	struct evdev_context devs[MAX_DEVS] = {0};
	struct pollfd pfds[MAX_DEVS] = {0};
	size_t num_devs = 0;

	if (argc >= 2) {
		int i;
		for (i = 0; i < argc - 1 && num_devs < MAX_DEVS; ++i) {
			if (open_dev(argv[i + 1], &devs[num_devs])) {
				++num_devs;
			}
		}
	} else {
		num_devs = find_devs(devs);
	}
	if (!num_devs) {
		return 0;
	}
	size_t i;
	for (i = 0; i < num_devs; ++i) {
		pfds[i].fd = libevdev_get_fd(devs[i].dev);
		pfds[i].events = POLLIN;
	}

	bool press_active = false;
	while (true) {
		for (i = 0; i < num_devs; ++i) {
			pfds[i].fd = libevdev_get_fd(devs[i].dev);
			pfds[i].revents = 0;
		}

		int res = poll(pfds, num_devs, -1);
		if (res < 0 && errno == EINTR && press_active && got_alarm) {
			press_active = false;
			do_press("long");
		} else if (res <= 0) {
			continue;
		}

		for (i = 0; i < num_devs; ++i) {
			struct evdev_context* ctx = &devs[i];
			if (!(pfds[i].revents & POLLIN)) {
				continue;
			}
			struct input_event ev;
			do {
				res = libevdev_next_event(ctx->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
				while (res == LIBEVDEV_READ_STATUS_SYNC) {
					res = libevdev_next_event(ctx->dev, LIBEVDEV_READ_FLAG_SYNC, &ev);
				}
				if (res != LIBEVDEV_READ_STATUS_SUCCESS) {
					break;
				}
				if (ev.type == EV_KEY) {
					switch (ev.code) {
					case KEY_POWER:
						if (ev.value == 1) {
							press_active = true;
							alarm(1);
						} else if (ev.value == 0 && press_active) {
							press_active = false;
							do_press("short");
						}
						break;
					case KEY_LEFTSHIFT:
						devs[i].modifiers &= ~MOD_LSHIFT;
						if (ev.value > 0) {
							devs[i].modifiers |= MOD_LSHIFT;
						}
						break;
					case KEY_RIGHTSHIFT:
						devs[i].modifiers &= ~MOD_RSHIFT;
						if (ev.value > 0) {
							devs[i].modifiers |= MOD_RSHIFT;
						}
						break;
					case KEY_LEFTCTRL:
						devs[i].modifiers &= ~MOD_LCTRL;
						if (ev.value > 0) {
							devs[i].modifiers |= MOD_LCTRL;
						}
						break;
					case KEY_RIGHTCTRL:
						devs[i].modifiers &= ~MOD_RCTRL;
						if (ev.value > 0) {
							devs[i].modifiers |= MOD_RCTRL;
						}
						break;
					case KEY_LEFTALT:
						devs[i].modifiers &= ~MOD_LALT;
						if (ev.value > 0) {
							devs[i].modifiers |= MOD_LALT;
						}
						break;
					case KEY_RIGHTALT:
						devs[i].modifiers &= ~MOD_RALT;
						if (ev.value > 0) {
							devs[i].modifiers |= MOD_RALT;
						}
						break;
					case KEY_LEFTMETA:
						devs[i].modifiers &= ~MOD_LMETA;
						if (ev.value > 0) {
							devs[i].modifiers |= MOD_LMETA;
						}
						break;
					case KEY_RIGHTMETA:
						devs[i].modifiers &= ~MOD_RMETA;
						if (ev.value > 0) {
							devs[i].modifiers |= MOD_RMETA;
						}
						break;
					case KEY_F16:
						if (ev.value == 1 && (ctx->modifiers & MOD_META) && !(ctx->modifiers & ~MOD_META)) {
							press_active = false;
							do_press("long");
						}
						break;
					}
				} else if (ev.type == EV_SW) {
					if (ev.code == SW_LID && ev.value == 1) {
						press_active = false;
						do_press("short");
					}
				}
			} while (libevdev_has_event_pending(ctx->dev) > 0);
			if (res == -EINTR && press_active && got_alarm) {
				press_active = false;
				do_press("long");
			}
		}
	}
}
