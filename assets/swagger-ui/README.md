# Swagger UI Distribution

Pinned official npm package: `swagger-ui-dist@5.17.14`.
Imported on 2026-10-07 without modifying the upstream bytes.

- Publisher: `swagger-api` (Swagger / SmartBear).
- Package metadata: https://registry.npmjs.org/swagger-ui-dist/5.17.14
- Archive: https://registry.npmjs.org/swagger-ui-dist/-/swagger-ui-dist-5.17.14.tgz
- Upstream project: https://github.com/swagger-api/swagger-ui
- License: Apache-2.0; original `LICENSE` and `NOTICE` are included.
- npm `dist.shasum`: `e2c222e5bf9e15ccf80ec4bc08b4aaac09792fd6`.
- npm `dist.integrity`: `sha512-CVbSfaLpstV65OnSjbXfVd6Sta3q3F7Cj/yYuvHMp1P90LztOLs6PfUnKEVAeiIVQt9u2SaPwv0LiH/OyMjHRw==`.
- Archive SHA-256: `c57badf459aa6e65cc036b3862d0502a63f9a22546407ffcb0e64f85f816bb28`.

`SHA256SUMS` records the SHA-256 of each imported resource and license file.
Verify the vendored bytes with `sha256sum --check SHA256SUMS` in this directory.
The npm archive's SHA-512 was checked against its registry integrity value.
When the optional API module is enabled, the build checks all seven upstream
entries before generating its embedded resources. Missing or empty files,
invalid checksum entries, and hash mismatches fail the generation step. The
upstream files and checksum manifest are not rewritten. No resources are
downloaded during the build or at runtime.

Only the required CSS, browser bundle, standalone preset, two favicons and
license files are vendored. Source maps, npm tooling, upstream HTML, petstore
initializer and OAuth redirect are not product dependencies. The five browser
assets, `LICENSE`, `NOTICE`, this `README.md`, and `SHA256SUMS` are compiled into
the optional API library. Galay generates its own same-origin HTML and
initializer before registering the document routes. `HttpSwagger{}` and
`install_docs` use only these embedded bytes: deployments need no UI directory,
source-tree paths, or particular working directory. The nine resources are
served beneath the configured UI path, including the license, provenance, and
checksum files. There is no CDN or missing-file fallback.

Applications that explicitly want file-backed resources may implement a custom
policy using `install_docs_from_directory`. That installer requires all nine
named files to be nonempty regular files, loads them before registering any
document routes, and reports file errors without filling gaps from the embedded
resources. It does not certify custom files against the build-time official
checksums. Loaded bytes remain available for the current process if the directory
is subsequently moved; a later startup still requires the explicit directory.

The initializer explicitly sets `validatorUrl: null`,
`persistAuthorization: false` and `queryConfigEnabled: false`. External
documentation URLs in an OpenAPI document are display links, not startup
fetches. Swagger UI's upstream configuration reference is
https://swagger.io/docs/open-source-tools/swagger-ui/usage/configuration/ .

License, version, provenance, and checksum metadata are also installed under
`${prefix}/share/galay/swagger-ui`. This directory is not a dependency of the
default document service or of `find_package(galay)`. Consumers need no resource
path variable or compile definition, and the package does not check that directory
at runtime or package discovery. Normal executable and linked-library deployment
requirements still apply.
