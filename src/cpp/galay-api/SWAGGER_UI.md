# Built-in Swagger UI

The optional API library contains a fixed snapshot of the official
`swagger-ui-dist@5.17.14` distribution in `ui_assets.cc`. The HTML shell and
initializer are defined in `docs.cc`. Builds and default deployments require
no separate UI files, downloads, CDN, or resource generation step.

## Provenance

- Publisher: `swagger-api` (Swagger / SmartBear).
- Imported on 2026-10-07 without modifying the upstream browser assets.
- Package metadata: https://registry.npmjs.org/swagger-ui-dist/5.17.14
- Archive: https://registry.npmjs.org/swagger-ui-dist/-/swagger-ui-dist-5.17.14.tgz
- Upstream project: https://github.com/swagger-api/swagger-ui
- License: Apache-2.0; original `LICENSE` and `NOTICE` are included in the snapshot
  and served under the configured documentation path.
- npm `dist.shasum`: `e2c222e5bf9e15ccf80ec4bc08b4aaac09792fd6`.
- npm `dist.integrity`: `sha512-CVbSfaLpstV65OnSjbXfVd6Sta3q3F7Cj/yYuvHMp1P90LztOLs6PfUnKEVAeiIVQt9u2SaPwv0LiH/OyMjHRw==`.
- Archive SHA-256: `c57badf459aa6e65cc036b3862d0502a63f9a22546407ffcb0e64f85f816bb28`.

The snapshot includes CSS, the browser bundle, the standalone preset, two
favicons, `LICENSE`, `NOTICE`, this document as `README.md`, and `SHA256SUMS`.
Source maps, npm tooling, upstream HTML, the petstore initializer, and OAuth
redirect are not product dependencies. The initializer disables the external
validator, persisted authorization, and query configuration overrides.

The `api.transports` test verifies the seven upstream resources against pinned
SHA-256 values using actual HTTP responses. The browser acceptance script uses
the same checks and exercises the UI in desktop and mobile browsers.

Applications may explicitly use `docs(config, directory)` to load a complete
custom distribution at startup. Missing, empty, unreadable, or oversized files
fail before document routes are registered; no resources are filled from the
built-in snapshot. Successfully loaded bytes remain owned by the server.

## Updating The Snapshot

An intentional upstream upgrade must update the bytes in `ui_assets.cc`, the
embedded license and provenance, and the pinned hashes in
`test/cpp/api/transport_acceptance.cjs` together. Verify the package integrity
and original files before encoding them into C++ string literals. Run the docs,
embedded-resource, transport, installation, and browser acceptance tests after
the update. No build-time generator or external asset directory is retained.
