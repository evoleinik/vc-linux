"""Offline native/browser content parity and the privileged BBS launch boundary."""

from pathlib import Path
import ctypes
import json
import re
import resource
import subprocess
import sys
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[1]
DOORS = ROOT / "infra/bbs/doors"
sys.path.insert(0, str(ROOT / "tools"))

from vcini import VcIni  # noqa: E402


def demo_arguments():
    build = ROOT / "build"
    return [str(build / relative) for relative in (
        "gwbasic/GWBASIC.EXE", "games", "bootlogo/LOGO.COM",
        "rogue/ROGUE.EXE", "vz/VZ.COM", "kermit/KERMIT.EXE")]


class EmbeddedFile(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char_p), ("data", ctypes.c_void_p),
                ("size", ctypes.c_uint32)]


def compiled_door_payload(tmp_path):
    source = tmp_path / "door_demo.c"
    subprocess.run([sys.executable, str(ROOT / "tools/door_demo.py"),
                    str(source), *demo_arguments()], check=True, capture_output=True)
    library_path = tmp_path / "door_demo.so"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-shared",
                    "-fPIC", "-I", str(ROOT / "runtime"), str(source),
                    "-o", str(library_path)], check=True, capture_output=True)

    return ctypes.CDLL(str(library_path)), source


def test_native_embedded_drive_is_the_exact_browser_demo(tmp_path):
    """Compare materialized bytes and paths, not two matching hand-written lists."""
    browser = tmp_path / "browser"
    subprocess.run([sys.executable, str(ROOT / "tools/web_demo.py"),
                    str(browser), *demo_arguments()], check=True, capture_output=True)
    library, source = compiled_door_payload(tmp_path)
    count = ctypes.c_int.in_dll(library, "door_demo_file_count").value
    table = (EmbeddedFile * count).in_dll(library, "door_demo_files")
    native = {entry.name.decode("utf-8"): ctypes.string_at(entry.data, entry.size)
              for entry in table}
    expected = {path.relative_to(browser).as_posix(): path.read_bytes()
                for path in browser.rglob("*") if path.is_file()}
    assert count == len(native) == len(expected)
    assert native == expected
    assert "ПРОЧТИ.TXT" in native
    assert "GAMES/ROGUE.EXE" in native
    assert sum(map(len, native.values())) < 1_000_000
    assert source.read_bytes() == (ROOT / "build/gen/door_demo.c").read_bytes()


def test_door_ini_starts_both_panels_at_h_and_hides_internal_settings(tmp_path):
    original = (ROOT / "data/VC.INI").read_bytes()
    library, _ = compiled_door_payload(tmp_path)
    entry = EmbeddedFile.in_dll(library, "door_default_ini")
    assert entry.name == b"VC.INI"
    ini = VcIni(ctypes.string_at(entry.data, entry.size))
    assert ini.checksum_ok, "VC rejects the entire setup if its checksum is wrong"
    assert len(ini.data) == len(original)
    base = VcIni(original)
    hidden_system = ini.sym["fa_HIDDEN"] | ini.sym["fa_SYSTEM"]
    allowed = {len(original) - 2, len(original) - 1, ini.main_offset("HidDirs")}
    for panel in (0, 1):
        assert ini.panel_path(panel) == "H:\\"
        assert ini.panel(panel, "WinTyp") == ini.sym["p_File"]
        assert ini.panel(panel, "Visible") == 1
        assert ini.panel(panel, "FiltAttr") == base.panel(panel, "FiltAttr") & ~hidden_system
        assert ini.panel(panel, "LongNames") == base.panel(panel, "LongNames") == 1
        allowed.update(ini.panel_offset(panel, field) for field in ("WinTyp", "Visible", "FiltAttr"))
        start = ini.panel_offset(panel, "WinShortPath")
        allowed.update(range(start, start + ini.sym["LenPath"]))
    assert ini.main("HidDirs") == 0
    assert ini.main("ConvCase") == base.main("ConvCase") == 0
    assert all(index in allowed or changed == old
               for index, (changed, old) in enumerate(zip(ini.data, original)))
    assert (ROOT / "data/VC.INI").read_bytes() == original
    count = ctypes.c_int.in_dll(library, "door_demo_file_count").value
    table = (EmbeddedFile * count).in_dll(library, "door_demo_files")
    assert all(item.name != b"VC.INI" for item in table)


