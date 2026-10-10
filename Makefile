CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS = -lmosquitto -lcjson -lpthread -lm
PREFIX ?= /usr

pylon2mqtt: pylon2mqtt.c
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: pylon2mqtt
	install -D -m 0755 pylon2mqtt $(DESTDIR)$(PREFIX)/bin/pylon2mqtt

clean:
	rm -f pylon2mqtt

.PHONY: install clean
