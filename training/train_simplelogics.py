# ============================================================
# SimpleLogics NNUE V1
# Training launcher
# ============================================================

import os
import subprocess
import sys
from pathlib import Path


# ------------------------------------------------------------
# Configuration
# ------------------------------------------------------------

TRAIN_DATA = os.environ.get(
    "SIMPLELOGICS_TRAIN_DATA",
    "/content/train.binpack"
)

VALID_DATA = os.environ.get(
    "SIMPLELOGICS_VALID_DATA",
    "/content/validation.binpack"
)

WORK_DIR = Path(
    os.environ.get(
        "SIMPLELOGICS_WORK",
        "/content/simplelogics_nnue"
    )
)

TRAINER_DIR = WORK_DIR / "nnue-pytorch"
OUTPUT_DIR = WORK_DIR / "output"


# ------------------------------------------------------------
# Helper
# ------------------------------------------------------------

def run(command, cwd=None):
    print()
    print("=" * 70)
    print("RUNNING:")
    print(" ".join(str(x) for x in command))
    print("=" * 70)

    subprocess.run(
        command,
        cwd=cwd,
        check=True
    )


# ------------------------------------------------------------
# Check dataset
# ------------------------------------------------------------

def check_file(path, name):

    path = Path(path)

    if not path.exists():
        print()
        print("ERROR:")
        print(f"{name} tidak ditemukan:")
        print(path)
        print()
        print(
            "Letakkan dataset .binpack pada lokasi tersebut "
            "atau ubah environment variable."
        )
        sys.exit(1)

    size_gb = path.stat().st_size / (1024 ** 3)

    print(f"{name}: {path}")
    print(f"Size: {size_gb:.2f} GB")


# ------------------------------------------------------------
# Main
# ------------------------------------------------------------

def main():

    print()
    print("==============================================")
    print("       SIMPLELOGICS NNUE V1 TRAINER")
    print("==============================================")
    print()

    check_file(TRAIN_DATA, "Training dataset")
    check_file(VALID_DATA, "Validation dataset")

    WORK_DIR.mkdir(
        parents=True,
        exist_ok=True
    )

    OUTPUT_DIR.mkdir(
        parents=True,
        exist_ok=True
    )

    # --------------------------------------------------------
    # Download official trainer
    # --------------------------------------------------------

    if not TRAINER_DIR.exists():

        run([
            "git",
            "clone",
            "--depth",
            "1",
            "https://github.com/official-stockfish/nnue-pytorch.git",
            str(TRAINER_DIR)
        ])

    # --------------------------------------------------------
    # Install Python dependencies
    # --------------------------------------------------------

    run([
        sys.executable,
        "-m",
        "pip",
        "install",
        "-r",
        "requirements.txt"
    ], cwd=TRAINER_DIR)

    # --------------------------------------------------------
    # Build the C++ data loader
    # --------------------------------------------------------

    compile_script = TRAINER_DIR / "compile_data_loader.sh"

    if compile_script.exists():

        run([
            "bash",
            str(compile_script)
        ], cwd=TRAINER_DIR)

    # --------------------------------------------------------
    # Check CUDA
    # --------------------------------------------------------

    try:

        import torch

        print()
        print("PyTorch version:", torch.__version__)
        print(
            "CUDA available:",
            torch.cuda.is_available()
        )

        if torch.cuda.is_available():

            print(
                "GPU:",
                torch.cuda.get_device_name(0)
            )

        else:

            print()
            print(
                "WARNING: CUDA tidak tersedia."
            )
            print(
                "Training NNUE besar akan sangat lambat."
            )

    except Exception as exc:

        print(
            "PyTorch check failed:",
            exc
        )

    # --------------------------------------------------------
    # Training
    # --------------------------------------------------------

    train_py = TRAINER_DIR / "train.py"

    if not train_py.exists():

        print(
            "ERROR: train.py tidak ditemukan."
        )

        sys.exit(1)

    command = [

        sys.executable,
        str(train_py),

        TRAIN_DATA,
        VALID_DATA,

        "--features",
        "HalfKAv2_hm^",

        "--batch-size",
        "16384",

        "--num-workers",
        "4",

        "--random-fen-skipping",
        "3",

        "--lambda",
        "1.0",

        "--max-epochs",
        "100",

        "--default-root-dir",
        str(OUTPUT_DIR)
    ]

    # --------------------------------------------------------
    # GPU
    # --------------------------------------------------------

    if torch.cuda.is_available():

        command.extend([
            "--accelerator",
            "cuda",
            "--gpus",
            "1"
        ])

    # --------------------------------------------------------
    # Start training
    # --------------------------------------------------------

    print()
    print("==============================================")
    print("       START SIMPLELOGICS NNUE TRAINING")
    print("==============================================")
    print()

    run(command)

    print()
    print("==============================================")
    print("       TRAINING FINISHED")
    print("==============================================")
    print()

    print(
        "Output directory:"
    )

    print(
        OUTPUT_DIR
    )


if __name__ == "__main__":

    main()
