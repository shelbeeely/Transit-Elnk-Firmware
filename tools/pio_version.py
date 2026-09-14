"""Bakes a firmware version and env name into the build.

Answering "which build is on this board?" from a serial log is most of what
makes a bringup log worth keeping -- a report that says "unversioned" next
to a bug is a report you cannot correlate with a commit. This runs `git
describe` at build time and hands the result to the firmware as
FREEINK_FW_VERSION, alongside FREEINK_BUILD_ENV (the PlatformIO env name,
so a bringup build is never mistaken for a field build in a pasted log).

Failing to determine a version is not a build failure: a tarball export or a
checkout with no git available still builds, and reports itself honestly as
unversioned.
"""

import subprocess

Import("env")  # noqa: F821 -- injected by PlatformIO's SCons environment


def _git_version():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--always", "--dirty", "--tags"],
            stderr=subprocess.DEVNULL,
        )
    except Exception:
        return "unversioned"
    version = out.decode("utf-8", "replace").strip()
    return version or "unversioned"


env.Append(  # noqa: F821
    CPPDEFINES=[
        ("FREEINK_FW_VERSION", env.StringifyMacro(_git_version())),  # noqa: F821
        ("FREEINK_BUILD_ENV", env.StringifyMacro(env["PIOENV"])),  # noqa: F821
    ]
)
