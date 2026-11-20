# mini_chromium Provenance

Reviewed: 2026-11-20

## Source

- Upstream project: <https://github.com/getsentry/mini_chromium>, Sentry's mirror of
  <https://chromium.googlesource.com/chromium/mini_chromium>
- Source ref: exact commit `2f5c168fa462cf3836c4a7801b4f4955496d975d` (2026-08-03), the revision
  the locked Crashpad commit names as its `third_party/mini_chromium/mini_chromium` submodule
- Archive: `https://codeload.github.com/getsentry/mini_chromium/tar.gz/2f5c168fa462cf3836c4a7801b4f4955496d975d`
- SHA-256 of the acquired archive:
  `00f60d55f810dd66a64cf882cb8f4d241809e9e14c24baaaa96031b66ca79ac4`
- Exact archive size: `142411` bytes

## Acquisition Record

- Acquired 2026-11-20 over HTTPS (curl). GitHub reports the commit's signature as verified.
- Archive inspection: 177 entries, **0 symbolic links, 0 absolute paths, 0 `..` paths**.
- GitHub publishes no detached signature for this commit archive (`provenancePolicy: not-published`).

## Status

- NOT QUALIFIED. Acquisition provenance only. No production prefix manifest exists. A source
  provider of the `crashpad` component: the superbuild places it in Crashpad's tree, and
  Crashpad's build compiles it.
