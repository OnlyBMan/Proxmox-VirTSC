CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local

is1ts: is1ts.c
	$(CC) $(CFLAGS) -pthread -o $@ $< -lm

install: is1ts
	install -D -m 0755 is1ts $(DESTDIR)$(PREFIX)/bin/is1ts

clean:
	rm -f is1ts

.PHONY: install clean
