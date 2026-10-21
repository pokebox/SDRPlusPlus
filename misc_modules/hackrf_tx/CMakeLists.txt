cmake_minimum_required(VERSION 3.13)
project(hackrf_tx)

file(GLOB SRC "src/*.cpp")

include(${SDRPP_MODULE_CMAKE})

if (MSVC)
    target_link_directories(hackrf_tx PRIVATE "C:/Program Files/PothosSDR/bin/")
    target_link_libraries(hackrf_tx PRIVATE hackrf)
elseif (ANDROID)
    target_include_directories(hackrf_tx PUBLIC
        /sdr-kit/${ANDROID_ABI}/include/libhackrf
    )
    target_link_libraries(hackrf_tx PUBLIC
        /sdr-kit/${ANDROID_ABI}/lib/libusb1.0.so
        /sdr-kit/${ANDROID_ABI}/lib/libhackrf.so
    )
else (MSVC)
    find_package(PkgConfig)
    pkg_check_modules(LIBHACKRF REQUIRED libhackrf)

    target_include_directories(hackrf_tx PRIVATE ${LIBHACKRF_INCLUDE_DIRS})
    target_link_directories(hackrf_tx PRIVATE ${LIBHACKRF_LIBRARY_DIRS})
    target_link_libraries(hackrf_tx PRIVATE ${LIBHACKRF_LIBRARIES})
endif ()
