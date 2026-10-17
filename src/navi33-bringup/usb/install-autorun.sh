#!/bin/sh
# One-time install of the boot autorun. On the Mac, from this stick:
#   sudo sh /Volumes/OPENCORE/navi33/install-autorun.sh
# Afterwards every macOS boot runs this folder's run.sh once, as root, as
# soon as this stick is mounted. Only this stick (its volume identity is
# pinned now) is trusted; anyone who can edit it can already change the
# kexts OpenCore injects from it. Remove with uninstall-autorun.sh.
set -eu
[ "$(id -u)" = 0 ] || exec sudo /bin/sh "$0" "$@"
here=$(cd "$(dirname "$0")" && pwd -P)
vol=$(dirname "$here")
dir=/usr/local/libexec/cobalt64
label=com.cobalt64.navi33-autorun
plist=/Library/LaunchDaemons/$label.plist
logfile=/var/log/cobalt64-navi33-autorun.log

info=$(diskutil info -plist "$vol")
id=
for key in VolumeUUID DiskUUID; do
    v=$(printf '%s' "$info" | plutil -extract "$key" raw -o - - 2>/dev/null || true)
    if [ -n "$v" ]; then id="$key:$v"; break; fi
done
[ -n "$id" ] || { echo "Cannot read a volume or partition UUID for $vol"; exit 1; }

install -d -o root -g wheel -m 755 "$dir"
install -o root -g wheel -m 755 "$here/n33autorun" "$dir/n33autorun"
install -o root -g wheel -m 755 "$here/navi33-autorun.sh" "$dir/navi33-autorun.sh"
printf "VOL='%s'\nPINNED_ID='%s'\n" "$vol" "$id" > "$dir/navi33-autorun.conf"
chown root:wheel "$dir/navi33-autorun.conf"
chmod 644 "$dir/navi33-autorun.conf"
install -o root -g wheel -m 644 "$here/$label.plist" "$plist"
echo "Installed for $vol ($id)."

# Prove it works now instead of after a reboot: RunAtLoad starts one run.
rm -f /var/db/cobalt64-navi33-autorun.boot
start=$(wc -l < "$logfile" 2>/dev/null || echo 0)
launchctl bootout "system/$label" 2>/dev/null || true
sleep 1   # bootstrap right after bootout can fail with an I/O error
launchctl bootstrap system "$plist"
echo "Test run started; waiting up to 3 minutes..."
i=0
while [ $i -lt 90 ]; do
    new=$(tail -n "+$((start + 1))" "$logfile" 2>/dev/null || true)
    case $new in
        *DONE:*|*BLOCKED:*|*SKIP:*) break ;;
    esac
    sleep 2; i=$((i + 1))
done
echo "$new"
case $new in
    *DONE:*) echo "OK: from now on, boot macOS, wait for the chime (or ~2 minutes), reboot." ;;
    *BLOCKED:*) echo "Grant Full Disk Access to $dir/n33autorun, then run this installer again." ;;
    *) echo "No result yet; check $logfile" ;;
esac
