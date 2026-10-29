# hekon - standalone duplicate cleaner with a master TARGET.
# Plain Makefile, no autoconf. Requires only a C compiler.

CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local
BINDIR = $(PREFIX)/bin
MANDIR = $(PREFIX)/share/man/man1

OBJS = hekon.o confirmmatch.o errormsg.o sigint.o removeifnotchanged.o md5/md5.o

all: hekon

hekon: $(OBJS)
	$(CC) $(CFLAGS) -o hekon $(OBJS)

hekon.o: hekon.c hekon.h file.h confirmmatch.h errormsg.h sigint.h removeifnotchanged.h
confirmmatch.o: confirmmatch.c confirmmatch.h hekon.h sigint.h
errormsg.o: errormsg.c errormsg.h hekon.h
sigint.o: sigint.c sigint.h
removeifnotchanged.o: removeifnotchanged.c removeifnotchanged.h file.h hekon.h
md5/md5.o: md5/md5.c md5/md5.h
	$(CC) $(CFLAGS) -c md5/md5.c -o md5/md5.o

install: hekon
	install -d $(DESTDIR)$(BINDIR) $(DESTDIR)$(MANDIR)
	install -m 755 hekon $(DESTDIR)$(BINDIR)/hekon
	install -m 644 hekon.1 $(DESTDIR)$(MANDIR)/hekon.1

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/hekon $(DESTDIR)$(MANDIR)/hekon.1

test: hekon
	./test_hekon.sh

clean:
	rm -f hekon $(OBJS)

.PHONY: all install uninstall test clean
