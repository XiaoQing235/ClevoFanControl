# README Optimization Design

Date: 2026-07-25
Status: Approved

## Objective

Rewrite the repository README so it is a reliable entry point for both users and
developers. The README must describe the current implementation, give a usable
installation path, explain important hardware and runtime limitations, and
document the available build and test entry points.

The change is documentation-only. It must not alter application behavior,
configuration formats, source files, binaries, or dependency files.

## Audience

- Users downloading the project and trying to control their Clevo-based laptop
  fans.
- Developers who need to inspect, build, or test the Win32/MFC project.

## Proposed README Structure

1. Project title, concise description, supported scope, and compatibility caveat.
2. Feature overview based on the current UI and implementation.
3. Quick start covering packaged files, driver installation, administrator
   privileges, executable location, and the first save operation.
4. Configuration and everyday usage, including curve editing, control modes,
   fan status, tray behavior, startup, and UI font size.
5. Preset behavior, including process-name matching, priority, automatic
   switching, storage, and the preset limit.
6. Known limitations and safety notes.
7. Developer build and test instructions for the existing solution.
8. References and acknowledgements.

## Factual Content Boundaries

The README may document the following verified facts:

- The project is a Windows Win32/MFC application with Win32 Debug and Release
  configurations.
- The runtime package includes `NTPortDrvSetup.exe`, `ClevoEcInfo.dll`, and
  `NVGPU_DLL.dll`; `ClevoEcInfo.dll` must be available beside the executable.
- The application requires administrator privileges and loads hardware access
  through the bundled EC DLL.
- CPU and GPU curves accept 2 through 16 points. Curve temperatures range from
  30 through 100 and duty values range from 0 through 100.
- Linear mode, transition temperature, soft control, forced cooling, tray
  behavior, startup behavior, and UI font sizes from 8 through 16 are current
  settings.
- Presets support up to 32 entries, match running executable names with `*` and
  `?` wildcards, and use list order as priority. Preset switching is optional
  and disabled by default.
- Global settings and presets are stored in `ClevoFanControl.json` and
  `ClevoFanControl.presets.json` beside the executable.
- The solution uses the existing MFC/v145 project configuration and the
  repository contains a standalone `FanCurveModelTests` test executable/source.

The README must remove the obsolete GPU frequency-limit feature description and
must not claim a complete hardware compatibility list, a release version,
automated installation, or a license that is not present in the repository.

## Writing and Scope Rules

- Use Chinese prose with the English UI labels and file names where that helps
  users match the application.
- Prefer short, task-oriented sections and Markdown links for external
  references.
- Explain behavior and risks without marketing language or unsupported claims.
- Preserve the existing acknowledgements and current working-tree README edits
  unless they conflict with verified project behavior.
- Do not add screenshots, badges, generated assets, or unrelated documentation.

## Acceptance and Verification

The finished README must:

- Let a new user find the install and launch path without reading source code.
- Let a developer identify the solution, platform, toolchain expectation, and
  test entry point.
- Use file names and feature names that match the repository.
- Clearly distinguish current features from removed historical features.
- Render as valid, navigable Markdown with no obvious broken links or malformed
  lists.

Verification consists of reviewing the final README against the source and
project files, checking `git diff --check`, and confirming that only README
content is changed after the design document is committed.
