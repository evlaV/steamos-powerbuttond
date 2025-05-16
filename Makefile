LIB_CFLAGS := $(shell pkg-config --cflags libevdev libudev)
LIB_LDFLAGS := $(shell pkg-config --libs libevdev libudev)

CFLAGS += -Wall -Wextra $(LIB_CFLAGS)

ifneq ($(ASAN),)
  CFLAGS += -g -fsanitize=address
  LDFLAGS += -g -fsanitize=address
else ifneq ($(DEBUG),)
  CFLAGS += -g
  LDFLAGS += -g
else
  CFLAGS += -O2
  LDFLAGS += -O2
endif

all: steamos-powerbuttond
.PHONY: clean install

steamos-powerbuttond: powerbuttond.o
	$(CC) $(LIB_LDFLAGS) $(LDFLAGS) $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) $^ -c -o $@

clean:
	rm -f steamos-powerbuttond powerbuttond.o

install: all LICENSE
	install -Ds -m 755 steamos-powerbuttond $(DESTDIR)/usr/lib/hwsupport/steamos-powerbuttond
	install -D -m 644 LICENSE $(DESTDIR)/usr/share/licenses/steamos-powerbuttond/LICENSE
	install -D -m 644 steamos-powerbuttond.service $(DESTDIR)/usr/lib/systemd/user/steamos-powerbuttond.service
	install -D -m 644 steamos-power-button.rules $(DESTDIR)/usr/lib/udev/rules.d/80-steamos-power-button.rules
	install -D -m 644 steamos-power-button.hwdb $(DESTDIR)/usr/lib/udev/hwdb.d/80-steamos-power-button.hwdb
	install -d -m 755 $(DESTDIR)/usr/lib/systemd/user/gamescope-session.service.wants
	ln -s ../steamos-powerbuttond.service $(DESTDIR)/usr/lib/systemd/user/gamescope-session.service.wants/
