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

.PHONY: all clean host-test physical-io-check metal-format-sdk-check
all: $(KEXT)

$(BUILD)/Navi33Probe.o: src/Navi33Probe.cpp src/Navi33DarwinPciOwner.h src/Navi33PciOwner.h src/Navi33ProductionOwner.h src/Navi33Identity.h src/Navi33Discovery.h src/Navi33HardwarePlan.h src/Navi33Dcn321.h src/Navi33FirmwareGuard.h src/Navi33Sdma6Packet.h src/Navi33Gmc11Gart.h src/Navi33GpuVaArena.h src/Navi33Sdma6Ring.h src/Navi33DmaScatter.h src/Navi33DarwinDma.h src/Navi33LiveMmio.h src/Navi33HwProbeLogic.h src/Navi33Gmc11VramLive.h src/Navi33Gmc11VramGeometry.h src/Navi33Nbio43Capacity.h src/Navi33VramSurvey.h src/Navi33Psp13Status.h src/Navi33Psp13Firmware.h src/Navi33Sdma6Status.h src/Navi33Sdma6QueueCensus.h src/Navi33MmioPeek.h src/Navi33PeekWindows.h src/Navi33Probe.hpp src/Navi33NativeClient.hpp src/Navi33NativePolicy.h src/Navi33DarwinStage.h src/Navi33StageOwner.h src/Navi33DarwinPspLoad.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33NativeClient.o: src/Navi33NativeClient.cpp src/Navi33NativeClient.hpp src/Navi33NativePolicy.h src/Navi33Probe.hpp src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33DarwinStage.o: src/Navi33DarwinStage.cpp src/Navi33DarwinStage.h src/Navi33DmaLifetime.h src/Navi33Sdma6Staging.h src/Navi33DmaScatter.h src/Navi33Gmc11Gart.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Physical IOKit Queue1 bridge is compile-checked on macOS only. Never
# link it into the live Navi33Probe.kext until PCI, VRAM, PSP and
# independent device recovery are implemented and audited.
$(BUILD)/Navi33DarwinRootReadback.o: src/Navi33DarwinRootReadback.cpp src/Navi33DarwinRootReadback.h src/Navi33Gmc11VramLease.h src/Navi33Gmc11RootVram.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33DarwinQueue1IO.o: src/Navi33DarwinQueue1IO.cpp src/Navi33DarwinQueue1IO.h src/Navi33DarwinStage.h src/Navi33Sdma6BarAdapter.h src/Navi33Sdma6MmioQueue.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

physical-io-check: $(BUILD)/Navi33DarwinQueue1IO.o $(BUILD)/Navi33DarwinRootReadback.o

# SDK-only enum compatibility check. This runs no Metal device code and is
# deliberately separate from host-test so Linux host-test needs no macOS SDK.
$(BUILD)/metal-format-enum-check: tools/metal_format_enum_check.m src/Navi33Gfx11Formats.h
	@mkdir -p $(BUILD)
	$(CC) -arch $(ARCH) -x objective-c -std=gnu11 -Wall -Wextra -Werror \
	    -isysroot $(SDK) -mmacosx-version-min=$(DEPLOY) $< -framework Metal -o $@

metal-format-sdk-check: $(BUILD)/metal-format-enum-check
	$(BUILD)/metal-format-enum-check

