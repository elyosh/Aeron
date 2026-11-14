# OpenAL Soft default HRTF

`Default HRTF.mhr` is the default HRTF data set of OpenAL Soft, copied
unmodified from https://github.com/kcat/openal-soft at commit
`255a0e278578bb369cf1a095c7c9657bf449a80e` (`hrtf/Default HRTF.mhr`).

- Size: 382,557 bytes
- SHA-256: `40ed10f203488fdeca5f873bfd64f6fae232f4f7412ba07ae5e501ad882adf8c`
- Format: `MinPHR03`, 48,000 Hz, one ear (the other is its mirror image),
  64 taps, one field at 1.2 m, 13 elevations from -90° to +90°, 1,982
  responses.

The audio mixer embeds it at build time (see `cmake/embed_file.cmake`), so
headphone output needs no extra file.

## Credit and license

The data set is derived from subject D1 (a Neumann KU100 dummy head) of the
SADIE II database: C. Armstrong, L. Thresh, D. Murphy and G. Kearney, The
Audio Lab, University of York,
https://www.york.ac.uk/sadie-project/database.html.

OpenAL Soft provides it under the Apache License 2.0, in
`LICENSE.Apache-2.0.txt`.
