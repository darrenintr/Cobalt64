#!/bin/sh
# Cobalt64 Navi33 read-only diagnostics: every check in one pass.
#   Manual:  sh /Volumes/OPENCORE/navi33/run.sh       (asks for sudo)
#   At boot: the com.cobalt64.navi33-autorun LaunchDaemon (install-autorun.sh)
# Results are written next to this script so they can be read from Linux.
# Nothing here writes to the GPU: every tool is a read-only N33N client.
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
    step "n33vram (framebuffer geometry)"; run ./n33vram
    step "n33peek --all (allowlisted BAR5 register windows)"
    run ./n33peek --rom rx7600-ipd.bin --all
    step ioreg; ioreg -l -w0 -r -c Navi33Probe
    step "kernel log, this boot"
    log show --last boot --style compact \
        --predicate 'eventMessage CONTAINS "Navi33"' 2>&1 | tail -80
    step done
} > "$out" 2>&1
sync
[ "$trigger" = manual ] && cat "$out"
echo "Saved to $(pwd)/$out"
