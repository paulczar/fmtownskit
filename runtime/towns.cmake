# fmtownskit's runtime for a port's CMakeLists: set FMT_RUNTIME to this directory, then
# include() this file. It defines fmt_towns: the CPU's helpers (x86_*) and the machine (towns_*).
# A port links it with its recompiled modules and its own modules.cpp (g_x86_modules).
add_library(fmt_towns STATIC
    ${FMT_RUNTIME}/x86_core.cpp
    ${FMT_RUNTIME}/x86_flags.cpp
    ${FMT_RUNTIME}/towns_cpu.cpp
    ${FMT_RUNTIME}/towns_disc.cpp
    ${FMT_RUNTIME}/towns_dos.cpp
    ${FMT_RUNTIME}/towns_io.cpp
    ${FMT_RUNTIME}/towns_mem.cpp
    ${FMT_RUNTIME}/towns_video.cpp)
target_include_directories(fmt_towns PUBLIC ${FMT_RUNTIME})
