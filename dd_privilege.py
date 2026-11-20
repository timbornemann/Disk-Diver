"""Explicit, short-lived elevation. The desktop and its file manager stay unprivileged."""

import argparse
import contextlib
import ctypes
import os
from pathlib import Path
import secrets
import sqlite3
import subprocess
import sys
import tempfile
import threading
from multiprocessing.connection import Client, Listener


def program_files():
    buf = ctypes.create_unicode_buffer(32768)
    if ctypes.windll.shell32.SHGetFolderPathW(None, 0x26, None, 0, buf):
        raise OSError("Could not locate Program Files")
    return Path(buf.value)


def trusted(path):
    path = path.resolve(strict=True)
    if os.name == "nt":
        return path.is_relative_to(program_files())
    return all(p.stat().st_uid == 0 and not p.stat().st_mode & 0o022 for p in (path, *path.parents))


def enable_backup_privilege():
    """Enable the backup privilege already assigned to the full-scan helper's primary token."""
    from ctypes import wintypes as wt

    class Luid(ctypes.Structure):
        _fields_ = [("low", wt.DWORD), ("high", wt.LONG)]

    class TokenPrivileges(ctypes.Structure):
        # TOKEN_PRIVILEGES with one LUID_AND_ATTRIBUTES entry.
        _fields_ = [("count", wt.DWORD), ("luid", Luid), ("attributes", wt.DWORD)]

    api = ctypes.WinDLL("kernel32", use_last_error=True)
    adv = ctypes.WinDLL("advapi32", use_last_error=True)
    api.GetCurrentProcess.restype = wt.HANDLE
    api.CloseHandle.argtypes = [wt.HANDLE]
    adv.OpenProcessToken.argtypes = [wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.HANDLE)]
    adv.LookupPrivilegeValueW.argtypes = [wt.LPCWSTR, wt.LPCWSTR, ctypes.POINTER(Luid)]
    adv.AdjustTokenPrivileges.argtypes = [
        wt.HANDLE, wt.BOOL, ctypes.POINTER(TokenPrivileges), wt.DWORD, wt.LPVOID, wt.LPVOID,
    ]
    token = wt.HANDLE()
    token_adjust_privileges = 0x20
    if not adv.OpenProcessToken(api.GetCurrentProcess(), token_adjust_privileges, ctypes.byref(token)):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        desired = TokenPrivileges(count=1, attributes=2)  # SE_PRIVILEGE_ENABLED
        if not adv.LookupPrivilegeValueW(None, "SeBackupPrivilege", ctypes.byref(desired.luid)):
            raise ctypes.WinError(ctypes.get_last_error())
        ctypes.set_last_error(0)
        ok = adv.AdjustTokenPrivileges(token, False, ctypes.byref(desired), 0, None, None)
        error = ctypes.get_last_error()
        if not ok or error:  # TRUE can still report ERROR_NOT_ALL_ASSIGNED.
            raise ctypes.WinError(error)
    finally:
        api.CloseHandle(token)


def protected_directory(directory):
    """Create an inheritable SYSTEM/Administrators-only ACL, including SQLite sidecars."""
    if os.name != "nt":
        return
    if not trusted(directory.parent):
        raise OSError("Install Disk-Diver under Program Files before using full mode.")
    from ctypes import wintypes as wt
    class Attributes(ctypes.Structure):
        _fields_ = [("length", wt.DWORD), ("descriptor", wt.LPVOID), ("inherit", wt.BOOL)]
    adv = ctypes.WinDLL("advapi32", use_last_error=True)
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.LocalFree.argtypes = [wt.LPVOID]
    adv.GetNamedSecurityInfoW.argtypes = [
        wt.LPWSTR, wt.DWORD, wt.DWORD, wt.LPVOID, wt.LPVOID, wt.LPVOID, wt.LPVOID, ctypes.POINTER(wt.LPVOID),
    ]
    adv.GetNamedSecurityInfoW.restype = wt.DWORD
    adv.ConvertSecurityDescriptorToStringSecurityDescriptorW.argtypes = [
        wt.LPVOID, wt.DWORD, wt.DWORD, ctypes.POINTER(wt.LPWSTR), wt.LPVOID,
    ]
    dacl_information = 4

    def dacl_key(sd):
        text = wt.LPWSTR()
        if not adv.ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, 1, dacl_information, ctypes.byref(text), None):
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            flags, *aces = text.value.split("(")
            # Windows can set these auto-inheritance bookkeeping flags. Keep the
            # protection flag, actual trustees, allow/deny masks and ACE inheritance flags.
            return flags.replace("AI", "").replace("AR", ""), sorted(aces)
        finally:
            api.LocalFree(ctypes.cast(text, wt.LPVOID))

    descriptor = wt.LPVOID()
    if not adv.ConvertStringSecurityDescriptorToSecurityDescriptorW(
            "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)", 1, ctypes.byref(descriptor), None):
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        attrs = Attributes(ctypes.sizeof(Attributes), descriptor, False)
        if not api.CreateDirectoryW(str(directory), ctypes.byref(attrs)):
            error = ctypes.get_last_error()
            if error != 183:
                raise ctypes.WinError(error)
            attributes = directory.stat(follow_symlinks=False).st_file_attributes
            if not attributes & 0x10 or attributes & 0x400:
                raise OSError("Invalid private-cache directory")
            actual = wt.LPVOID()
            error = adv.GetNamedSecurityInfoW(str(directory), 1, dacl_information, None, None, None, None,
                                              ctypes.byref(actual))
            if error:
                raise ctypes.WinError(error)
            try:
                if dacl_key(actual) != dacl_key(descriptor):
                    raise OSError("The existing private-cache directory has unexpected Windows permissions.")
            finally:
                api.LocalFree(actual)
    finally:
        api.LocalFree(descriptor)