def test_ini_panel_setters_preserve_other_fields_and_reject_invalid_paths():
    original = (ROOT / "data/VC.INI").read_bytes()
    ini = VcIni(original)
    second = ini.panel_path(1)
    ini.set_panel_path(0, "H:\\ПРОЧТИ")
    assert ini.panel_path(0) == "H:\\ПРОЧТИ"
    assert ini.panel_path(1) == second
    assert ini.checksum_ok
    ini.set_panel(0, "FiltAttr", 0)
    assert ini.panel(0, "FiltAttr") == 0
    assert ini.panel(1, "FiltAttr") == VcIni(original).panel(1, "FiltAttr")
    assert ini.checksum_ok
    for path in ("X" * ini.sym["LenPath"], "H:\\bad\0path"):
        before = bytes(ini.data)
        with pytest.raises(ValueError):
            ini.set_panel_path(0, path)
        assert bytes(ini.data) == before
    with pytest.raises(ValueError):
        ini.set_panel_path(-1, "H:\\")


@pytest.fixture
def wrapper_harness(tmp_path):
    """Replace absolute deployment executables, never actually run as root.

    Production has no PATH hook or configurable binary/root: the harness rewrites
    exact constants in its private copy, then exercises the real shell control
    flow, environment clearing, option order and argument quoting.
    """
    output = tmp_path / "launch.json"
    install_output = tmp_path / "install.json"
    privilege_output = tmp_path / "privilege.json"
    limits_output = tmp_path / "limits.json"
    owners = tmp_path / "owners.json"
    session_root = tmp_path / "vc-doors"
    install = tmp_path / "install"
    install.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                       f"open({str(install_output)!r}, 'a').write(json.dumps(sys.argv[1:]) + '\\n')\n"
                       "args = sys.argv[1:]\n"
                       "target = args[-1]\n"
                       f"assert target.startswith({str(tmp_path) + '/'!r})\n"
                       "os.makedirs(target, exist_ok=True)\n"
                       "os.chmod(target, int(args[args.index('-m') + 1], 8))\n"
                       f"owner_path = {str(owners)!r}\n"
                       "state = json.load(open(owner_path)) if os.path.exists(owner_path) else {}\n"
                       "state[target] = [int(args[args.index('-o') + 1]), int(args[args.index('-g') + 1])]\n"
                       "open(owner_path, 'w').write(json.dumps(state))\n")
    stat = tmp_path / "stat"
    stat.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                    "assert sys.argv[1:-1] == ['-c', '%u:%g', '--']\n"
                    "os.lstat(sys.argv[-1])\n"
                    f"state = json.load(open({str(owners)!r}))\n"
                    "print(':'.join(map(str, state[sys.argv[-1]])))\n")
    setpriv = tmp_path / "setpriv"
    setpriv.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                       f"open({str(privilege_output)!r}, 'w').write(json.dumps(sys.argv[1:5]))\n"
                       "os.execv(sys.argv[5], sys.argv[5:])\n")
    prlimit = tmp_path / "prlimit"
    prlimit.write_text(f"#!{sys.executable}\nimport json, os, sys\n"
                       f"open({str(limits_output)!r}, 'w').write(json.dumps(sys.argv[1:]))\n"
                       "os.execv('/usr/bin/prlimit', ['/usr/bin/prlimit', *sys.argv[1:]])\n")
    binary = tmp_path / "vc"
    binary.write_text(f"#!{sys.executable}\nimport json, os, resource, sys\n"
                      f"open({str(output)!r}, 'w').write(json.dumps({{"
                      "'args': sys.argv[1:], 'env': dict(os.environ), "
                      "'limits': {name: resource.getrlimit(getattr(resource, 'RLIMIT_' + name)) "
                      "for name in ('CPU', 'AS', 'CORE', 'FSIZE', 'NPROC')}}))\n")
    for path in (install, stat, setpriv, prlimit, binary):
        path.chmod(0o755)
    script = (DOORS / "run-door.sh").read_text()
    script = script.replace("/usr/bin/install", str(install))
    script = script.replace("/usr/bin/setpriv", str(setpriv))
    script = script.replace("/usr/bin/prlimit", str(prlimit))
    script = script.replace("/usr/bin/stat", str(stat))
    script = script.replace("/enigma-bbs/mods/vc-door/vc", str(binary))
    script = script.replace("/run/vc-doors", str(session_root))
    wrapper = tmp_path / "run-door.sh"
    wrapper.write_text(script)

    def run(node="42", program="vc", dropfile="/ignored/dropfile", extra=()):
        # This is deliberately hermetic: a planted environment-clearing defect
        # must never serialize real developer/CI credentials into test output.
        env = dict(PATH="/usr/bin:/bin", TERM="vt100", VC_DOOR_ROOT="/wrong/root",
                   VC_DOOR_NODE="99", EDITOR="bad-editor", VC_TRACE="1",
                   VC_SCREEN_DUMP="/wrong/dump", VC_MODEM_555_1992="example:23")
        return subprocess.run(["/bin/sh", str(wrapper), dropfile, node, program, *extra],
                              env=env, capture_output=True, text=True)

    def directory(path, uid, gid, mode):
        path.mkdir(exist_ok=True)
        path.chmod(mode)
        state = json.loads(owners.read_text()) if owners.exists() else {}
        state[str(path)] = [uid, gid]
        owners.write_text(json.dumps(state))

    return SimpleNamespace(run=run, output=output, install_output=install_output,
                           privilege_output=privilege_output, limits_output=limits_output,
                           root=session_root, owners=owners, directory=directory)


