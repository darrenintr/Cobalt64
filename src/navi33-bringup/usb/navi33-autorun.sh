#!/bin/sh
# Boot autorun for the Navi33 diagnostics, installed root-owned into
# /usr/local/libexec/cobalt64 by install-autorun.sh and started through
# n33autorun by launchd at load and on every volume mount. It runs the
# stick's run.sh at most once per boot, and only from the volume whose
# identity was pinned at install time.
DIR=/usr/local/libexec/cobalt64
STAMP=/var/db/cobalt64-navi33-autorun.boot
. "$DIR/navi33-autorun.conf" || exit 1   # VOL, PINNED_ID

# Prints "VolumeUUID:<uuid>" or "DiskUUID:<uuid>" for a mounted volume.
vol_id() {
    info=$(diskutil info -plist "$1" 2>/dev/null) || return 1
    for key in VolumeUUID DiskUUID; do
        v=$(printf '%s' "$info" | plutil -extract "$key" raw -o - - 2>/dev/null)
        [ -n "$v" ] && { echo "$key:$v"; return 0; }
    done
    return 1
}
log() { echo "$(date '+%F %T') $*"; }

boot=$(sysctl -n kern.boottime)
[ -f "$STAMP" ] && [ "$(cat "$STAMP")" = "$boot" ] && exit 0
[ -d "$VOL" ] || exit 0   # not mounted yet; launchd retries on next mount
err=$(ls "$VOL/navi33" 2>&1 >/dev/null)
case $err in
    *"not permitted"*)
        log "BLOCKED: macOS privacy refused $VOL/navi33: $err"
        log "BLOCKED: grant Full Disk Access to $DIR/n33autorun (System Settings > Privacy & Security)"
        exit 0 ;;
    ?*) exit 0 ;;          # some other volume without a navi33 folder
esac
id=$(vol_id "$VOL")
if [ "$id" != "$PINNED_ID" ]; then
    log "SKIP: $VOL is $id, not the pinned $PINNED_ID"
    exit 0
fi
[ -f "$VOL/navi33/run.sh" ] || { log "SKIP: no $VOL/navi33/run.sh"; exit 0; }
echo "$boot" > "$STAMP"
log "START: $VOL/navi33/run.sh"
/bin/sh "$VOL/navi33/run.sh" boot
log "DONE: run.sh exit $?"
# Audible "safe to reboot" cue; harmless if no audio is available.
afplay /System/Library/Sounds/Glass.aiff >/dev/null 2>&1
exit 0
