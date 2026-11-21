#!/usr/bin/env python3
"""Carry a user's data over from an old rootfs.hfs into a freshly built one.

Only reads the old image; the new image is modified.  Paths carried:
  /private/var/mobile                      (Media, Library/Preferences,
                                            Library/SpringBoard, Library/Keyboard, ...)
  /private/var/preferences/SystemConfiguration   (saved Wi-Fi networks)
  /private/var/Keychains                        (keychain: Wi-Fi passwords, ...)

/private/var/lib/dpkg and installed Cydia package files are deliberately NOT
carried: an update can change the Cydia/Substrate/base system and the dpkg
database plus package files would then desync from the new image (broken
packages, stale scripts).  Reinstall packages from Cydia after updating.

Usage:
  python3 scripts/carryover.py OLD.hfs NEW.hfs [--hfsplus TOOL] [--backup]
"""
import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

CARRY_PATHS = [
    "/private/var/mobile",
    "/private/var/preferences/SystemConfiguration",
    "/private/var/Keychains",
]


def _run(hfs, img, *args):
    return subprocess.run([hfs, img] + list(args), capture_output=True, text=True)


def _is_fresh_env():
    for v in ("IOS3DS_FRESH", "FRESH"):
        if os.environ.get(v, "").lower() in ("1", "true", "yes", "on"):
            return True
    return False


def ls_entries(hfs, img, path):
    """Return list of (mode_str, uid, gid, size, name) or None if missing."""
    r = _run(hfs, img, "ls", path)
    out = (r.stdout or "") + (r.stderr or "")
    if "No such file or directory" in out:
        return None
    entries = []
    for line in (r.stdout or "").splitlines():
        m = re.match(r"^(\d{6})\s+(\d+)\s+(\d+)\s+(\d+)\s+\S+\s+\S+\s+(.+?)\s*$", line)
        if not m:
            continue
        name = m.group(5).strip()
        if not name or name.endswith(":"):
            continue
        # skip the "Total filesystem size" footer
        if name.startswith("Total filesystem"):
            continue
        entries.append((m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), name))
    return entries


def exists_path(hfs, img, path):
    return ls_entries(hfs, img, path) is not None


def path_mode(hfs, img, path):
    path = path.rstrip("/")
    if path == "":
        path = "/"
    parent = os.path.dirname(path)
    if parent == "":
        parent = "/"
    name = os.path.basename(path)
    if name == "":
        return None
    ents = ls_entries(hfs, img, parent)
    if not ents:
        return None
    for mode, uid, gid, size, ename in ents:
        if ename == name:
            return mode
    return None


def _ensure_parents(hfs, img, path):
    parts = path.strip("/").split("/")
    cur = ""
    for part in parts[:-1]:
        cur += "/" + part
        if not exists_path(hfs, img, cur):
            _run(hfs, img, "mkdir", cur)


def _copy_file(hfs, old_img, new_img, old_path, new_path, mode):
    fd, tmp = tempfile.mkstemp(prefix="ios3ds-carry-")
    os.close(fd)
    os.remove(tmp)
    try:
        r = subprocess.run([hfs, old_img, "extract", old_path, tmp],
                           capture_output=True, text=True)
        if r.returncode:
            sys.stderr.write("warn: extract %s failed: %s\n" % (old_path, (r.stderr or r.stdout).strip()))
            return False
        if exists_path(hfs, new_img, new_path):
            _run(hfs, new_img, "rm", new_path)
        _ensure_parents(hfs, new_img, new_path)
        r = _run(hfs, new_img, "add", tmp, new_path)
        if r.returncode:
            sys.stderr.write("warn: add %s failed: %s\n" % (new_path, (r.stderr or r.stdout).strip()))
            return False
        if mode:
            _run(hfs, new_img, "chmod", mode, new_path)
        return True
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass


