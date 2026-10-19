// Copyright (c) 2026 Navi33 contributors. MIT.
// Only address arithmetic. Does not access a PCI device or GPU register.
// A UEFI PE_Video address must be confined to the card's assigned BAR0.
// This is a safety gate; passing it does not prove DCN is still scanning out.
#pragma once
#include "Navi33GopLayout.h"

namespace navi33fb {

enum class OwnershipError : uint8_t {
    Ok = 0, BadLayout, MissingBar, InvalidBar, OutsideBar
};

struct Ownership {
    OwnershipError status;
    uint64_t offset;
};

constexpr Ownership checkBar0(const Layout &layout, uint64_t barPhys, uint64_t barLength) {
    if (!valid(layout)) return {OwnershipError::BadLayout, 0};
    if (!barPhys || !barLength) return {OwnershipError::MissingBar, 0};
    if (barPhys > UINT64_MAX - barLength) return {OwnershipError::InvalidBar, 0};
    if (layout.physicalBase < barPhys) return {OwnershipError::OutsideBar, 0};
    const uint64_t offset = layout.physicalBase - barPhys;
    if (offset > barLength || layout.length > barLength - offset)
        return {OwnershipError::OutsideBar, 0};
    return {OwnershipError::Ok, offset};
}

constexpr bool ownedByBar0(const Ownership &ownership) {
    return ownership.status == OwnershipError::Ok;
}

// VM path (G37): the guest has no GOP, so the console is the one the host's
// firmware left in BAR0 (vm/prepare-card.sh records it in hostfb.txt). The
// result still has to pass checkBar0 against the guest's BAR0 resource.
constexpr Layout hostConsole(uint64_t barPhys, uint64_t barLength, uint64_t offset,
                             uint64_t bytes, uint32_t width, uint32_t height,
                             uint32_t rowBytes) {
    const bool bad = !barPhys || !barLength || (offset & 0xfffull) || !bytes ||
                     offset > barLength || bytes > barLength - offset ||
                     barPhys > UINT64_MAX - barLength;
    return bad ? Layout{LayoutError::NullFramebuffer, 0, 0, width, height, rowBytes, 32u, 32u}
               : inspectGop(barPhys + offset, bytes, width, height, rowBytes, 32u);
}
} // namespace navi33fb
