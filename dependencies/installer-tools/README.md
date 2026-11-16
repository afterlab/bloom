# Installer tools

The Windows installer of Bloom is built with the Inno Setup compiler. `innosetup.json` is the
only place it is written: the release, its address, SHA-256 and size, its detached signature and
the key that made it, the source commit it was built from, its license file as checked in here,
and the switches the fetch runs it with.
`dependencies/superbuild/projects/installer-tools-fetch.cmake` reads it.

Inno Setup is taken as published, by checksum, and is not built here. On Windows the superbuild
puts it into the dependency prefix's `innosetup` directory. Nothing of Bloom links it.

| Item | Release | Platform | License | In the prefix |
| --- | --- | --- | --- | --- |
| Inno Setup 6 | 6.7.4 | Windows | The Inno Setup license (`licenses/innosetup/license.txt`), a permissive license with conditions on redistribution | `innosetup/ISCC.exe` and what the compiler needs |

## What is in a Bloom package

Inno Setup is a build tool. Nothing of it is in a Bloom package except the installer's own
setup stub, which the compiler copies into every installer it makes. The license's conditions
apply to that stub: the copyright notices stay in place in the binary, and a changed version is
marked as changed. `THIRD_PARTY_NOTICES.md` carries the notice.

## How the fetch installs it

The published file is itself an installer, built for 32-bit Windows. On Windows the fetch checks
its size and SHA-256, then runs it with the switches in the record:

    innosetup-6.7.4.exe /VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART /NOCANCEL /CURRENTUSER
        /NOICONS /PORTABLE=1 /DIR=<destination>

The switches come from the Inno Setup documentation for the Setup program
(`https://jrsoftware.org/ishelp/topic_setupcmdline.htm`):

- `/VERYSILENT`: "When a setup is very silent this installation progress window is not
  displayed."
- `/SUPPRESSMSGBOXES`: "Instructs Setup to suppress message boxes. Only has an effect when
  combined with /SILENT or /VERYSILENT."
- `/SP-`: "Disables the This will install... Do you wish to continue? prompt at the beginning of
  Setup."
- `/CURRENTUSER`: "Instructs Setup to install in non administrative install mode. Only has an
  effect when the [Setup] section directive PrivilegesRequiredOverridesAllowed allows the
  commandline override." The Inno Setup installer's own script sets
  `PrivilegesRequiredOverridesAllowed=dialog`, and "Allowing dialog automatically allows
  commandline" (`https://jrsoftware.org/ishelp/topic_setup_privilegesrequiredoverridesallowed.htm`),
  so the switch works on this installer.
- `/NOICONS`: "Instructs Setup to initially check the Don't create a Start menu folder checkbox
  on the Select Start Menu Folder wizard page."
- `/DIR="x:\dirname"`: "Overrides the default directory name displayed on the Select Destination
  Location wizard page."
- `/PORTABLE=1` is not in the general documentation. It is a switch of the Inno Setup installer
  itself: its script (`setup.iss` and `isportable.iss` at the recorded commit) shows the text
  "/PORTABLE=1 Enable portable mode." in its help and, in that mode, sets `Uninstallable=no`
  (no uninstall entry in the registry), skips the Start menu page, removes the desktop icon and
  file association tasks, and does not reuse a previous directory. With `/DIR`, the tool lands
  only where it is told to.
- `/NORESTART` and `/NOCANCEL` keep a silent run from restarting the machine or being cancelled.

The installer exits with 0 when it succeeded (`https://jrsoftware.org/ishelp/topic_setupexitcodes.htm`).
The fetch stops on any other exit code, or when `ISCC.exe` is not in the destination afterwards.
The fetch step was written from the documentation and the installer's script. It was not run on
Windows when it was written.

On a host that is not Windows the fetch checks the checksum, says that the tool is unpacked on
Windows only, and installs nothing.

## Run the compiler

Per the compiler documentation (`https://jrsoftware.org/ishelp/topic_compilercmdline.htm`),
`iscc [options] <script name>`, with these options (short options also accept `/` as the
prefix):

- `--output-dir=<path>`, `-o<path>`: "Outputs files to specified path (overrides OutputDir)."
- `--output-filename=<filename>`, `-f<filename>`: "Specifies an output filename (overrides
  OutputBaseFilename)."
- `--define=<name>[=<value>]`, `-d<name>[=<value>]`: "Emulates #define public <name> <value>"
  (the preprocessor's page, `https://jrsoftware.org/ispphelp/topic_isppcc.htm`).
- `--quiet`, `-q`, `--no-signing`, `-ns`, `--version`: quiet output, no signing, print the
  compiler engine version.
- Exit codes: 0 success, 1 invalid command line or a fatal error, 2 the compile failed.

In the script, `PrivilegesRequired=lowest` makes the installer run for the current user without
a request for administrative rights: "When set to lowest, Setup will not request to be run with
administrative privileges even if it was started by a member of the Administrators group and
will always run in non administrative install mode."
(`https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm`). The default is `admin`.
A built installer accepts the same Setup switches as above, so `/VERYSILENT /SUPPRESSMSGBOXES
/NORESTART` installs it silently.

## Signatures of the download

The release has a signature file next to the installer, `innosetup-6.7.4.exe.issig`, in the
Inno Setup project's own format (`issig-v2`). It names the file, its size and SHA-256, and the
key id `def020ed...`. The key id is the one of the public key file `def02.ispublickey` in the
project's source at the recorded commit, which is checked in here as
`signing-publickey.ispublickey`. The signature is verified with the project's `ISSigTool`, a
Windows program, so it was not verified when the record was made. What was verified: the file's
size and SHA-256 are the ones in the signature file and on the release page.

The installer also carries an Authenticode signature (its PE file has a certificate table).
That was not verified either, as the check needs Windows.
