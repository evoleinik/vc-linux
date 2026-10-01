# Brief 29: harden door mode before it goes public

Brief 28's change is uncommitted in this worktree. A security review found no host escape, but
door mode will face anonymous callers on a public BBS, on a host that also runs other services.
Fix everything below. For each item, first write a test that fails on the current code and show
it red, then fix and show it green. Same toolchain as brief 28. The axis kernel is 6.14 with
Landlock active (`lockdown,capability,landlock,yama,apparmor,...`), so Landlock ABI 4 network
rules are available there. Your sandbox may not allow Landlock or seccomp; if a test cannot run
here, say so plainly and leave it in the suite so I run it.

## 1. A caller can leave a session directory behind (major)

`remove_contents` (`runtime/door.c:97`) gives up when its path buffer would overflow. DOS rename
can move whole directories inside H: (`runtime/dos_fs.c:2518`, `:2554`), so repeated moves build a
tree deeper than 4096 bytes from short steps, and the session and its data stay forever.
Delete with `openat`/`unlinkat` relative to directory handles, so depth never needs a path
buffer, or move an over-deep subtree up to the session root with `renameat` and continue. Test
with a tree deeper than 4096 bytes built the way a caller would, through DOS calls.

## 2. A caller can fill the disk or run out of inodes (major)

The 8 MiB quota counts bytes only (`runtime/dos_fs.c:333`). Count every entry created, in
`fs_open` with create and in `fs_mkdir`, and fail past 4096 entries with DOS "disk full". Test it.

## 3. Resource limits (minor)

Run the door under `prlimit`: CPU time 3700 s, address space 256 MiB, core size 0, file size
16 MiB. Do not set a process-count limit. Test that the wrapper applies them.

## 4. Each node its own user, and Landlock and seccomp (defence in depth)

- The wrapper runs each node as uid and gid 200000 plus the node number, never 65534. It makes
  `/run/vc-doors` root-owned with mode 0711 and a per-node directory owned by that uid with mode
  0700, which the runtime creates its session inside.
- In door mode, after H: is populated and before any guest code runs, the runtime applies
  Landlock so the process can only read and write beneath its own session directory, plus read
  and execute its own binary if Landlock requires that. Handle network rules too where the kernel
  supports ABI 4 (TCP bind and connect denied). If Landlock is unavailable, door mode refuses to
  start with one clear line, unless `--door-allow-unconfined` is given for tests.
- Also install a seccomp filter that kills the process on `execve`, `execveat`, `socket`,
  `connect`, `ptrace`, `mount` and the other syscalls door mode never needs. Build it with plain
  BPF, without libseccomp, so the static musl ARM64 build still links.
- Tests: in door mode on x86-64, opening a file outside the session is denied by Landlock even
  when the runtime's own checks are bypassed through a test-only hook; an `execve` from a
  test-only hook kills the process. Show both red without the confinement.

## 5. Bounds on every copy between host and guest memory

In door mode the caller can write any guest byte, so every native `memcpy`, `memmove`,
`memset` and string copy in the DOS and BIOS layers (`runtime/dos_core.c`, `runtime/rt.c`,
`runtime/bios.c`, `runtime/dos_fs.c`, `runtime/modem.c`) must stay inside the guest memory array.
Route them through one checked helper that fails the call when start plus length passes
`MEM_SIZE`, and audit each site by hand. Add a fuzz test that runs INT 21h, INT 10h, INT 16h and
INT 14h in door mode with random registers over random guest memory for at least 200,000 calls
under AddressSanitizer and UBSan, with zero reports.

## 6. The pod

In `infra/bbs/enigma.yaml`: mount `/run/vc-doors` as an `emptyDir` with `medium: Memory` and
`sizeLimit: 128Mi`, and give the pod resource requests and limits (CPU limit 2, memory limit
1Gi). Explain each in `infra/bbs/doors/README.md`. I apply it.

## Done means

`make test` (apart from the two loopback TCP tests your sandbox blocks), `make web`,
`make test-web` green, and `make build/vc-door-aarch64` builds. The summary lists each fix with
red and green evidence, every bounds site you changed, and three weaknesses of your own work,
each with a real `file:line`. Do not commit.
