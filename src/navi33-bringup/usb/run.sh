#!/bin/sh
# Cobalt64 Navi33 read-only diagnostics: every check in one pass.
#   Manual:  sh /Volumes/OPENCORE/navi33/run.sh       (asks for sudo)
#   At boot: the com.cobalt64.navi33-autorun LaunchDaemon (install-autorun.sh)
# Results are written next to this script so they can be read from Linux.
# Nothing here writes to the GPU: every call is a read-only N33N request.
# n33psp runs in plan mode only; the PSP load is started by hand over SSH.
[ "$(id -u)" = 0 ] || exec sudo /bin/sh "$0" "$@"
cd "$(dirname "$0")" || exit 1
trigger=${1:-manual}
out=results-$(date +%Y%m%d-%H%M%S)-$trigger.txt
step() { echo; echo "== $*"; }
run() { "$@"; echo "exit=$?"; }
{
    echo "== trigger: $trigger, $(date)"
    echo "boot: $(sysctl -n kern.boottime)"
    step sw_vers; sw_vers
    step boot-args; nvram boot-args
    step "loaded kext"
    kmutil showloaded 2>/dev/null | grep -i navi33 || echo "Navi33Probe NOT loaded"
    step "n33query (HELLO + capabilities)"; run ./n33query
    step "n33compute --rom (live preflight, supplied IP Discovery)"
    run ./n33compute --rom rx7600-ipd.bin
    step "n33compute --probe (preflight from ATY,bin_image)"; run ./n33compute --probe
    step "n33compute --queue (SDMA6 Queue0 census)"; run ./n33compute --queue
    step "n33vram (framebuffer geometry)"; run ./n33vram --read
    step "n33peek --all (allowlisted BAR5 register windows)"
    run ./n33peek --rom rx7600-ipd.bin --all
    step "n33psp --plan (PSP13 sOS load plan, reads only)"
    if [ -f psp_13_0_7_sos.bin ]; then
        run ./n33psp --plan --rom rx7600-ipd.bin --fw psp_13_0_7_sos.bin
    else
        echo "psp_13_0_7_sos.bin is not on the stick; plan skipped"
    fi
    step ioreg; ioreg -l -w0 -r -c Navi33Probe
    # Owner probe and DMA preflight verdicts survive as ioreg properties even
    # when the kernel message buffer has rolled past the boot IOLog lines.
    step "owner probe and DMA preflight (ioreg properties)"
    ioreg -l -w0 -r -c Navi33Probe | grep -E '"Navi33,(Owner|DmaPreflight)' ||
        echo "no owner/DMA properties (gates off?)"
    step "IOMMU and DMA mappers (read-only)"
    echo "IOMapper instances: $(ioreg -w0 -r -c IOMapper | grep -c '+-o')"
    ioreg -w0 -r -c IOMapper | head -20
    echo "AppleVTD instances: $(ioreg -w0 -r -c AppleVTD | grep -c '+-o')"
    ioreg -l -w0 -r -c AppleACPIPlatformExpert |
        grep -o '"ACPI Tables" = {[^}]*}' | tr ',' '\n' | cut -d= -f1 | head -60
    step "display stack on the card (read-only)"
    echo "IOFramebuffer instances: $(ioreg -w0 -r -c IOFramebuffer | grep -c '+-o')"
    ioreg -w0 -r -c IOFramebuffer | head -20
    echo "IOAccelerator instances: $(ioreg -w0 -r -c IOAccelerator | grep -c '+-o')"
    ioreg -w0 -r -c IOAccelerator | head -20
    system_profiler SPDisplaysDataType 2>/dev/null | head -60
    step "kernel log, this boot"
    # IOLog lines reach dmesg, not this log query; the stick sets msgbuf= so
    # the boot-time lines are still in the buffer when this runs.
    dmesg | grep -i navi33 | tail -200
    log show --last boot --style compact \
        --predicate 'eventMessage CONTAINS[c] "navi33"' 2>&1 | tail -200
    step done
} > "$out" 2>&1
sync
[ "$trigger" = manual ] && cat "$out"
echo "Saved to $(pwd)/$out"
