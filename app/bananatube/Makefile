# banana-tube. Cross builds set CXX (and PKG_CONFIG for the target's sysroot); Buildroot does both.
#   make                 the client (needs libsdl2, SDL2_ttf, SDL2_image, libcurl)
CXX ?= g++
PKG_CONFIG ?= pkg-config
CXXFLAGS ?= -O2
override CXXFLAGS += -std=c++17 -Wall -Wno-unused-function
override LDLIBS += -lpthread

# the JSON reader is banana-ui's (ui/src/json.cpp): one parser, not a copy per app. The cross build's source dir is
# the package's own, so Buildroot passes JSON_SRC (see buildroot/package/banana-tube/banana-tube.mk).
JSON_SRC ?= ../../ui/src
SRC = main.cpp $(JSON_SRC)/json.cpp
PKGS = sdl2 SDL2_ttf SDL2_image libcurl
override CXXFLAGS += -I$(JSON_SRC)
override CXXFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKGS))
override LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKGS))

banana-tube: $(SRC) $(JSON_SRC)/json.h
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ $(SRC) $(LDLIBS)

install:
	install -D -m 755 banana-tube $(DESTDIR)/usr/bin/banana-tube
	install -D -m 644 app.json $(DESTDIR)/usr/share/banana/apps/bananatube/app.json
	install -D -m 644 BananaTube.app $(DESTDIR)/usr/share/banana/apps/bananatube/BananaTube.app

clean:
	rm -f banana-tube

.PHONY: install clean
