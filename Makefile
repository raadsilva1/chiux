SHELL := /bin/sh

PROJECT_NAME := chiux
BUILD_DIR ?= build
PREFIX ?= /usr/local
DESTDIR ?=
CONFIG_HOME ?= $(HOME)/.config/chiux
CONFIG_FILE ?= $(CONFIG_HOME)/config.ini
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')
CMAKE ?= cmake
XORG_BIN ?= /usr/lib/Xorg
VT ?= vt8
XINIT_TARGET ?= :1

INSTALL_BINDIR := $(DESTDIR)$(PREFIX)/bin
INSTALL_DATADIR := $(DESTDIR)$(PREFIX)/share/chiux
INSTALL_RESOURCEDIR := $(INSTALL_DATADIR)/res

.PHONY: all configure build run install install-files bootstrap clean distclean help

all: build

help:
	@printf '%s\n' \
		'Targets:' \
		'  make build       - configure and build chiux + chiux-te' \
		'  make install     - build, install, and bootstrap ~/.config/chiux/config.ini if missing' \
		'  make bootstrap   - alias for make install' \
		'  make clean       - clean the CMake build tree' \
		'  make distclean   - remove the CMake build tree'

configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt $(shell find src -type f 2>/dev/null) res/chimera.png res/config.ini
	@$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$(PREFIX)

build: configure
	@$(CMAKE) --build $(BUILD_DIR) -j $(JOBS)

run: build
	@exec "$(BUILD_DIR)/chiux"

install: build install-files bootstrap-config

install-files:
	@install -d "$(INSTALL_BINDIR)" "$(INSTALL_RESOURCEDIR)"
	@install -m 755 "$(BUILD_DIR)/chiux" "$(INSTALL_BINDIR)/chiux"
	@install -m 755 "$(BUILD_DIR)/chiux-te" "$(INSTALL_BINDIR)/chiux-te"
	@install -m 644 "res/chimera.png" "$(INSTALL_RESOURCEDIR)/chimera.png"
	@install -m 644 "res/config.ini" "$(INSTALL_DATADIR)/config.ini.example"

bootstrap-config:
	@mkdir -p "$(CONFIG_HOME)"
	@if [ ! -f "$(CONFIG_FILE)" ]; then \
		install -m 644 "$(INSTALL_DATADIR)/config.ini.example" "$(CONFIG_FILE)"; \
	fi

bootstrap: install-files bootstrap-config

clean:
	@$(CMAKE) --build $(BUILD_DIR) --target clean

distclean:
	@rm -rf "$(BUILD_DIR)"
