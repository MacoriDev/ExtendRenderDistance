cmake_minimum_required(VERSION 3.22)
project(ExtendRenderDistance CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
    preloader
    GIT_REPOSITORY https://github.com/LiteLDev/preloader-android.git
    GIT_TAG main
)

FetchContent_GetProperties(preloader)
if(NOT preloader_POPULATED)
    FetchContent_Populate(preloader)

    set(PRELOADER_LOGGER "${preloader_SOURCE_DIR}/src/pl/Logger.h")

    if(EXISTS "${PRELOADER_LOGGER}")
        file(READ "${PRELOADER_LOGGER}" LOGGER_TEXT)

        string(REPLACE
            "const auto message = std::vformat(fmtStr, std::make_format_args(args...));"
            "const auto message = std::string(fmtStr);"
            LOGGER_TEXT
            "${LOGGER_TEXT}"
        )

        file(WRITE "${PRELOADER_LOGGER}" "${LOGGER_TEXT}")
    endif()

    add_subdirectory("${preloader_SOURCE_DIR}" "${preloader_BINARY_DIR}")
endif()

add_library(ExtendRenderDistance SHARED
    main.cpp
)

target_include_directories(ExtendRenderDistance
    PRIVATE
    "${preloader_SOURCE_DIR}/src"
)

target_link_libraries(ExtendRenderDistance
    PRIVATE
    preloader
    log
)

target_compile_options(ExtendRenderDistance PRIVATE
    -O2
    -fvisibility=hidden
    -ffunction-sections
    -fdata-sections
)

target_link_options(ExtendRenderDistance PRIVATE
    -Wl,--gc-sections
)