def _copy_symlink(hfs, old_img, new_img, old_path, new_path, mode):
    r = subprocess.run([hfs, old_img, "cat", old_path], capture_output=True)
    if r.returncode:
        sys.stderr.write("warn: cat %s failed\n" % old_path)
        return False
    target = r.stdout.decode("utf-8", "surrogateescape")
    # drop a single trailing NUL if present
    if target.endswith("\x00"):
        target = target[:-1]
    if exists_path(hfs, new_img, new_path):
        _run(hfs, new_img, "rm", new_path)
    _ensure_parents(hfs, new_img, new_path)
    rr = _run(hfs, new_img, "symlink", new_path, target)
    if rr.returncode:
        sys.stderr.write("warn: symlink %s failed: %s\n" % (new_path, (rr.stderr or rr.stdout).strip()))
        return False
    if mode:
        _run(hfs, new_img, "chmod", mode, new_path)
    return True


def copy_dir(hfs, old_img, new_img, old_path, new_path, mode=None, depth=0):
    """Recursively copy old_path -> new_path. Returns True on success."""
    entries = ls_entries(hfs, old_img, old_path)
    if entries is None:
        return False
    if not exists_path(hfs, new_img, new_path):
        _ensure_parents(hfs, new_img, new_path)
        _run(hfs, new_img, "mkdir", new_path)
    if mode:
        _run(hfs, new_img, "chmod", mode, new_path)
    ok = True
    for emode, uid, gid, size, name in entries:
        old_child = old_path.rstrip("/") + "/" + name
        new_child = new_path.rstrip("/") + "/" + name
        m = int(emode, 8)
        if stat.S_ISDIR(m):
            if not copy_dir(hfs, old_img, new_img, old_child, new_child, emode, depth + 1):
                ok = False
        elif stat.S_ISLNK(m):
            if not _copy_symlink(hfs, old_img, new_img, old_child, new_child, emode):
                ok = False
        else:
            if not _copy_file(hfs, old_img, new_img, old_child, new_child, emode):
                ok = False
    return ok


def carry_over(old_img, new_img, hfs, backup=False, verbose=True):
    """Carry user data from old_img into new_img. Returns True if any data carried."""
    if _is_fresh_env():
        if verbose:
            print("[carryover] IOS3DS_FRESH set: skipping carry-over")
        return False
    if not os.path.exists(old_img):
        if verbose:
            print("[carryover] no previous rootfs.hfs at %s: nothing to carry" % old_img)
        return False
    if not os.path.exists(new_img):
        if verbose:
            print("[carryover] new image missing: %s" % new_img)
        return False
    if backup:
        bak = old_img + ".bak"
        try:
            if not os.path.exists(bak):
                shutil.copy2(old_img, bak)
                if verbose:
                    print("[carryover] backed up old image to %s" % bak)
        except Exception as e:
            sys.stderr.write("warn: backup failed: %s\n" % e)
    carried = False
    for p in CARRY_PATHS:
        if not exists_path(hfs, old_img, p):
            continue
        if verbose:
            print("[carryover] carrying %s" % p)
        mode = path_mode(hfs, old_img, p)
        if copy_dir(hfs, old_img, new_img, p, p, mode):
            carried = True
        else:
            sys.stderr.write("warn: problems carrying %s (continuing)\n" % p)
    if verbose:
        print("[carryover] %s" % ("done" if carried else "nothing carried"))
    return carried


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("old", help="old rootfs.hfs")
    ap.add_argument("new", help="new rootfs.hfs")
    ap.add_argument("--hfsplus", default=None, help="hfsplus tool path")
    ap.add_argument("--backup", action="store_true", help="keep a backup copy of the old image")
    args = ap.parse_args()
    hfs = args.hfsplus
    if not hfs:
        hfs = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "build", "tools", "hfsplus")
    if not os.path.exists(hfs):
        # try PATH
        import shutil as _sh
        hfs = _sh.which("hfsplus") or hfs
    if not os.path.exists(args.old):
        die = lambda m: (sys.stderr.write(m + "\n"), sys.exit(1))
        die("old image not found: %s" % args.old)
    if not os.path.exists(args.new):
        sys.stderr.write("new image not found: %s\n" % args.new)
        sys.exit(1)
    try:
        carry_over(args.old, args.new, hfs, backup=args.backup, verbose=True)
    except Exception as e:
        sys.stderr.write("ERROR: carry-over failed: %s\n" % e)
        sys.exit(1)


if __name__ == "__main__":
    main()
