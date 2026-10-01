# Volkov Commander and Rogue doors on axis

These are files for the sysop to apply, not an automated deployment. No BBS,
network service, live menu, or container was changed while preparing them.
ENiGMA's node-pty supplies the caller's terminal; output stays UTF-8 for ENiGMA
to convert to the caller's code page. The caller needs at least 80 columns and
25 rows. Each visit has a new, temporary H: and a default 60-minute limit.

## Build and install

Build locally with the pinned offline toolchains:

```sh
export WATCOM=$PWD/build/openwatcom
make build/vc-door-aarch64
file build/vc-door-aarch64
```

The result must say `ARM aarch64` and `statically linked`. Zig defaults to
`build/zig/zig` (0.16.0); `ZIG=/path/to/zig` overrides it. Tagged releases also
attach the same build as `vc-door-linux-aarch64`. The normal `build/vc` includes
the same `--door` switches for x86-64 testing; there is no separate native fork.

Copy the binary and `run-door.sh` to axis yourself. The commands below assume
the copied binary is named `vc-door-linux-aarch64`, whether built locally or
downloaded from a release. In the existing root-running Debian 12 ENiGMA
container, first verify `/usr/bin/setpriv`, `/usr/bin/prlimit`, `/usr/bin/env`,
`/usr/bin/stat`, and `/usr/bin/install` exist (util-linux and coreutils).
Install into the persistent
`mods` volume rather than the container's disposable root filesystem:

```sh
sudo k3s kubectl -n bbs get pods -l app=enigma
sudo k3s kubectl -n bbs exec deploy/enigma -- \
  install -d -o 0 -g 0 -m 0755 /enigma-bbs/mods/vc-door
# Replace POD with the exact pod name printed above. Paths on the left are
# the files you copied to axis; kubectl cp requires tar in the container.
sudo k3s kubectl -n bbs cp vc-door-linux-aarch64 POD:/enigma-bbs/mods/vc-door/vc
sudo k3s kubectl -n bbs cp run-door.sh POD:/enigma-bbs/mods/vc-door/run-door.sh
sudo k3s kubectl -n bbs exec deploy/enigma -- \
  chown 0:0 /enigma-bbs/mods/vc-door/vc /enigma-bbs/mods/vc-door/run-door.sh
sudo k3s kubectl -n bbs exec deploy/enigma -- \
  chmod 0755 /enigma-bbs/mods/vc-door/vc /enigma-bbs/mods/vc-door/run-door.sh
```

The parent directories, binary, and wrapper must be root-owned and not writable
by door users. Reserve numeric UIDs and GIDs `200000 + node number` exclusively
for doors; no passwd entries are required. The wrapper accepts decimal node
numbers 1–999999999, strips leading zeroes, and never uses `nobody` (65534).
For example, node 42 runs as UID/GID 200042 with supplementary groups cleared
and `no-new-privs` set. `/run` must remain a trusted root-owned parent. The
wrapper installs `/run/vc-doors` as root:root 0711, then
`/run/vc-doors/node-42` as 200042:200042 0700. It refuses symlinks, non-directory
paths, and existing directories with unexpected owners; it never recursively
changes ownership. When migrating the older nobody-owned parent, stop old
door sessions and replace that storage with the new emptyDir before launching
this wrapper. Do not chown a live caller-controlled tree recursively.

Inherited environment overrides are cleared. `VC_DOOR_ROOT` is the private
per-node directory and the normalized decimal `VC_DOOR_NODE` names each random
session beneath it. Separate nodes cannot enter each other's directories;
successive visits on the same node reuse its numeric identity. The H: limits
are 8 MiB and 4096 entries per session, including the seeded files and
directories. The 8 MiB counts each file's size rounded up to whole 4 KiB pages,
because tmpfs gives every non-empty file at least one page. So 4096 one-byte
files cannot use 16 MiB of memory: their writes are disk full once 8 MiB of
pages are charged.

