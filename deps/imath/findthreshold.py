#!/usr/bin/env python3
##
## Name:     findthreshold.py
## Purpose:  Find a good threshold for recursive multiplication.
## Author:   M. J. Fromberger <http://spinning-yarns.org/michael/>
## Info:     $Id: findthreshold.py 456 2006-08-16 14:24:06Z sting $
##
## This tool computes some timing statistics to help you select a suitable
## recursive multiplication breakpoint.  It uses the imtimer tool to run a
## series of tests varying precision and breakpoint, and prints out a summary
## of the "best" values for each category.  Each summary line contains the
## following fields, tab-separated:
##
## prec     -- the precision of the operands (in digits).
## thresh   -- the threshold for recursive multiplication (digits).
## trec     -- total time using recursive algorithm (sec).
## tnorm    -- total time without recursive algorithm (sec).
## ratio    -- speedup (ratio of tnorm/trec).
##
## You are responsible for reading and interpreting the resulting table to
## obtain a useful value for your workload.  Change the value of MP_MULT_THRESH
## in imath.h once you have a satisfactory result.
##
import os
import sys
import time
from pathlib import Path


def get_timing_stats(num_tests, precision, threshold, seed=None):
    """Obtain timing statistics for multiplication.

    num_tests      -- number of tests to run.
    precision      -- number of digits per operand.
    threshold      -- threshold in digits for recursive multiply.
    seed           -- random seed; if None, the clock is used.

    Returns a tuple of (seed, bits, time) where seed is the random seed used,
    bits is the number of bits per operand, and time is a float giving the
    total time taken for the test run.
    """
    if seed is None:
        seed = int(time.time())

    line = os.popen(
        f"./imtimer -mn -p {precision} -t {threshold} -s {seed} {num_tests}", "r"
    ).readline()

    count, prec, bits, thresh, status = line.strip().split("\t")
    kind, total, unit = status.split()

    return seed, int(bits), float(total)


def check_binary(name):
    path = Path(name)
    if not path.exists():
        os.system(f"make {name}")
        if not path.exists():
            raise ValueError(f"Unable to build {name}")
    elif not path.is_file():
        raise ValueError("Path exists with wrong type")


def compute_stats():
    check_binary("imtimer")
    seed = int(time.time())

    print("Computing timer statistics (this may take a while)", file=sys.stderr)
    stats = {}
    for prec in (32, 40, 64, 80, 128, 150, 256, 384, 512, 600, 768, 1024):
        sys.stderr.write(f"{prec:<4d} ")
        stats[prec] = (None, 1000000.0, 0.0)

        for thresh in range(16, 65, 2):
            s, b, t = get_timing_stats(1000, prec, thresh, seed)
            sp, bp, tp = get_timing_stats(1000, prec, prec + 1, seed)

            if t < stats[prec][1]:
                stats[prec] = (thresh, t, tp)
                sys.stderr.write("+")
            else:
                sys.stderr.write(".")
        sys.stderr.write("\n")

    return list((p, h, t, tp) for p, (h, t, tp) in stats.items())


if __name__ == "__main__":
    stats = compute_stats()
    stats.sort(key=lambda s: s[3] / s[2])
    for prec, thresh, trec, tnorm in stats:
        print(f"{prec}\t{thresh}\t{trec:.3f}\t{tnorm:.3f}\t{tnorm / trec:.4f}")

    print()

# Here there be dragons
