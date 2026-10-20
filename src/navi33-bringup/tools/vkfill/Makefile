# vkfill: SPIR-V on Linux (glslc), the program in the macOS guest (clang, Vulkan headers from Mesa).
vkfill_spv.h: fill.comp
	glslc -fshader-stage=compute --target-env=vulkan1.0 -O fill.comp -o fill.spv
	{ echo '// generated from fill.comp by glslc (make vkfill_spv.h)'; echo 'static const uint32_t fill_spv[] = {'; \
	  od -An -v -t x4 -w16 fill.spv | sed 's/ \([0-9a-f]\{8\}\)/0x\1u,/g'; echo '};'; } > $@
	rm -f fill.spv
