cmake_minimum_required(VERSION 3.20)

project(SimpleLogics LANGUAGES NONE)

set(SF_TAG "stockfish-dev-20260919-17a6c8f1" CACHE STRING
    "Stockfish development release tag")

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


print("================================================")
print(" SIMPLELOGICS HYBRID V2 BUILD")
print("================================================")
print("Stockfish tag :", SF_TAG)
print("NNUE source   :", NNUE_SOURCE)
print("Architecture  :", SF_ARCH)
print("Compiler      :", SF_COMP)
print("")


# ============================================================
# VERIFY NNUE
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

print("NNUE size       :", size)
print("NNUE SHA256     :", sha256)
print("Network filename:", network_name)
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
        str(SF_DIR),
    ],
    check=True,
)

if not SF_SRC.is_dir():
    raise SystemExit(
        f"ERROR: Stockfish source directory not found: {SF_SRC}"
    )


# ============================================================
# ENGINE OPTIONS
# ============================================================

engine = SF_SRC / "engine.cpp"
s = engine.read_text()

anchor = '''    options.add("Move Overhead", Option(10, 0, 5000));'''

insert = '''    options.add("Move Overhead", Option(10, 0, 5000));

    options.add(
      "SimpleLogics Human Mode",
      Option(false));

    options.add(
      "SimpleLogics Human Elo",
      Option(2650, 2501, 2800));

    options.add(
      "SimpleLogics LMR Boost",
      Option(true));

    options.add(
      "SimpleLogics Hybrid Search",
      Option(true));'''

if anchor not in s:
    raise SystemExit(
        "ERROR: engine.cpp option anchor not found"
    )

s = s.replace(anchor, insert, 1)

engine.write_text(s)


# ============================================================
# SEARCH PATCH
# ============================================================

search = SF_SRC / "search.cpp"
s = search.read_text()


# ------------------------------------------------------------
# HUMAN MODE
# ------------------------------------------------------------

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
# SEARCH V1 + HYBRID V2
# ============================================================

anchor = '''        int r = reduction(improving, depth, moveCount, delta);

        // Increase reduction for ttPv nodes'''

insert = '''        int r = reduction(improving, depth, moveCount, delta);

        // ====================================================
        // SimpleLogics Search V1
        // Quality-aware LMR
        // ====================================================

        if (options["SimpleLogics LMR Boost"]
            && depth >= 8
            && moveCount > 1)
        {
            int slHistory =
              (*contHist[0])[movedPiece][move.to_sq()]
              + (*contHist[1])[movedPiece][move.to_sq()]
              + sharedHistory.pawn_entry(pos)[movedPiece][move.to_sq()]
              + mainHistory[us][move.raw()];

            int slQuality =
              std::clamp(slHistory / 24, -384, 384);

            r -= slQuality;

            if (capture)
                r -= 192;

            if (givesCheck)
                r -= 320;

            if (PvNode && moveCount <= 4)
                r -= 160;

            if (moveCount > 8 && slQuality < -192)
                r += 128;
        }


        // ====================================================
        // SimpleLogics Hybrid Search V2
        //
        // Lc0-inspired policy guidance.
        //
        // This does NOT run Lc0 as a second engine.
        // Instead it adds a bounded neural-policy-style
        // strategic prior to the alpha-beta search.
        // ====================================================

        if (options["SimpleLogics Hybrid Search"]
            && depth >= 7
            && moveCount > 1)
        {
            int policyPrior = 0;

            const int toFile =
              int(file_of(move.to_sq()));

            const int toRank =
              int(rank_of(move.to_sq()));

            const int centerDistance =
              std::abs(toFile - 3)
              + std::abs(toRank - 3);


            // ------------------------------------------------
            // Central influence
            // ------------------------------------------------

            policyPrior +=
              (6 - centerDistance) * 18;


            // ------------------------------------------------
            // Pawn advancement
            // ------------------------------------------------

            if (type_of(movedPiece) == PAWN)
            {
                const int advance =
                  us == WHITE
                    ? int(rank_of(move.to_sq()))
                      - int(rank_of(move.from_sq()))
                    : int(rank_of(move.from_sq()))
                      - int(rank_of(move.to_sq()));

                policyPrior +=
                  advance * 22;
            }


            // ------------------------------------------------
            // Minor-piece development
            // ------------------------------------------------

            if (type_of(movedPiece) == KNIGHT
                || type_of(movedPiece) == BISHOP)
            {
                const int homeRank =
                  us == WHITE ? 0 : 7;

                if (int(rank_of(move.from_sq()))
                    == homeRank)
                {
                    policyPrior += 55;
                }
            }


            // ------------------------------------------------
            // King safety
            // ------------------------------------------------

            if (move.type_of() == CASTLING)
                policyPrior += 120;


            // ------------------------------------------------
            // Promotion
            // ------------------------------------------------

            if (move.type_of() == PROMOTION)
                policyPrior += 320;


            // ------------------------------------------------
            // Checks
            // ------------------------------------------------

            if (givesCheck)
                policyPrior += 180;


            // ------------------------------------------------
            // Bound the policy contribution
            // ------------------------------------------------

            policyPrior =
              std::clamp(
                policyPrior,
                -192,
                384);


            // ------------------------------------------------
            // Good policy candidates:
            // search deeper.
            //
            // Bad policy candidates:
            // reduce slightly more.
            // ------------------------------------------------

            r -= policyPrior;
        }


        // Increase reduction for ttPv nodes'''

