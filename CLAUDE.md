# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

This is a Visual Studio C++ InSAR processing toolkit. The README describes support for SAR image co-registration, interferometric phase deramping, denoising/filtering, phase unwrapping, geocoding, DEM generation, SBAS-InSAR processing, and related format conversion for datasets including Sentinel-1, TerraSAR-X/TanDEM-X, ALOS2, COSMO-SkyMed, LuTan-1, Hongtu-1, and Fucheng-1.

## Build commands

This repository is organized around `InSAR.sln` and `.vcxproj` files, not CMake or Make.

Use MSBuild from a Visual Studio Developer shell with the Visual Studio C++ toolchain installed:

```bash
MSBuild.exe InSAR.sln /m /p:Configuration=Debug /p:Platform=x64
MSBuild.exe InSAR.sln /m /p:Configuration=Release /p:Platform=x64
```

Build one project while iterating:

```bash
MSBuild.exe InSAR.sln /m /t:Registration /p:Configuration=Debug /p:Platform=x64
MSBuild.exe InSAR.sln /m /t:FormatConversion /p:Configuration=Release /p:Platform=x64
```

Clean a configuration:

```bash
MSBuild.exe InSAR.sln /t:Clean /p:Configuration=Debug /p:Platform=x64
```

There is no discovered automated unit-test runner. The `test`, `test2`, and `test3` projects are console/application harnesses rather than a conventional test framework. The only active `main` found is in `unziptool/unziptool.cpp`; much of `test/test.cpp` is commented exploratory code. Build or run the relevant harness manually after changing processing code.

Most x64 project outputs are configured to `bin/`; debug DLL targets usually use the `_d` suffix, while release targets use the project name.

## External dependencies and environment

The Visual Studio projects currently contain absolute include/library paths. Common dependencies referenced across projects include:

- OpenCV, with `opencv_world450d.lib` for debug and `opencv_world450.lib` for release via `include/Package.h`.
- HDF5 for format conversion and some processing modules.
- GDAL (`gdal_i.lib`) for geospatial/raster I/O.
- Eigen for numeric operations in some modules.
- OpenMP enabled in many x64 configurations.
- libtorch for the `Filter` project.

If a build fails before compilation, inspect the relevant `.vcxproj` include and library paths first; many Release paths point at `D:\softwarepackages\...`, while many Debug x64 paths point at `D:\SRC\...`.

## Architecture

Public APIs live in `include/` and are exported with `InSAR_API`, defined in `include/Package.h` as `__declspec(dllexport)`. `Package.h` also centralizes shared constants, OpenCV includes, OpenMP include, and basic SAR geometry structs such as `Position`, `Velocity`, and `OSV`.

The main processing modules are split into one directory per Visual Studio project. Each module generally has a public header in `include/`, an implementation `.cpp` in its project directory, and a DLL entry point file:

- `ComplexMat` provides the complex matrix abstraction used throughout the processing pipeline, typically wrapping separate OpenCV `Mat` real/imaginary components.
- `Utils` contains general image/matrix/SAR utility routines and depends on `ComplexMat`.
- `FormatConversion` handles project XML/HDF5/raster/SAR format conversion and includes sensor-specific readers/utilities. It also vendors/compiles TinyXML sources.
- `Registration` implements pixel/subpixel co-registration, interpolation, FFT helpers, DEM-to-SAR positioning, offset fitting, and resampling.
- `Deflat`, `Filter`, `Unwrap`, `Dem`, `SBAS`, `simulation`, and `Evaluation` implement the corresponding InSAR processing stages or analysis tools.
- `unziptool` is a small console application project that builds an `unzip` executable.
- `test`, `test2`, and `test3` are ad hoc harness projects for manual experiments and integration checks.

Most modules exchange image data as OpenCV `Mat` and complex SAR data as `ComplexMat`. Many APIs return `int`, with comments indicating `0` for success and `-1` for failure.

## Code organization notes

Header files use Windows-style relative includes such as `..\include\Package.h`, and project files also reference source files from sibling directories, especially TinyXML files from `FormatConversion`. Keep this project-file layout in mind when moving files or adding sources; a source added to disk will not be compiled unless the relevant `.vcxproj` includes it.

Several comments and identifiers are domain-specific and some Chinese comments may display with mojibake depending on encoding. Preserve existing file encoding and style when editing legacy files.

## File encoding rules

Source files are unified as UTF-8 with MSVC `/utf-8` compiler flag enabled. When editing files, follow these rules to avoid introducing encoding issues:

1. **Confirm encoding before editing**: Do not assume all source files are UTF-8. Before editing `.cpp` / `.h` files containing Chinese text, confirm their actual encoding (UTF-8 / GBK / UTF-8 BOM) to avoid writing with the wrong encoding and causing garbled output.

2. **Explicitly specify encoding for automated modifications**: Do not use PowerShell default redirection or `Get-Content` / `Set-Content` to batch-modify source code. Use Python or IDE tools with explicit `encoding="utf-8"` for read/write operations. When processing historical GBK files, identify the input encoding first; never read blindly as UTF-8.

3. **Save new or modified source files as UTF-8**: With the `/utf-8` compiler flag enabled, new files do not need a BOM.

4. **Hex escapes only as last resort**: Do not proactively convert Chinese comments or strings to `\xe6...` form. Only consider this when specific strings are repeatedly corrupted across toolchains or cross-platform migration, and obtain user consent first.

5. **External library path encoding follows actual API conventions**: Do not blanket-convert all external library paths to GBK. Check whether the library supports UTF-8 paths or provides wide-character interfaces. For OpenCV, HDF5, GDAL, etc., handle path encoding based on actual project usage.
