#pragma once

// How a program's file name ends on this platform: ".exe" on Windows, nothing elsewhere. A string
// literal, so it joins the literal before it: "glslangValidator" BLOOM_EXECUTABLE_SUFFIX.

#if defined(_WIN32)
#define BLOOM_EXECUTABLE_SUFFIX ".exe"
#else
#define BLOOM_EXECUTABLE_SUFFIX ""
#endif
