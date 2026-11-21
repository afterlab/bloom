# web-platform-tests WebVTT parsing corpus provenance

- Source repository: https://github.com/web-platform-tests/wpt
- Revision: `6852422b49e7dc8037d7f130ee07cf991fb73f05` (`master` HEAD when resolved)
- Date acquired: 2026-11-21
- License: see `LICENSE.md` in this directory (verbatim copy of the repository `LICENSE.md`).

## Layout

- `cue-text/*.dat`: byte-exact copies of `webvtt/parsing/cue-text-parsing/dat/*.dat`.
  `cue-text/cases.json` maps each file to its number of `#data` cases.
- `file-parsing/<case>.vtt`: for each upstream `support/<case>.test`, the WebVTT payload after
  the `===` line, decoded exactly as upstream `tools/build.py` does
  (`bytes(text, "utf-8").decode("unicode_escape")`, written as UTF-8). Upstream
  `support/*.vtt` files (invalid-signature cases) are copied byte-exact.
- `file-parsing/expected.json`: one entry per case. `signatureInvalid` is true only for the
  upstream `support/*.vtt` files, which upstream expects to fire an error event; every `.test`
  file expects a load event (its harness fails on error).

## Conversion of the JavaScript assertions

The assertions in each `.test` file are executed by `node` against a recording stand-in for
the cue list, so loops and helpers are evaluated rather than parsed. Only asserted
properties are present. `cueCount` is `assert_equals(cues.length, N)`. Cue entries are
positional (index = cue index; an unasserted cue is `{}`). Region assertions are expressed
inline: `"region": null`, `"region": {"lines": 2}` (asserted region properties),
`regionPresent` (`assert_true(!!region)`), `regionSameAsCue` (the cue's region is the same
object as that cue's region) and top-level `regionsDistinct` (`assert_not_equals` pairs of
cue indices). Property values keep their JavaScript number or string type.

- Hand-converted cases: none.
- Unconverted (raw assertion kept in `unconverted`): stylesheets (asserts `document.styleSheets`, which is not a cue property).

## Upstream files used (SHA-256 of the upstream bytes)

