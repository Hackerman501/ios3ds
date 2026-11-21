#!/usr/bin/env python3
"""Carry a user's data over from an old rootfs.hfs into a freshly built one.

Only reads the old image; the new image is modified.  Paths carried:
  /private/var/mobile                      (Media, Library/Preferences,
                                            Library/SpringBoard, Library/Keyboard, ...)
  /private/var/preferences/SystemConfiguration   (saved Wi-Fi networks)
  /private/var/Keychains                        (keychain: Wi-Fi passwords, ...)

Never carried:
  /private/var/lib/dpkg and installed Cydia package files: an update can change
    Cydia/Substrate/the base system, so carrying the dpkg database plus package
    files would desync from the new image (broken packages, stale scripts).
    Reinstall packages from Cydia after updating.
  /private/var/mobile/Library/Caches: MobileInstallation's app list cache would
    be stale after an update.
  /private/var/mobile/Library/Logs: old crash and system logs are not useful on
    a new build and can be large.
  /private/var/mobile/Media/iTunes_Control: the Music library is rebuilt from
    SD/ios3ds/music (and the generated test song) by scripts/music.py on every
    install; the new library must win over the old one.
  any *.sqlite-journal / *.lock leftovers: transient files, not user data.

Ownership: xpwn's hfsplus has no chown command, and its source is not in this
repository (it lives in build/xpwn, shared by other worktrees).  Like
scripts/mkrootfs.py, we rely on hfsplus add/mkdir inheriting the owner, group
and folder mode from the parent directory: the new image already has
/private/var/mobile at 501:501 (mobile) and /private/var/{Keychains,preferences}
at root, so carried files get the owners of the old image.  Every copied entry
is checked against the old image's mode/uid/gid/size afterwards; a mismatch is
reported as a partial carry-over so the installer can keep the old image.

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

# subtrees that must never overwrite a freshly built one
EXCLUDED_DIRS = {
    "/private/var/mobile/Library/Caches",
    "/private/var/mobile/Library/Logs",
    "/private/var/mobile/Media/iTunes_Control",
}

# transient file-name suffixes that must never be carried
SKIP_SUFFIXES = (".sqlite-journal", ".lock")


class CarryResult(str):
    """A status string ("carried"/"partial"/"nothing") with failure details."""

    def __new__(cls, status, reason="", backup=None):
        obj = str.__new__(cls, status)
        obj.status = status
        obj.reason = reason
        obj.backup = backup
        return obj


def _run(hfs, img, *args):
    return subprocess.run([hfs, img] + list(args), capture_output=True, text=True)


def _is_fresh_env():
    for v in ("IOS3DS_FRESH", "FRESH"):
        if os.environ.get(v, "").lower() in ("1", "true", "yes", "on"):
            return True
    return False


def _warn(msg):
    sys.stderr.write("warn: %s\n" % msg)


def ls_entries(hfs, img, path):
    """Return (mode, uid, gid, size, name) tuples for a directory; None if absent."""
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
        if not name or name.endswith(":") or name.startswith("Total filesystem"):
            continue
        entries.append((m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), name))
    return entries


def exists_path(hfs, img, path):
    return ls_entries(hfs, img, path) is not None


def path_mode(hfs, img, path):
    """Mode of path as printed by hfsplus ls (including the file-type bits)."""
    path = path.rstrip("/")
    if not path:
        path = "/"
    parent = os.path.dirname(path) or "/"
    name = os.path.basename(path)
    if not name:
        return None
    for mode, uid, gid, size, ename in ls_entries(hfs, img, parent) or []:
        if ename == name:
            return mode
    return None


def free_space(hfs, img):
    """Parse the free-byte count from the 'hfsplus IMG ls /' footer."""
    r = _run(hfs, img, "ls", "/")
    m = re.search(r"free:\s*(\d+)", (r.stdout or "") + (r.stderr or ""))
    return int(m.group(1)) if m else 0


def _skip_name(name):
    return any(name.endswith(s) for s in SKIP_SUFFIXES)


def _child(parent, name):
    return parent.rstrip("/") + "/" + name


def scan_tree(hfs, img, path, tree):
    """Snapshot one directory (recursively) into tree[path] = [entries].

    One 'ls' per directory, reused for the size check, the copy and the
    verification pass.  Returns False if the directory could not be listed.
    """
    entries = ls_entries(hfs, img, path)
    if entries is None:
        return False
    kept = []
    ok = True
    for mode, uid, gid, size, name in entries:
        if _skip_name(name):
            continue
        child = _child(path, name)
        if child in EXCLUDED_DIRS:
            continue
        if stat.S_ISDIR(int(mode, 8)):
            if not scan_tree(hfs, img, child, tree):
                _warn("cannot list %s in the old image" % child)
                ok = False
                continue
        kept.append((mode, uid, gid, size, name))
    tree[path] = kept
    return ok


def _tree_size(tree):
    total = 0
    for entries in tree.values():
        for mode, uid, gid, size, name in entries:
            if not stat.S_ISDIR(int(mode, 8)):
                total += size
    return total


def _ensure_dir(hfs, img, path, created):
    """Ensure directory path exists (create if missing), tracking creations."""
    if path in created:
        return
    parts = path.strip("/").split("/")
    cur = ""
    for part in parts:
        cur += "/" + part
        if cur not in created:
            _run(hfs, img, "mkdir", cur)  # "already exists" is fine
            created.add(cur)


def _copy_file(hfs, old_img, new_img, old_path, new_path, mode, created):
    _ensure_dir(hfs, new_img, os.path.dirname(new_path), created)
    fd, tmp = tempfile.mkstemp(prefix="ios3ds-carry-")
    os.close(fd)
    os.remove(tmp)
    try:
        r = subprocess.run([hfs, old_img, "extract", old_path, tmp],
                           capture_output=True, text=True)
        if r.returncode or not os.path.exists(tmp):
            _warn("extract %s failed: %s" % (old_path, (r.stderr or r.stdout).strip()))
            return False
        _run(hfs, new_img, "rm", new_path)  # ignore "no such file"
        r = _run(hfs, new_img, "add", tmp, new_path)
        if r.returncode:
            _warn("add %s failed: %s" % (new_path, (r.stderr or r.stdout).strip()))
            return False
        if mode:
            _run(hfs, new_img, "chmod", mode, new_path)
        return True
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass


def _copy_symlink(hfs, old_img, new_img, old_path, new_path, mode, created):
    _ensure_dir(hfs, new_img, os.path.dirname(new_path), created)
    r = subprocess.run([hfs, old_img, "cat", old_path], capture_output=True)
    if r.returncode:
        _warn("cat %s failed" % old_path)
        return False
    target = r.stdout.decode("utf-8", "surrogateescape")
    if target.endswith("\x00"):
        target = target[:-1]
    _run(hfs, new_img, "rm", new_path)
    rr = _run(hfs, new_img, "symlink", new_path, target)
    if rr.returncode:
        _warn("symlink %s failed: %s" % (new_path, (rr.stderr or rr.stdout).strip()))
        return False
    if mode:
        _run(hfs, new_img, "chmod", mode, new_path)
    return True


def copy_dir(hfs, old_img, new_img, old_path, new_path, mode=None, tree=None,
             created=None, depth=0):
    """Copy a snapshotted directory into the new image. Returns (ok, bytes)."""
    if created is None:
        created = set()
    entries = tree[old_path] if tree is not None else ls_entries(hfs, old_img, old_path)
    if entries is None:
        return False, 0
    _ensure_dir(hfs, new_img, new_path, created)
    if mode:
        _run(hfs, new_img, "chmod", mode, new_path)
    ok, total = True, 0
    for emode, uid, gid, size, name in entries:
        old_child, new_child = _child(old_path, name), _child(new_path, name)
        m = int(emode, 8)
        if stat.S_ISDIR(m):
            cok, nbytes = copy_dir(hfs, old_img, new_img, old_child, new_child,
                                   emode, tree, created, depth + 1)
            ok = ok and cok
            total += nbytes
        elif stat.S_ISLNK(m):
            ok = _copy_symlink(hfs, old_img, new_img, old_child, new_child, emode, created) and ok
        else:
            if _copy_file(hfs, old_img, new_img, old_child, new_child, emode, created):
                total += size
            else:
                ok = False
    return ok, total


def verify_tree(hfs, new_img, tree):
    """Check every copied entry: it must exist with the old mode, uid, gid, size."""
    errors = []
    for path, entries in tree.items():
        present = {name: (mode, uid, gid, size)
                   for mode, uid, gid, size, name in (ls_entries(hfs, new_img, path) or [])}
        for mode, uid, gid, size, name in entries:
            got = present.get(name)
            if got is None:
                errors.append("missing after copy: %s" % _child(path, name))
                continue
            want = (mode, uid, gid)
            if not stat.S_ISDIR(int(mode, 8)):
                want += (size,)
            if got[:len(want)] != want:
                errors.append("owner/mode/size changed: %s (%s -> %s)" %
                              (_child(path, name), "/".join(map(str, want)), "/".join(map(str, got))))
    return errors


def _copy_backup(old_img, bak):
    try:
        shutil.copy2(old_img, bak)
        return True
    except Exception as e:
        _warn("could not keep the previous iOS disk at %s: %s" % (bak, e))
        return False


def keep_backup(old_img, reason, already=False):
    """Keep the old image on the card; return its backup path (or None)."""
    bak = old_img + ".bak"
    if already or _copy_backup(old_img, bak):
        msg = "carry-over %s; kept the previous iOS disk at %s" % (reason, bak)
        _warn(msg)
        return bak
    return None


def carry_over(old_img, new_img, hfs, backup=False, verbose=True):
    """Carry user data from old_img into new_img.

    Returns a CarryResult (a str) with status "carried", "partial" or "nothing",
    a reason, and the path of the automatic backup if one was made.  A backup is
    made whenever the data cannot be carried completely, so an update never
    silently loses the old image.
    """
    if _is_fresh_env():
        if verbose:
            print("[carryover] IOS3DS_FRESH set: skipping carry-over")
        return CarryResult("nothing", "fresh")
    if not os.path.exists(old_img):
        if verbose:
            print("[carryover] no previous rootfs.hfs at %s: nothing to carry" % old_img)
        return CarryResult("nothing", "no old image")
    if not os.path.exists(new_img):
        if verbose:
            print("[carryover] new image missing: %s" % new_img)
        return CarryResult("nothing", "no new image")

    bak_done = False
    if backup:
        bak_done = _copy_backup(old_img, old_img + ".bak")
        if bak_done and verbose:
            print("[carryover] backed up old image to %s" % (old_img + ".bak"))

    # one ls per directory in the old image: size check, copy and verification
    # all use the same snapshot (also avoids an 'ls' of each parent per file)
    tree = {}
    for p in CARRY_PATHS:
        if exists_path(hfs, old_img, p):
            scan_tree(hfs, old_img, p, tree)
    if not tree:
        if verbose:
            print("[carryover] nothing to carry")
        return CarryResult("nothing", "no user data", bak_done and old_img + ".bak" or None)

    total = _tree_size(tree)
    free = free_space(hfs, new_img)
    if not free or total > free:
        _warn("carried data (%d bytes) does not fit in new image free space (%d bytes)" %
              (total, free))
        bak = keep_backup(old_img, "data does not fit", already=bak_done)
        if verbose:
            print("[carryover] nothing carried (does not fit)")
        return CarryResult("nothing", "does not fit", bak)

    created = set()
    try:
        for p in CARRY_PATHS:
            if p not in tree:
                continue
            if verbose:
                print("[carryover] carrying %s" % p)
            mode = path_mode(hfs, old_img, p)
            copy_dir(hfs, old_img, new_img, p, p, mode, tree, created)
        errors = verify_tree(hfs, new_img, tree)
    except Exception as e:
        errors = ["carry-over exception: %s" % e]

    if errors:
        for msg in errors[:20]:
            _warn(msg)
        if len(errors) > 20:
            _warn("... and %d more" % (len(errors) - 20))
        bak = keep_backup(old_img, "was partial", already=bak_done)
        if verbose:
            print("[carryover] partial")
        return CarryResult("partial", "; ".join(errors[:3]), bak)
    if verbose:
        print("[carryover] done")
    return CarryResult("carried", "", bak_done and old_img + ".bak" or None)


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
        hfs = shutil.which("hfsplus") or hfs
    for path in (args.old, args.new):
        if not os.path.exists(path):
            sys.stderr.write("image not found: %s\n" % path)
            sys.exit(1)
    try:
        result = carry_over(args.old, args.new, hfs, backup=args.backup, verbose=True)
        print("[carryover] result: %s" % result)
    except Exception as e:
        sys.stderr.write("ERROR: carry-over failed: %s\n" % e)
        sys.exit(1)


if __name__ == "__main__":
    main()
