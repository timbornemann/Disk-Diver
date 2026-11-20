"""Wall-clock and OS-recorded peak memory for one child, without polling."""

import ctypes
import os
import subprocess
import time


def windows_peak(handle):
    class Counters(ctypes.Structure):
        _fields_ = [("size", ctypes.c_uint32), ("faults", ctypes.c_uint32)] + [
            (name, ctypes.c_size_t) for name in ("peak", "working", "pp", "p", "np", "n", "page", "peak_page")]

    api = ctypes.WinDLL("psapi", use_last_error=True)
    get = api.GetProcessMemoryInfo
    get.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_uint32]
    counters = Counters()
    counters.size = ctypes.sizeof(counters)
    if not get(int(handle), ctypes.byref(counters), counters.size):
        raise ctypes.WinError(ctypes.get_last_error())
    return counters.peak


def invoke(command, out, err, env=None):
    start = time.perf_counter_ns()
    with subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=out, stderr=err, env=env) as process:
        if os.name == "nt":
            code = process.wait()
            elapsed = time.perf_counter_ns() - start
            peak = windows_peak(process._handle)
        else:
            _, status, usage = os.wait4(process.pid, 0)
            elapsed = time.perf_counter_ns() - start
            code = os.waitstatus_to_exitcode(status)
            process.returncode = code
            peak = usage.ru_maxrss * 1024  # Linux reports this value in KiB.
    if code:
        raise RuntimeError(f"benchmark command failed ({code}): {command[0]} {command[1:]}")
    return {"wall_ms": round(elapsed / 1e6, 4), "peak_rss_bytes": peak}
