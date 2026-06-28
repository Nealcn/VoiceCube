#!/usr/bin/env python3
"""Build VoiceStick Windows installer using PyInstaller + NSIS"""
import os
import sys
import shutil
import subprocess

APP_NAME = "VoiceStick"
SCRIPT = os.path.join(os.path.dirname(__file__), "desktop", "python", "main.py")
DIST_DIR = os.path.join(os.path.dirname(__file__), "dist")
BUILD_DIR = os.path.join(os.path.dirname(__file__), "build", "pyinstaller")

# Clean previous build
for d in [DIST_DIR, BUILD_DIR]:
    if os.path.exists(d):
        shutil.rmtree(d)

# PyInstaller command
cmd = [
    sys.executable, "-m", "PyInstaller",
    "--name", APP_NAME,
    "--onefile",
    "--windowed",           # GUI app, no console
    "--noconfirm",
    "--clean",
    "--distpath", DIST_DIR,
    "--workpath", BUILD_DIR,
    # Add data files (config default, etc.)
    "--add-data", f"desktop/python/voicestick{os.pathsep}voicestick",
    # Hidden imports for bleak and aiohttp
    "--hidden-import", "bleak",
    "--hidden-import", "bleak.backends.winrt.client",
    "--hidden-import", "bleak.backends.winrt.scanner",
    "--hidden-import", "aiohttp",
    "--hidden-import", "PyQt5.sip",
    # Collect all submodules
    "--collect-submodules", "voicestick",
    "--collect-submodules", "bleak",
    "--collect-submodules", "aiohttp",
    # The entry point
    SCRIPT,
]

print("Running PyInstaller...")
subprocess.check_call(cmd)
print(f"\n✅ Executable built: {os.path.join(DIST_DIR, APP_NAME, f'{APP_NAME}.exe')}")
print(f"   Size: {os.path.getsize(os.path.join(DIST_DIR, APP_NAME, f'{APP_NAME}.exe')) / 1024 / 1024:.1f} MB")

# Check if makensis is available
nsis = shutil.which("makensis")
if nsis:
    print("\nBuilding NSIS installer...")
    nsis_script = os.path.join(os.path.dirname(__file__), "installer.nsi")
    if os.path.exists(nsis_script):
        subprocess.check_call([nsis, nsis_script])
        installer = os.path.join(DIST_DIR, f"{APP_NAME}_Setup.exe")
        if os.path.exists(installer):
            print(f"✅ Installer built: {installer}")
else:
    print("\n⚠️ NSIS not found, skipping installer. Install NSIS and run:")
    print("   makensis installer.nsi")
