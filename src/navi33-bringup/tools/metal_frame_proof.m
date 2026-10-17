// SPDX-License-Identifier: MIT
// Mac-only acceptance test: a real Metal frame on the RX7600 or failure.
// This test NEVER falls back to a software framebuffer or another GPU.
// It is compile-verified in CI but requires future physical Navi33/Metal.
// Build: clang -fobjc-arc -O2 -Wall -Wextra -Werror -std=gnu11
//        -framework Foundation -framework Metal -framework IOKit
//        metal_frame_proof.m -o metal_frame_proof
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../src/Navi33NativeABI.h"

static BOOL nativeNavi33Capabilities(struct n33_capabilities *out) {
    if (!out) return NO;
    memset(out,0,sizeof(*out));
    io_service_t service=IOServiceGetMatchingService(
        kIOMainPortDefault,IOServiceMatching("Navi33Probe"));
    if (!service) {
        fprintf(stderr,"No Navi33Probe IORegistry service.\n");
        return NO;
    }
    io_connect_t conn=IO_OBJECT_NULL;
    kern_return_t ret=IOServiceOpen(service,mach_task_self(),N33_UC_TYPE,&conn);
    IOObjectRelease(service);
    if(ret!=KERN_SUCCESS) {
        fprintf(stderr,"Cannot open root-gated N33N: 0x%x\n",ret);
        return NO;
    }
    uint64_t helloInput[2]={N33_ABI_VERSION,0u};
    uint64_t helloOut[3]={0u,0u,0u};
    uint32_t helloCount=3u;
    ret=IOConnectCallScalarMethod(conn,N33_SEL_HELLO,helloInput,2u,
                                  helloOut,&helloCount);
    if(ret!=KERN_SUCCESS || helloCount!=3u ||
       helloOut[0]!=N33_ABI_VERSION || helloOut[1]!=N33_UC_TYPE) {
        fprintf(stderr,"N33N ABI handshake failed\n");
        IOServiceClose(conn);
        return NO;
    }
    size_t sz=sizeof(*out);
    ret=IOConnectCallStructMethod(conn,N33_SEL_QUERY_CAPS,NULL,0u,out,&sz);
    IOServiceClose(conn);
    if(ret!=KERN_SUCCESS || sz!=sizeof(*out) ||
       !n33_is_reserved_clean(out) ||
       out->vendor_id!=N33_PCI_VENDOR ||
       out->device_id!=N33_PCI_DEVICE ||
       out->family!=N33_FAMILY_GFX1102 ||
       out->gfx_ip_version!=N33_GFX_IP_VERSION) {
        fprintf(stderr,"No verified gfx1102 query result\n");
        return NO;
    }
    const uint32_t required=N33_CAP_HW_VM|N33_CAP_HW_GFX|N33_CAP_METAL;
    if((out->capabilities & required)!=required) {
        fprintf(stderr,"RX7600 GPU/Metal hardware flags are NOT proven (caps 0x%08x)\n",
                out->capabilities);
        return NO;
    }
    return YES;
}

