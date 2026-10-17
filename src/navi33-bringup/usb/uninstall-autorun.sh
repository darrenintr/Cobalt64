#!/bin/sh
# Remove the boot autorun:  sudo sh /Volumes/OPENCORE/navi33/uninstall-autorun.sh
# The log /var/log/cobalt64-navi33-autorun.log is kept.
[ "$(id -u)" = 0 ] || exec sudo /bin/sh "$0" "$@"
label=com.cobalt64.navi33-autorun
launchctl bootout "system/$label" 2>/dev/null || true
rm -f "/Library/LaunchDaemons/$label.plist" /var/db/cobalt64-navi33-autorun.boot
rm -rf /usr/local/libexec/cobalt64
echo "Boot autorun removed."
