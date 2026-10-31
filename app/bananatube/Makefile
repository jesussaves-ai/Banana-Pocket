# banana-tube. Cross builds set CXX (and PKG_CONFIG for the target's sysroot); Buildroot does both.
#   make                 the client (needs libsdl2, SDL2_ttf, SDL2_image, libcurl)
CXX ?= g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2
override CXXFLAGS += -std=c++17 -Wall -Wno-unused-function
override LDLIBS += -lpthread

SRC = main.cpp json.cpp
PKGS = sdl2 SDL2_ttf SDL2_image libcurl
override CXXFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
override LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

banana-tube: $(SRC) json.h
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

install:
	install -D -m 755 banana-tube $(DESTDIR)/usr/bin/banana-tube
	install -D -m 644 app.json $(DESTDIR)/usr/share/banana/apps/bananatube/app.json
	install -D -m 644 BananaTube.app $(DESTDIR)/usr/share/banana/apps/bananatube/BananaTube.app

clean:
	rm -f banana-tube

.PHONY: install clean
