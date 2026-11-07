import sys

from mylib.core import compute


def main() -> int:
    print("cli", compute(20), len(sys.argv))
    return 3
