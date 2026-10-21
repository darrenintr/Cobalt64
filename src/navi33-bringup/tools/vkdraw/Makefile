# Headers are the only build dependency; the ICD is loaded explicitly at runtime.
CC ?= cc
VK_INCLUDE ?= /usr/include
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Werror
LDLIBS ?= -ldl

all: vkdraw
vkdraw: vkdraw.c draw_spv.h
	$(CC) $(CFLAGS) -I"$(VK_INCLUDE)" vkdraw.c $(LDLIBS) -o $@

shaders:
	glslc --target-env=vulkan1.0 -O draw.vert -o draw.vert.spv
	glslc --target-env=vulkan1.0 -O draw.frag -o draw.frag.spv
	python3 embed.py
	rm -f draw.vert.spv draw.frag.spv

clean:
	rm -f vkdraw draw.vert.spv draw.frag.spv
.PHONY: all shaders clean
