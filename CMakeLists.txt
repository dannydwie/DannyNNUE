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

set(SIMPLELOGICS_PATCH "${CMAKE_BINARY_DIR}/simplelogics_patch.py")

file(WRITE "${SIMPLELOGICS_PATCH}" [=[
from pathlib import Path
import sys

src = Path(sys.argv[1])

# ------------------------------------------------------------
# 1. Add SimpleLogics Human Mode options
# ------------------------------------------------------------

engine = src / "engine.cpp"
s = engine.read_text()

anchor = '    options.add("Move Overhead", Option(10, 0, 5000));'

insert = '''    options.add("Move Overhead", Option(10, 0, 5000));

    options.add(
      "SimpleLogics Human Mode",
      Option(true));

    options.add(
      "SimpleLogics Human Elo",
      Option(2650, 2501, 2800));'''

if anchor not in s:
    raise SystemExit("SimpleLogics patch failed: engine.cpp anchor not found")

s = s.replace(anchor, insert, 1)
engine.write_text(s)

# ------------------------------------------------------------
# 2. Connect Human Elo to Stockfish Skill system
# ------------------------------------------------------------

search = src / "search.cpp"
s = search.read_text()

old_main = '''    Skill   skill =
      Skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);'''

new_main = '''    Skill   skill =
      Skill(options["Skill Level"],
            options["SimpleLogics Human Mode"]
              ? int(options["SimpleLogics Human Elo"])
              : (options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0));'''

if old_main not in s:
    raise SystemExit("SimpleLogics patch failed: main Skill anchor not found")

s = s.replace(old_main, new_main, 1)

old_iter = '''    Skill skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);'''

new_iter = '''    Skill skill(options["Skill Level"],
                options["SimpleLogics Human Mode"]
                  ? int(options["SimpleLogics Human Elo"])
                  : (options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0));'''

if old_iter not in s:
    raise SystemExit("SimpleLogics patch failed: iterative Skill anchor not found")

s = s.replace(old_iter, new_iter, 1)

search.write_text(s)

# ------------------------------------------------------------
# 3. Brand engine as SimpleLogics
# ------------------------------------------------------------

misc = src / "misc.cpp"
s = misc.read_text()

old = 'ss << "Stockfish " << version << std::setfill'
new = 'ss << "SimpleLogics " << version << std::setfill'

if old not in s:
    raise SystemExit("SimpleLogics patch failed: misc.cpp anchor not found")

s = s.replace(old, new, 1)
misc.write_text(s)

print("SimpleLogics Human Mode patch applied successfully.")
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
