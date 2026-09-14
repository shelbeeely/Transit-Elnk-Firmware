#pragma once

// Transit-Elnk-Firmware — build identity, supplied by the build system.
//
// FREEINK_FW_VERSION and FREEINK_BUILD_ENV come from tools/pio_version.py,
// which runs `git describe` at build time (see platformio.ini's
// extra_scripts). They are defaulted here so this code still compiles
// outside this project's PlatformIO envs -- a build with no version
// information should report itself as unversioned, not fail to compile.
//
// Both are string literals, not identifiers: the script passes them through
// SCons's StringifyMacro, so `#define FREEINK_FW_VERSION "a2945b9-dirty"`.
//
// This lives in its own header because two unrelated places need it: the
// boot report (src/main.cpp) says which build is running, and the settings
// portal (src/transit/setup_flow.cpp) shows the same string next to the
// firmware-upload form -- and the OTA pull path compares it against the
// manifest's version to decide whether there is anything to install.

#ifndef FREEINK_FW_VERSION
#define FREEINK_FW_VERSION "unversioned"
#endif

#ifndef FREEINK_BUILD_ENV
#define FREEINK_BUILD_ENV "unknown"
#endif

// 1 in [env:xteink_x4_bringup] only. See docs/HARDWARE_BRINGUP.md for what
// it changes; defaulted to 0 so `#if FREEINK_BRINGUP` reads correctly in
// every other build.
#ifndef FREEINK_BRINGUP
#define FREEINK_BRINGUP 0
#endif
