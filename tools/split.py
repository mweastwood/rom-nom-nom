#!/usr/bin/env python3
"""
Reproducible ROM splitting wrapper for rom-nom-nom.
Verifies ROM presence and SHA-1 checksum before invoking splat.
"""

import hashlib
import os
import subprocess
import sys
from pathlib import Path

def load_config(path: Path) -> dict:
    """Load config with pyyaml if available, or lightweight key-value fallback."""
    try:
        import yaml
        with open(path, "r", encoding="utf-8") as f:
            return yaml.safe_load(f) or {}
    except ImportError:
        data = {"options": {}}
        with open(path, "r", encoding="utf-8") as f:
            in_options = False
            for line in f:
                stripped = line.strip()
                if not stripped or stripped.startswith("#"):
                    continue
                if line.startswith("options:"):
                    in_options = True
                    continue
                elif line and not line.startswith(" ") and not line.startswith("\t"):
                    in_options = False
                if ":" in stripped:
                    k, v = stripped.split(":", 1)
                    k = k.strip()
                    v = v.split("#")[0].strip().strip('"').strip("'")
                    if in_options:
                        data["options"][k] = v
                    else:
                        data[k] = v
        return data

if "BUILD_WORKSPACE_DIRECTORY" in os.environ:
    os.chdir(os.environ["BUILD_WORKSPACE_DIRECTORY"])

REPO_ROOT = Path(
    os.environ.get("BUILD_WORKSPACE_DIRECTORY") or Path(__file__).resolve().parent.parent
).resolve()
SPLAT_DIR = REPO_ROOT / "splat"
ROMS_DIR = REPO_ROOT / "roms"
VENV_BIN = REPO_ROOT / ".venv" / "bin"


def get_available_games() -> list[str]:
    """Find all configured game yaml files in splat/."""
    if not SPLAT_DIR.exists():
        return []
    return sorted(p.stem for p in SPLAT_DIR.glob("*.yaml"))


def compute_sha1(file_path: Path) -> str:
    """Compute sha1 hash of a file."""
    h = hashlib.sha1()
    with open(file_path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest().lower()


def find_splat_executable() -> str:
    """Find splat binary in virtualenv or PATH."""
    venv_splat = VENV_BIN / "splat"
    if venv_splat.exists() and os.access(venv_splat, os.X_OK):
        return str(venv_splat)
    # Check system PATH
    sys_splat = subprocess.run(["which", "splat"], capture_output=True, text=True)
    if sys_splat.returncode == 0:
        return sys_splat.stdout.strip()
    return ""


def main():
    available = get_available_games()

    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(f"Usage: {sys.argv[0]} <name-of-game> [splat-args...]")
        print("\nAvailable games:")
        if available:
            for g in available:
                print(f"  - {g}")
        else:
            print("  (none found in splat/)")
        sys.exit(0 if len(sys.argv) >= 2 and sys.argv[1] in ("-h", "--help") else 1)

    game_name = sys.argv[1]
    extra_args = sys.argv[2:]

    # Resolve game name (support exact match or stripping extensions)
    if game_name.endswith(".yaml"):
        game_name = game_name[:-5]
    if game_name.endswith(".z64"):
        game_name = game_name[:-4]

    config_path = SPLAT_DIR / f"{game_name}.yaml"
    if not config_path.exists():
        print(f"Error: Splat config not found: {config_path}")
        print(f"Available games: {', '.join(available)}")
        sys.exit(1)

    config_data = load_config(config_path)

    expected_sha1 = config_data.get("sha1", "").lower()
    options = config_data.get("options", {})
    target_rel = options.get("target_path", f"roms/{game_name}.z64")
    
    # Resolve ROM path relative to REPO_ROOT
    rom_path = (REPO_ROOT / target_rel).resolve()

    print(f"=== Splitting {config_data.get('name', game_name)} ===")
    print(f"Config:   {config_path.relative_to(REPO_ROOT)}")
    print(f"ROM:      {rom_path}")

    if not rom_path.exists():
        print(f"\n[ERROR] Target ROM not found at: {rom_path}")
        print(f"Please place the ROM file at: roms/{game_name}.z64")
        sys.exit(1)

    # Verify SHA-1 for reproducibility
    if expected_sha1:
        print("Verifying ROM SHA-1 checksum...")
        actual_sha1 = compute_sha1(rom_path)
        if actual_sha1 != expected_sha1:
            print(f"\n[ERROR] ROM SHA-1 mismatch for {rom_path.name}!")
            print(f"  Expected: {expected_sha1}")
            print(f"  Actual:   {actual_sha1}")
            print("This split configuration is pinned to the exact ROM hash above.")
            sys.exit(1)
        print(f"SHA-1 verified: {actual_sha1}")

    splat_exe = find_splat_executable()
    if not splat_exe:
        print("\n[ERROR] 'splat' executable not found!")
        print("Please activate the virtualenv or run:")
        print("  python3 -m venv .venv && .venv/bin/pip install -r requirements.txt")
        sys.exit(1)

    # Ensure build and asm output directories exist
    build_dir = REPO_ROOT / "build" / game_name
    build_dir.mkdir(parents=True, exist_ok=True)

    cmd = [splat_exe, "split", str(config_path)] + extra_args
    print(f"Running:  {' '.join(cmd)}\n")

    result = subprocess.run(cmd, cwd=REPO_ROOT)
    if result.returncode != 0:
        print(f"\n[ERROR] splat exited with code {result.returncode}")
        sys.exit(result.returncode)

    print(f"\n[SUCCESS] Split completed successfully for {game_name}.")
    print(f"Disassembly: asm/{game_name}/")
    print(f"Build data:  build/{game_name}/")


if __name__ == "__main__":
    main()
