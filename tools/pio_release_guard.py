# Guard for the opt-in production lockdown env (cdc_badge_release).
#
# A bootloader built from sdkconfig.release SELF-BURNS secure-boot and
# flash-encryption eFuses on the first boot of an unlocked chip. This script
# makes sure that image can never be built by accident or uploaded directly:
#
#   1. Building requires the explicit opt-in  CDC_PROVISION_EXPERIMENTAL=1
#      (the same variable tools/provision.py demands).
#   2. `pio run -e cdc_badge_release -t upload` is ALWAYS refused. The only
#      supported flash path is tools/provision.py, which signs, encrypts and
#      writes the image after the eFuses were burned externally.

import os
import sys

Import("env")  # noqa: F821 - provided by the PlatformIO/SCons runtime

RED = "\033[1;31m"
RST = "\033[0m"

if "upload" in COMMAND_LINE_TARGETS:  # noqa: F821 - SCons global
    sys.stderr.write(
        RED + "\n"
        "REFUSED: never upload cdc_badge_release directly.\n"
        "Its bootloader burns secure-boot/flash-encryption eFuses on the first\n"
        "boot of an unlocked chip. Flash it ONLY via tools/provision.py\n"
        "(lock-esp step flash_release / reflash).\n\n" + RST)
    env.Exit(1)  # noqa: F821

if os.environ.get("CDC_PROVISION_EXPERIMENTAL") != "1":
    sys.stderr.write(
        RED + "\n"
        "REFUSED: cdc_badge_release is the production LOCKDOWN build.\n"
        "It must never be built by accident. If you really mean it, export\n"
        "  CDC_PROVISION_EXPERIMENTAL=1\n"
        "and re-run. See website docs: dev/production-hardening.\n\n" + RST)
    env.Exit(1)  # noqa: F821

# Only now (opt-in confirmed, not an upload) layer the lockdown sdkconfig.
# IDF reads SDKCONFIG_DEFAULTS (semicolon-separated) and, when set, uses ONLY
# this list. The release image must be the SHIPPED firmware plus hardening, so
# the base is the committed full dev config sdkconfig.cdc_badge_usb (1200+
# menuconfig-tuned options, e.g. the NimBLE central role the firmware links
# against) - NOT the sparse sdkconfig.defaults, which would drop those and fail
# to link. sdkconfig.release is layered last so its secure-boot / flash-
# encryption / NVS-encryption options win. Keeping this out of platformio.ini
# means the option set exists only in this process, only past the guard.
project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
base = os.path.join(project_dir, "sdkconfig.cdc_badge_usb")
overlay = os.path.join(project_dir, "sdkconfig.release")
for f in (base, overlay):
    if not os.path.exists(f):
        sys.stderr.write(RED + f"\nmissing {f}\n\n" + RST)
        env.Exit(1)  # noqa: F821
os.environ["SDKCONFIG_DEFAULTS"] = ";".join((base, overlay))
