# Third-party components

The x64 rewrite uses Rust dependencies pinned in `Cargo.lock`. Copies of the dependencies' available license/notice files are in `licenses/`, including build dependencies for reproducibility.

- fltk-rs / fltk-sys / cfltk: MIT. The bundled FLTK library uses LGPL with the static-linking exceptions in `licenses/FLTK-COPYING.txt`. FLTK also includes PNG, JPEG and zlib code; their notices are included.
- serde, serde_json, windows-sys, libloading, sha2 and their transitive dependencies retain their upstream notices. Refer to the versioned files in `licenses/`.
- The application icons in `assets/` originate from the existing ClevoFanControl project. This rewrite does not relicense inherited materials or assert that upstream proprietary binaries are open source. The legacy C++ source and unused vendor binaries have been removed from the current source tree and remain in Git history.
- InsydeDCHU.dll and AcpiBridge.sys are installed separately by the manufacturer's Control Center package. They are proprietary and are **not** included in the new release archive. This application only locates and calls supported installed binaries.

FLTK binary source: the fltk-rs 1.5.23 `fltk-bundled` build mechanism. The exact Rust dependency graph is locked in Cargo.lock. A distributable package consists of the executable, README, this notice and the licenses directory; it does not include the old NTPort, ClevoEcInfo or NVGPU files.
