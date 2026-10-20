# Development

Windows x64, MSVC C++ tools, Qt5.15 MSVC x64, Python3 x64. Initial tested build used
Qt5.15.0 and MSVC14.50. HIDAPI is an unmodified0.14.0 git submodule; shared DLLs may
be replaced with ABI-compatible builds. Build from an x64 Developer Command Prompt
or let `desktop/build.cmd` discover Visual Studio using vswhere.

## Desktop

```powershell
git submodule update --init --recursive
$env:WARLOCK_QT_DIR = 'C:\Qt\5.15.2\msvc2019_64'
.\desktop\build.cmd
.\dist\studio\WarlockStudio.exe --self-test
```

The self-test writes six screenshots beside the executable and round-trips a
temporary profile/preset. It does not access USB. `--radio` selects receiver;
`--demo` starts Rainbow after connecting. For icon regeneration only, install Pillow
and run `desktop/make_icon.py`; the generated icon is already included.

## Firmware

Install Arm GNU Toolchain14.2.Rel1 (`arm-none-eabi-gcc`, binutils on PATH) and the
test dependency `py -3 -m pip install -r firmware/requirements-dev.txt`. Supply your
original stock image at `firmware/stock/919_v104.bin` with the documented SHA256.

```powershell
py -3 firmware/aw60/build_patch.py
py -3 firmware/aw60/test_emulation.py
```

Check both exit codes. Build never opens USB or flashes. It verifies the complete
stock hash, nine BL callsites, four vectors, divider prologue and zero code window;
leaves the first65536 bytes unchanged, then emits rev9.bin and manifest-rev9.json.
The output is expected to match SHA256 from the release. Emulation takes several
minutes on some hosts and executes the actual Thumb code, including regression
protection for stock keymap staging. Do not run flash tools just to test source changes.

No vendor image, compiled toolchain, personal flash logs, paths, layouts or backups
are tracked. Firmware binary is an explicit release asset, not part of the Git tree.
