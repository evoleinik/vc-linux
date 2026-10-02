#!/bin/sh
# ENiGMA abracadabra: DROP_FILE NODE {vc|rogue|hack}. The drop file is not opened.
set -eu

if [ "$#" -ne 3 ]; then
    printf '%s\n' 'Usage: run-door.sh DROP_FILE NODE {vc|rogue|hack}' >&2
    exit 2
fi
vc_door_node=$2
case "$vc_door_node" in
    ''|*[!0-9]*)
        printf '%s\n' 'VC door: node must contain only decimal digits.' >&2
        exit 2
        ;;
esac
if [ "${#vc_door_node}" -gt 9 ] || [ "$vc_door_node" -eq 0 ]; then
    printf '%s\n' 'VC door: node must be between 1 and 999999999.' >&2
    exit 2
fi
# Strip leading zeroes before shell arithmetic: 008 is decimal node 8, not octal.
while [ "${vc_door_node#0}" != "$vc_door_node" ]; do
    vc_door_node=${vc_door_node#0}
done
vc_door_id=$((200000 + vc_door_node))
case "$3" in
    vc) set -- ;;
    rogue) set -- --door-run ROGUE.EXE ;;
    hack) set -- --door-run HACK.EXE ;;
    *)
        printf '%s\n' 'VC door: choose vc, rogue, or hack.' >&2
        exit 2
        ;;
esac

umask 077
vc_door_root=/run/vc-doors
vc_door_node_root=$vc_door_root/node-$vc_door_node

vc_door_check_directory() {
    # Do not follow existing symlinks, repair unexpected owners, or touch any
    # contents. /run is trusted and root-owned; after the parent is installed
    # 0711, nodes cannot rename its children, so these checks are stable against
    # a caller swapping the checked entry before install/chown.
    if [ -L "$1" ] || { [ -e "$1" ] && [ ! -d "$1" ]; }; then
        printf 'VC door: unsafe directory: %s.\n' "$1" >&2
        exit 1
    fi
    if [ -d "$1" ] && [ "$(/usr/bin/stat -c '%u:%g' -- "$1")" != "$2" ]; then
        printf 'VC door: unexpected directory owner: %s.\n' "$1" >&2
        exit 1
    fi
}

vc_door_check_directory "$vc_door_root" 0:0
/usr/bin/install -d -o 0 -g 0 -m 0711 -- "$vc_door_root"
vc_door_check_directory "$vc_door_node_root" "$vc_door_id:$vc_door_id"
/usr/bin/install -d -o "$vc_door_id" -g "$vc_door_id" -m 0700 -- "$vc_door_node_root"

# No inherited editor, shell, modem endpoints, debug paths, preload variables,
# or configuration-home overrides reach the untrusted guest. Keep only the
# terminal label and the two door settings; node-pty already supplies its size.
exec /usr/bin/env -i \
    PATH=/usr/bin:/bin LC_ALL=C.UTF-8 TERM="${TERM:-ansi}" \
    VC_DOOR_ROOT="$vc_door_node_root" VC_DOOR_NODE="$vc_door_node" \
    /usr/bin/prlimit --cpu=3700:3700 --as=268435456:268435456 \
    --core=0:0 --fsize=16777216:16777216 -- \
    /usr/bin/setpriv --reuid="$vc_door_id" --regid="$vc_door_id" \
    --clear-groups --no-new-privs \
    /enigma-bbs/mods/vc-door/vc --door "$@"
