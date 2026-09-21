cmake_minimum_required(VERSION 3.20)

project(SimpleLogics LANGUAGES NONE)

set(SF_TAG "sf_19" CACHE STRING "Stockfish release tag")
set(SF_DIR "${CMAKE_BINARY_DIR}/stockfish")
set(SF_SRC "${SF_DIR}/src")
set(SF_OUT "${CMAKE_BINARY_DIR}/SimpleLogics")

if(ANDROID)
    set(SF_COMP "ndk")
    set(SF_ARCH "armv8")
else()
    set(SF_COMP "gcc")
    set(SF_ARCH "x86-64")
endif()

add_custom_command(
    OUTPUT "${SF_OUT}"

    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SF_DIR}"

    COMMAND git clone --depth 1 --branch ${SF_TAG}
            https://github.com/official-stockfish/Stockfish.git
            "${SF_DIR}"

    COMMAND sed -i
            "s/ss << \\\"Stockfish \\\" << version/ss << \\\"SimpleLogics \\\" << version/"
            "${SF_SRC}/misc.cpp"

    COMMAND make -C "${SF_SRC}" -j2 net

    COMMAND make -C "${SF_SRC}" -j2
            build
            ARCH=${SF_ARCH}
            COMP=${SF_COMP}

    COMMAND "${CMAKE_COMMAND}" -E copy
            "${SF_SRC}/stockfish"
            "${SF_OUT}"

    VERBATIM
)

add_custom_target(SimpleLogics ALL
    DEPENDS "${SF_OUT}"
)
