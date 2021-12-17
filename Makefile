PROG := src/x-window-selector

PKGS = xcb xcb-keysyms xcb-render xcb-ewmh xcb-renderutil xcb-icccm freetype2 fontconfig
CFLAGS = -Wall -Werror -Wno-unused `pkg-config --cflags $(PKGS)` -g
LDLIBS = `pkg-config --libs $(PKGS)` -lm

INSTALL_PROGRAM := install

prefix := /usr/local
exec_prefix := $(prefix)
bindir := $(exec_prefix)/bin

all: $(PROG)

clean:
	- $(RM) $(PROG)

distclean: clean

install:
	mkdir -p "$(DESTDIR)$(bindir)"
	$(INSTALL_PROGRAM) $(PROG) "$(DESTDIR)$(bindir)"

uninstall:
	$(RM) -r "$(DESTDIR)$(bindir)/$(PROG)"

format:
	bash format.sh src

run: all
	./src/x-window-selector

.PHONY: all clean distclean install uninstall