int main(void) {
    @autoreleasepool {
        struct n33_capabilities caps={0};
        if(!nativeNavi33Capabilities(&caps)) return 2;
        NSArray<id<MTLDevice>> *devices=MTLCopyAllDevices();
        id<MTLDevice> dev=nil;
        for(id<MTLDevice> d in devices) {
            NSString *name=d.name;
            if ([name rangeOfString:@"RX 7600" options:NSCaseInsensitiveSearch].location!=NSNotFound ||
                [name rangeOfString:@"Navi33" options:NSCaseInsensitiveSearch].location!=NSNotFound) {
                dev=d;break;
            }
        }
        if (!dev) {
            fprintf(stderr,"No matching Navi33/RX7600 Metal device; refusing other GPUs\n");
            return 3;
        }
        NSError *error=nil;
        NSString *shader=@"#include <metal_stdlib>\n"
            @"using namespace metal;\n"
            @"struct V { float4 position [[position]]; };\n"
            @"vertex V vmain(uint id [[vertex_id]]) {\n"
            @"  float2 p[3]={float2(-0.8,-0.8),float2(0.8,-0.8),float2(0.0,0.8)};\n"
            @"  V v;v.position=float4(p[id],0.0,1.0);return v;\n"
            @"}\n"
            @"fragment float4 fmain(){return float4(0.15,0.85,0.30,1.0);}\n";
        id<MTLLibrary> lib=[dev newLibraryWithSource:shader options:nil error:&error];
        if(!lib) {
            fprintf(stderr,"Metal shader compilation failed: %s\n",
                    error.localizedDescription.UTF8String);return 4;
        }
        id<MTLFunction> vf=[lib newFunctionWithName:@"vmain"];
        id<MTLFunction> ff=[lib newFunctionWithName:@"fmain"];
        if(!vf || !ff) return 5;
        MTLRenderPipelineDescriptor *pd=[[MTLRenderPipelineDescriptor alloc] init];
        pd.vertexFunction=vf;pd.fragmentFunction=ff;
        pd.colorAttachments[0].pixelFormat=MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> pipeline=[dev newRenderPipelineStateWithDescriptor:pd
                                                                               error:&error];
        if(!pipeline) {
            fprintf(stderr,"Pipeline creation failed: %s\n",
                    error.localizedDescription.UTF8String);return 6;
        }
        const NSUInteger width=64u,height=64u,bpr=width*4u;
        MTLTextureDescriptor *td=[MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
            width:width height:height mipmapped:NO];
        td.usage=MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode=MTLStorageModePrivate;
        id<MTLTexture> texture=[dev newTextureWithDescriptor:td];
        id<MTLBuffer> readback=[dev newBufferWithLength:bpr*height
            options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> queue=[dev newCommandQueue];
        if(!texture || !readback || !queue) return 7;
        MTLRenderPassDescriptor *rp=[MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture=texture;
        rp.colorAttachments[0].loadAction=MTLLoadActionClear;
        rp.colorAttachments[0].storeAction=MTLStoreActionStore;
        rp.colorAttachments[0].clearColor=MTLClearColorMake(0,0,0,1);
        id<MTLCommandBuffer> command=[queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:rp];
        if(!encoder) return 8;
        [encoder setRenderPipelineState:pipeline];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        id<MTLBlitCommandEncoder> blit=[command blitCommandEncoder];
        if(!blit)return 9;
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0
                sourceOrigin:MTLOriginMake(0,0,0)
                  sourceSize:MTLSizeMake(width,height,1)
                    toBuffer:readback destinationOffset:0
         destinationBytesPerRow:bpr destinationBytesPerImage:bpr*height];
        [blit endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if(command.status!=MTLCommandBufferStatusCompleted) {
            fprintf(stderr,"GPU submission did not complete: %s\n",
                    command.error.localizedDescription.UTF8String);return 10;
        }
        const uint8_t *pixels=(const uint8_t*)readback.contents;
        if(!pixels)return 11;
        const NSUInteger center=(32u*bpr+32u*4u);
        const NSUInteger outside=(1u*bpr+1u*4u);
        if(pixels[center+1u]<150u || pixels[center+3u]<245u ||
           pixels[outside+0u]>5u || pixels[outside+1u]>5u ||
           pixels[outside+2u]>5u) {
            fprintf(stderr,"Render readback pixels wrong (center G=%u)\n",
                    pixels[center+1u]);return 12;
        }
        uint64_t hash=1469598103934665603ull;
        for(NSUInteger i=0;i<bpr*height;++i) {
            hash=(hash ^ pixels[i])*1099511628211ull;
        }
        printf("RX7600 Metal triangle GPU readback PASS: device=%s hash=%016llx GPUtime=%lf\n",
               dev.name.UTF8String,(unsigned long long)hash,
               command.GPUEndTime-command.GPUStartTime);
        return 0;
    }
}
