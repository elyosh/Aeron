# OpenXR headers

The headers of the Khronos OpenXR SDK, release 1.1.65, from the `include/openxr`
folder of `https://github.com/KhronosGroup/OpenXR-SDK`. This is the version
that the pinned SDL builds with. They are licensed under Apache-2.0 or MIT.

aeron builds against these headers and loads the OpenXR loader at run time
through SDL, so it does not link the loader.