@pytest.mark.parametrize("program,expected", [
    ("vc", ["--door"]),
    ("rogue", ["--door", "--door-run", "ROGUE.EXE"]),
])
def test_wrapper_drops_privileges_and_passes_only_the_checked_node(wrapper_harness,
                                                                 program, expected):
    harness = wrapper_harness
    result = harness.run(program=program, dropfile="/never opened/$(false)")
    assert result.returncode == 0, result.stderr
    assert [json.loads(line) for line in harness.install_output.read_text().splitlines()] == [
        ["-d", "-o", "0", "-g", "0", "-m", "0711", "--", str(harness.root)],
        ["-d", "-o", "200042", "-g", "200042", "-m", "0700", "--",
         str(harness.root / "node-42")],
    ]
    assert json.loads(harness.privilege_output.read_text()) == [
        "--reuid=200042", "--regid=200042", "--clear-groups", "--no-new-privs"]
    launch = json.loads(harness.output.read_text())
    assert launch["args"] == expected
    assert launch["env"] == {
        "PATH": "/usr/bin:/bin", "LC_ALL": "C.UTF-8", "TERM": "vt100",
        "VC_DOOR_ROOT": str(harness.root / "node-42"), "VC_DOOR_NODE": "42",
    }
    assert harness.root.stat().st_mode & 0o777 == 0o711
    assert (harness.root / "node-42").stat().st_mode & 0o777 == 0o700


