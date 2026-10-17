# Separate Navi 33 Stage-0 probe: no shared Navi48 sources, no firmware.
# On macOS with Xcode CLT: make MKSDK=/path/to/MacKernelSDK
# On any POSIX C++17 host: make host-test

PRODUCT := Navi33Probe
ARCH := x86_64
DEPLOY := 11.0
MKSDK ?= ../../RDNA4FB/MacKernelSDK
SDK ?= $(shell xcrun --sdk macosx --show-sdk-path)

CXX := clang++
CC := clang
HOST_CXX ?= c++
BUILD := build
KEXT := $(BUILD)/$(PRODUCT).kext
EXEC := $(KEXT)/Contents/MacOS/$(PRODUCT)

COMMON := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) -isysroot $(SDK) \
    -I$(MKSDK)/Headers -Isrc -mmacosx-version-min=$(DEPLOY) \
    -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT \
    -D_FORTIFY_SOURCE=0 -nostdinc -fno-builtin -fno-common \
    -fno-stack-protector -mkernel -fapple-kext -Wall -Os -g
CXXFLAGS := $(COMMON) -std=c++17 -fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors
CFLAGS := $(COMMON)
LDFLAGS := -arch $(ARCH) -target $(ARCH)-apple-macos$(DEPLOY) \
    -isysroot $(SDK) -nostdlib -Xlinker -kext \
    -L$(MKSDK)/Library/x86_64 -lkmodc++ -lkmod -Wl,-no_deduplicate

.PHONY: all clean host-test
all: $(KEXT)

$(BUILD)/Navi33Probe.o: src/Navi33Probe.cpp src/Navi33Identity.h src/Navi33Discovery.h src/Navi33HardwarePlan.h src/Navi33Dcn321.h src/Navi33FirmwareGuard.h src/Navi33Sdma6Packet.h src/Navi33Gmc11Gart.h src/Navi33GpuVaArena.h src/Navi33Sdma6Ring.h src/Navi33DmaScatter.h src/Navi33DarwinDma.h src/Navi33LiveMmio.h src/Navi33Psp13Status.h src/Navi33Psp13Firmware.h src/Navi33Sdma6Status.h src/Navi33Probe.hpp src/Navi33NativeClient.hpp src/Navi33NativePolicy.h src/Navi33DarwinStage.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33NativeClient.o: src/Navi33NativeClient.cpp src/Navi33NativeClient.hpp src/Navi33NativePolicy.h src/Navi33Probe.hpp src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33DarwinStage.o: src/Navi33DarwinStage.cpp src/Navi33DarwinStage.h src/Navi33Sdma6Staging.h src/Navi33DmaScatter.h src/Navi33Gmc11Gart.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33DarwinDma.o: src/Navi33DarwinDma.cpp src/Navi33DarwinDma.h src/Navi33DmaScatter.h src/Navi33Gmc11Gart.h src/Navi33Identity.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kmod_info.o: src/kmod_info.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KEXT): $(BUILD)/Navi33Probe.o $(BUILD)/Navi33NativeClient.o $(BUILD)/Navi33DarwinDma.o $(BUILD)/Navi33DarwinStage.o $(BUILD)/kmod_info.o Info.plist
	@mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(BUILD)/Navi33Probe.o $(BUILD)/Navi33NativeClient.o $(BUILD)/Navi33DarwinDma.o $(BUILD)/Navi33DarwinStage.o $(BUILD)/kmod_info.o -o $(EXEC)
	cp Info.plist $(KEXT)/Contents/Info.plist
	xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT)
	@echo "Built $(KEXT); NOT installed"

HOST_FLAGS ?= -std=c++17 -Wall -Wextra -Werror -pedantic

$(BUILD)/test-identity: tests/test_identity.cpp src/Navi33Identity.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_identity.cpp -o $@

$(BUILD)/test-discovery: tests/test_discovery.cpp src/Navi33Discovery.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_discovery.cpp -o $@

$(BUILD)/discovery-dump: tools/discovery_dump.cpp src/Navi33Discovery.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tools/discovery_dump.cpp -o $@

$(BUILD)/test-hardware-plan: tests/test_hardware_plan.cpp src/Navi33HardwarePlan.h src/Navi33Dcn321.h src/Navi33Discovery.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_hardware_plan.cpp -o $@

$(BUILD)/test-firmware: tests/test_firmware.cpp src/Navi33FirmwareGuard.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_firmware.cpp -o $@

$(BUILD)/test-sdma6-packet: tests/test_sdma6_packet.cpp src/Navi33Sdma6Packet.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_packet.cpp -o $@

$(BUILD)/test-gmc11-gart: tests/test_gmc11_gart.cpp src/Navi33Gmc11Gart.h src/Navi33GpuVaArena.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_gart.cpp -o $@