$(BUILD)/Navi33DarwinDma.o: src/Navi33DarwinDma.cpp src/Navi33DarwinDma.h src/Navi33DmaScatter.h src/Navi33Gmc11Gart.h src/Navi33Identity.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33DarwinPspLoad.o: src/Navi33DarwinPspLoad.cpp src/Navi33DarwinPspLoad.h src/Navi33Psp13Loader.h src/Navi33Psp13Firmware.h src/Navi33FirmwareGuard.h src/Navi33Sha256.h src/Navi33Dcn321.h src/Navi33Gmc11VramLive.h src/Navi33Gmc11VramGeometry.h src/Navi33HardwarePlan.h src/Navi33Discovery.h src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33DarwinPspRing.o: src/Navi33DarwinPspRing.cpp src/Navi33Psp13Ring.h src/Navi33DarwinPspLoad.h src/Navi33Psp13Loader.h src/Navi33Psp13Firmware.h src/Navi33FirmwareGuard.h src/Navi33Sha256.h src/Navi33Dcn321.h src/Navi33Gmc11VramLive.h src/Navi33Gmc11VramGeometry.h src/Navi33HardwarePlan.h src/Navi33Discovery.h src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33DarwinPspFw.o: src/Navi33DarwinPspFw.cpp src/Navi33Psp13FwLoad.h src/Navi33Psp13IpFw.h src/Navi33Psp13Ring.h src/Navi33DarwinPspLoad.h src/Navi33Psp13Loader.h src/Navi33Psp13Firmware.h src/Navi33FirmwareGuard.h src/Navi33Sha256.h src/Navi33Dcn321.h src/Navi33Gmc11VramLive.h src/Navi33Gmc11VramGeometry.h src/Navi33HardwarePlan.h src/Navi33Discovery.h src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/Navi33DarwinPciOwner.o: src/Navi33DarwinPciOwner.cpp src/Navi33DarwinPciOwner.h src/Navi33PciOwner.h src/Navi33ProductionOwner.h src/Navi33Sdma6Queue1MemoryContract.h src/Navi33Identity.h src/Navi33Discovery.h
	$(CXX) $(CXXFLAGS) -c src/Navi33DarwinPciOwner.cpp -o $@

$(BUILD)/Navi33DarwinSmu.o: src/Navi33DarwinSmu.cpp src/Navi33Smu13Enable.h src/Navi33DarwinPspLoad.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h src/Navi33Discovery.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kmod_info.o: src/kmod_info.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(KEXT): $(BUILD)/Navi33DarwinSmu.o $(BUILD)/Navi33Probe.o $(BUILD)/Navi33NativeClient.o $(BUILD)/Navi33DarwinDma.o $(BUILD)/Navi33DarwinStage.o $(BUILD)/Navi33DarwinPspLoad.o $(BUILD)/Navi33DarwinPspRing.o $(BUILD)/Navi33DarwinPspFw.o $(BUILD)/Navi33DarwinPciOwner.o $(BUILD)/kmod_info.o Info.plist
	@mkdir -p $(KEXT)/Contents/MacOS
	$(CXX) $(LDFLAGS) $(BUILD)/Navi33DarwinSmu.o $(BUILD)/Navi33Probe.o $(BUILD)/Navi33NativeClient.o $(BUILD)/Navi33DarwinDma.o $(BUILD)/Navi33DarwinStage.o $(BUILD)/Navi33DarwinPspLoad.o $(BUILD)/Navi33DarwinPspRing.o $(BUILD)/Navi33DarwinPspFw.o $(BUILD)/Navi33DarwinPciOwner.o $(BUILD)/kmod_info.o -o $(EXEC)
	cp Info.plist $(KEXT)/Contents/Info.plist
	xattr -cr $(KEXT)
	codesign --force --sign - $(KEXT)
	@echo "Built $(KEXT); NOT installed"

HOST_FLAGS ?= -std=c++17 -Wall -Wextra -Werror -pedantic
# Optional: PSP_FW=/path/to/psp_13_0_7_sos.bin also checks the real file
# against the Kext's SHA-256 pin (the file is not in this repository).
# Optional: a directory with the eight decompressed firmware files, to check them
# against tests/fixtures/rx7600-fw-plan.tsv (see tools/fwplan.py).
IPFW_DIR ?=
PSP_FW ?=

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

$(BUILD)/test-dma-backend: tests/test_dma_backend.cpp tests/dma_mock_gpu.h src/Navi33DmaBackend.h src/Navi33Gmc11Gart.h src/Navi33Sdma6Packet.h src/Navi33Sdma6Ring.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_dma_backend.cpp -o $@

DMA_ENGINE_DEPS=tests/test_dma_engine.cpp tests/dma_mock_gpu.h src/Navi33DmaEngine.h src/Navi33DmaBackend.h src/Navi33Sdma6MmioQueue.h src/Navi33Sdma6Ring.h src/Navi33Gmc11Gart.h src/Navi33Sdma6Packet.h