def test_wrapper_applies_kernel_resource_limits_without_limiting_process_count(wrapper_harness):
    harness = wrapper_harness
    result = harness.run()
    assert result.returncode == 0, result.stderr
    limits = json.loads(harness.output.read_text())["limits"]
    assert limits == {
        "CPU": [3700, 3700], "AS": [256 * 1024 * 1024] * 2,
        "CORE": [0, 0], "FSIZE": [16 * 1024 * 1024] * 2,
        "NPROC": list(resource.getrlimit(resource.RLIMIT_NPROC)),
    }
    options = json.loads(harness.limits_output.read_text())
    assert options[:5] == ["--cpu=3700:3700", "--as=268435456:268435456",
                           "--core=0:0", "--fsize=16777216:16777216", "--"]
    assert not any(option.startswith("--nproc") or option == "-u" for option in options)


@pytest.mark.parametrize("node,canonical,identity", [
    ("1", "1", 200001), ("000000008", "8", 200008),
    ("999999999", "999999999", 1000199999),
])
def test_wrapper_has_a_unique_decimal_identity_and_private_parent_per_node(
        wrapper_harness, node, canonical, identity):
    harness = wrapper_harness
    result = harness.run(node=node)
    assert result.returncode == 0, result.stderr
    assert json.loads(harness.privilege_output.read_text())[:2] == [
        f"--reuid={identity}", f"--regid={identity}"]
    env = json.loads(harness.output.read_text())["env"]
    assert env["VC_DOOR_NODE"] == canonical
    assert env["VC_DOOR_ROOT"] == str(harness.root / f"node-{canonical}")


def test_wrapper_reuses_only_matching_owned_node_directories(wrapper_harness):
    harness = wrapper_harness
    harness.directory(harness.root, 0, 0, 0o711)
    node = harness.root / "node-42"
    harness.directory(node, 200042, 200042, 0o700)
    marker = node / "existing-session-marker"
    marker.write_bytes(b"keep existing data")
    result = harness.run()
    assert result.returncode == 0, result.stderr
    assert marker.read_bytes() == b"keep existing data"
    assert json.loads(harness.output.read_text())["env"]["VC_DOOR_ROOT"] == str(node)


@pytest.mark.parametrize("location", ["root", "node"])
@pytest.mark.parametrize("kind", ["symlink", "dangling", "file", "wrong-owner"])
def test_wrapper_refuses_unsafe_existing_paths_without_touching_the_target(
        wrapper_harness, tmp_path, location, kind):
    harness = wrapper_harness
    target = tmp_path / "do-not-touch"
    target.mkdir(mode=0o755)
    marker = target / "marker"
    marker.write_bytes(b"not door data")
    if location == "node":
        harness.directory(harness.root, 0, 0, 0o711)
        path = harness.root / "node-42"
    else:
        path = harness.root
    if kind == "symlink":
        path.symlink_to(target, target_is_directory=True)
    elif kind == "dangling":
        path.symlink_to(tmp_path / "missing")
    elif kind == "file":
        path.write_bytes(b"do not overwrite")
    else:
        harness.directory(path, 65534, 65534, 0o700)
    result = harness.run()
    assert result.returncode != 0
    assert "VC door:" in result.stderr
    assert not harness.output.exists()
    assert not harness.privilege_output.exists()
    assert not harness.limits_output.exists()
    assert target.stat().st_mode & 0o777 == 0o755
    assert marker.read_bytes() == b"not door data"
    if kind == "file":
        assert path.read_bytes() == b"do not overwrite"
    if kind in ("symlink", "dangling"):
        assert path.is_symlink()
    if kind == "dangling":
        assert not (tmp_path / "missing").exists()


@pytest.mark.parametrize("node", ["", "0", "000", "1/../../escape", "-1", "1 2", "1;ls",
                                  "1234567890", "a"])
def test_wrapper_rejects_bad_node_before_privileged_setup(wrapper_harness, node):
    harness = wrapper_harness
    result = harness.run(node=node)
    assert result.returncode == 2
    assert "VC door: node" in result.stderr
    assert not any(path.exists() for path in (harness.output, harness.install_output,
                                            harness.privilege_output, harness.limits_output))