def start_windows(program, args):
    from ctypes import wintypes as wt
    class ExecuteInfo(ctypes.Structure):
        _fields_ = [("size", wt.DWORD), ("mask", wt.ULONG), ("window", wt.HWND),
                    ("verb", wt.LPCWSTR), ("file", wt.LPCWSTR), ("params", wt.LPCWSTR),
                    ("directory", wt.LPCWSTR), ("show", ctypes.c_int),
                    ("instance", wt.HINSTANCE), ("idlist", wt.LPVOID), ("cls", wt.LPCWSTR),
                    ("key", wt.HKEY), ("hotkey", wt.DWORD), ("icon", wt.HANDLE),
                    ("process", wt.HANDLE)]
    info = ExecuteInfo()
    info.size, info.mask = ctypes.sizeof(info), 0x40 | 0x400
    info.verb, info.file = "runas", str(program)
    info.params = subprocess.list2cmdline(args)
    info.directory = str(Path(__file__).resolve().parent)
    shell = ctypes.WinDLL("shell32", use_last_error=True)
    if not shell.ShellExecuteExW(ctypes.byref(info)):
        raise ctypes.WinError(ctypes.get_last_error())
    api = ctypes.WinDLL("kernel32", use_last_error=True)
    api.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    api.CloseHandle.argtypes = [wt.HANDLE]
    return (lambda: api.WaitForSingleObject(info.process, 0) == 0,
            lambda: api.CloseHandle(info.process))


def windows_pipe_owner(connection):
    """Get the desktop account SID from the process serving this local named pipe."""
    from ctypes import wintypes as wt

    api = ctypes.WinDLL("kernel32", use_last_error=True)
    adv = ctypes.WinDLL("advapi32", use_last_error=True)
    api.GetNamedPipeServerProcessId.argtypes = [wt.HANDLE, ctypes.POINTER(wt.ULONG)]
    api.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    api.OpenProcess.restype = wt.HANDLE
    api.CloseHandle.argtypes = [wt.HANDLE]
    api.LocalFree.argtypes = [wt.LPVOID]
    adv.OpenProcessToken.argtypes = [wt.HANDLE, wt.DWORD, ctypes.POINTER(wt.HANDLE)]
    adv.GetTokenInformation.argtypes = [wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD, ctypes.POINTER(wt.DWORD)]
    adv.ConvertSidToStringSidW.argtypes = [wt.LPVOID, ctypes.POINTER(wt.LPWSTR)]
    pid = wt.ULONG()
    if not api.GetNamedPipeServerProcessId(connection.fileno(), ctypes.byref(pid)):
        raise ctypes.WinError(ctypes.get_last_error())
    process = api.OpenProcess(0x1000, False, pid.value)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    with contextlib.ExitStack() as cleanup:
        cleanup.callback(api.CloseHandle, process)
        token = wt.HANDLE()
        if not adv.OpenProcessToken(process, 8, ctypes.byref(token)):  # TOKEN_QUERY
            raise ctypes.WinError(ctypes.get_last_error())
        cleanup.callback(api.CloseHandle, token)
        size = wt.DWORD()
        adv.GetTokenInformation(token, 1, None, 0, ctypes.byref(size))  # TokenUser
        if ctypes.get_last_error() != 122 or not size.value:
            raise ctypes.WinError(ctypes.get_last_error())
        info = ctypes.create_string_buffer(size.value)
        if not adv.GetTokenInformation(token, 1, info, size, ctypes.byref(size)):
            raise ctypes.WinError(ctypes.get_last_error())
        sid = ctypes.cast(info, ctypes.POINTER(wt.LPVOID))[0]  # TOKEN_USER.User.Sid
        text = wt.LPWSTR()
        if not adv.ConvertSidToStringSidW(sid, ctypes.byref(text)):
            raise ctypes.WinError(ctypes.get_last_error())
        cleanup.callback(api.LocalFree, ctypes.cast(text, wt.LPVOID))
        return text.value


def peer_owner(connection):
    if os.name == "nt":
        return windows_pipe_owner(connection)
    import socket
    import struct
    credentials = struct.Struct("iII")  # pid_t, uid_t, gid_t (Linux struct ucred)
    with socket.socket(fileno=os.dup(connection.fileno())) as peer:
        _, uid, _ = credentials.unpack(peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, credentials.size))
    launcher = os.environ.get("PKEXEC_UID")
    if launcher is not None and launcher != str(uid):
        raise OSError("The connected desktop account does not match the authorization request.")
    return str(uid)


