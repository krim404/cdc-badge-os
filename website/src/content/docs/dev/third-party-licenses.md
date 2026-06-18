---
title: Third-party licenses
description: Open-source libraries bundled or linked by the image viewer and their licenses.
---

The image viewer links the following third-party libraries. Their licenses are
permissive and allow redistribution, including in the firmware binary.

## libjpeg (Independent JPEG Group)

Baseline and progressive JPEG decoding use the Independent JPEG Group's libjpeg
(version 9f), vendored under `components/libjpeg/`. This software is based in
part on the work of the Independent JPEG Group. The full IJG license is in
`components/libjpeg/README` ("LEGAL ISSUES"); it permits use for any purpose
provided that README is distributed with the source.

Project additions, kept separate from the pristine upstream sources:

- `jconfig.h` - build configuration for the ESP toolchain.
- `jmem_psram.c` - the system-dependent memory manager, allocating from PSRAM
  (replaces the upstream `jmemnobs.c`, which is left unmodified).
- `CMakeLists.txt` - ESP-IDF component build.

## libpng

PNG decoding uses `espressif/libpng` (libpng 1.6), pulled by the ESP component
manager. Distributed under the PNG Reference Library License (libpng license).

## zlib

PNG inflate uses `espressif/zlib`, a dependency of libpng. Distributed under the
zlib license.
