# -*- mode: python ; coding: utf-8 -*-
from PyInstaller.utils.hooks import collect_submodules

hiddenimports = ['bleak', 'bleak.backends.winrt.client', 'bleak.backends.winrt.scanner', 'aiohttp', 'PyQt5.sip']
hiddenimports += collect_submodules('voicestick')
hiddenimports += collect_submodules('bleak')
hiddenimports += collect_submodules('aiohttp')


a = Analysis(
    ['D:\\yuyin-fixed\\desktop\\python\\main.py'],
    pathex=[],
    binaries=[],
    datas=[('desktop/python/voicestick', 'voicestick')],
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
    optimize=0,
)
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='VoiceStick',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
)
