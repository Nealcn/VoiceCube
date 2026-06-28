#!/usr/bin/env python3
"""Build and flash M5AtomS3 Voice Stick firmware."""
import os, sys, subprocess

env = os.environ.copy()
env.pop('MSYSTEM', None)

IDF_PATH = r'D:\esp-idf'
IDF_TOOLS_PATH = r'D:\espressif'
env['IDF_PATH'] = IDF_PATH
env['IDF_TOOLS_PATH'] = IDF_TOOLS_PATH
env['ESP_ROM_ELF_DIR'] = r'C:\Users\Administrator\.espressif\tools\esp-rom-elfs\20241011'

python_env_dir = r'C:\Users\Administrator\.espressif\python_env\idf5.5_py3.10_env\Scripts'
env['IDF_PYTHON_ENV_PATH'] = os.path.dirname(python_env_dir)

tools_bin = [
    python_env_dir,
    os.path.join(IDF_TOOLS_PATH, 'tools', 'cmake', '3.30.2', 'bin'),
    os.path.join(IDF_TOOLS_PATH, 'tools', 'ninja', '1.12.1'),
    os.path.join(IDF_TOOLS_PATH, 'tools', 'xtensa-esp-elf', 'esp-14.2.0_20260121', 'xtensa-esp-elf', 'bin'),
    os.path.join(IDF_TOOLS_PATH, 'tools', 'esp32ulp-elf', '2.38_20240113', 'esp32ulp-elf', 'bin'),
    os.path.join(IDF_TOOLS_PATH, 'tools', 'idf-exe', '1.0.3'),
    os.path.join(IDF_PATH, 'tools'),
    os.path.join(IDF_PATH, 'tools', 'esp_app_trace'),
]
existing = [p for p in tools_bin if os.path.isdir(p)]
env['PATH'] = ';'.join(existing) + ';' + env.get('PATH', '')

idf_py = os.path.join(IDF_PATH, 'tools', 'idf.py')
fw_dir = r'D:\yuyin-fixed\firmware'

def run_cmd(cmd, desc="", timeout=120):
    print(f"[{desc}]...", end="", flush=True)
    p = subprocess.run(cmd, cwd=fw_dir, env=env, capture_output=True, text=True, timeout=timeout)
    print(f" exit={p.returncode}")
    if p.returncode != 0:
        for line in (p.stdout + p.stderr).split('\n'):
            if any(k in line for k in ['error', 'Error', 'FAIL', 'fail', 'warning']):
                print(f"  {line.strip()}")
    return p

# Force rebuild of changed files
print("Rebuilding...")
r = run_cmd([os.path.join(python_env_dir, 'python.exe'), idf_py, 'build'], "build")
if r.returncode != 0:
    print("Build failed, check errors above")
    sys.exit(1)

# Flash
r = run_cmd([os.path.join(python_env_dir, 'python.exe'), idf_py, '-p', 'COM3', 'flash'], "flash")
if r.returncode != 0:
    print("Flash failed")
    sys.exit(1)
else:
    print("\nOK - Device reset.")