@pytest.mark.parametrize("program,extra", [("sh", ()), ("vc", ("--unsafe",))])
def test_wrapper_rejects_extra_commands_and_arguments(wrapper_harness, program, extra):
    harness = wrapper_harness
    result = harness.run(program=program, extra=extra)
    assert result.returncode == 2
    assert not any(path.exists() for path in (harness.output, harness.install_output,
                                            harness.privilege_output, harness.limits_output))


def test_enigma_pod_bounds_ephemeral_door_storage_and_compute_resources():
    manifest = (ROOT / "infra/bbs/enigma.yaml").read_text()
    deployment = next(doc for doc in manifest.split("\n---\n")
                      if "\nkind: Deployment\n" in doc)
    assert re.search(r"(?m)^            - \{name: door-sessions, mountPath: /run/vc-doors\}$",
                     deployment)
    assert re.search(r"(?m)^        - name: door-sessions\n"
                     r"          emptyDir:\n"
                     r"            medium: Memory\n"
                     r"            sizeLimit: 128Mi$", deployment)
    assert re.search(r"(?m)^          resources:\n"
                     r"            requests: \{cpu: 250m, memory: 256Mi\}\n"
                     r"            limits: \{cpu: 2, memory: 1Gi\}$", deployment)


def test_enigma_pod_reaps_orphaned_door_janitors():
    """The janitor outlives its worker; pause, as PID 1, must reap it."""
    manifest = (ROOT / "infra/bbs/enigma.yaml").read_text()
    deployment = next(doc for doc in manifest.split("\n---\n")
                      if "\nkind: Deployment\n" in doc)
    assert re.search(r"(?m)^      shareProcessNamespace: true$", deployment)


def test_door_tmpfs_holds_every_live_session_twice_over():
    """Both doors' nodeMax x the per-session page quota fits half the emptyDir."""
    menus = (DOORS / "notanemulator_bbs-doors.hjson").read_text()
    sessions = sum(int(n) for n in re.findall(r"(?m)^\s*nodeMax:\s*(\d+)\s*$", menus))
    quota = re.search(r"#define DOOR_QUOTA \(UINT64_C\((\d+)\) \* 1024 \* 1024\)",
                      (ROOT / "runtime/door.c").read_text())
    manifest = (ROOT / "infra/bbs/enigma.yaml").read_text()
    size_limit = re.search(r"(?m)^            sizeLimit: (\d+)Mi$", manifest)
    assert sessions == 8 and quota and size_limit
    assert 2 * sessions * int(quota.group(1)) <= int(size_limit.group(1))


def test_enigma_entries_follow_the_available_stdio_example():
    menus = (DOORS / "notanemulator_bbs-doors.hjson").read_text()
    for fragment in ("module: abracadabra", "io: stdio", "dropFileType: DOOR",
                     "cmd: /enigma-bbs/mods/vc-door/run-door.sh"):
        assert menus.count(fragment) == 2
    assert 'args: [ "{dropFilePath}", "{node}", "vc" ]' in menus
    assert 'args: [ "{dropFilePath}", "{node}", "rogue" ]' in menus


def test_arm_build_is_static_isolated_and_released_with_the_pinned_zig():
    makefile = (ROOT / "Makefile").read_text()
    assert "ZIG     ?= $(B)/zig/zig" in makefile
    assert "$(ZIG) cc -target aarch64-linux-musl -static" in makefile
    assert "$(B)/obj-door-aarch64/gen/%.o: $(B)/gen/%.c" in makefile
    assert "$(B)/vc-door-aarch64: $(DOOR_GEN_OBJ) $(DOOR_RT_OBJ)" in makefile
    workflow = (ROOT / ".github/workflows/ci.yml").read_text()
    assert "uv pip install ziglang==0.16.0" in workflow
    assert 'make build/vc-door-aarch64 ZIG="$zig"' in workflow
    assert "vc-door-linux-aarch64 --clobber" in workflow
