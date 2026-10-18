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
#include "../src/Navi33Gfx11Formats.h"

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
    return YES;
}

static int enumerateMetalDevices(void) {
    NSArray<id<MTLDevice>> *devices=MTLCopyAllDevices();
    printf("Metal device enumeration: %lu device(s)\n",
           (unsigned long)devices.count);
    NSUInteger index=0;
    for(id<MTLDevice> device in devices) {
        printf("Metal device[%lu]: name=%s registryID=0x%016llx lowPower=%u headless=%u\n",
               (unsigned long)index++,device.name.UTF8String,
               (unsigned long long)device.registryID,
               device.isLowPower ? 1u : 0u,device.isHeadless ? 1u : 0u);
    }
    // Enumeration is a diagnostic only. Do not create a queue, compile a
    // device pipeline, allocate a texture, or submit any GPU work here.
    return 0;
}

static void printRegistryProperty(io_service_t service,const char *label,
                                  CFStringRef key) {
    CFTypeRef value=IORegistryEntryCreateCFProperty(
        service,key,kCFAllocatorDefault,0);
    if(!value) {
        printf(" %s=<absent>",label);
        return;
    }
    CFStringRef description=CFCopyDescription(value);
    char text[256]="<unprintable>";
    if(description) {
        (void)CFStringGetCString(description,text,sizeof(text),
                                 kCFStringEncodingUTF8);
        CFRelease(description);
    }
    printf(" %s=%s",label,text);
    CFRelease(value);
}

static int enumerateAcceleratorServices(void) {
    io_iterator_t iterator=IO_OBJECT_NULL;
    kern_return_t ret=IOServiceGetMatchingServices(
        kIOMainPortDefault,IOServiceMatching("IOAccelerator"),&iterator);
    if(ret!=KERN_SUCCESS) {
        fprintf(stderr,"IOAccelerator service census failed: 0x%x\n",ret);
        return 1;
    }
    NSUInteger count=0;
    io_service_t service=IO_OBJECT_NULL;
    while((service=IOIteratorNext(iterator))!=IO_OBJECT_NULL) {
        char name[128]="?",path[512]="?";
        (void)IORegistryEntryGetName(service,name);
        (void)IORegistryEntryGetPath(service,kIOServicePlane,path);
        printf("IOAccelerator[%lu]: name=%s path=%s",
               (unsigned long)count++,name,path);
        printRegistryProperty(service,"MetalPluginName",CFSTR("MetalPluginName"));
        printRegistryProperty(service,"MetalPluginClassName",CFSTR("MetalPluginClassName"));
        printRegistryProperty(service,"IOClass",CFSTR("IOClass"));
        putchar('\n');
        IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    printf("IOAccelerator service enumeration: %lu service(s)\n",
           (unsigned long)count);
    return 0;
}

int main(int argc,const char *argv[]) {
    @autoreleasepool {
        BOOL enumerateOnly=NO;
        if(argc==2 && strcmp(argv[1],"--enumerate-only")==0) {
            enumerateOnly=YES;
        } else if(argc!=1) {
            fprintf(stderr,"usage: metal_frame_proof [--enumerate-only]\n");
            return 64;
        }
        if(enumerateOnly) {
            struct n33_capabilities diagnosticCaps={0};
            if(nativeNavi33Capabilities(&diagnosticCaps))
                printf("N33N RX7600 caps: 0x%08x\n",diagnosticCaps.capabilities);
            else
                fprintf(stderr,"N33N caps unavailable; continuing system Metal enumeration.\n");
            const int acceleratorStatus=enumerateAcceleratorServices();
            const int metalStatus=enumerateMetalDevices();
            return acceleratorStatus ? acceleratorStatus : metalStatus;
        }
        struct n33_capabilities caps={0};
        if(!nativeNavi33Capabilities(&caps)) return 2;
        printf("N33N RX7600 caps: 0x%08x\n",caps.capabilities);
        const uint32_t required=N33_CAP_HW_VM|N33_CAP_HW_GFX|N33_CAP_METAL;
        if((caps.capabilities & required)!=required) {
            fprintf(stderr,"RX7600 GPU/Metal hardware flags are NOT proven (caps 0x%08x)\n",
                    caps.capabilities);
            return 2;
        }
        const Navi33Gfx11RenderTargetFormat *outputFormat=
            navi33Gfx11RenderTargetFormatForMetal(
                (uint32_t)MTLPixelFormatBGRA8Unorm);
        if(!outputFormat ||
           outputFormat->metalPixelFormat!=N33_MTL_BGRA8_UNORM ||
           outputFormat->vkFormat!=N33_VK_B8G8R8A8_UNORM ||
           outputFormat->cbFormat!=N33_GFX11_CB_FMT_8_8_8_8 ||
           outputFormat->cbNumberType!=N33_GFX11_CB_NUMBER_UNORM ||
           outputFormat->cbSwap!=N33_GFX11_CB_SWAP_ALT ||
           outputFormat->imageFormat!=N33_GFX11_IMG_FMT_8_8_8_8_UNORM ||
           outputFormat->bytesPerTexel!=4u ||
           outputFormat->imageDstSelect[0]!=N33_GFX11_SEL_Z ||
           outputFormat->imageDstSelect[1]!=N33_GFX11_SEL_Y ||
           outputFormat->imageDstSelect[2]!=N33_GFX11_SEL_X ||
           outputFormat->imageDstSelect[3]!=N33_GFX11_SEL_W) {
            fprintf(stderr,"Navi33 BGRA8Unorm GFX11 output mapping is incomplete\n");
            return 13;
        }
        printf("Navi33 output mapping: Metal BGRA8Unorm -> Vulkan B8G8R8A8_UNORM, GFX11 SWAP_ALT\n");
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
            @"fragment float4 fmain(){return float4(0.10,0.50,0.90,1.0);}\n";
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
        // BGRA8Unorm readback stores B,G,R,A bytes. Use asymmetric RGB
        // values so an accidental RGBA/BGRA swap cannot pass on green alone.
        if(pixels[center+0u]<210u ||
           pixels[center+1u]<100u || pixels[center+1u]>155u ||
           pixels[center+2u]>50u || pixels[center+3u]<245u ||
           pixels[outside+0u]>5u || pixels[outside+1u]>5u ||
           pixels[outside+2u]>5u) {
            fprintf(stderr,"Render readback pixels wrong (BGRA=%u,%u,%u,%u)\n",
                    (unsigned)pixels[center+0u],
                    (unsigned)pixels[center+1u],
                    (unsigned)pixels[center+2u],
                    (unsigned)pixels[center+3u]);return 12;
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
