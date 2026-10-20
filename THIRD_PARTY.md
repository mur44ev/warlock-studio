# Third-party components

- **Qt5.15.0**, dynamically linked Core/Gui/Widgets, Windows platform/style plugins:
  Copyright The Qt Company and contributors. LGPLv3/GPLv3 and applicable third-party
  notices. Sources: https://download.qt.io/archive/qt/5.15/5.15.0/single/
  (`qt-everywhere-src-5.15.0`); build packages originally from
  https://github.com/OpenRGBDevelopers/OpenRGB-Qt-Packages . Qt plugins/DLLs remain
  replaceable; this project does not restrict replacing/debugging them. Qt source
  notices: https://code.qt.io/cgit/qt/qtbase.git/tree/LICENSE.LGPL3?h=v5.15.0 .
- **HIDAPI0.14.0**, unmodified source submodule pinned at
  `d3013f0af3f4029d82872c1a9487ea461a56dee4` (verify `.gitmodules`/gitlink for the
  authoritative full commit). Windows DLL is built from that source by build.cmd.
  Selected license: BSD-3-Clause, `LICENSES/hidapi-BSD.txt`; upstream
  https://github.com/libusb/hidapi/tree/hidapi-0.14.0 .
- **ANGLE**, optional libEGL/libGLESv2 runtime from the Qt distribution:
  Google and contributors, BSD-style license in `LICENSES/ANGLE.txt`.
  Qt5.15.0 source distribution includes the exact bundled sources and notices at
  `qtbase/src/3rdparty/angle`; upstream https://github.com/google/angle .
- **GNU Arm toolchain/libgcc** is used to build the overlay. Runtime exceptions
  and notices: https://www.gnu.org/licenses/gcc-exception-3.1.html and
  https://gcc.gnu.org/onlinedocs/gccint/Libgcc.html . Toolchain source/license
  packages: https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads .
- **Microsoft Visual C++ runtime** is a system prerequisite, downloaded directly
  from Microsoft; its installer is not bundled with this release.

The modified firmware image retains unmodified vendor code/data; the GPL notices
for the custom overlay do not claim ownership of, or relicense, those portions.
The original stock image is not tracked in Git. No ARDOR driver software is bundled.
