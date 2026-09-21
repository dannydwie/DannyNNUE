cmake_minimum_required(VERSION 3.20)

project(SimpleLogics LANGUAGES NONE)

set(SF_TAG "stockfish-dev-20260919-17a6c8f1" CACHE STRING "Stockfish development release tag")
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

# SimpleLogics source modifications
set(SIMPLELOGICS_PATCH "${CMAKE_BINARY_DIR}/simplelogics_patch.py")

file(WRITE "${SIMPLELOGICS_PATCH}" [=[
from pathlib import Path
import sys

src = Path(sys.argv[1])

# ------------------------------------------------------------
# 1. Add a real SimpleLogics UCI search option
# ------------------------------------------------------------
engine = src / "engine.cpp"
s = engine.read_text()

old = '    options.add("Move Overhead", Option(10, 0, 5000));'
new = '''    options.add("Move Overhead", Option(10, 0, 5000));

    options.add(
      "SimpleLogics Tactical Extension",
      Option(true, [](const Option&) {
          return std::nullopt;
      }));'''

if old not in s:
    raise SystemExit("SimpleLogics patch failed: engine.cpp anchor not found")

s = s.replace(old, new, 1)
engine.write_text(s)

# ------------------------------------------------------------
# 2. Add a real tactical search extension
# ------------------------------------------------------------
search = src / "search.cpp"
s = search.read_text()

old = '''        // Step 17. Make the move
        do_move(pos, move, st, givesCheck, ss);

        // Add extension to new depth
        newDepth += extension;'''

new = '''        // Step 17. Make the move
        do_move(pos, move, st, givesCheck, ss);

        // Add extension to new depth
        newDepth += extension;

        // SimpleLogics tactical extension.
        // Checks at meaningful search depths receive one additional ply.
        // This is intentionally controlled by a real UCI option so it can
        // be benchmarked against the unmodified Stockfish search.
        if (options["SimpleLogics Tactical Extension"]
            && givesCheck
            && depth >= 5
            && depth <= 14
            && extension <= 0)
            newDepth += 1;'''

if old not in s:
    raise SystemExit("SimpleLogics patch failed: search.cpp anchor not found")

s = s.replace(old, new, 1)
search.write_text(s)

# ------------------------------------------------------------
# 3. Brand the engine as SimpleLogics
# ------------------------------------------------------------
misc = src / "misc.cpp"
s = misc.read_text()

old = 'ss << "Stockfish " << version << std::setfill'
new = 'ss << "SimpleLogics " << version << std::setfill'

if old not in s:
    raise SystemExit("SimpleLogics patch failed: misc.cpp anchor not found")

s = s.replace(old, new, 1)
misc.write_text(s)

print("SimpleLogics source modifications applied successfully.")
]=])

add_custom_command(
    OUTPUT "${SF_OUT}"

    COMMAND "${CMAKE_COMMAND}" -E rm -rf "${SF_DIR}"

    COMMAND git clone
            --depth 1
            --branch ${SF_TAG}
            https://github.com/official-stockfish/Stockfish.git
            "${SF_DIR}"

    COMMAND python3
            "${SIMPLELOGICS_PATCH}"
            "${SF_SRC}"

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
