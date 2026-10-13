CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
LDLIBS = -lmosquitto -lcjson -lpthread -lm
PREFIX ?= /usr
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include

# shared library libpylontech (no dependencies besides libc and libm)
LIB_VERSION = 1.0.0
LIB_SONAME = libpylontech.so.1
LIB_SHARED = libpylontech.so.$(LIB_VERSION)

all: pylon2mqtt lib

# the library is compiled into the binary, it does not need libpylontech.so
pylon2mqtt: pylon2mqtt.c pylontech.c pylontech.h
	$(CC) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ pylon2mqtt.c pylontech.c $(LDLIBS)

lib: $(LIB_SHARED)

$(LIB_SHARED): pylontech.c pylontech.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -fPIC -fvisibility=hidden -shared -Wl,-soname,$(LIB_SONAME) \
		$(LDFLAGS) -o $@ pylontech.c -lm
	ln -sf $(LIB_SHARED) $(LIB_SONAME)
	ln -sf $(LIB_SONAME) libpylontech.so

pylontech.pc: pylontech.pc.in
	sed -e 's|@PREFIX@|$(PREFIX)|' -e 's|@LIBDIR@|$(LIBDIR)|' -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|' \
		-e 's|@VERSION@|$(LIB_VERSION)|' $< > $@

examples/pylon_dump: examples/pylon_dump.c pylontech.h $(LIB_SHARED)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I. $(LDFLAGS) -o $@ $< -L. -lpylontech

example: examples/pylon_dump

install: pylon2mqtt
	install -D -m 0755 pylon2mqtt $(DESTDIR)$(PREFIX)/bin/pylon2mqtt

install-lib: $(LIB_SHARED) pylontech.pc
	install -D -m 0755 $(LIB_SHARED) $(DESTDIR)$(LIBDIR)/$(LIB_SHARED)
	ln -sf $(LIB_SHARED) $(DESTDIR)$(LIBDIR)/$(LIB_SONAME)
	ln -sf $(LIB_SONAME) $(DESTDIR)$(LIBDIR)/libpylontech.so
	install -D -m 0644 pylontech.h $(DESTDIR)$(INCLUDEDIR)/pylontech.h
	install -D -m 0644 pylontech.pc $(DESTDIR)$(LIBDIR)/pkgconfig/pylontech.pc

clean:
	rm -f pylon2mqtt $(LIB_SHARED) $(LIB_SONAME) libpylontech.so pylontech.pc examples/pylon_dump

.PHONY: all lib example install install-lib clean
