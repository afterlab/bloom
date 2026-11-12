# Post-install step for FFmpeg's MSVC layout: `make install` writes each shared library's import
# library (avcodec.lib, ...) next to its DLL in bin/, while the package wrapper
# (ffmpeg-config.cmake) and every Windows consumer look for lib/<name>.lib. Run as
#   cmake -DFFMPEG_PREFIX=<install prefix> -P ffmpeg-windows-import-libraries.cmake
# Idempotent: an import library already moved is not an error.
if(NOT DEFINED FFMPEG_PREFIX OR NOT IS_DIRECTORY "${FFMPEG_PREFIX}/bin")
    message(FATAL_ERROR "FFMPEG_PREFIX must name the FFmpeg install prefix")
endif()
file(MAKE_DIRECTORY "${FFMPEG_PREFIX}/lib")
foreach(library IN ITEMS avutil swresample swscale avcodec avformat avfilter avdevice)
    set(import_library "${FFMPEG_PREFIX}/bin/${library}.lib")
    if(EXISTS "${import_library}")
        file(RENAME "${import_library}" "${FFMPEG_PREFIX}/lib/${library}.lib")
    elseif(NOT EXISTS "${FFMPEG_PREFIX}/lib/${library}.lib")
        message(FATAL_ERROR "FFmpeg installed no import library for ${library}")
    endif()
endforeach()
