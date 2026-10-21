#!/bin/sh
# Explicit GPU submissions; never invoked by the read-only run.sh/boot autorun.
# The matching kext must already have autostart/client-gpu/gfx-map gates enabled.
set -eu
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ] || { [ "$#" -eq 2 ] && [ "$2" != --metal ]; }; then
    echo "usage: $0 /absolute/path/to/libvulkan_radeon.dylib [--metal]" >&2
    exit 64
fi
case "$1" in /*) ;; *) echo "RADV library path must be absolute" >&2; exit 64;; esac
[ -f "$1" ] || { echo "RADV library not found: $1" >&2; exit 1; }
[ "$(uname -s)" = Darwin ] || { echo "Run this on the test Mac" >&2; exit 1; }
[ "$(id -u)" = 0 ] || exec sudo /bin/sh "$0" "$@"
library=$1
metal=${2:-}
cd "$(dirname "$0")"
for tool in n33gpu n33n48n n33-vkfill n33-vkdraw n33-metal-frame-proof; do
    [ -x "./$tool" ] || { echo "Missing executable: $tool" >&2; exit 1; }
done
[ -f gradient.co ] || { echo "Missing gradient.co" >&2; exit 1; }
stamp=$(date +%Y%m%d-%H%M%S)
out="render-$stamp.txt"
snapshot="render-$stamp.ioreg.txt"

capture() { ioreg -l -w0 -r -c Navi33Probe > "$snapshot"; cat "$snapshot"; }
check_core() {
    grep -Eq 'Navi33,ClientGfx.*state 7 ' "$snapshot" || {
        echo "STOP: graphics ring fence has not completed"; return 1;
    }
    grep -Eq 'Navi33,ClientGpu.*misses 0' "$snapshot" || {
        echo "STOP: clean TLB evidence is absent"; return 1;
    }
}
run_checks() {
    echo "Navi33 rendering attempt: $(date)"
    sysctl kern.boottime
    sw_vers
    kmutil showloaded 2>/dev/null | grep -i navi33 || return 1
    shasum -a 256 "$library" ./n33-vkdraw ./n33-metal-frame-proof
    capture
    grep -Eq 'Navi33,Autostart.*DONE' "$snapshot" || {
        echo "STOP: autostart has not completed"; return 1;
    }
    # First client triggers G40/G42 once. Never loop or retry a failed startup.
    ./n33gpu --nop || { capture; return 1; }
    capture
    check_core || return 1
    ./n33gpu --fw gradient.co --frame 200 --repeat 200 || return 1
    ./n33n48n --fw gradient.co || return 1
    ./n33-vkfill "$library" 65536 || return 1
    ./n33-vkdraw "$library" || return 1
    capture
    check_core || return 1
    ./n33-metal-frame-proof --enumerate-only || return 1
    if [ "$metal" = --metal ]; then
        ./n33-metal-frame-proof --experimental-g41 || return 1
        echo "Metal offscreen readback completed; desktop presentation still needs separate evidence."
    fi
    echo "== rendering checks complete (not desktop certification)"
}
# Avoid a pipeline so failures retain their exit status on macOS /bin/sh.
status=0
run_checks > "$out" 2>&1 || status=$?
{
    echo "== final registry, exit=$status"
    capture
    echo "== done"
} >> "$out" 2>&1
sync
cat "$out"
echo "Saved to $(pwd)/$out"
exit "$status"
