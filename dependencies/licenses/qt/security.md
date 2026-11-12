# Qt Security Review

Reviewed: 2026-11-12

## Reachability

Qt runs in the desktop application's process. Its inputs there are the user's own actions, the
SVG icons Bloom ships, and one network path: the font catalog and font files the font picker
downloads over HTTPS through `QNetworkAccessManager`. Project files, media and scripts are not
parsed by Qt. The command line and MCP programs and the media and color workers do not link Qt.

## Bounded Review

The Qt Project's own list of security advisories
(`https://wiki.qt.io/List_of_known_vulnerabilities_in_Qt_products`) was read at review time.

- Fixed in 6.11.3 or earlier, among the advisories for parts Bloom uses: CVE-2026-95620 and
  CVE-2026-96432 (HTTP/2 in `QNetworkAccessManager`), CVE-2026-76151 (Cache-Control header),
  CVE-2026-105946 (NTLM authentication), CVE-2026-105924 and CVE-2026-107157 (CBOR),
  CVE-2026-101201 (JSON), CVE-2026-16762 and CVE-2026-78253 (`QXmlStreamReader`),
  CVE-2026-8168 and CVE-2026-6210 (SVG), CVE-2026-107479, CVE-2026-107590, CVE-2026-107472 and
  CVE-2026-9499 (`QTextCodec`).
- **CVE-2026-116337 is not fixed in 6.11.3.** It is uncontrolled memory use in Qt Network's
  HTTP response handling: a malicious or compromised server can make the application run out
  of memory (denial of service). The advisory lists 6.8.10 and 6.12.1 as fixed and no release
  of the 6.11 series. At review time neither fixed version had open-source binaries for all
  three platforms (6.8.10 is a commercial long-term-support release; no 6.12.1 binaries or
  source archive were published), so no locked version without it was available. Bloom reaches
  the affected code only when the font picker downloads from the font catalog's hosts over
  HTTPS with certificate verification; Bloom's transport refuses a response larger than its
  limit and has a transfer timeout, but the advisory does not say whether those bound the
  memory use, so this is recorded as an accepted risk (`cve-2026-116337`), not as mitigated.
  The lock must move to a fixed open-source release when one has binaries for all three
  platforms.
- CVE-2026-15037 (XML injection when serializing with QDom, fixed in 6.12.0 only) is in the Qt
  Xml library, which Bloom neither links nor carries (`cve-2026-15037`, not affected). The
  other advisories of 2026 not listed above are in modules Bloom does not use (HTTP Server,
  WebSockets, Qt 3D, CoAP, MQTT, Qt 5 compatibility, VNC server, Qt Quick, NFC).
- The bundled third-party code in the carried libraries (among it libpng, libjpeg, FreeType,
  HarfBuzz, PCRE2, zlib) is the version this Qt release ships; the bill of materials inside the
  binaries records each version. They were not reviewed one by one.
- This is a **bounded review, not a proof of zero vulnerabilities**. No NVD/CVE sweep, scanner
  or fuzzing was performed.

## Disposition

- Accepted for non-release intake as an unqualified component, with the open advisory above
  stated. This record does not qualify it for release.

## Security Release Tracking

- Advisory source: <https://wiki.qt.io/List_of_known_vulnerabilities_in_Qt_products>
- The Qt Project's list of advisories with the releases that fix each. See "Security release
  tracking" in `docs/architecture/dependency-intake.md`.
