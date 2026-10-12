import hashlib
import http.server
import io
import os
from pathlib import Path
import pwd
import socket
import subprocess
import tarfile
import tempfile
import threading

DRIVER = Path(__file__).resolve().with_name("download_driver.out")
requests = []


class Mirror(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        requests.append(self.path)
        route, _, filename = self.path.lstrip("/").partition("/")
        path = self.server.directory / filename
        if route == "missing" or not path.is_file():
            self.send_error(404)
            return
        data = path.read_bytes()
        start = 0
        if route == "ok" and self.headers.get("Range", "").startswith("bytes="):
            start = int(self.headers["Range"][6:].split("-", 1)[0])
            if start >= len(data):
                self.send_error(416)
                return
            self.send_response(206)
            self.send_header("Content-Range", f"bytes {start}-{len(data) - 1}/{len(data)}")
        else:
            self.send_response(200)
        self.send_header("Content-Length", str(len(data) - start))
        self.end_headers()
        try:
            if route == "broken":
                self.wfile.write(data[: max(1, len(data) // 2)])
                self.wfile.flush()
                self.connection.shutdown(socket.SHUT_RDWR)
            elif route == "corrupt" and filename.endswith(".pkg.tar"):
                self.wfile.write(b"!" * len(data))
            else:
                self.wfile.write(data[start:])
        except (BrokenPipeError, ConnectionResetError):
            pass


def tar(path, entries):
    with tarfile.open(path, "w") as archive:
        for name, contents in entries.items():
            data = contents.encode() if isinstance(contents, str) else contents
            item = tarfile.TarInfo(name)
            item.mode = 0o644
            item.size = len(data)
            archive.addfile(item, io.BytesIO(data))


def package(directory, name="fixture", version="1-1", size=1048576):
    path = directory / f"{name}-{version}-any.pkg.tar"
    tar(path, {".PKGINFO": f"pkgname = {name}\npkgver = {version}\npkgdesc = Test\narch = any\nsize = {size}\n",
               f"usr/share/{name}": b"x" * 1048576})
    return path, name, version, size


def repository(directory, packages):
    entries = {}
    for path, name, version, size in packages:
        fields = {"NAME": name, "VERSION": version, "DESC": "Test", "ARCH": "any",
                  "FILENAME": path.name, "CSIZE": path.stat().st_size, "ISIZE": size,
                  "SHA256SUM": hashlib.sha256(path.read_bytes()).hexdigest()}
        entries[f"{name}-{version}/desc"] = "".join(f"%{k}%\n{v}\n\n" for k, v in fields.items())
    tar(directory / "test.db", entries)


def gpg(home, *args, input=None):
    return subprocess.run(["gpg", "--homedir", str(home), "--batch", "--yes", "--no-auto-key-retrieve",
                           *args], input=input, check=True, capture_output=True).stdout


def run(mode, config, target=None, success=True, disk_full=False):
    command = [str(DRIVER), mode, str(config)]
    if target is not None:
        command.append(str(target))
    fault = {"LD_PRELOAD": str(DRIVER.with_name("disk_full.so")), "AXON_TEST_DISK_FULL": "1"} if disk_full else {}
    completed = subprocess.run(command, capture_output=True, text=True, timeout=30,
                               env={**os.environ, "LC_ALL": "C", "http_proxy": "", "https_proxy": "",
                                    "HTTP_PROXY": "", "HTTPS_PROXY": "", "ALL_PROXY": "", "NO_PROXY": "*", **fault})
    assert completed.returncode == 0, (command, completed.stderr)
    values = dict(line.split("=", 1) for line in completed.stdout.splitlines())
    values = {k: int(v) for k, v in values.items()}
    assert (values["status"] == 0) == success, (command, values, completed.stderr)
    assert not (config.parent / "db/db.lck").exists()
    if not success:
        assert not values["committed"]
    return values, completed.stderr


def main(base):
    mirror = base / "mirror"
    mirror.mkdir()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Mirror)
    server.directory = mirror
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    url = f"http://127.0.0.1:{server.server_port}"
    counter = 0

    def config(routes=("ok",), extra="", sig="Never", keyring=None, second=False):
        nonlocal counter
        counter += 1
        root = base / str(counter)
        root.mkdir()
        for name in ("root", "db", "cache", "keys"):
            (root / name).mkdir()
        (root / "keys").chmod(0o700)
        path = root / "pacman.conf"
        path.write_text(f"[options]\nRootDir = {root}/root\nDBPath = {root}/db\n"
                        f"CacheDir = {root}/cache\nLogFile = {root}/log\nGPGDir = {keyring or root / 'keys'}\n"
                        f"Architecture = any\nSigLevel = {sig}\nParallelDownloads = 1\n{extra}\n[test]\n"
                        + "".join(f"Server = {url}/{route}\n" for route in routes)
                        + (f"[absent]\nServer = {url}/missing\n" if second else ""))
        return path

    try:
        pkg = package(mirror)
        repository(mirror, [pkg])
        good = config(routes=("missing", "ok"))
        run("refresh", good)
        run("install", good, "fixture")
        assert (good.parent / "root/usr/share/fixture").exists()
        assert "/missing/test.db" in requests and "/ok/test.db" in requests
        assert f"/missing/{pkg[0].name}" in requests and f"/ok/{pkg[0].name}" in requests

        retry = config(routes=("broken", "ok"))
        run("refresh", retry)
        run("install", retry, "fixture")

        failed = config(routes=("missing",))
        run("refresh", failed, success=False)
        partial = config(second=True)
        values, _ = run("upgrade-plan", partial, "unused", success=False)
        assert values["changed"] and "plan" not in values
        assert (partial.parent / "db/sync/test.db").exists()
        before = len(requests)
        run("bad-upgrade", partial, "fixture", success=False)
        assert len(requests) == before

        for route in ("broken", "corrupt"):
            path = config(routes=(route,))
            if route == "broken":
                run("refresh", path, success=False)
                path.write_text(path.read_text().replace("/broken", "/ok"))
                run("refresh", path)
                path.write_text(path.read_text().replace("/ok", "/broken"))
            else:
                run("refresh", path)
            run("install", path, "fixture", success=False)
            assert not (path.parent / "root/usr/share/fixture").exists()

        interrupted = config()
        run("refresh", interrupted)
        values, _ = run("install-cancel", interrupted, "fixture", success=False)
        assert values["cancelled"] and values["downloads"]
        assert not (interrupted.parent / "root/usr/share/fixture").exists()
        values, _ = run("refresh-cancel", config(), success=False)
        assert values["cancelled"]

        for extra in ("", "DisableSandbox", "DisableSandboxFilesystem", "DisableSandboxSyscalls",
                      f"DownloadUser = {pwd.getpwuid(os.getuid()).pw_name}"):
            path = config(extra=extra)
            run("options", path)
            run("refresh", path)
            if extra.startswith("DownloadUser"):
                values, _ = run("install-cancel", path, "fixture", success=False)
                assert values["cancelled"]
                assert not (path.parent / "root/usr/share/fixture").exists()
        bad_user = config(extra="DownloadUser = axon-nonexistent-test-user")
        run("options", bad_user, success=False)
        run("refresh", bad_user, success=False)

        huge = package(mirror, name="huge", size=1048576)
        repository(mirror, [huge])
        path = config(extra="CheckSpace")
        run("refresh", path)
        _, error = run("install", path, "huge", success=False, disk_full=True)
        assert "space" in error.lower(), error
        assert not (path.parent / "root/usr/share/huge").exists()
        repository(mirror, [pkg])

        if os.getuid() != 0:
            path = config()
            run("refresh", path)
            (path.parent / "cache").chmod(0o500)
            # A second writable cache avoids libalpm's global /tmp fallback.
            (path.parent / "cache2").mkdir()
            path.write_text(path.read_text().replace("[test]", f"CacheDir = {path.parent}/cache2\n[test]"))
            run("install", path, "fixture")
            assert (path.parent / "cache2" / pkg[0].name).exists()
            (path.parent / "cache").chmod(0o700)

        if os.getuid() != 0:
            path = config()
            run("refresh", path)
            (path.parent / "db/sync").chmod(0o500)
            try:
                run("refresh", path, success=False)
            finally:
                (path.parent / "db/sync").chmod(0o700)

        signing = base / "signing"
        signing.mkdir(mode=0o700)
        gpg(signing, "--pinentry-mode", "loopback", "--passphrase", "", "--quick-generate-key",
            "Axon Fixture <fixture@example.invalid>", "ed25519", "sign", "0")
        public = gpg(signing, "--export")
        fingerprint = next(line.split(":")[9] for line in gpg(signing, "--with-colons", "--list-keys").decode().splitlines()
                           if line.startswith("fpr:"))
        trusted, untrusted = base / "trusted", base / "untrusted"
        for home in (trusted, untrusted):
            home.mkdir(mode=0o700)
            (home / "gpg.conf").write_text("no-auto-key-retrieve\n")
            gpg(home, "--import", input=public)
        gpg(trusted, "--import-ownertrust", input=f"{fingerprint}:6:\n".encode())
        for path in (pkg[0], mirror / "test.db"):
            gpg(signing, "--detach-sign", str(path))
        signed = config(sig="Required", keyring=trusted)
        run("refresh", signed)
        run("install", signed, "fixture")
        run("file", config(sig="Required", keyring=trusted), pkg[0])
        run("file", config(sig="Required", keyring=untrusted), pkg[0], success=False)
        run("refresh", config(sig="Required", keyring=untrusted), success=False)

        path = config(sig="PackageRequired DatabaseNever", keyring=untrusted)
        run("refresh", path)
        run("install", path, "fixture", success=False)

        signature = pkg[0].with_suffix(pkg[0].suffix + ".sig")
        valid = signature.read_bytes()
        other = package(mirror, "different")
        gpg(signing, "--detach-sign", str(other[0]))
        signature.write_bytes(other[0].with_suffix(other[0].suffix + ".sig").read_bytes())
        run("file", config(sig="Required", keyring=trusted), pkg[0], success=False)
        path = config(sig="Required", keyring=trusted)
        run("refresh", path)
        run("install", path, "fixture", success=False)
        signature.write_bytes(valid)
        signature.unlink()
        run("file", config(sig="Required", keyring=trusted), pkg[0], success=False)
        path = config(sig="Required", keyring=trusted)
        run("refresh", path)
        run("install", path, "fixture", success=False)
        signature.write_bytes(b"invalid signature")
        run("file", config(sig="Required", keyring=trusted), pkg[0], success=False)
        path = config(sig="Required", keyring=trusted)
        run("refresh", path)
        run("install", path, "fixture", success=False)
        signature.write_bytes(valid)
        dbsig = mirror / "test.db.sig"
        dbsig.unlink()
        run("refresh", config(sig="Required", keyring=trusted), success=False)
        dbsig.write_bytes(b"invalid signature")
        run("refresh", config(sig="Required", keyring=trusted), success=False)

        # The upgrade plan must contain every installed package with an available update.
        a, b = package(mirror, "first"), package(mirror, "second")
        upgrade = config()
        run("file", upgrade, a[0])
        run("file", upgrade, b[0])
        repository(mirror, [package(mirror, "first", "2-1"), package(mirror, "second", "2-1")])
        dbsig.unlink()
        values, _ = run("upgrade-plan", upgrade, "unused")
        assert values["plan"] == 2 and values["refreshed"] and not values["committed"]
        run("upgrade", upgrade, "unused")
        print("HTTP mirrors, transfers, signatures, sandbox options, space, and full-upgrade checks passed.")
    finally:
        server.shutdown()
        server.server_close()
        thread.join()
        for home in (base / "signing", base / "trusted", base / "untrusted"):
            if home.exists():
                subprocess.run(["gpgconf", "--homedir", str(home), "--kill", "all"], capture_output=True)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory(prefix="axon-download-") as temporary:
        main(Path(temporary))