$(BUILD)/test-sdma6-ring: tests/test_sdma6_ring.cpp src/Navi33Sdma6Ring.h src/Navi33Sdma6Packet.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_ring.cpp -o $@

$(BUILD)/test-dma-scatter: tests/test_dma_scatter.cpp src/Navi33DmaScatter.h src/Navi33Gmc11Gart.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_dma_scatter.cpp -o $@

$(BUILD)/test-gfx11-native: tests/test_gfx11_native.cpp src/Navi33NativeABI.h src/Navi33Gfx11Command.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gfx11_native.cpp -o $@

$(BUILD)/native-caps: tools/native_caps.cpp src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tools/native_caps.cpp -o $@

$(BUILD)/test-live-mmio: tests/test_live_mmio.cpp src/Navi33LiveMmio.h src/Navi33HardwarePlan.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_live_mmio.cpp -o $@

$(BUILD)/test-native-client: tests/test_native_client.cpp src/Navi33NativePolicy.h src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_native_client.cpp -o $@

$(BUILD)/test-psp13-status: tests/test_psp13_status.cpp src/Navi33Psp13Status.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_psp13_status.cpp -o $@

$(BUILD)/test-psp13-firmware: tests/test_psp13_firmware.cpp src/Navi33Psp13Firmware.h src/Navi33FirmwareGuard.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_psp13_firmware.cpp -o $@

$(BUILD)/test-sdma6-status: tests/test_sdma6_status.cpp src/Navi33Sdma6Status.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_status.cpp -o $@

$(BUILD)/test-sdma6-staging: tests/test_sdma6_staging.cpp src/Navi33Sdma6Staging.h src/Navi33DmaScatter.h src/Navi33Sdma6Packet.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_staging.cpp -o $@

$(BUILD)/test-sdma6-hardware-gate: tests/test_sdma6_hardware_gate.cpp src/Navi33Sdma6HardwareGate.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_hardware_gate.cpp -o $@

$(BUILD)/test-sdma6-mmio-queue: tests/test_sdma6_mmio_queue.cpp src/Navi33Sdma6MmioQueue.h src/Navi33Sdma6Staging.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_mmio_queue.cpp -o $@

$(BUILD)/test-sdma6-mmio-off: tests/test_sdma6_mmio_off.cpp src/Navi33Sdma6MmioQueue.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_mmio_off.cpp -o $@

$(BUILD)/test-sdma6-bar-adapter: tests/test_sdma6_bar_adapter.cpp src/Navi33Sdma6BarAdapter.h src/Navi33Sdma6MmioQueue.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_bar_adapter.cpp -o $@

$(BUILD)/backend-report: tools/backend_report.cpp src/Navi33HardwarePlan.h src/Navi33Dcn321.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tools/backend_report.cpp -o $@

host-test: $(BUILD)/test-identity $(BUILD)/test-discovery $(BUILD)/test-hardware-plan $(BUILD)/test-firmware $(BUILD)/test-sdma6-packet $(BUILD)/test-gmc11-gart $(BUILD)/test-sdma6-ring $(BUILD)/test-dma-scatter $(BUILD)/discovery-dump $(BUILD)/backend-report $(BUILD)/test-gfx11-native $(BUILD)/native-caps $(BUILD)/test-live-mmio $(BUILD)/test-native-client $(BUILD)/test-psp13-status $(BUILD)/test-psp13-firmware $(BUILD)/test-sdma6-status $(BUILD)/test-sdma6-staging $(BUILD)/test-sdma6-hardware-gate $(BUILD)/test-sdma6-mmio-queue $(BUILD)/test-sdma6-mmio-off $(BUILD)/test-sdma6-bar-adapter
	$(BUILD)/test-identity
	$(BUILD)/test-discovery
	$(BUILD)/test-hardware-plan
	$(BUILD)/test-firmware
	$(BUILD)/test-sdma6-packet
	$(BUILD)/test-gmc11-gart
	$(BUILD)/test-sdma6-ring
	$(BUILD)/test-dma-scatter
	$(BUILD)/test-gfx11-native
	$(BUILD)/test-live-mmio
	$(BUILD)/test-native-client
	$(BUILD)/test-psp13-status
	$(BUILD)/test-psp13-firmware
	$(BUILD)/test-sdma6-status
	$(BUILD)/test-sdma6-staging
	$(BUILD)/test-sdma6-hardware-gate
	$(BUILD)/test-sdma6-mmio-queue
	$(BUILD)/test-sdma6-mmio-off
	$(BUILD)/test-sdma6-bar-adapter
	$(BUILD)/native-caps
	python3 tests/test_manifest.py

clean:
	rm -rf $(BUILD)
