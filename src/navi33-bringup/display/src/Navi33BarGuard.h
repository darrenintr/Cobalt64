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
} // namespace navi33fb
