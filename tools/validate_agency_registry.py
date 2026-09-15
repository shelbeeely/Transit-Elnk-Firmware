#!/usr/bin/env python3
"""Validates agencies/registry.json against agencies/registry.schema.json.

Run by .github/workflows/agency-registry.yml on every PR touching
agencies/**, so a malformed or incomplete entry is caught in CI rather than
merged and only noticed when someone tries to build a pack from it.

Checks beyond plain schema validation (things JSON Schema can't express, or
that would make CI flaky if hard-enforced):

  * `id` is unique across every entry -- the schema can bound each entry's
    shape but can't compare entries to each other.
  * URLs are well-formed. NOT fetched: an agency's server having a bad
    minute must not fail an unrelated PR's CI, and this workflow has no
    business making outbound requests to third-party transit infrastructure
    on every push anyway. A maintainer reviewing a status=supported PR is
    expected to actually hit the URLs by hand, the same diligence
    docs/STA_INTEGRATION.md's own Compliance section was written from.

Usage: tools/validate_agency_registry.py [path-to-registry.json]
"""

import json
import sys
from pathlib import Path
from urllib.parse import urlparse

try:
    import jsonschema
except ImportError:
    print("error: this needs the 'jsonschema' package (pip install jsonschema)", file=sys.stderr)
    sys.exit(2)

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_REGISTRY = REPO_ROOT / "agencies" / "registry.json"
SCHEMA_PATH = REPO_ROOT / "agencies" / "registry.schema.json"


def load_json(path: Path):
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as e:
        print(f"error: {path} is not valid JSON: {e}", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print(f"error: {path} not found", file=sys.stderr)
        sys.exit(1)


def check_url_shape(errors: list, agency_id: str, field: str, url: str) -> None:
    parsed = urlparse(url)
    if parsed.scheme != "https" or not parsed.netloc:
        errors.append(f"{agency_id}: {field} ({url!r}) must be an https:// URL")


def main() -> int:
    registry_path = Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_REGISTRY

    schema = load_json(SCHEMA_PATH)
    registry = load_json(registry_path)

    try:
        jsonschema.validate(instance=registry, schema=schema)
    except jsonschema.ValidationError as e:
        print(f"error: {registry_path} does not match {SCHEMA_PATH.name}:", file=sys.stderr)
        print(f"  at {'/'.join(str(p) for p in e.absolute_path) or '(root)'}: {e.message}", file=sys.stderr)
        return 1

    errors: list = []
    seen_ids: dict = {}
    for i, agency in enumerate(registry["agencies"]):
        agency_id = agency.get("id", f"<entry {i}>")
        if agency_id in seen_ids:
            errors.append(f"duplicate id {agency_id!r} (entries {seen_ids[agency_id]} and {i})")
        else:
            seen_ids[agency_id] = i

        if "website" in agency:
            check_url_shape(errors, agency_id, "website", agency["website"])
        if "gtfs_rt" in agency:
            check_url_shape(errors, agency_id, "gtfs_rt.trip_updates_url",
                             agency["gtfs_rt"]["trip_updates_url"])

        # A supported entry claiming an auth value that still looks like a
        # placeholder is very likely a copy-paste from another agency's
        # entry (or this file's own STA example) rather than that agency's
        # actual key -- catch it here instead of on real hardware.
        if agency.get("status") == "supported":
            auth_value = agency.get("gtfs_rt", {}).get("auth_header_value")
            if auth_value and auth_value.strip().upper() in {"TODO", "CHANGEME", "YOUR-KEY-HERE"}:
                errors.append(f"{agency_id}: gtfs_rt.auth_header_value looks like an unfilled placeholder")

    if errors:
        print(f"error: {registry_path} failed validation:", file=sys.stderr)
        for err in errors:
            print(f"  - {err}", file=sys.stderr)
        return 1

    print(f"{registry_path}: {len(registry['agencies'])} agencies, all valid")
    return 0


if __name__ == "__main__":
    sys.exit(main())
