# Releasing TBCAD

Release builds come from `.github/workflows/release.yml`. It never touches `ci.yml`.

## Making a release

1. Make sure `main` is green.
2. Tag and push: `git tag v0.1.0 && git push origin v0.1.0`.
3. The **Release** workflow builds Release with MSVC, runs every test, creates
   - `TBCAD-<version>-setup.exe` (Inno Setup installer), and
   - `TBCAD-<version>-portable.zip`,

   and attaches both to a **draft** GitHub Release. Read the draft, then publish it by hand.

To try the pipeline without a tag, run the workflow from the Actions tab (**Run workflow**). The
files appear as the `tbcad-windows-release` artifact and no release is created.

## What the installer does

- Installs for the current user by default (`%LOCALAPPDATA%\Programs\TBCAD`); the first page also
  offers an all-users install.
- Start menu entry, optional desktop shortcut.
- Optional `.tbcad` association (icon, double click opens in TBCAD).
- Optional "Open with" entry for `.dxf`: it converts the file with `tbcad-convert` next to the
  original and opens the result. TBCAD never becomes the default program for DXF.
- Uninstall removes everything it installed and the registry entries. Settings
  (`%APPDATA%\TBCAD`) and drawings are left alone.

Files shipped: `tbcad.exe`, `tbcad-convert.exe`, `tbcad-mcp.exe`, `assets\` (fonts, patterns), `LICENSE`,
`THIRD_PARTY_NOTICES.txt` (generated at build time from `third_party/notices`), and the user
guide in `docs\`.

## Version, icon

- The version comes from `project(TBCAD VERSION ...)` in the top `CMakeLists.txt` and is written
  into the exe's version resource (`packaging/windows/tbcad.rc.in`). Keep it in step with the tag.
- The icon is drawn by `packaging/make_icon.py` (original artwork). Run it to regenerate
  `packaging/windows/tbcad.ico`.

## Code signing (not done yet)

Windows SmartScreen warns about unsigned installers. Signing needs a code-signing certificate
(roughly 100 to 400 USD per year for an OV certificate; an EV certificate costs more and removes
the warning immediately). When the owner has one, add a step after the build that runs
`signtool sign` on `tbcad.exe`, `tbcad-convert.exe` and the installer, with the certificate kept
in a repository secret. Nothing else in the pipeline needs to change.

## Manual check on Windows (M4 gate)

1. Install the setup file. The app starts from the Start menu.
2. Double click a `.tbcad` file: it opens in TBCAD.
3. Right click a `.dxf`, **Open with > DXF drawing (import into TBCAD)**: a `.tbcad` appears
   beside it and opens.
4. Convert the owner's DXF samples with `tbcad-convert` and read the report.
5. Uninstall: nothing remains outside the user's documents and settings.