A detached janitor process deletes the session and enforces the time limit
and carrier loss. It is forked before confinement, so the caller cannot reach
it: seccomp stops the worker aiming file signals elsewhere (`F_SETOWN`) and
signalling any other PID, and on Landlock ABI 6 (axis's 6.14) signal scoping
blocks both as well. If the worker closes the janitor's lifetime pipe and
keeps running, the janitor ends it (SIGTERM, then SIGKILL) and then cleans up.
The janitor outlives every worker, so it is never the worker's to reap: the
manifest's `shareProcessNamespace: true` makes the pod's pause process PID 1,
and pause reaps it. Without that, ENiGMA's PID 1 (`pm2-runtime`) would keep
one zombie per finished session. The empty per-node parent remains for later
callers. A SIGKILL of the janitor itself, kernel failure, or container OOM
can interrupt cleanup: discarded sessions then remain in the bounded tmpfs
until a sysop removes them or the pod is replaced. Nothing in this wrapper
prunes other sessions.

## Isolation and resource budget

The runtime requires Landlock and seccomp before running guest code. Landlock
allows reading, writing, creating, renaming and deleting only beneath the
session. Nothing else on the filesystem is reachable, not even the door's own
binary: the static binary is already mapped and never execs, so it needs no
rule. Execute, device, socket, FIFO and symlink creation stay denied inside
the session too. On axis's Linux 6.14 kernel, Landlock's ABI 4 network rules
deny TCP bind and connect as well. Seccomp is an allow-list built from plain
BPF, without libseccomp. Any other system call kills the process, including
exec, sockets, ptrace, mount, fork, namespaces and executable memory.
Door mode refuses to start if confinement is unavailable; the explicit
`--door-allow-unconfined` escape hatch is for isolated tests only and is never
passed by the production wrapper. The door cannot invoke host programs,
mount C:, or dial out.

`prlimit` sets both the soft and hard limits before dropping privileges:

- CPU time: 3700 seconds, slightly beyond the default 60-minute wall-clock
  session timeout; a CPU-bound or broken guest still has a kernel-enforced cap.
- Address space: 256 MiB per process, bounding virtual allocations independently
  of the pod's combined physical-memory limit.
- Core size: zero, preventing memory dumps of guest or host process state.
- File size: 16 MiB per file, a kernel backstop above the 8 MiB DOS session quota.

No process-count limit is set. These per-door limits complement the ENiGMA
container resources in `../enigma.yaml`: requests of 250 millicores and 256 MiB
reserve scheduling capacity for the BBS; the CPU limit of 2 throttles all BBS
and door processes together, and the 1 GiB memory limit caps their combined
memory use. Exceeding that memory limit can cause an OOM kill. Adjust caller
concurrency against this shared budget after observing real usage.

The same manifest mounts `/run/vc-doors` as an `emptyDir` with `medium: Memory`
and `sizeLimit: 128Mi`. It keeps anonymous caller files off persistent host
disks and bounds aggregate scratch storage across nodes, including sessions
whose cleanup was interrupted. The sizing:

- One session holds at most 8 MiB of tmpfs pages. Every file, seeded or
  created, open or deleted-but-open, is charged in whole 4 KiB pages.
  Directories and inodes take no tmpfs pages.
- The two doors allow `nodeMax: 4` each, so at most 8 sessions run at once:
  8 × 8 MiB = 64 MiB.
- 128 MiB therefore holds every live session plus 64 MiB, room for 8 more
  sessions whose cleanup was interrupted, before writes fail.
- This assumes tmpfs without huge pages, the kernel default. If the node
  enables shmem huge pages, one file could take 2 MiB.
- If you raise `nodeMax`, keep the total of both doors' `nodeMax` × 8 MiB
  at or below half the `sizeLimit`. `tests/test_door_packaging.py` checks it.

Tmpfs pages count toward the container memory budget; its 128 MiB maximum is
not extra memory beyond the 1 GiB limit. Each session's up to 4096 inodes
also cost kernel memory (roughly 1 KiB each) inside that limit. Data
survives a container restart within the same pod but is discarded with pod
replacement. The sysop applies `infra/bbs/enigma.yaml`; this work does not apply
it or change the live BBS.

Both file panels start at `H:\`. VC's own internal program/settings files live
in a hidden `.VC` directory under that same private H:. Door-only setup hides
hidden/system entries and tree directories initially; Ctrl-H may reveal them.
The ordinary native/browser `data/VC.INI` is unchanged. For a short local test,
`--door-minutes 0.05` sets a three-second limit; positive fractional minutes
are supported as well as the default 60 minutes.

## Menus

Back up the board's existing `notanemulator_bbs-doors.hjson` on its config
volume. Merge the two entries from the adjacent file into its existing menu
map. Add visible choices named **Volkov Commander** and **Rogue** pointing to
`doorVolkovCommander` and `doorRogue`, using that menu's existing selection
pattern. Do not replace the entire menu file. Each door allows four concurrent
nodes (`nodeMax: 4`); adjust this to the container's CPU/memory budget.

Both entries use `abracadabra` and `io: stdio`. The wrapper expects the drop-file
path, node number, and `vc` or `rogue`; it never opens the drop file. Rogue uses
`vc --door --door-run ROGUE.EXE` and returns to the BBS when Rogue ends. VC uses
`vc --door` and returns to the BBS when the caller quits VC.

No local abracadabra source/manual or existing live menu was found in this
repository, nearby source/tool caches, `/opt`, `/usr/share/doc`, or `/tmp`.
The entries therefore follow the exact supplied example below. Support for
`dropFileType: NONE` could not be verified offline, so they retain `DOOR`.
After checking the installed ENiGMA version's documentation on axis, the sysop
may switch to `NONE` only if its argument substitution supports the ignored
first argument too. The menu integration still needs the user's live test.

```hjson
doorAbracadabraExample: {
    desc: Abracadabra Example
    module: abracadabra
    config: {
        name: Abracadabra
        dropFileType: DOOR
        cmd: /path/to/door
        args: [ "{dropFilePath}", "{node}" ]
        nodeMax: 1
        tooManyArt: DOORMANY
        io: stdio
    }
}
```

Reload menus using the board's existing procedure, or arrange a restart when
no callers are connected. On a test call, check both choices, F-keys/arrows,
Rogue quit, and carrier drop; confirm that the matching node's session
directory vanishes. Leave the existing WebSocket/Tailscale Funnel unchanged.
