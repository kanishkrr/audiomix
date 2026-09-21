cmake_minimum_required(VERSION 3.16)
project(audiomix LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
pkg_check_modules(FFTW3 REQUIRED fftw3)
pkg_check_modules(SNDFILE REQUIRED sndfile)

add_executable(audiomix
    src/main.cpp
    src/mixer.cpp
    src/channel.cpp
    src/feedback_detector.cpp
    src/notch_filter.cpp
    src/audio_io.cpp
)

target_include_directories(audiomix PRIVATE
    src
    ${PORTAUDIO_INCLUDE_DIRS}
    ${FFTW3_INCLUDE_DIRS}
    ${SNDFILE_INCLUDE_DIRS}
)

target_compile_options(audiomix PRIVATE
    -Wall -Wextra
    ${PORTAUDIO_CFLAGS_OTHER}
    ${FFTW3_CFLAGS_OTHER}
    ${SNDFILE_CFLAGS_OTHER}
)

target_link_libraries(audiomix PRIVATE
    ${PORTAUDIO_LIBRARIES}
    ${FFTW3_LIBRARIES}
    ${SNDFILE_LIBRARIES}
    Threads::Threads
    m
)

target_link_directories(audiomix PRIVATE
    ${PORTAUDIO_LIBRARY_DIRS}
    ${FFTW3_LIBRARY_DIRS}
    ${SNDFILE_LIBRARY_DIRS}
)

message(STATUS "AudioMix configured:")
message(STATUS "  PortAudio: ${PORTAUDIO_VERSION}")
message(STATUS "  FFTW3:     ${FFTW3_VERSION}")
message(STATUS "  libsndfile:${SNDFILE_VERSION}")
