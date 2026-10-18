SHELL := /bin/sh

BUNDLE := build/Navi33Metal.bundle
EXECUTABLE := $(BUNDLE)/Contents/MacOS/Navi33Metal
RESOURCE_DIR := $(BUNDLE)/Contents/Resources
CLANG ?= xcrun --sdk macosx clang
MACOSX_DEPLOYMENT_TARGET := 26.0
SDK_VERSION := $(shell xcrun --sdk macosx --show-sdk-version 2>/dev/null)
SDK_MAJOR := $(firstword $(subst ., ,$(SDK_VERSION)))
HOST_CC ?= cc
HOST_FLAGS ?= -std=c11 -Wall -Wextra -Werror -pedantic -O2
HOST_TEST := build/test_n33_metal_gate

OBJCFLAGS := -arch x86_64 -mmacosx-version-min=$(MACOSX_DEPLOYMENT_TARGET) \
	-fobjc-arc -fblocks -O2 -std=gnu11 -Wall -Wextra -Werror
FRAMEWORKS := -framework Foundation -framework Metal -framework IOKit

.PHONY: all bundle artifact clean check-sdk host-test

all: bundle artifact

check-sdk:
	@if [ "$(SDK_MAJOR)" != "26" ]; then \
		echo "Navi33Metal requires the macOS 26 SDK (found: $(SDK_VERSION)); build on macOS 26 with Xcode selected." >&2; \
		exit 2; \
	fi

host-test: $(HOST_TEST)
	./$(HOST_TEST)

$(HOST_TEST): test_n33_metal_gate.c Navi33MetalGate.h
	@mkdir -p build
	$(HOST_CC) $(HOST_FLAGS) -I. test_n33_metal_gate.c -o $@

bundle: $(EXECUTABLE)
	@plutil -lint $(BUNDLE)/Contents/Info.plist
	@codesign --force --sign - $(BUNDLE)
	@codesign --verify --strict $(BUNDLE)

$(EXECUTABLE): check-sdk Navi33Device.m Navi33MetalGate.h Info.plist
	@mkdir -p $(dir $@) $(RESOURCE_DIR)
	@cp Info.plist $(BUNDLE)/Contents/Info.plist
	$(CLANG) $(OBJCFLAGS) -bundle $(FRAMEWORKS) -o $@ Navi33Device.m

artifact: bundle
	@tar --no-xattrs --no-mac-metadata -C build -cf build/Navi33Metal.bundle.tar Navi33Metal.bundle
	@file $(EXECUTABLE)
	@echo "bundle artifact: build/Navi33Metal.bundle.tar (not installed or registered)"

clean:
	@rm -rf build
