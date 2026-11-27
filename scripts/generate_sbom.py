#!/usr/bin/env python3
"""RavenEngine SBOM generator — emits a CycloneDX 1.5 JSON document describing every
vendored third-party component (see third_party/manifest.json) plus this repository.

Usage:
    scripts/generate_sbom.py [--output sbom.cdx.json] [--repo-root .]

No network access is required. The output is valid CycloneDX 1.5 and can be fed to
vulnerability scanners (e.g. `grype sbom:sbom.cdx.json`) or attached to releases.
"""

import argparse
import datetime
import json
import os
import sys


def load_manifest(root):
    path = os.path.join(root, "third_party", "manifest.json")
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def license_entries(license_str):
    """manifest licenses are simple strings ('MIT', 'Apache-2.0', 'MIT/PD', ...);
    map the common ones onto SPDX expressions, otherwise fall back to a named entry."""
    expr_map = {
        "MIT": "MIT",
        "Apache-2.0": "Apache-2.0",
        "Zlib": "Zlib",
        "MIT/PD": "(MIT OR Unlicense)",
        "Unlicense/MIT-0": "(Unlicense OR MIT-0)",
    }
    text = license_str.strip()
    spdx = expr_map.get(text, text if "/" not in text else None)
    if spdx:
        return [{"expression": spdx}]
    return [{"license": {"name": text}}]


def main():
    parser = argparse.ArgumentParser(description="Generate CycloneDX SBOM for RavenEngine")
    parser.add_argument("--output", default="-", help="output file (default: stdout)")
    parser.add_argument("--repo-root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    args = parser.parse_args()

    root = os.path.abspath(args.repo_root)
    manifest = load_manifest(root)

    project_version = "0.1.0"
    project_name = "RavenEngine"

    components = []
    for dep in manifest.get("dependencies", []):
        component = {
            "type": "library",
            "bom-ref": f"pkg:generic/{dep['name']}@{dep['version']}",
            "name": dep["name"],
            "version": str(dep["version"]).split(" ")[0],
            "description": dep.get("purpose", ""),
            "licenses": license_entries(dep.get("license", "NOASSERTION")),
            "externalReferences": [{"type": "vcs", "url": dep["source"]}] if dep.get("source") else [],
        }
        components.append(component)

    bom = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "serialNumber": f"urn:uuid:ravenengine-{datetime.datetime.now(datetime.timezone.utc):%Y%m%d%H%M%S}",
        "metadata": {
            "timestamp": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
            "tools": [{"vendor": "RavenEngine", "name": "generate_sbom.py", "version": "1.0.0"}],
            "component": {
                "type": "application",
                "bom-ref": f"pkg:generic/{project_name}@{project_version}",
                "name": project_name,
                "version": project_version,
                "description": "RavenEngine — low-resource-focused C++20 Vulkan game engine and editor",
                "licenses": [{"expression": "MIT"}],
            },
        },
        "components": components,
    }

    payload = json.dumps(bom, indent=2)
    if args.output == "-":
        print(payload)
    else:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(payload + "\n")
        print(f"SBOM written to {args.output} ({len(components)} components)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
