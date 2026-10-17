// Strict, CPU-only EDID validator for optional OpenCore DeviceProperties.
// EDID is injected by the bootloader; no I2C/DDC/DP AUX or GPU MMIO access.
// Do not invent a monitor identity: reject bad checksums or malformed sizes.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace navi33fb { namespace edid {
constexpr size_t kBlockBytes=128u;
constexpr size_t kMaxBlocks=2u;
enum class Status : uint8_t {
    Ok=0, Missing, UnsupportedLength, BadHeader, BadChecksum,
    WrongExtensionCount, WrongVersion
};
struct Summary {
    Status code=Status::Missing;
    uint8_t blocks=0u;
    uint16_t manufacturer=0u, product=0u;
    bool valid() const {return code==Status::Ok;}
};
inline Summary inspect(const uint8_t *bytes,size_t length){
    if(!bytes)return {};
    if(length!=128u && length!=256u)
        return {Status::UnsupportedLength,0u,0u,0u};
    static const uint8_t magic[8]={0x00u,0xffu,0xffu,0xffu,
                                   0xffu,0xffu,0xffu,0x00u};
    for(size_t i=0;i<8u;++i)
        if(bytes[i]!=magic[i])return {Status::BadHeader,0u,0u,0u};
    if(bytes[18]!=1u || bytes[19]>5u)
        return {Status::WrongVersion,0u,0u,0u};
    if(length==256u && bytes[126]==0u)
        return {Status::WrongExtensionCount,0u,0u,0u};
    for(size_t block=0;block<length/kBlockBytes;++block){
        uint8_t checksum=0u;
        for(size_t i=block*kBlockBytes;i<(block+1u)*kBlockBytes;++i)
            checksum=static_cast<uint8_t>(checksum+bytes[i]);
        if(checksum!=0u)
            return {Status::BadChecksum,0u,0u,0u};
    }
    const uint16_t vendor=(static_cast<uint16_t>(bytes[8])<<8u)|bytes[9];
    const uint16_t product=static_cast<uint16_t>(bytes[10])|
                           (static_cast<uint16_t>(bytes[11])<<8u);
    return {Status::Ok,static_cast<uint8_t>(length/kBlockBytes),
            vendor,product};
}
} } // namespace navi33fb::edid
