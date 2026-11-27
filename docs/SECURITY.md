# Security Policy

## Reporting a vulnerability

RavenEngine is a game engine/editor, not a network service; the principal risk surface is
**parsing of untrusted content** (scene JSON, asset files, mesh importers, Lua scripts) and the
**build pipeline** (vendored dependencies).

If you find a vulnerability, please report it privately by opening a GitHub Security Advisory on
the repository (Security → Advisories → "Report a vulnerability") or by contacting the maintainer
via the email on the publisher's GitHub profile. Do not open a public issue for
vulnerability details.

We aim to acknowledge within 7 days and to ship a fix or mitigation in the next tagged release.

## Supply chain

- All third-party dependencies are **vendored** under `third_party/` with pinned versions,
  recorded in `third_party/manifest.json` (with upstream source URLs and licenses).
- CI generates a **CycloneDX 1.5 SBOM** on every Linux run (`scripts/generate_sbom.py`);
  download the `sbom` artifact from the workflow run, or generate it locally:

  ```sh
  python3 scripts/generate_sbom.py --output sbom.cdx.json
  ```

- The SBOM can be scanned with any CycloneDX-aware scanner, e.g. `grype sbom:sbom.cdx.json`.

## Periodic audit procedure (maintainers)

1. Re-generate the SBOM and diff it against the previous release's SBOM.
2. For each component, compare the pinned version against upstream releases for known CVEs
   (GitHub advisory database, OSV).
3. If a component must be bumped: vendor the new version, update
   `third_party/manifest.json`, regenerate the SBOM, rebuild, and run the full test suite
   (`ctest --output-on-failure`).
4. Lua is embedded — audit any scripts loaded from untrusted projects before running them;
   the engine's Lua bindings should not grant filesystem or shell access to untrusted scripts.

## Supported versions

Only the latest release on `main` receives security fixes.
