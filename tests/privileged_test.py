import hashlib
import io
import os
from pathlib import Path
import re
import selectors
import time
import shutil
import signal
import subprocess
import sys
import tarfile
import tempfile

PROJECT = Path(__file__).resolve().parent.parent


def archive(path, files):
    with tarfile.open(path, "w") as output:
        for name, text in files.items():
            data = text.encode()
            item = tarfile.TarInfo(name)
            item.uid = item.gid = 0
            item.mode = 0o644
            item.size = len(data)
            output.addfile(item, io.BytesIO(data))


def package(root, name, version, script=True):
    path = root / f"repo/{name}-{version}-any.pkg.tar"
    files = {".PKGINFO": f"pkgname = {name}\npkgver = {version}\narch = any\nsize = 16\npkgdesc = Isolated test\n",
             f"usr/share/{name}": version}
    if script:
        files[".INSTALL"] = "\n".join(f'{event}() {{ echo {event} >> /scriptlet-results; }}'
                                      for event in ("post_install", "post_upgrade", "post_remove")) + "\n"
    archive(path, files)
    return path


def repository(root, names):
    files = {}
    for name, version in names:
        path = package(root, name, version)
        fields = {"NAME": name, "VERSION": version, "FILENAME": path.name, "ARCH": "any",
                  "DESC": "Fixture", "CSIZE": path.stat().st_size, "ISIZE": 16,
                  "SHA256SUM": hashlib.sha256(path.read_bytes()).hexdigest()}
        files[f"{name}-{version}/desc"] = "".join(f"%{key}%\n{value}\n\n" for key, value in fields.items())
    archive(root / "repo/test.db", files)


def copy_binary(root, source, destination=None):
    destination = destination or source
    paths = [(source, destination)]
    dependencies = subprocess.run(["ldd", str(source)], capture_output=True, text=True, check=True).stdout
    paths += [(path, path) for path in re.findall(r"(/[^\s()]+)", dependencies)]
    for source, destination in paths:
        target = root / str(destination).lstrip("/")
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def enter(root):
    os.chroot(root)
    os.chdir("/")


