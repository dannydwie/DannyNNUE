cmake_minimum_required(VERSION 3.20)

project(SimpleLogics LANGUAGES NONE)

# ============================================================
# SIMPLELOGICS 1.0
#
# STRENGTH RESET
#
# Official Stockfish development source
# Official compatible NNUE
#
# NO experimental NNUE
# NO Hybrid V2
# NO artificial search weakening
# ============================================================

set(SF_TAG "stockfish-dev-20260922-0a215d6c"
    CACHE STRING "Stockfish source tag")

set(SF_DIR
    "${CMAKE_BINARY_DIR}/stockfish")

set(SF_SRC
    "${SF_DIR}/src")

set(SF_OUT
    "${CMAKE_BINARY_DIR}/SimpleLogics")

set(BUILD_SCRIPT
    "${CMAKE_BINARY_DIR}/build_simplelogics.py")


# ============================================================
# BUILD SCRIPT
# ============================================================

file(WRITE "${BUILD_SCRIPT}" [=[

from pathlib import Path
import shutil
import subprocess
import sys


SF_DIR = Path(sys.argv[1])
SF_SRC = Path(sys.argv[2])
SF_OUT = Path(sys.argv[3])
SF_TAG = sys.argv[4]
SF_ARCH = sys.argv[5]
SF_COMP = sys.argv[6]


print("")
print("================================================")
print(" SIMPLELOGICS 1.0")
print(" STRENGTH RESET")
print("================================================")
print("Source :", SF_TAG)
print("Arch   :", SF_ARCH)
print("Comp   :", SF_COMP)
print("")


# ============================================================
# CLEAN
# ============================================================

if SF_DIR.exists():
    shutil.rmtree(SF_DIR)


# ============================================================
# CLONE OFFICIAL STOCKFISH
# ============================================================

print("Cloning official Stockfish...")

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
# DOWNLOAD OFFICIAL NNUE
#
# IMPORTANT:
#
# We no longer use the small experimental
# SimpleLogics NNUE.
#
# Stockfish downloads the network that is
# compatible with this exact source.
# ============================================================

print("")
print("================================================")
print(" DOWNLOADING OFFICIAL NNUE")
print("================================================")
print("")


subprocess.run(
    [
        "make",
        "-C",
        str(SF_SRC),
        "net"
    ],
    check=True
)


nets = sorted(
    SF_SRC.glob("nn-*.nnue"),
    key=lambda p: p.stat().st_mtime,
    reverse=True
)


if not nets:

    raise SystemExit(
        "ERROR: Official NNUE was not downloaded."
    )


official_net = nets[0]


print("")
print("Official NNUE:")
print(official_net)

print(
    "Size:",
    official_net.stat().st_size
)

print("")


# ============================================================
# BRAND ENGINE
# ============================================================

misc = SF_SRC / "misc.cpp"

s = misc.read_text()


old = 'ss << "Stockfish " << version'


if old in s:

    s = s.replace(
        old,
        'ss << "SimpleLogics " << version',
        1
    )

else:

    print(
        "WARNING: branding line not found."
    )


misc.write_text(s)


# ============================================================
# BUILD
# ============================================================

print("")
print("================================================")
print(" BUILDING SIMPLELOGICS 1.0")
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

binary = SF_SRC / "stockfish"


if not binary.is_file():

    raise SystemExit(
        "ERROR: SimpleLogics binary not produced."
    )


# ============================================================
# COPY FINAL ENGINE
# ============================================================

shutil.copy2(
    binary,
    SF_OUT
)


print("")
print("================================================")
print(" SIMPLELOGICS 1.0 BUILD SUCCESS")
print("================================================")
print("Binary :", SF_OUT)
print("NNUE   :", official_net.name)
print("")


]=])


# ============================================================
# PLATFORM
# ============================================================

if(ANDROID)

    set(SF_ARCH "armv8")
    set(SF_COMP "ndk")

else()

    set(SF_ARCH "x86-64")
    set(SF_COMP "gcc")

endif()


# ============================================================
# CMAKE TARGET
# ============================================================

add_custom_command(

    OUTPUT
        "${SF_OUT}"

    COMMAND
        python3
        "${BUILD_SCRIPT}"
        "${SF_DIR}"
        "${SF_SRC}"
        "${SF_OUT}"
        "${SF_TAG}"
        "${SF_ARCH}"
        "${SF_COMP}"

    VERBATIM
)


add_custom_target(

    SimpleLogics ALL

    DEPENDS
        "${SF_OUT}"
)
