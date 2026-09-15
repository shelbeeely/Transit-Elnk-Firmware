#!/usr/bin/env python3
"""Render the firmware's own setup/settings portal pages and screenshot them.

Those pages are not mockups: they are the exact HTML `setup_flow.cpp` serves
from the board's access point, extracted verbatim from the C++ raw string
literals so a screenshot here cannot drift from what a phone actually sees.

The board itself isn't running, so the JSON endpoints the pages poll are
stubbed with realistic responses and the multi-step flow is driven directly
through its own showStep() function. Everything visible is the page's real
markup, styling and rendering logic.

Usage: tools/capture_portal_screenshots.py [output-dir]
       (default output-dir: docs/screenshots)

Requires: pip install playwright, plus a Chromium it can drive. Set
PLAYWRIGHT_CHROMIUM to point at a browser binary when the pip package's
pinned build isn't the one installed (common when Chromium comes from the
environment rather than from `playwright install`).
"""

import json
import os
import pathlib
import re
import shutil
import sys
import tempfile

from playwright.sync_api import sync_playwright

REPO = pathlib.Path(__file__).resolve().parent.parent
SOURCE = REPO / "src" / "transit" / "setup_flow.cpp"

# A phone held in portrait is how these pages are actually used -- the
# stylesheet's own max-width is 480px, so anything wider just adds margin.
VIEWPORT = {"width": 420, "height": 1200}


def extract(name: str) -> str:
    src = SOURCE.read_text()
    match = re.search(re.escape(name) + r'\[\]\s*PROGMEM\s*=\s*R"HTML\((.*?)\)HTML";', src, re.S)
    if not match:
        raise SystemExit(f"could not find {name} in {SOURCE}")
    return match.group(1)


# Responses the real board would send. Kept deliberately plausible -- real
# Spokane stop names, a real-looking STA stop code -- so the screenshots read
# as a working device rather than as lorem ipsum.
STUB_ROUTES = {
    "/scan": ["Cedar Street 2.4", "Cedar Street 5G", "STA-WiFi", "xfinitywifi"],
    "/status": {"wifiState": "connected", "provisioned": False, "apiKeySet": False, "stopSet": False},
    "/getorientation": {"portrait": False, "tz": "PST8PDT,M3.2.0,M11.1.0"},
    "/listagencies": {
        "agencies": [
            {
                "id": "sta",
                "name": "Spokane Transit Authority",
                "region": "Spokane, WA, USA",
                "attributionRequired": False,
                "attributionText": "",
                "termsUrl": "https://www.spokanetransit.com/developers-terms-of-use/",
            },
        ],
    },
    "/getagencies": {
        "agencies": [
            {
                "id": "sta",
                "stopCode": "4377",
                "enabled": True,
                "name": "Spokane Transit Authority",
                "region": "Spokane, WA, USA",
                "attributionRequired": False,
                "attributionText": "",
                "termsUrl": "https://www.spokanetransit.com/developers-terms-of-use/",
            },
        ],
        "showAllEnabled": False,
        "activeAgencyId": "",
    },
    "/getpresets": {
        "home": {"legCount": 2, "walkMin": 4},
        "work": {"legCount": 1, "walkMin": 7},
        "transferBufferMin": 3,
    },
    "/getbuswifi": {"ssid": "STA-WiFi", "identity": "rider@example.com", "submitUrl": "", "fieldName": ""},
    # A plausible `git describe` output and a real OTA slot label -- the
    # Firmware step's whole job is telling you which build is running, so a
    # placeholder version here would make the screenshot say nothing.
    "/fwinfo": {
        "version": "v0.1.0-14-g2736ac3",
        "partition": "app0",
        "otaUrl": "https://github.com/shelbeeely/Transit-Elnk-Firmware/releases/latest/download/manifest.json",
        "trial": "",
    },
    "/stopsearch": {
        "ok": True,
        "results": [
            {"index": 0, "name": "SCC Transit Center Bay 3", "stopId": "1:SCCBAY3", "distanceMeters": 120},
            {"index": 1, "name": "Mission Ave & Greene St", "stopId": "1:MISGRE", "distanceMeters": 340},
            {"index": 2, "name": "Spokane Falls Blvd & Howard", "stopId": "1:SFBHOW", "distanceMeters": 610},
        ],
    },
    "/legdirections": {
        "ok": True,
        "routeId": "1:671",
        "routeShortName": "31",
        "directions": [
            {"directionId": 0, "headsign": "Dakota & Jay"},
            {"directionId": 1, "headsign": "SCC Transit Center"},
        ],
    },
}

# An IIFE, not a bare arrow function: add_init_script() *evaluates* the
# script rather than calling it, so a lone function expression would be
# constructed and immediately discarded -- leaving the real fetch in place
# and every page rendering its "could not reach the board" error path.
STUB_JS_TEMPLATE = """
(() => {
  const routes = __ROUTES__;
  window.fetch = function (url) {
    const path = String(url).split('?')[0];
    const body = Object.prototype.hasOwnProperty.call(routes, path) ? routes[path] : { ok: true };
    return Promise.resolve({
      status: 200,
      ok: true,
      json: () => Promise.resolve(body),
      text: () => Promise.resolve(JSON.stringify(body)),
    });
  };
})();
"""


def stub_script() -> str:
    return STUB_JS_TEMPLATE.replace("__ROUTES__", json.dumps(STUB_ROUTES))