def invoke(root, argv, expected=0, input=None):
    result = subprocess.run(argv, input=input, text=True, capture_output=True,
                            preexec_fn=lambda: enter(root), timeout=20,
                            env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
    assert result.returncode == expected, (argv, result.returncode, result.stdout, result.stderr)
    assert not (root / "db/db.lck").exists(), (argv, "lock leaked")
    return result


def backend(root, mode, target=None, success=True):
    args = ["/usr/bin/driver", mode, "/etc/pacman.conf"]
    if target is not None:
        args.append(target)
    result = invoke(root, args)
    values = dict(line.split("=", 1) for line in result.stdout.splitlines())
    assert (values["status"] == "0") == success, (args, result.stdout, result.stderr)
    return values


def inside():
    assert os.getuid() == os.geteuid() == 0
    assert os.getresuid() == (0, 0, 0) and os.getresgid() == (0, 0, 0)
    with tempfile.TemporaryDirectory(prefix="axon-privileged-") as temporary:
        root = Path(temporary)
        for name in ("etc", "db/local", "cache", "repo", "hooks", "dev", "tmp", "keys"):
            (root / name).mkdir(parents=True, exist_ok=True)
        (root / "dev/null").touch()
        for binary in ("/bin/sh", "/usr/bin/bash", "/usr/bin/touch", "/usr/bin/sleep"):
            copy_binary(root, binary)
        copy_binary(root, PROJECT / "tests/privileged_driver.out", "/usr/bin/driver")
        copy_binary(root, PROJECT / "axon", "/usr/bin/axon")
        (root / "etc/pacman.conf").write_text(
            "[options]\nRootDir = /\nDBPath = /db\nCacheDir = /cache\nLogFile = /axon.log\n"
            "GPGDir = /keys\nHookDir = /hooks\nArchitecture = any\nSigLevel = Never\n"
            "[test]\nServer = file:///repo\n")
        (root / "hooks/test.hook").write_text(
            "[Trigger]\nOperation = Install\nOperation = Upgrade\nOperation = Remove\n"
            "Type = Package\nTarget = fixture\n[Action]\nWhen = PostTransaction\n"
            "Exec = /usr/bin/touch /hook-ran\n")
        repository(root, [("fixture", "1-1")])
        backend(root, "refresh")
        backend(root, "install", "fixture")
        assert (root / "scriptlet-results").exists(), (root / "axon.log").read_text()
        payload = root / "usr/share/fixture"
        assert payload.read_text() == "1-1" and payload.stat().st_uid == payload.stat().st_gid == 0
        assert payload.stat().st_mode & 0o777 == 0o644
        assert (root / "hook-ran").exists()
        assert "post_install" in (root / "scriptlet-results").read_text()
        (root / "hook-ran").unlink()
        repository(root, [("fixture", "2-1")])
        backend(root, "upgrade", "unused")
        assert payload.read_text() == "2-1" and (root / "hook-ran").exists()
        assert "post_upgrade" in (root / "scriptlet-results").read_text()
        (root / "hook-ran").unlink()
        backend(root, "remove", "fixture")
        assert not payload.exists() and (root / "hook-ran").exists()
        assert "post_remove" in (root / "scriptlet-results").read_text()
        failing_hook = root / "hooks/fail.hook"
        failing_hook.write_text("[Trigger]\nOperation = Install\nType = Package\nTarget = fixture\n"
                                "[Action]\nWhen = PreTransaction\nAbortOnFail\nExec = /bin/sh -c 'exit 1'\n")
        backend(root, "install", "fixture", success=False)
        assert not payload.exists()
        failing_hook.unlink()
        bad = root / "repo/bad.pkg.tar"
        bad.write_text("broken archive")
        backend(root, "file", "/repo/bad.pkg.tar", success=False)
        print("Production root install/upgrade/remove, ownership, hooks, scriptlets, and cleanup passed.")
        if "--cli" in sys.argv:
            cli(root)


def cli(root):
    binary = "/usr/bin/axon"
    target = root / "usr/share/fixture"
    for args in (("-Sy", "fixture"), ("-Syu", "fixture"), ("-R",), ("-U",),
                 ("-QS",), ("-Ssi", "fixture"), ("-R", "--allow-downgrade", "fixture"),
                 ("--noconfirm", "-S", "fixture")):
        invoke(root, [binary, *args], expected=2)
    invoke(root, [binary, "-Sy"], expected=1, input="n\n")
    invoke(root, [binary, "-Sy"], input="y\n")
    for response in ("n\n", "", "yes" , "y" * 100 + "\ny\n"):
        result = invoke(root, [binary, "-S", "fixture"], expected=1, input=response)
        assert "Prepared transaction" in result.stdout and not target.exists()
    result = invoke(root, [binary, "-S", "test/fixture"], input="y\n")
    assert "Transaction completed" in result.stdout and target.read_text() == "2-1"
    invoke(root, [binary, "-R", "fixture"], expected=1, input="n\n")
    assert target.exists()
    config = root / "etc/pacman.conf"
    config.write_text(config.read_text() + "[options]\nHoldPkg = fixture\n")
    invoke(root, [binary, "-R", "fixture"], expected=1, input="n\ny\n")
    assert target.exists()
    invoke(root, [binary, "-R", "fixture"], input="y\ny\n")
    assert not target.exists()
    invoke(root, [binary, "-U", "/repo/fixture-1-1-any.pkg.tar"], input="y\n")
    assert target.read_text() == "1-1"
    result = invoke(root, [binary, "-Syu"], input="y\ny\n")
    assert "upgrade fixture: 1-1 -> 2-1" in result.stdout and target.read_text() == "2-1"
    invoke(root, [binary, "-U", "/repo/fixture-1-1-any.pkg.tar"], expected=1, input="y\n")
    assert target.read_text() == "2-1"
    invoke(root, [binary, "-U", "--allow-downgrade", "/repo/fixture-1-1-any.pkg.tar"], input="y\ny\n")
    assert target.read_text() == "1-1"
    invoke(root, [binary, "-Su"], input="y\n")
    assert target.read_text() == "2-1"
    result = invoke(root, [binary, "-Su"], input="")
    assert "Nothing to do" in result.stdout
    result = invoke(root, [binary, "-S", "absent"], expected=1, input="y\n")
    assert "target: absent" in result.stderr
    invoke(root, [binary, "-U", "/repo/bad.pkg.tar"], expected=1, input="y\n")
    for signum in (signal.SIGINT, signal.SIGTERM):
        process = subprocess.Popen([binary, "-R", "fixture"], stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   preexec_fn=lambda: enter(root), env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
        selector = selectors.DefaultSelector()
        selector.register(process.stderr, selectors.EVENT_READ)
        error = b""
        try:
            deadline = time.monotonic() + 10
            while b"[y/N]" not in error:
                assert time.monotonic() < deadline, error
                if selector.select(0.1):
                    chunk = os.read(process.stderr.fileno(), 4096)
                    assert chunk, error
                    error += chunk
            process.send_signal(signum)
            output, rest = process.communicate(timeout=10)
            assert process.returncode == 128 + signum, (output, error, rest)
        finally:
            selector.close()
            if process.poll() is None:
                process.kill()
                process.wait()
        assert not (root / "db/db.lck").exists() and target.exists()
    first = package(root, "cancel-a", "1-1")
    archive(first, {".PKGINFO": "pkgname = cancel-a\npkgver = 1-1\narch = any\nsize = 16\n",
                    "usr/share/cancel-a": "first",
                    ".INSTALL": "post_install() { echo cancel-during-install; /usr/bin/sleep 1; }\n"})
    second = package(root, "cancel-b", "1-1", script=False)
    process = subprocess.Popen([binary, "-U", "/repo/" + first.name, "/repo/" + second.name],
                               stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               preexec_fn=lambda: enter(root), env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
    process.stdin.write(b"y\n")
    process.stdin.flush()
    selector = selectors.DefaultSelector()
    selector.register(process.stderr, selectors.EVENT_READ)
    error = b""
    try:
        deadline = time.monotonic() + 10
        while b"cancel-during-install" not in error:
            assert time.monotonic() < deadline, error
            if selector.select(0.1):
                chunk = os.read(process.stderr.fileno(), 4096)
                assert chunk, error
                error += chunk
        process.send_signal(signal.SIGINT)
        output, rest = process.communicate(timeout=10)
        assert process.returncode == 130, (output, error, rest)
        assert b"changes may have occurred" in rest, rest
    finally:
        selector.close()
        if process.poll() is None:
            process.kill()
            process.wait()
    assert (root / "usr/share/cancel-a").exists() and not (root / "usr/share/cancel-b").exists()
    assert not (root / "db/db.lck").exists()
    lock = root / "db/db.lck"
    lock.write_text("another process\n")
    result = subprocess.run([binary, "-S", "fixture"], input="y\n", text=True, capture_output=True,
                            preexec_fn=lambda: enter(root), timeout=10)
    assert result.returncode == 1 and "lock" in result.stderr and lock.read_text() == "another process\n"
    lock.unlink()
    fresh = package(root, "output-failure", "1-1", script=False)
    with open("/dev/full", "wb") as full:
        result = subprocess.run([binary, "-U", "/repo/" + fresh.name], input=b"y\n", stdout=full,
                                stderr=subprocess.PIPE, preexec_fn=lambda: enter(root), timeout=10)
        assert result.returncode == 1 and not (root / "usr/share/output-failure").exists()
        assert not (root / "db/db.lck").exists()
    print("CLI mutations, argument validation, approvals, downgrade policy, signals, errors, and exit statuses passed.")


def main():
    if "--inside" in sys.argv:
        inside()
        return
    with tempfile.TemporaryDirectory(prefix="axon-nonroot-") as temporary:
        path = Path(temporary) / "pacman.conf"
        path.parent.chmod(0o755)
        path.write_text(f"[options]\nRootDir = {temporary}\nDBPath = {temporary}/db\n")
        driver = path.parent / "driver"
        shutil.copy2(PROJECT / "tests/privileged_driver.out", driver)
        def drop_identity():
            os.setgroups([])
            os.setgid(65534)
            os.setuid(65534)
        result = subprocess.run([str(driver), "file", str(path), "/missing.pkg.tar"],
                                preexec_fn=drop_identity if os.getuid() == 0 else None,
                                capture_output=True, text=True, check=True)
        assert "status=3\n" in result.stdout and not (path.parent / "db").exists(), result
    command = ["unshare", "--user", "--map-root-user", "--mount", "--pid", "--fork", "--net",
               sys.executable, str(Path(__file__).resolve()), "--inside"]
    if "--cli" in sys.argv:
        command.append("--cli")
    subprocess.run(command, check=True, timeout=120)


if __name__ == "__main__":
    main()
