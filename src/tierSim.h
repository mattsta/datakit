#pragma once

#ifdef DATAKIT_TEST
/* The tier simulation (tierSimTest.c): drives every tiered container
 * family through growth, churn, drains, oscillation at each tier
 * boundary, bulk removal and walks that cross mutations, against a shadow
 * model, and reports which tier transitions it reached. */
int tierSimTest(int argc, char *argv[]);
#endif