def chromium_launch_args() -> dict:
    """Prefer a Chromium the environment already provides.

    The pip package pins one exact browser build and refuses anything else,
    which fails wherever Chromium was installed separately (as it is in this
    project's container). Pointing executablePath at the real binary is the
    documented escape hatch and avoids re-downloading ~150MB.
    """
    explicit = os.environ.get("PLAYWRIGHT_CHROMIUM")
    if explicit and pathlib.Path(explicit).exists():
        return {"executable_path": explicit}

    root = pathlib.Path(os.environ.get("PLAYWRIGHT_BROWSERS_PATH", "/opt/pw-browsers"))
    for candidate in sorted(root.glob("chromium*/chrome-linux/chrome"), reverse=True):
        return {"executable_path": str(candidate)}

    found = shutil.which("chromium") or shutil.which("chromium-browser") or shutil.which("google-chrome")
    return {"executable_path": found} if found else {}


def shrink(path: pathlib.Path) -> None:
    """Palette-quantize a portal screenshot.

    These are flat UI renders -- a handful of greys, one blue, black text --
    so 8-bit colour is visually lossless here while cutting each file by
    roughly 75%. That matters because they all land on one documentation
    page. Skipped silently without Pillow; a larger PNG is still a correct
    PNG.
    """
    try:
        from PIL import Image
    except ImportError:
        return
    before = path.stat().st_size
    with Image.open(path) as im:
        quantized = im.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT)
        quantized.save(path, optimize=True)
    after = path.stat().st_size
    if after > before:  # quantizing made it worse; keep whichever is smaller
        print(f"    (kept original, quantized was larger)")


def shoot(page, out_dir: pathlib.Path, name: str, section: str, setup_js: str = ""):
    page.evaluate(f"showStep({json.dumps(section)})")
    if setup_js:
        page.evaluate(setup_js)
    page.wait_for_timeout(120)
    path = out_dir / f"{name}.png"
    # Clipped to the body box rather than full_page: these pages are short
    # and a full-page shot pads them out to the viewport height, which on a
    # product page reads as a broken image with a lot of dead space.
    page.locator("body").screenshot(path=str(path))
    shrink(path)
    try:
        print(f"  {path.relative_to(REPO)}")
    except ValueError:
        print(f"  {path}")  # an output dir outside the repo (a scratch run)


def main() -> None:
    out_dir = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else (REPO / "docs" / "screenshots")
    out_dir.mkdir(parents=True, exist_ok=True)

    # Written to disk and navigated to, rather than handed to set_content():
    # add_init_script() only runs on a real navigation, so with set_content()
    # the fetch stub lands too late and every page renders its "could not
    # reach the board" error path instead of its populated state.
    work = pathlib.Path(tempfile.mkdtemp(prefix="portal-shots-"))
    setup_file = work / "setup.html"
    settings_file = work / "settings.html"
    setup_file.write_text(extract("kSetupPageHtml"))
    settings_file.write_text(extract("kSettingsPageHtml"))

    with sync_playwright() as p:
        browser = p.chromium.launch(**chromium_launch_args())
        page = browser.new_page(viewport=VIEWPORT, device_scale_factor=2)

        print("first-run setup portal:")
        page.add_init_script(stub_script())
        page.goto(setup_file.as_uri())
        page.wait_for_timeout(250)

        shoot(page, out_dir, "portal_setup_wifi", "step-wifi")
        shoot(page, out_dir, "portal_setup_apikey", "step-apikey",
              "document.getElementById('apikey').value='sk_live_************ab12';")
        shoot(page, out_dir, "portal_setup_stop", "step-stop", """
            document.getElementById('lat').value='47.6588';
            document.getElementById('lon').value='-117.4260';
            document.getElementById('query').value='SCC';
            searchStops();
        """)
        shoot(page, out_dir, "portal_setup_done", "step-done")

        print("settings portal:")
        page2 = browser.new_page(viewport=VIEWPORT, device_scale_factor=2)
        page2.add_init_script(stub_script())
        page2.goto(settings_file.as_uri())
        page2.wait_for_timeout(250)

        shoot(page2, out_dir, "portal_settings_display", "step-orientation")
        shoot(page2, out_dir, "portal_settings_agencies", "step-agencies", "loadAgencies();")
        shoot(page2, out_dir, "portal_settings_presets", "step-presets", """
            loadPresets();
            addLeg('home');
            presets.home.legs[0].routeQuery='31';
            presets.home.legs[0].routeId='1:671';
            presets.home.legs[0].routeShortName='31';
            presets.home.legs[0].boardStopName='SCC Transit Center Bay 3';
            presets.home.legs[0].boardStopId='1:SCCBAY3';
            presets.home.legs[0].alightStopName='Division & Sprague';
            presets.home.legs[0].alightStopId='1:DIVSPR';
            presets.home.legs[0].directions=[{directionId:0,headsign:'Dakota & Jay'},
                                             {directionId:1,headsign:'SCC Transit Center'}];
            presets.home.legs[0].directionId=0;
            renderLegs('home');
        """)
        shoot(page2, out_dir, "portal_settings_buswifi", "step-buswifi",
              "gotoBusWifi();")
        shoot(page2, out_dir, "portal_settings_firmware", "step-firmware",
              "loadFirmwareInfo();")
        shoot(page2, out_dir, "portal_settings_done", "step-done")

        browser.close()
    shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    main()
