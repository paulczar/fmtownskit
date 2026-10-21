# fmtownskit's runtime for a port's CMakeLists: set FMT_RUNTIME to this directory, then
# include() this file. It defines fmt_towns: the CPU's helpers (x86_*), the machine (towns_*)
# and the YM3438's core (ymfm/, BSD-3). A port links it with its recompiled modules and its
# own modules.cpp (g_x86_modules).
add_library(fmt_towns STATIC
    ${FMT_RUNTIME}/x86_core.cpp
    ${FMT_RUNTIME}/x86_flags.cpp
    ${FMT_RUNTIME}/towns_cpu.cpp
    ${FMT_RUNTIME}/towns_disc.cpp
    ${FMT_RUNTIME}/towns_dos.cpp
    ${FMT_RUNTIME}/towns_io.cpp
    ${FMT_RUNTIME}/towns_mem.cpp
    ${FMT_RUNTIME}/towns_sound.cpp
    ${FMT_RUNTIME}/towns_video.cpp
    ${FMT_RUNTIME}/ymfm/ymfm_opn.cpp
    ${FMT_RUNTIME}/ymfm/ymfm_adpcm.cpp
    ${FMT_RUNTIME}/ymfm/ymfm_ssg.cpp)
target_include_directories(fmt_towns PUBLIC ${FMT_RUNTIME})
target_include_directories(fmt_towns PRIVATE ${FMT_RUNTIME}/ymfm)

# The window (towns_sdl.h), when SDL3 is there: fmt_towns_sdl, linked statically if it can be.
find_package(SDL3 CONFIG QUIET)
if(SDL3_FOUND)
  add_library(fmt_towns_sdl STATIC ${FMT_RUNTIME}/towns_sdl.cpp)
  target_link_libraries(fmt_towns_sdl PUBLIC fmt_towns)
  if(TARGET SDL3::SDL3-static)
    target_link_libraries(fmt_towns_sdl PUBLIC SDL3::SDL3-static)
  else()
    target_link_libraries(fmt_towns_sdl PUBLIC SDL3::SDL3)
  endif()
endif()