def elevate(worker, args, write):
    script = worker.HERE / "worker.py"
    bundled = worker.HERE / "python" / "python.exe"
    interpreter = bundled if os.name == "nt" and bundled.is_file() else Path(sys.executable)
    if not all(trusted(p) for p in (script, worker.HERE / "dd_journal.py",
                                     worker.HERE / "dd_privilege.py", interpreter,
                                     worker.scanner_path(True))):
        raise OSError("Full mode needs a system-owned installation of Disk-Diver and Python.")
    with tempfile.TemporaryDirectory() as directory:
        key = secrets.token_bytes(32)
        family = "AF_PIPE" if os.name == "nt" else "AF_UNIX"
        address = (r"\\.\pipe\disk-diver-" + secrets.token_hex(24) if os.name == "nt"
                   else str(Path(directory) / "session.sock"))
        with Listener(address, family=family, authkey=key) as listener:
            argv = ["-I", "-S", "-B", str(script), args.operation]
            if args.root:
                argv.append(os.path.abspath(args.root))
            argv.extend(["--channel", address, "--key", key.hex()])
            if args.no_cache:
                argv.append("--no-cache")
            if os.name == "nt":
                done, close = start_windows(interpreter, argv)
            else:
                command = [str(interpreter), *argv]
                env = None
                if os.geteuid() == 0:
                    # The desktop already has root credentials, possibly via pkexec itself.
                    # The new helper verifies the credentials of this desktop connection.
                    env = os.environ.copy()
                    env.pop("PKEXEC_UID", None)
                else:
                    command.insert(0, "/usr/bin/pkexec")
                process = subprocess.Popen(command, stdin=subprocess.DEVNULL, env=env)
                done, close = lambda: process.poll() is not None, lambda: process.wait(timeout=5)
            connections, errors = [], []
            ready = threading.Event()

            def accept():
                try:
                    connections.append(listener.accept())
                except Exception as exc:
                    errors.append(exc)
                finally:
                    ready.set()

            threading.Thread(target=accept, daemon=True).start()
            try:
                while not ready.wait(0.1):
                    if done():
                        raise OSError("Full scan was declined or the helper failed to start.")
                if errors:
                    raise errors[0]
                with connections[0] as conn:
                    while True:
                        data = conn.recv_bytes(worker.CHUNK + 4)
                        if data == b"Z":
                            return
                        if data[:1] == b"D":
                            write(data[1:])
                        elif data[:1] == b"S":
                            worker.status(data[1:].decode("utf8", "replace"))
                        else:
                            raise OSError(data[1:].decode("utf8", "replace"))
            finally:
                close()


def serve(worker, args):
    admin = ctypes.windll.shell32.IsUserAnAdmin() if os.name == "nt" else os.geteuid() == 0
    if not admin or not trusted(worker.HERE / "worker.py"):
        raise OSError("Full-mode helper needs a trusted installation and system authorization.")
    with Client(args.channel, authkey=bytes.fromhex(args.key)) as conn:
        def disconnected():
            try:
                conn.recv_bytes()
            except (OSError, EOFError):
                pass
            worker.cancel()
        threading.Thread(target=disconnected, daemon=True).start()
        try:
            owner = peer_owner(conn)
            if os.name == "nt" and args.operation == "scan":
                try:
                    enable_backup_privilege()
                except OSError as exc:
                    conn.send_bytes(b"S" + f"Backup privilege unavailable: {exc}".encode("utf8", "replace"))
            worker.run(args.operation, args.root, True, args.no_cache, owner,
                       lambda b: conn.send_bytes(b"D" + b),
                       lambda s: conn.send_bytes(b"S" + str(s).encode("utf8", "replace")))
            conn.send_bytes(b"Z")
        except Exception as exc:
            conn.send_bytes(b"E" + str(exc).encode("utf8", "replace"))


def main(worker):
    parser = argparse.ArgumentParser(description=worker.__doc__)
    parser.add_argument("operation", choices=("scan", "clear"))
    parser.add_argument("root", nargs="?")
    parser.add_argument("--full", action="store_true")
    parser.add_argument("--no-cache", action="store_true")
    for option in ("channel", "key"):
        parser.add_argument("--" + option, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if (args.operation == "scan") != bool(args.root):
        parser.error("scan requires a root, clear takes no root")
    def write(block):
        if block:
            sys.stdout.buffer.write(block)
            sys.stdout.buffer.flush()
    try:
        if args.channel:
            serve(worker, args)
        else:
            threading.Thread(target=worker.watch_stdin, daemon=True).start()
            if args.full:
                elevate(worker, args, write)
            else:
                worker.run(args.operation, args.root, False, args.no_cache, "", write, worker.status)
    except (OSError, ValueError, EOFError, RuntimeError, sqlite3.Error) as exc:
        worker.status(str(exc))
        raise SystemExit(1)
