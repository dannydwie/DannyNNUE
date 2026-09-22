cmake_minimum_required(VERSION 3.20)

project(SimpleLogics LANGUAGES NONE)

set(SF_TAG "stockfish-dev-20260919-17a6c8f1" CACHE STRING "Stockfish development release tag")
set(SF_DIR "${CMAKE_BINARY_DIR}/stockfish")
set(SF_SRC "${SF_DIR}/src")
set(SF_OUT "${CMAKE_BINARY_DIR}/SimpleLogics")

set(SIMPLELOGICS_NNUE
    "${CMAKE_SOURCE_DIR}/nnue/SimpleLogics-test.nnue")

if(ANDROID)
    set(SF_COMP "ndk")
    set(SF_ARCH "armv8")
else()
    set(SF_COMP "gcc")
    set(SF_ARCH "x86-64")
endif()

set(SIMPLELOGICS_BUILD_SCRIPT
    "${CMAKE_BINARY_DIR}/build_simplelogics.py")

file(WRITE "${SIMPLELOGICS_BUILD_SCRIPT}" [=[
from pathlib import Path
import hashlib
import shutil
import subprocess
import sys

ROOT = Path(sys.argv[1])
SF_DIR = Path(sys.argv[2])
SF_SRC = Path(sys.argv[3])
SF_OUT = Path(sys.argv[4])
NNUE_SOURCE = Path(sys.argv[5])
SF_TAG = sys.argv[6]
SF_ARCH = sys.argv[7]
SF_COMP = sys.argv[8]

print("==============================================")
print("SIMPLELOGICS BUILD")
print("==============================================")
print("Stockfish tag:", SF_TAG)
print("NNUE source:", NNUE_SOURCE)
print("")

# ============================================================
# 1. CHECK CUSTOM NNUE
# ============================================================

if not NNUE_SOURCE.is_file():
    raise SystemExit(
        f"ERROR: Custom NNUE not found: {NNUE_SOURCE}"
    )

size = NNUE_SOURCE.stat().st_size

if size < 1_000_000:
    raise SystemExit(
        f"ERROR: NNUE file is unexpectedly small: {size} bytes"
    )

sha256 = hashlib.sha256(
    NNUE_SOURCE.read_bytes()
).hexdigest()

network_name = f"nn-{sha256[:12]}.nnue"

print("Custom NNUE size :", size)
print("Custom NNUE SHA256:", sha256)
print("Stockfish NNUE name:", network_name)
print("")

# ============================================================
# 2. CLONE STOCKFISH
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
        str(SF_DIR),
    ],
    check=True,
)

if not SF_SRC.is_dir():
    raise SystemExit(
        f"ERROR: Stockfish source directory not found: {SF_SRC}"
    )

# ============================================================
# 3. SIMPLELOGICS UCI OPTIONS
# ============================================================

engine = SF_SRC / "engine.cpp"
s = engine.read_text()

anchor = '    options.add("Move Overhead", Option(10, 0, 5000));'

insert = '''    options.add("Move Overhead", Option(10, 0, 5000));

    options.add(
      "SimpleLogics Human Mode",
      Option(false));

    options.add(
      "SimpleLogics Human Elo",
      Option(2650, 2501, 2800));

    options.add(
      "SimpleLogics LMR Boost",
      Option(true));'''

if anchor not in s:
    raise SystemExit(
        "ERROR: engine.cpp anchor not found"
    )

s = s.replace(anchor, insert, 1)
engine.write_text(s)

# ============================================================
# 4. HUMAN MODE
# ============================================================

search = SF_SRC / "search.cpp"
s = search.read_text()

old_main = '''    Skill   skill =
      Skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);'''

new_main = '''    Skill   skill =
      Skill(options["Skill Level"],
            options["SimpleLogics Human Mode"]
              ? int(options["SimpleLogics Human Elo"])
              : (options["UCI_LimitStrength"]
                   ? int(options["UCI_Elo"])
                   : 0));'''

if old_main not in s:
    raise SystemExit(
        "ERROR: main Skill anchor not found"
    )

s = s.replace(old_main, new_main, 1)

old_iter = '''    Skill skill(options["Skill Level"], options["UCI_LimitStrength"] ? int(options["UCI_Elo"]) : 0);'''

new_iter = '''    Skill skill(
      options["Skill Level"],
      options["SimpleLogics Human Mode"]
        ? int(options["SimpleLogics Human Elo"])
        : (options["UCI_LimitStrength"]
             ? int(options["UCI_Elo"])
             : 0));'''

if old_iter not in s:
    raise SystemExit(
        "ERROR: iterative Skill anchor not found"
    )

s = s.replace(old_iter, new_iter, 1)

# ============================================================
# 5. SIMPLELOGICS LMR BOOST
# ============================================================

anchor = '''        int r = reduction(improving, depth, moveCount, delta);

        // Increase reduction for ttPv nodes'''

insert = '''        int r = reduction(improving, depth, moveCount, delta);

        // ====================================================
        // SimpleLogics Strength V5
        // ====================================================

        if (options["SimpleLogics LMR Boost"]
            && PvNode
            && !capture
            && !givesCheck
            && depth >= 8
            && moveCount <= 4)
        {
            r -= 256;
        }

        // Increase reduction for ttPv nodes'''

if anchor not in s:
    raise SystemExit(
        "ERROR: LMR anchor not found"
    )

s = s.replace(anchor, insert, 1)
search.write_text(s)

# ============================================================
# 6. BRANDING
# ============================================================

misc = SF_SRC / "misc.cpp"
s = misc.read_text()

old = 'ss << "Stockfish " << version << std::setfill'
new = 'ss << "SimpleLogics " << version << std::setfill'

if old not in s:
    raise SystemExit(
        "ERROR: misc.cpp branding anchor not found"
    )

s = s.replace(old, new, 1)
misc.write_text(s)

# ============================================================
# 7. INSTALL CUSTOM NNUE WITH CORRECT SHA256 NAME
# ============================================================

network_path = SF_SRC / network_name

shutil.copy2(
    NNUE_SOURCE,
    network_path
)

print("")
print("Custom NNUE installed:")
print(network_path)
print("")

# Verify filename matches SHA256
installed_sha256 = hashlib.sha256(
    network_path.read_bytes()
).hexdigest()

if installed_sha256[:12] != sha256[:12]:
    raise SystemExit(
        "ERROR: Installed NNUE SHA256 mismatch"
    )

# ============================================================
# 8. PATCH STOCKFISH DEFAULT NETWORK
# ============================================================

evaluate = SF_SRC / "evaluate.h"
s = evaluate.read_text()

old_default = None

for line in s.splitlines():
    if "#define EvalFileDefaultName" in line:
        old_default = line
        break

if old_default is None:
    raise SystemExit(
        "ERROR: EvalFileDefaultName not found in evaluate.h"
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

print("Stockfish default NNUE patched:")
print(new_default)
print("")

# ============================================================
# 9. BUILD STOCKFISH
# ============================================================

print("==============================================")
print("BUILDING SIMPLELOGICS")
print("==============================================")
print("")

subprocess.run(
    [
        "make",
        "-C",
        str(SF_SRC),
        "-j2",
        "build",
        f"ARCH={SF_ARCH}",
        f"COMP={SF_COMP}",
    ],
    check=True,
)

# ============================================================
# 10. VERIFY NNUE WAS NOT REPLACED
# ============================================================

final_network = SF_SRC / network_name

if not final_network.is_file():
    raise SystemExit(
        "ERROR: Custom NNUE disappeared during build."
    )

final_sha256 = hashlib.sha256(
    final_network.read_bytes()
).hexdigest()

print("")
print("==============================================")
print("FINAL NNUE VERIFICATION")
print("==============================================")
print("Network:", final_network.name)
print("SHA256 :", final_sha256)
print("")

if final_sha256[:12] != final_network.name[3:15]:
    raise SystemExit(
        "ERROR: Final NNUE filename does not match SHA256."
    )

print("FINAL NNUE SHA256 verification PASSED.")
print("")

# ============================================================
# 11. COPY ENGINE
# ============================================================

stockfish_binary = SF_SRC / "stockfish"

if not stockfish_binary.is_file():
    raise SystemExit(
        "ERROR: Stockfish binary was not produced."
    )

shutil.copy2(
    stockfish_binary,
    SF_OUT
)

print("SimpleLogics binary:")
print(SF_OUT)
print("")

print("==============================================")
print("SIMPLELOGICS BUILD SUCCESS")
print("==============================================")
]=])

add_custom_command(
    OUTPUT "${SF_OUT}"

    COMMAND python3
            "${SIMPLELOGICS_BUILD_SCRIPT}"
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

add_custom_target(SimpleLogics ALL
    DEPENDS "${SF_OUT}"
)
