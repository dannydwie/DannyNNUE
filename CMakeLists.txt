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

# ------------------------------------------------------------
# SimpleLogics source modifications
# ------------------------------------------------------------

set(SIMPLELOGICS_PATCH "${CMAKE_BINARY_DIR}/simplelogics_patch.py")

file(WRITE "${SIMPLELOGICS_PATCH}" [=[
from pathlib import Path
import sys

src = Path(sys.argv[1])

# ------------------------------------------------------------
# 1. SimpleLogics Human Mode
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
# 2. Connect Human Elo to Stockfish's existing Skill system
# ------------------------------------------------------------

search = src / "search.cpp"
s = search.read_text()

old1 = '''Skill skill =
  Skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);'''

new1 = '''Skill skill =
  Skill(options["Skill Level"],
        options["SimpleLogics Human Mode"]
          ? int(options["SimpleLogics Human Elo"])
          : (options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0));'''

if old1 in s:
    s = s.replace(old1, new1, 1)

old2 = '''Skill skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);'''

new2 = '''Skill skill(options["Skill Level"],
             options["SimpleLogics Human Mode"]
               ? int(options["SimpleLogics Human Elo"])
               : (options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0));'''

if old2 in s:
    s = s.replace(old2, new2, 1)

if old1 not in s and old2 not in s:
    raise SystemExit("SimpleLogics patch failed: Skill anchor not found")

search.write_text(s)

# ------------------------------------------------------------
# 3. Brand engine as SimpleLogics
# ------------------------------------------------------------

misc = src / "misc.cpp"
s = misc.read_text()

old = 'ss << "Stockfish " << version <<
