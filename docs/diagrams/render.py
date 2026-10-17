"""Regenerate the checked-in rendering diagrams using a local Graphviz installation."""
from pathlib import Path
import shutil
import subprocess


def main():
    dot = shutil.which("dot")
    if dot is None:
        raise SystemExit("Graphviz 'dot' was not found on PATH. Use a local Graphviz installation.")
    directory = Path(__file__).resolve().parent
    for source in sorted(directory.glob("*.dot")):
        result = subprocess.run(
            [dot, "-Tsvg", source.name],
            cwd=directory,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.stderr:
            raise SystemExit(result.stderr.decode("utf-8", errors="replace"))
        target = source.with_suffix(".svg")
        target.write_bytes(result.stdout)
        print(target.name)


if __name__ == "__main__":
    main()
