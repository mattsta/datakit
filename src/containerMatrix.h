#pragma once

/* containerMatrix: create, read, update and delete throughput and bytes
 * per element of every container, for each element kind and size, as
 * MATRIX rows on stdout (`dktools matrix` builds the report; see
 * docs/PERFORMANCE_MATRIX.md).  As a test it runs one small size and
 * checks every result; DK_MATRIX=full measures the full sizes. */

#ifdef DATAKIT_TEST
int containerMatrixTest(int argc, char *argv[]);
#endif