$(BUILD)/test-dma-engine: $(DMA_ENGINE_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_dma_engine.cpp -o $@

# Same source, write path compiled in; drives the simulated SDMA6 block.
$(BUILD)/test-dma-engine-unsafe: $(DMA_ENGINE_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) -DCOBALT_NAVI33_ENABLE_UNSAFE_MMIO tests/test_dma_engine.cpp -o $@

$(BUILD)/test-gfx11-native: tests/test_gfx11_native.cpp src/Navi33NativeABI.h src/Navi33Gfx11Command.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gfx11_native.cpp -o $@

$(BUILD)/test-gfx11-formats: tests/test_gfx11_formats.cpp src/Navi33Gfx11Formats.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gfx11_formats.cpp -o $@

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

$(BUILD)/test-sha256: tests/test_sha256.cpp src/Navi33Sha256.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sha256.cpp -o $@

$(BUILD)/test-psp13-ipfw: tests/test_psp13_ipfw.cpp src/Navi33Psp13IpFw.h src/Navi33Sha256.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_psp13_ipfw.cpp -o $@

PSP_RING_DEPS := src/Navi33Psp13Ring.h src/Navi33Psp13Loader.h src/Navi33Psp13Firmware.h src/Navi33Sha256.h src/Navi33Dcn321.h src/Navi33Gmc11VramLive.h src/Navi33Gmc11VramGeometry.h src/Navi33HardwarePlan.h src/Navi33NativeABI.h tests/rx7600_ipd_fixture.h

$(BUILD)/test-psp13-ring: tests/test_psp13_ring.cpp $(PSP_RING_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_psp13_ring.cpp -o $@

$(BUILD)/test-psp13-fwload: tests/test_psp13_fwload.cpp $(PSP_RING_DEPS) src/Navi33Psp13FwLoad.h src/Navi33Psp13IpFw.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_psp13_fwload.cpp -o $@

$(BUILD)/test-psp13-ring-run: tests/test_psp13_ring_run.cpp $(PSP_RING_DEPS)
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_psp13_ring_run.cpp -o $@

$(BUILD)/test-psp13-loader: tests/test_psp13_loader.cpp src/Navi33Psp13Loader.h src/Navi33Psp13Firmware.h src/Navi33FirmwareGuard.h src/Navi33Sha256.h src/Navi33Dcn321.h src/Navi33Gmc11VramLive.h src/Navi33Gmc11VramGeometry.h src/Navi33HardwarePlan.h src/Navi33NativeABI.h tests/rx7600_ipd_fixture.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_psp13_loader.cpp -o $@

$(BUILD)/test-sdma6-status: tests/test_sdma6_status.cpp src/Navi33Sdma6Status.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_status.cpp -o $@

$(BUILD)/test-sdma6-queue-census: tests/test_sdma6_queue_census.cpp src/Navi33Sdma6QueueCensus.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_queue_census.cpp -o $@

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

$(BUILD)/test-gmc11-mmhub: tests/test_gmc11_mmhub.cpp src/Navi33Gmc11Mmhub.h src/Navi33HardwarePlan.h src/Navi33Gmc11Gart.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_mmhub.cpp -o $@

$(BUILD)/test-gmc11-mmhub-off: tests/test_gmc11_mmhub_off.cpp src/Navi33Gmc11Mmhub.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_mmhub_off.cpp -o $@

$(BUILD)/test-hw-probe-logic: tests/test_hw_probe_logic.cpp src/Navi33HwProbeLogic.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_hw_probe_logic.cpp -o $@

$(BUILD)/test-sdma6-ring-test: tests/test_sdma6_ring_test.cpp src/Navi33Sdma6RingTest.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_ring_test.cpp -o $@

$(BUILD)/test-mmio-peek: tests/test_mmio_peek.cpp src/Navi33MmioPeek.h src/Navi33PeekWindows.h src/Navi33HwProbeLogic.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h tests/rx7600_ipd_fixture.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_mmio_peek.cpp -o $@

$(BUILD)/test-sdma6-proof-run: tests/test_sdma6_proof_run.cpp src/Navi33Sdma6ProofRun.h src/Navi33Gmc11Mmhub.h src/Navi33Sdma6MmioQueue.h src/Navi33Gmc11RootVram.h src/Navi33Sdma6QueueOwner.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_proof_run.cpp -o $@

$(BUILD)/test-sdma6-queue1-config-plan: tests/test_sdma6_queue1_config_plan.cpp src/Navi33Sdma6Queue1ConfigPlan.h src/Navi33Gmc11VramLease.h src/Navi33Gmc11VramGeometry.h src/Navi33Gmc11RootVram.h src/Navi33Gmc11Mmhub.h src/Navi33Sdma6MmioQueue.h src/Navi33HardwarePlan.h tests/rx7600_ipd_fixture.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_queue1_config_plan.cpp -o $@

$(BUILD)/test-gfx11-mec-enable: tests/test_gfx11_mec_enable.cpp src/Navi33Gfx11MecEnable.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gfx11_mec_enable.cpp -o $@

$(BUILD)/test-gfx11-kiq-hqd: tests/test_gfx11_kiq_hqd.cpp src/Navi33Gfx11KiqHqd.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gfx11_kiq_hqd.cpp -o $@

$(BUILD)/test-gfx11-mqd: tests/test_gfx11_mqd.cpp src/Navi33Gfx11Mqd.h src/Navi33Gfx11KiqHqd.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gfx11_mqd.cpp -o $@

$(BUILD)/test-gfx11-mes: tests/test_gfx11_mes.cpp src/Navi33GfxMes.h src/Navi33Gfx11KiqHqd.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gfx11_mes.cpp -o $@

$(BUILD)/test-pci-owner: tests/test_pci_owner.cpp src/Navi33PciOwner.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_pci_owner.cpp -o $@

$(BUILD)/test-production-owner: tests/test_production_owner.cpp src/Navi33ProductionOwner.h src/Navi33PciOwner.h src/Navi33Sdma6Queue1MemoryContract.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_production_owner.cpp -o $@

$(BUILD)/test-gfx11-mes-mqd: tests/test_gfx11_mes_mqd.cpp src/Navi33GfxMesMqd.h src/Navi33GfxMes.h src/Navi33Gfx11Mqd.h src/Navi33Gfx11KiqHqd.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gfx11_mes_mqd.cpp -o $@

$(BUILD)/test-sdma6-queue1-memory-contract: tests/test_sdma6_queue1_memory_contract.cpp src/Navi33Sdma6Queue1MemoryContract.h src/Navi33Sdma6Queue1ConfigPlan.h src/Navi33Gmc11VramLease.h src/Navi33Gmc11RootVram.h src/Navi33Gmc11Mmhub.h src/Navi33Sdma6MmioQueue.h src/Navi33HardwarePlan.h tests/rx7600_ipd_fixture.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_queue1_memory_contract.cpp -o $@

$(BUILD)/test-dma-lifetime: tests/test_dma_lifetime.cpp src/Navi33DmaLifetime.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_dma_lifetime.cpp -o $@

$(BUILD)/test-stage-owner: tests/test_stage_owner.cpp src/Navi33StageOwner.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_stage_owner.cpp -o $@

$(BUILD)/test-gmc11-root-vram: tests/test_gmc11_root_vram.cpp src/Navi33Gmc11RootVram.h src/Navi33Gmc11Mmhub.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_root_vram.cpp -o $@

$(BUILD)/test-sdma6-queue-owner: tests/test_sdma6_queue_owner.cpp src/Navi33Sdma6QueueOwner.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_sdma6_queue_owner.cpp -o $@

$(BUILD)/test-gmc11-vram-geometry: tests/test_gmc11_vram_geometry.cpp src/Navi33Gmc11VramGeometry.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_vram_geometry.cpp -o $@

$(BUILD)/test-vram-bo-allocator: tests/test_vram_bo_allocator.cpp src/Navi33VramBoAllocator.h src/Navi33Gmc11VramGeometry.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_vram_bo_allocator.cpp -o $@

$(BUILD)/test-gmc11-vram-live: tests/test_gmc11_vram_live.cpp src/Navi33Gmc11VramLive.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h src/Navi33Gmc11VramGeometry.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_vram_live.cpp -o $@

$(BUILD)/test-gmc11-vram-lease: tests/test_gmc11_vram_lease.cpp src/Navi33Gmc11VramLease.h src/Navi33VramBoAllocator.h src/Navi33Gmc11RootVram.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_vram_lease.cpp -o $@

$(BUILD)/test-nbio43-capacity: tests/test_nbio43_capacity.cpp src/Navi33Nbio43Capacity.h src/Navi33Gmc11VramLive.h src/Navi33VramSurvey.h src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_nbio43_capacity.cpp -o $@

$(BUILD)/test-gmc11-vram-upload: tests/test_gmc11_vram_upload.cpp src/Navi33Gmc11VramUpload.h src/Navi33Gmc11VramLease.h src/Navi33Gmc11RootVram.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_vram_upload.cpp -o $@

$(BUILD)/test-gmc11-vram-upload-off: tests/test_gmc11_vram_upload_off.cpp src/Navi33Gmc11VramUpload.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_gmc11_vram_upload_off.cpp -o $@

$(BUILD)/test-rx7600-silicon-bases: tests/test_rx7600_silicon_bases.cpp src/Navi33HardwarePlan.h src/Navi33Discovery.h src/Navi33Dcn321.h src/Navi33LiveMmio.h src/Navi33Psp13Status.h src/Navi33Sdma6Status.h src/Navi33Sdma6QueueCensus.h src/Navi33Sdma6BarAdapter.h src/Navi33Gmc11Mmhub.h src/Navi33Gmc11VramLive.h src/Navi33Nbio43Capacity.h src/Navi33HwProbeLogic.h tests/rx7600_ipd_fixture.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tests/test_rx7600_silicon_bases.cpp -o $@

$(BUILD)/backend-report: tools/backend_report.cpp src/Navi33HardwarePlan.h src/Navi33Dcn321.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) tools/backend_report.cpp -o $@

$(BUILD)/test-smu13-enable: tests/test_smu13_enable.cpp src/Navi33Smu13Enable.h src/Navi33NativeABI.h src/Navi33HardwarePlan.h tests/rx7600_ipd_fixture.h
	@mkdir -p $(BUILD)
	$(HOST_CXX) $(HOST_FLAGS) $< -o $@

host-test: $(BUILD)/test-smu13-enable $(BUILD)/test-identity $(BUILD)/test-discovery $(BUILD)/test-hardware-plan $(BUILD)/test-firmware $(BUILD)/test-sdma6-packet $(BUILD)/test-gmc11-gart $(BUILD)/test-sdma6-ring $(BUILD)/test-dma-scatter $(BUILD)/test-dma-backend $(BUILD)/test-dma-engine $(BUILD)/test-dma-engine-unsafe $(BUILD)/discovery-dump $(BUILD)/backend-report $(BUILD)/test-gfx11-native $(BUILD)/test-gfx11-formats $(BUILD)/native-caps $(BUILD)/test-live-mmio $(BUILD)/test-native-client $(BUILD)/test-psp13-status $(BUILD)/test-psp13-firmware $(BUILD)/test-sha256 $(BUILD)/test-psp13-loader $(BUILD)/test-psp13-ring $(BUILD)/test-psp13-ring-run $(BUILD)/test-psp13-ipfw $(BUILD)/test-psp13-fwload $(BUILD)/test-sdma6-status $(BUILD)/test-sdma6-queue-census $(BUILD)/test-sdma6-staging $(BUILD)/test-sdma6-hardware-gate $(BUILD)/test-sdma6-mmio-queue $(BUILD)/test-sdma6-mmio-off $(BUILD)/test-sdma6-bar-adapter $(BUILD)/test-gmc11-mmhub $(BUILD)/test-gmc11-mmhub-off $(BUILD)/test-hw-probe-logic $(BUILD)/test-mmio-peek $(BUILD)/test-sdma6-ring-test $(BUILD)/test-sdma6-proof-run $(BUILD)/test-sdma6-queue1-config-plan $(BUILD)/test-sdma6-queue1-memory-contract $(BUILD)/test-gfx11-mec-enable $(BUILD)/test-gfx11-kiq-hqd $(BUILD)/test-gfx11-mqd $(BUILD)/test-gfx11-mes $(BUILD)/test-gfx11-mes-mqd $(BUILD)/test-pci-owner $(BUILD)/test-production-owner $(BUILD)/test-dma-lifetime $(BUILD)/test-stage-owner $(BUILD)/test-gmc11-root-vram $(BUILD)/test-sdma6-queue-owner $(BUILD)/test-gmc11-vram-geometry $(BUILD)/test-vram-bo-allocator $(BUILD)/test-gmc11-vram-live $(BUILD)/test-gmc11-vram-lease $(BUILD)/test-nbio43-capacity $(BUILD)/test-gmc11-vram-upload $(BUILD)/test-gmc11-vram-upload-off $(BUILD)/test-rx7600-silicon-bases
	$(BUILD)/test-smu13-enable
	$(BUILD)/test-identity
	$(BUILD)/test-discovery
	$(BUILD)/test-hardware-plan
	$(BUILD)/test-firmware
	$(BUILD)/test-sdma6-packet
	$(BUILD)/test-gmc11-gart
	$(BUILD)/test-sdma6-ring
	$(BUILD)/test-dma-scatter
	$(BUILD)/test-dma-backend
	$(BUILD)/test-dma-engine
	$(BUILD)/test-dma-engine-unsafe
	$(BUILD)/test-gfx11-native
	$(BUILD)/test-gfx11-formats
	$(BUILD)/test-live-mmio
	$(BUILD)/test-native-client
	$(BUILD)/test-psp13-status
	$(BUILD)/test-psp13-firmware
	$(BUILD)/test-sha256
	$(BUILD)/test-psp13-loader $(PSP_FW)
	$(BUILD)/test-psp13-ring
	$(BUILD)/test-psp13-ring-run
	$(BUILD)/test-psp13-ipfw $(IPFW_DIR) $(if $(IPFW_DIR),tests/fixtures/rx7600-fw-plan.tsv)
	$(BUILD)/test-psp13-fwload
	$(BUILD)/test-sdma6-status
	$(BUILD)/test-sdma6-queue-census
	$(BUILD)/test-sdma6-staging
	$(BUILD)/test-sdma6-hardware-gate
	$(BUILD)/test-sdma6-mmio-queue
	$(BUILD)/test-sdma6-mmio-off
	$(BUILD)/test-sdma6-bar-adapter
	$(BUILD)/test-gmc11-mmhub
	$(BUILD)/test-gmc11-mmhub-off
	$(BUILD)/test-hw-probe-logic
	$(BUILD)/test-mmio-peek
	$(BUILD)/test-sdma6-ring-test
	$(BUILD)/test-sdma6-proof-run
	$(BUILD)/test-sdma6-queue1-config-plan
	$(BUILD)/test-sdma6-queue1-memory-contract
	$(BUILD)/test-gfx11-mec-enable
	$(BUILD)/test-gfx11-kiq-hqd
	$(BUILD)/test-gfx11-mqd
	$(BUILD)/test-gfx11-mes
	$(BUILD)/test-gfx11-mes-mqd
	$(BUILD)/test-pci-owner
	$(BUILD)/test-production-owner
	$(BUILD)/test-dma-lifetime
	$(BUILD)/test-stage-owner
	$(BUILD)/test-gmc11-root-vram
	$(BUILD)/test-sdma6-queue-owner
	$(BUILD)/test-gmc11-vram-geometry
	$(BUILD)/test-vram-bo-allocator
	$(BUILD)/test-gmc11-vram-live
	$(BUILD)/test-gmc11-vram-lease
	$(BUILD)/test-nbio43-capacity
	$(BUILD)/test-gmc11-vram-upload
	$(BUILD)/test-gmc11-vram-upload-off
	$(BUILD)/test-rx7600-silicon-bases
	$(BUILD)/native-caps
	python3 tests/test_manifest.py

clean:
	rm -rf $(BUILD)
