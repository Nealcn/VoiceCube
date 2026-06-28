#!/usr/bin/env python3
"""Monitor AtomS3 serial output for 5 seconds to verify boot."""
import os, subprocess, time, signal

env = os.environ.copy()
env.pop('MSYSTEM', None)

python = r'C:\Users\Administrator\.espressif\python_env\idf5.5_py3.10_env\Scripts\python.exe'
idf_py = r'D:\esp-idf\tools\idf.py'
firmware = r'D:\yuyin-fixed\firmware'

print("Connecting to COM3 (Ctrl+C to stop)...")
proc = subprocess.Popen(
    [python, idf_py, '-p', 'COM3', 'monitor'],
    cwd=firmware, env=env,
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    text=True, bufsize=1
)

# Read for 8 seconds then stop
start = time.time()
timeout = 8
try:
    while time.time() - start < timeout:
        line = proc.stdout.readline()
        if line:
            print(line, end='', flush=True)
except KeyboardInterrupt:
    pass
finally:
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=3)
    except:
        proc.kill()