if anchor not in s:
    raise SystemExit(
        "ERROR: LMR anchor not found"
    )

s = s.replace(anchor, insert, 1)

search.write_text(s)


# ============================================================
# MOVEPICKER HYBRID POLICY
# ============================================================

movepick = SF_SRC / "movepick.cpp"
s = movepick.read_text()


# Add required headers.

if "#include <algorithm>" not in s:
    s = s.replace(
        "#include <cassert>\n",
        "#include <cassert>\n#include <algorithm>\n",
        1
    )

if "#include <cmath>" not in s:
    s = s.replace(
        "#include <cassert>\n",
        "#include <cassert>\n#include <cmath>\n",
        1
    )


# ------------------------------------------------------------
# Quiet move policy prior
# ------------------------------------------------------------

mp_anchor = '''            // bonus for checks
            value += ((pos.check_squares(pt) & to) && pos.see_ge(m, -75)) * 16384;

            // penalty for moving to a square threatened by a lesser piece'''

mp_insert = '''            // bonus for checks
            value += ((pos.check_squares(pt) & to) && pos.see_ge(m, -75)) * 16384;


            // =================================================
            // SimpleLogics Hybrid Policy Prior
            //
            // Lc0-inspired strategic move ordering.
            // History remains the main signal.
            // This is only a bounded additional prior.
            // =================================================

            int policyPrior = 0;

            const int fileDistance =
              std::abs(int(file_of(to)) - 3);

            const int rankDistance =
              std::abs(int(rank_of(to)) - 3);


            // -------------------------------------------------
            // Central influence
            // -------------------------------------------------

            policyPrior +=
              (6 - fileDistance - rankDistance) * 32;


            // -------------------------------------------------
            // Pawn advancement
            // -------------------------------------------------

            if (pt == PAWN)
            {
                const int advance =
                  us == WHITE
                    ? int(rank_of(to))
                      - int(rank_of(from))
                    : int(rank_of(from))
                      - int(rank_of(to));

                policyPrior +=
                  advance * 28;
            }


            // -------------------------------------------------
            // Minor-piece development
            // -------------------------------------------------

            if (pt == KNIGHT || pt == BISHOP)
            {
                const int homeRank =
                  us == WHITE ? 0 : 7;

                if (int(rank_of(from)) == homeRank)
                    policyPrior += 90;
            }


            // -------------------------------------------------
            // Castling
            // -------------------------------------------------

            if (m.type_of() == CASTLING)
                policyPrior += 180;


            // -------------------------------------------------
            // Promotion
            // -------------------------------------------------

            if (m.type_of() == PROMOTION)
                policyPrior += 400;


            // -------------------------------------------------
            // Keep policy bounded.
            // -------------------------------------------------

            policyPrior =
              std::clamp(
                policyPrior,
                -256,
                512);


            value += policyPrior;


            // penalty for moving to a square threatened by a lesser piece'''

if mp_anchor not in s:
    raise SystemExit(
        "ERROR: movepick policy anchor not found"
    )

s = s.replace(
    mp_anchor,
    mp_insert,
    1
)

movepick.write_text(s)


# ============================================================
# NNUE INSTALLATION
# ============================================================

network_path =
    SF_SRC / network_name

shutil.copy2(
    NNUE_SOURCE,
    network_path
)

installed_sha256 = hashlib.sha256(
    network_path.read_bytes()
).hexdigest()

if installed_sha256[:12] != sha256[:12]:
    raise SystemExit(
        "ERROR: Installed NNUE SHA256 mismatch"
    )

print("")
print("NNUE installed:")
print(network_path)
print("")


# ============================================================
# PATCH DEFAULT NNUE
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
        "ERROR: EvalFileDefaultName not found"
    )

new_default =
    f'#define EvalFileDefaultName "{network_name}"'

s = s.replace(
    old_default,
    new_default,
    1
)

evaluate.write_text(s)


# ============================================================
# BRANDING
# ============================================================

misc = SF_SRC / "misc.cpp"
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
print(" BUILDING SIMPLELOGICS HYBRID V2")
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
        f"COMP={SF_COMP}",
    ],
    check=True
)


# ============================================================
# VERIFY ENGINE
# ============================================================

stockfish_binary =
    SF_SRC / "stockfish"

if not stockfish_binary.is_file():
    raise SystemExit(
        "ERROR: Engine binary was not produced."
    )


# Verify final network.

final_network =
    SF_SRC / network_name

if not final_network.is_file():
    raise SystemExit(
        "ERROR: Final NNUE is missing."
    )

final_sha256 = hashlib.sha256(
    final_network.read_bytes()
).hexdigest()

if final_sha256[:12] != network_name[3:15]:
    raise SystemExit(
        "ERROR: Final NNUE SHA256 mismatch."
    )


# ============================================================
# COPY FINAL ENGINE
# ============================================================

shutil.copy2(
    stockfish_binary,
    SF_OUT
)

print("")
print("================================================")
print(" SIMPLELOGICS HYBRID V2 BUILD SUCCESS")
print("================================================")
print("Binary :", SF_OUT)
print("NNUE   :", network_name)
print("SHA256 :", final_sha256)
print("")
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
