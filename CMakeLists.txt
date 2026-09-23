cmake_minimum_required(VERSION 3.20)

project(SimpleLogics LANGUAGES NONE)

# ============================================================
# SIMPLELOGICS - STRONG BASE
# Stockfish search + official compatible NNUE
# No experimental Hybrid V2 heuristics.
# ============================================================

set(SF_TAG "stockfish-dev-20260919-17a6c8f1"
    CACHE STRING "Stockfish version")

set(SF_DIR
    "${CMAKE_BINARY_DIR}/stockfish")

set(SF_SRC
    "${SF_DIR}/src")

set(SF_OUT
    "${CMAKE_BINARY_DIR}/SimpleLogics")

set(SIMPLELOGICS_NNUE
    "${CMAKE_SOURCE_DIR}/nnue/SimpleLogics-test.nnue")


# ============================================================
# PLATFORM
# ============================================================

if(ANDROID)

    set(SF_COMP "ndk")
    set(SF_ARCH "armv8")

else()

    set(SF_COMP "gcc")
    set(SF_ARCH "x86-64")

endif()


# ============================================================
# BUILD SCRIPT
# ============================================================

set(BUILD_SCRIPT
    "${CMAKE_BINARY_DIR}/build_simplelogics.py")


file(WRITE "${BUILD_SCRIPT}" [=[

from pathlib import Path
import hashlib
import shutil
import subprocess
import sys


# ============================================================
# ARGUMENTS
# ============================================================

ROOT = Path(sys.argv[1])
SF_DIR = Path(sys.argv[2])
SF_SRC = Path(sys.argv[3])
SF_OUT = Path(sys.argv[4])
NNUE_SOURCE = Path(sys.argv[5])
SF_TAG = sys.argv[6]
SF_ARCH = sys.argv[7]
SF_COMP = sys.argv[8]


print("")
print("================================================")
print(" SIMPLELOGICS STRONG BUILD")
print("================================================")
print("Stockfish :", SF_TAG)
print("Arch      :", SF_ARCH)
print("Compiler  :", SF_COMP)
print("NNUE      :", NNUE_SOURCE)
print("")


# ============================================================
# CHECK NNUE
# ============================================================

if not NNUE_SOURCE.is_file():

    raise SystemExit(
        f"ERROR: NNUE not found: {NNUE_SOURCE}"
    )


size = NNUE_SOURCE.stat().st_size


if size < 1_000_000:

    raise SystemExit(
        f"ERROR: NNUE file too small: {size}"
    )


sha256 = hashlib.sha256(
    NNUE_SOURCE.read_bytes()
).hexdigest()


network_name = (
    f"nn-{sha256[:12]}.nnue"
)


print("NNUE size :", size)
print("NNUE SHA  :", sha256)
print("Network   :", network_name)
print("")


# ============================================================
# CLONE STOCKFISH
# ============================================================

if SF_DIR.exists():

    shutil.rmtree(SF_DIR)


subprocess.run(
    [
        "git",
        "clone",
        "--depth",
        "1",
        "--branch",
        SF_TAG,
        "https://github.com/official-stockfish/Stockfish.git",
        str(SF_DIR)
    ],
    check=True
)


if not SF_SRC.is_dir():

    raise SystemExit(
        "ERROR: Stockfish source missing."
    )


# ============================================================
# INSTALL NNUE
# ============================================================

network_path = (
    SF_SRC / network_name
)


shutil.copy2(
    NNUE_SOURCE,
    network_path
)


installed_sha256 = hashlib.sha256(
    network_path.read_bytes()
).hexdigest()


if installed_sha256 != sha256:

    raise SystemExit(
        "ERROR: NNUE SHA mismatch."
    )


print("")
print("NNUE installed:")
print(network_path)
print("")


# ============================================================
# PATCH DEFAULT NNUE
# ============================================================

evaluate = (
    SF_SRC / "evaluate.h"
)

s = evaluate.read_text()


old_default = None


for line in s.splitlines():

    if "#define EvalFileDefaultName" in line:

        old_default = line
        break


if old_default is None:

    raise SystemExit(
        "ERROR: EvalFileDefaultName not found."
    )


new_default = (
    f'#define EvalFileDefaultName "{network_name}"'
)


s = s.replace(
    old_default,
    new_default,
    1
)


evaluate.write_text(s)


# ============================================================
# SIMPLELOGICS UCI OPTIONS
# ============================================================

engine = (
    SF_SRC / "engine.cpp"
)

s = engine.read_text()


anchor = (
    '    options.add("Move Overhead", '
    'Option(10, 0, 5000));'
)


insert = '''    options.add("Move Overhead", Option(10, 0, 5000));

    options.add(
      "SimpleLogics Human Mode",
      Option(false));

    options.add(
      "SimpleLogics Human Elo",
      Option(2650, 2501, 2800));'''


if anchor in s:

    s = s.replace(
        anchor,
        insert,
        1
    )


engine.write_text(s)


# ============================================================
# HUMAN MODE
# ============================================================

search = (
    SF_SRC / "search.cpp"
)

s = search.read_text()


old = '''    Skill   skill =
      Skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);'''


new = '''    Skill   skill =
      Skill(
        options["Skill Level"],
        options["SimpleLogics Human Mode"]
          ? int(options["SimpleLogics Human Elo"])
          : (options["UCI_LimitStrength"]
               ? int(options["UCI_Elo"])
               : 0));'''


if old in s:

    s = s.replace(
        old,
        new,
        1
    )


old2 = '''    Skill skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);'''


new2 = '''    Skill skill(
      options["Skill Level"],
      options["SimpleLogics Human Mode"]
        ? int(options["SimpleLogics Human Elo"])
        : (options["UCI_LimitStrength"]
             ? int(options["UCI_Elo"])
             : 0));'''


if old2 in s:

    s = s.replace(
        old2,
        new2,
        1
    )


search.write_text(s)


# ============================================================
# BRANDING
# ============================================================

misc = (
    SF_SRC / "misc.cpp"
)

s = misc.read_text()


if 'ss << "Stockfish " << version' in s:

    s = s.replace(
        'ss << "Stockfish " << version',
        'ss << "SimpleLogics " << version',
        1
    )


misc.write_text(s)


# ============================================================
# BUILD
# ============================================================

print("")
print("================================================")
print(" BUILDING SIMPLELOGICS")
print("================================================")
print("")


subprocess.run(
    [
        "make",
        "-C",
        str(SF_SRC),
        "-j2",
        "build",
        f"ARCH={SF_ARCH}",
        f"COMP={SF_COMP}"
    ],
    check=True
)


# ============================================================
# VERIFY
# ============================================================

binary = (
    SF_SRC / "stockfish"
)


if not binary.is_file():

    raise SystemExit(
        "ERROR: Engine binary missing."
    )


final_network = (
    SF_SRC / network_name
)


if not final_network.is_file():

    raise SystemExit(
        "ERROR: NNUE missing."
    )


# ============================================================
# COPY ENGINE
# ============================================================

shutil.copy2(
    binary,
    SF_OUT
)


print("")
print("================================================")
print(" SIMPLELOGICS BUILD SUCCESS")
print("================================================")
print("Binary :", SF_OUT)
print("NNUE   :", network_name)
print("SHA256 :", sha256)
print("")


]=])


# ============================================================
# CMAKE TARGET
# ============================================================

add_custom_command(
    OUTPUT "${SF_OUT}"

    COMMAND
        python3
        "${BUILD_SCRIPT}"
        "${CMAKE_SOURCE_DIR}"
        "${SF_DIR}"
        "${SF_SRC}"
        "${SF_OUT}"
        "${SIMPLELOGICS_NNUE}"
        "${SF_TAG}"
        "${SF_ARCH}"
        "${SF_COMP}"

    VERBATIM
)


add_custom_target(
    SimpleLogics ALL
    DEPENDS "${SF_OUT}"
)
