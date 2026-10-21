#!/usr/bin/env python3
"""The shipped help must document every opt-in flat pipeline flag."""

import subprocess
import sys

FLAGS = ("--opt=s17", "--opt=s19", "--opt=s20", "--opt=s24", "--opt=s24b", "--opt=s24c", "--opt=s25",
         "--dump-opt-stats")


def main():
    text = subprocess.check_output([sys.argv[1], "--help"], text=True)
    missing = [flag for flag in FLAGS if flag not in text]
    if missing:
        raise AssertionError(f"help is missing {missing}")
    print(f"CLI_HELP_FLAGS={len(FLAGS)} FAILURES=0")


if __name__ == "__main__":
    main()
