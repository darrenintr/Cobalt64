# Navi33GOPFB: software-only, GOP scanout adoption. No GPU firmware.
# Build requires macOS + Xcode Command Line Tools + MacKernelSDK.
PRODUCT := Navi33GOPFB
ARCH := x86_64
DEPLOY := 11.0
MKSDK ?= ../../../RDNA4FB/MacKernelSDK
SDK ?= $(shell xcrun --sdk macosx --show-sdk-path)
CXX := clang++
CC := clang
HOST_CXX ?= c++
HOST_FLAGS ?= -std=c++17 -Wall -Wextra -Werror -pedantic

BUILD := build
KEXT := $(BUILD)/$(PRODUCT).kext
EXEC := $(KEXT)/Contents/MacOS/$(PRODUCT)
COMMON := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
    -I$(MKSDK)/Headers -Isrc -I../src -mmacosx-version-min=$(DEPLOY) \
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
    -D_FORTIFY_SOURCE=0 -nostdinc -fno-builtin -fno-common \
    -fno-stack-protector -mkernel -fapple-kext -Wall -Os -g
CXXFLAGS := $(COMMON) -std=c++17 -fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors
CFLAGS := $(COMMON)
LDFLAGS := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
    -nostdlib -Xlinker -kext -L$(MKSDK)/Library/$(ARCH) \
    -lkmodc++ -lkmod -Wl,-no_deduplicate

.PHONY: all clean host-test
all: $(KEXT)

$(BUILD)/Navi33GOPFB.o: src/Navi33GOPFB.cpp src/Navi33GOPFB.hpp src/Navi33GopLayout.h src/Navi33EDID.h src/Navi33Pattern.h src/Navi33BarGuard.h ../src/Navi33Identity.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kmod_info.o: src/kmod_info.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KEXT): $(BUILD)/Navi33GOPFB.o $(BUILD)/kmod_info.o Info.plist
	@mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(BUILD)/Navi33GOPFB.o $(BUILD)/kmod_info.o -o $(EXEC)
	cp Info.plist $(KEXT)/Contents/Info.plist
	xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT)
	@test -f $(KEXT)/Contents/_CodeSignature/CodeResources
	@echo "Built (NOT INSTALLED): $(KEXT)"

$(BUILD)/test-layout: tests/test_layout.cpp src/Navi33GopLayout.h src/Navi33BarGuard.h

$(BUILD)/test-pattern: tests/test_pattern.cpp src/Navi33Pattern.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_pattern.cpp -o $@

$(BUILD)/test-edid: tests/test_edid.cpp src/Navi33EDID.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_edid.cpp -o $@
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_layout.cpp -o $@

host-test: $(BUILD)/test-layout $(BUILD)/test-edid $(BUILD)/test-pattern
	$(BUILD)/test-layout
	$(BUILD)/test-edid
	$(BUILD)/test-pattern
	python3 tests/test_manifest.py

clean:
	rm -rf $(BUILD)
