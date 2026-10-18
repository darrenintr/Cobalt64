CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Werror -pedantic -O2
BUILD := build
TEST := $(BUILD)/test_n33_accel_readiness
CLANG ?= clang++
C64_MKSDK ?=
N33_EVIDENCE_DIR ?=
PRIVATE_BUILD_DIR ?= /tmp/cobalt64-navi33accel-private
PRIVATE_HEADER := $(PRIVATE_BUILD_DIR)/IOAccelFamily2_decl.h
PRIVATE_OBJECT := $(PRIVATE_BUILD_DIR)/Navi33AccelServiceSkeleton.o

$(TEST): test_n33_accel_readiness.cpp n33_accel_readiness.h ../../../src/navi33-bringup/src/Navi33NativeABI.h
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) $< -o $@

.PHONY: test private-abi-check clean
test: $(TEST)
	$(TEST)

# Opt-in compile-only check. Requires the target KC analysis output and the
# workflow-fetched MacKernelSDK. The Apple-derived header/object stay under
# PRIVATE_BUILD_DIR, outside the repository. This does not link, bundle,
# register, install, or load the service.
private-abi-check:
	@test -n "$(N33_EVIDENCE_DIR)" || { echo "set N33_EVIDENCE_DIR to the local KC evidence directory" >&2; exit 2; }
	@test -n "$(C64_MKSDK)" || { echo "set C64_MKSDK to a MacKernelSDK checkout" >&2; exit 2; }
	@mkdir -p "$(PRIVATE_BUILD_DIR)"
	python3 prepare_private_abi_header.py "$(N33_EVIDENCE_DIR)" "$(PRIVATE_BUILD_DIR)"
	$(CLANG) -target x86_64-apple-macos11.0 -isysroot / -I"$(C64_MKSDK)/Headers" \
		-mmacosx-version-min=11.0 -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE \
		-DAPPLE -DNeXT -D_FORTIFY_SOURCE=0 -nostdinc -fno-builtin -fno-common \
		-fno-stack-protector -mkernel -fapple-kext -Os -std=c++17 \
		-fno-exceptions -fno-rtti -fcheck-new -fno-c++-static-destructors \
		-Wno-inconsistent-missing-override \
		-DN33_ACCEL_PRIVATE_LAYOUT_HEADER='"$(PRIVATE_HEADER)"' \
		-c Navi33AccelServiceSkeleton.cpp -o "$(PRIVATE_OBJECT)"
	@echo "PASS compile-only Navi33 IOGraphicsAccelerator2 fail-closed skeleton: $(PRIVATE_OBJECT)"

clean:
	rm -rf $(BUILD)