- `LICENSE.md`  5fac07febb0e2a97fb0d7b0def149ec08b642e1ba4b9c345283ab1cbd2af6570
- `webvtt/parsing/cue-text-parsing/dat/entities.dat`  e2c5e57848249afe98b05c67049773cff2d749b9efffd8df7c7674b7034cba40
- `webvtt/parsing/cue-text-parsing/dat/tags.dat`  427632cdc562cc463efb90c16ec622cca7217746196d75836a279f336bb34f9d
- `webvtt/parsing/cue-text-parsing/dat/text.dat`  480cd34cac200ab7e263bd34a86576e24e36e5cb5cd31ad4f38ce2c8498aee1f
- `webvtt/parsing/cue-text-parsing/dat/timestamps.dat`  9c0dd1b8d75a050d9c790537d7e0a4f5a8b87750be11403e64e1745d42b6a545
- `webvtt/parsing/cue-text-parsing/dat/tree-building.dat`  706d81da28994ab09baa9fe11042900fad0029d2e05a0fdf082959e319123159
- `webvtt/parsing/file-parsing/signature-invalid.html`  fdc9fd449bd376dabd14e9ccfa7efa108640a4e8f1b41342f76982b83c5de143
- `webvtt/parsing/file-parsing/support/arrows.test`  cd18585ca662a1deee4cf8beb3a742f9a242bb1486dabf0fe353cdc5a8ab41cf
- `webvtt/parsing/file-parsing/support/comment-in-cue-text.test`  056085738e6aca94c980c82816f860050f60613bffc0b9d4f6cc93d62180448c
- `webvtt/parsing/file-parsing/support/empty.vtt`  e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
- `webvtt/parsing/file-parsing/support/header-garbage.test`  6bafe371f1402ef2c5af491283f982a4031dd264c3736156505aa9ecff8ce22b
- `webvtt/parsing/file-parsing/support/header-space.test`  e4dc6f2a5b7060eaf18daf3975c074a14cfcc07996c2f99031f74fa6cc7e260c
- `webvtt/parsing/file-parsing/support/header-tab.test`  eb9ba38c4994de3e7603848000847050872a075568f2033e411cde33120e8c04
- `webvtt/parsing/file-parsing/support/header-timings.test`  303b363546d4ee06b6ad125a256e4b07a02b15fcfdd3d867bf197f7595f37420
- `webvtt/parsing/file-parsing/support/ids-across-region-block.test`  da3668dd9e5b28d367b897bfec8c752b2988c1267f176d21bed0da8373953f78
- `webvtt/parsing/file-parsing/support/ids-across-style-block.test`  fa85ca7eb81378c36784a8fbcaa4bb6e0c087024decc16e6adddfcf97f99652b
- `webvtt/parsing/file-parsing/support/ids.test`  3a0ea7d4a6b6f368d82b573c568734d33f997e3da453b1e0a779b9f5dbc819f4
- `webvtt/parsing/file-parsing/support/newlines.test`  1ec31449aafc965d38b5f2138b7d8f84ebf21e33bbae959208b824dc2e789280
- `webvtt/parsing/file-parsing/support/nulls.test`  c3fcc670ae74387d9503fc2674f4ee682f47d95f36cae77112468c36e4c824af
- `webvtt/parsing/file-parsing/support/regions-id.test`  153c172f8fe343e7b298c687513b80aed80de164920b5a24b97eb2ddcd99247d
- `webvtt/parsing/file-parsing/support/regions-lines.test`  dad49e7b160cc7a6e9d2b279c68f10990ca21913617b3938611bdeb8fe88937c
- `webvtt/parsing/file-parsing/support/regions-old.test`  258e3fa74dc4495bbfab728b678025d0c02b89aa38f897fe5902126aa84c3af1
- `webvtt/parsing/file-parsing/support/regions-regionanchor.test`  c1cba27ec78641c920e5026a301623b67582697bbc302bc148bc93812e34bf70
- `webvtt/parsing/file-parsing/support/regions-scroll.test`  4b6e1b9c2f4e070f27feeccf6c20490b4394b404a5eb5119984c2a8764b53c53
- `webvtt/parsing/file-parsing/support/regions-viewportanchor.test`  9a7563bbdfb48438bf85eaffa0a0cdcaef6a869a82d2c71d14e9450c7766ec3a
- `webvtt/parsing/file-parsing/support/settings-align.test`  0a5d42f755ef87f499ead52063a14badcf2a62e17ac542607c5caae9ac212768
- `webvtt/parsing/file-parsing/support/settings-line.test`  fccc6bd175049c07ac3482ca2f193554e732cdb2c70d5dfb325b801367654c90
- `webvtt/parsing/file-parsing/support/settings-multiple.test`  2c6d2dc75dd51f8d44f6788206aecf46f08fe7b65c9649dbac2948d53e0bf9c4
- `webvtt/parsing/file-parsing/support/settings-position.test`  ed814e1c867ae9ba56dc0e81dd43f0f3806dfa4be10a4d85c7225a72271d53e7
- `webvtt/parsing/file-parsing/support/settings-region.test`  c24e10bd02e1b7ffa83a3cc3db110b84b67f14d1c9fd1884e88af9614c953ed4
- `webvtt/parsing/file-parsing/support/settings-size.test`  625621e8de549f9f7dc77deab558172cc382f6a534954379bee222fea766824d
- `webvtt/parsing/file-parsing/support/settings-vertical.test`  fa1f6b9a242e347bd3b036a5b40fdff2f5b12852c53d54ebf73286f0fe7e74b7
- `webvtt/parsing/file-parsing/support/signature-bom.test`  67972c42d78223174556e5efd28f9855bc7f29c7020707342eba97333e2c1945
- `webvtt/parsing/file-parsing/support/signature-formfeed.vtt`  a12a7681fb26bdb114efad57eb615ec0f042591051270837b13e92140967773d
- `webvtt/parsing/file-parsing/support/signature-invalid-whitespace.vtt`  0664f525b06ec4659d2d110e5cbc492e4821dd246f644b65e01e3793e488e855
- `webvtt/parsing/file-parsing/support/signature-invalid.vtt`  d4832295f3ad7b9a95a3b98cc34dbe0c44404a5fc8429ffd46141a5390c2e455
- `webvtt/parsing/file-parsing/support/signature-lowercase.vtt`  1e1358c1aa811e3c1dcf19b32cb8bc2e4a05aefe72407d862d5842bf2adca5f8
- `webvtt/parsing/file-parsing/support/signature-missing-whitespace.vtt`  72ba7f91303d12713655aeab8ac6e472d6eb95b7f689d24aea1cc7d329695724
- `webvtt/parsing/file-parsing/support/signature-missing.vtt`  9a21137cf03abbf8d71907ff1fb7404616d47cf37dafba8385da23d8f2d5e125
- `webvtt/parsing/file-parsing/support/signature-no-newline.test`  fb5afe87060222255b2381fc57d9d4aef1d41caf167bb932497084ca81db7a07
- `webvtt/parsing/file-parsing/support/signature-null.vtt`  f8e700b0d15e5b552762175a2403b8b051c0cbbbd45b107ae8f65be7bd3d9495
- `webvtt/parsing/file-parsing/support/signature-partial.vtt`  5007595a7b099d13981aca23d4ca97a43d99e19d8ac4cc0333005f2dc7edac1a
- `webvtt/parsing/file-parsing/support/signature-space-no-newline.test`  14fc7c3bf2b7d3895d261a09c62aa0f5c2856cfc6d3639341086d2d62d23df31
- `webvtt/parsing/file-parsing/support/signature-space.test`  f575eb6eac04fe25d7d92d2814f14af861b24240b0a6338a5b07768cac7e43a2
- `webvtt/parsing/file-parsing/support/signature-tab-no-newline.test`  7a484cda2feb53d07b21cc44f973cf6696ccb9304eea1d1d4e4b65e759ae9874
- `webvtt/parsing/file-parsing/support/signature-tab.test`  8e0fcfba72b601cae6e4da83eb79f3c7466e485435eb28e0e3df1a175f48b33a
- `webvtt/parsing/file-parsing/support/signature-timings.test`  6ef812352587093454fee6ed04e3da98d85a2dce83697b2d8445318e8f2dd9fa
- `webvtt/parsing/file-parsing/support/signature-two-boms.vtt`  22aee1ef667e90e7e07e70b9a7ea662fc049940d8d5747af3ddddd7dfc88a55f
- `webvtt/parsing/file-parsing/support/signature-websrt.vtt`  e6d1613896f7da9591d500875598de3895d133025f77919eac5b7bc0fecb33ab
- `webvtt/parsing/file-parsing/support/stylesheets.test`  ae5cfab73016ee74756f67a5bc601b59839b5332281a277ac5e792b95e60dfc9
- `webvtt/parsing/file-parsing/support/timings-60.test`  c21be413d40e3b4f7693104d197ea48deb78ef15c1ccb4f6dc3386ec585e9fec
- `webvtt/parsing/file-parsing/support/timings-eof.test`  16a11be99e2cf1d5c2bdbce7883bf7d2a114e51bee9a5e8f8ee5f2b75be95dbd
- `webvtt/parsing/file-parsing/support/timings-garbage.test`  842578e55fe41637c48cd08733b3f75fe6b5d7fbc13cc3cfb281746a7df25b1b
- `webvtt/parsing/file-parsing/support/timings-negative.test`  594c96866a424209dc5aaef93a602043588efdcf2a1aab7664381a7cfcdbd9b0
- `webvtt/parsing/file-parsing/support/timings-omitted-hours.test`  28afb3f628c8dd084d8575f19cebabe9006692050e79649c7fb40d2528ebf859
- `webvtt/parsing/file-parsing/support/timings-too-long.test`  91aad120886588b53fdfa24f33e994667e87eaf74e5c2f4ae9449bee97aee116
- `webvtt/parsing/file-parsing/support/timings-too-short.test`  c0472591ffd78151402f8a10db00be84d0ed1e35d2d89f07d2517da72e37ad35
- `webvtt/parsing/file-parsing/support/whitespace-chars.test`  3a09caa988778641bce33c8a5d47ab4c357683ca50d7c0a830be9bb58460dcad
- `webvtt/parsing/file-parsing/tools/build.py`  5bb5e4a83b02d0bb0e5cf38cf7d04a6f04dd5f5c25f21490c54bdec826607fe1

## Regeneration

Requires `python3` and `node` on PATH and network access.

    python3 tools/subtitles/import_wpt_webvtt.py
    python3 tools/subtitles/import_wpt_webvtt.py --check
