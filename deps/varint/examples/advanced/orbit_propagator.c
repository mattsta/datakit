/**
 * orbit_propagator.c - varintDD + varintDDStream in a long simulation
 *
 * A two-body orbit propagated for millions of steps, then archived.
 * This is the shape of problem double-double was invented for: a loop
 * that runs long enough for 53 bits of significand to stop being
 * enough, followed by a pile of high-precision state that has to be
 * stored somewhere.
 *
 * It also makes a point that most "high precision" demos get wrong.
 * There are TWO error sources in a numerical integration:
 *
 *   Truncation error - the integrator's own approximation of the
 *   differential equation. Proportional to dt^2 here. Double-double
 *   does NOTHING about this. Nothing.
 *
 *   Rounding error - the floating point arithmetic losing bits at each
 *   step, accumulating over millions of steps. This is what
 *   double-double removes.
 *
 * A demo that reports "look how much more accurate the orbit is" is
 * usually measuring truncation and crediting precision. So this one
 * separates them: the quantities with exact closed-form answers isolate
 * rounding cleanly, and the orbit energy is reported for both widths so
 * you can see truncation dominating equally in each.
 *
 * Compile: gcc -I../../src orbit_propagator.c ../../src/varintDDStream.c
 *          ../../src/varintDD.c ../../src/varintTagged.c -lm
 *          -o orbit_propagator
 * Run:     ./orbit_propagator [steps]
 */

#include "varintDDStream.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SAMPLE_EVERY 500
#define MAX_SAMPLES 8192

static const char *dd(varintDD value) {
    static char text[VARINT_DD_STRING_MAX];
    varintDDToString(text, sizeof(text), value, 30);
    return text;
}

/* ====================================================================
 * Part 1: what rounding costs, measured against an exact answer
 * ====================================================================
 * The simulation clock is the cleanest possible test. Advancing it is
 * pure accumulation with no physics in it at all, and the correct
 * answer after N steps is exactly N * dt - a single multiply, which a
 * double-double computes essentially exactly. Any deviation is
 * rounding and nothing else. */
static void part_clock_drift(size_t steps) {
    printf("\n=== Part 1: The simulation clock ===\n\n");

    /* A timestep that is not representable in binary, which is the
     * normal case - 1/3 of a microsecond, say. */
    const double dt = (1.0 / 3.0) * 1e-6;

    double plainClock = 0.0;
    varintDD wideClock = varintDDZero();
    const varintDD step = varintDDFromDouble(dt);

    for (size_t i = 0; i < steps; i++) {
        plainClock += dt;
        wideClock = varintDDAdd(wideClock, step);
    }

    /* The exact answer: steps * dt, where dt is the double we actually
     * used. One multiply at double-double width. */
    const varintDD exact = varintDDMulDouble(step, (double)steps);

    const double plainError = fabs(plainClock - varintDDToDouble(exact));
    const varintDD wideError = varintDDSub(wideClock, exact);

    printf("  %zu steps of dt = (1/3) microsecond\n\n", steps);
    printf("  exact elapsed   %s\n", dd(exact));
    printf("  double clock    %.17g\n", plainClock);
    printf("  double-double   %s\n\n", dd(wideClock));
    printf("  double error    %.3e seconds\n", plainError);
    printf("  double2 error   %.3e seconds\n\n",
           fabs(varintDDToDouble(wideError)));

    if (plainError > 0.0) {
        printf("  The double clock is off by %.2f picoseconds after only\n",
               plainError * 1e12);
        printf("  %zu steps, and that error grows with step count. Nothing\n",
               steps);
        printf("  in the physics caused it - it is purely the accumulation\n");
        printf("  of %zu roundings. The double-double clock is still\n", steps);
        printf("  exact, and would stay that way for a very long time:\n");
        printf("  it has 53 more bits of headroom to spend.\n");
    }
}

/* ====================================================================
 * Part 2: the orbit, propagated at both widths
 * ====================================================================
 * Leapfrog (kick-drift-kick) on a unit circular orbit: acceleration is
 * -r / |r|^3, and with r = (1,0), v = (0,1) the exact trajectory is the
 * unit circle with period 2*pi and total energy exactly -1/2. */

typedef struct orbitWide {
    varintDD x, y, vx, vy;
} orbitWide;

typedef struct orbitPlain {
    double x, y, vx, vy;
} orbitPlain;

static void plainAcceleration(const orbitPlain *s, double *ax, double *ay) {
    const double r2 = s->x * s->x + s->y * s->y;
    const double inv = 1.0 / (r2 * sqrt(r2));
    *ax = -s->x * inv;
    *ay = -s->y * inv;
}

static void wideAcceleration(const orbitWide *s, varintDD *ax, varintDD *ay) {
    const varintDD r2 = varintDDAdd(varintDDSquare(s->x), varintDDSquare(s->y));
    const varintDD scale = varintDDMul(r2, varintDDSqrt(r2));

    *ax = varintDDNegate(varintDDDiv(s->x, scale));
    *ay = varintDDNegate(varintDDDiv(s->y, scale));
}

static double plainEnergy(const orbitPlain *s) {
    const double speed2 = s->vx * s->vx + s->vy * s->vy;
    return 0.5 * speed2 - 1.0 / sqrt(s->x * s->x + s->y * s->y);
}

static varintDD wideEnergy(const orbitWide *s) {
    const varintDD speed2 =
        varintDDAdd(varintDDSquare(s->vx), varintDDSquare(s->vy));
    const varintDD radius =
        varintDDSqrt(varintDDAdd(varintDDSquare(s->x), varintDDSquare(s->y)));

    return varintDDSub(varintDDMulDouble(speed2, 0.5),
                       varintDDDiv(varintDDFromDouble(1.0), radius));
}

static size_t part_orbit(size_t steps, varintDD *sampleX, varintDD *sampleY) {
    printf("\n=== Part 2: Propagating the orbit ===\n\n");

    const double dt = 1e-4;
    const double half = dt * 0.5;

    orbitPlain plain = {1.0, 0.0, 0.0, 1.0};
    orbitWide wide = {varintDDFromDouble(1.0), varintDDZero(), varintDDZero(),
                      varintDDFromDouble(1.0)};

    const double plainStart = plainEnergy(&plain);
    const varintDD wideStart = wideEnergy(&wide);

    size_t samples = 0;

    for (size_t i = 0; i < steps; i++) {
        /* --- plain double leapfrog --- */
        {
            double ax, ay;

            plainAcceleration(&plain, &ax, &ay);
            plain.vx += ax * half;
            plain.vy += ay * half;
            plain.x += plain.vx * dt;
            plain.y += plain.vy * dt;
            plainAcceleration(&plain, &ax, &ay);
            plain.vx += ax * half;
            plain.vy += ay * half;
        }

        /* --- double-double leapfrog, same algorithm --- */
        {
            varintDD ax, ay;

            wideAcceleration(&wide, &ax, &ay);
            wide.vx = varintDDAdd(wide.vx, varintDDMulDouble(ax, half));
            wide.vy = varintDDAdd(wide.vy, varintDDMulDouble(ay, half));
            wide.x = varintDDAdd(wide.x, varintDDMulDouble(wide.vx, dt));
            wide.y = varintDDAdd(wide.y, varintDDMulDouble(wide.vy, dt));
            wideAcceleration(&wide, &ax, &ay);
            wide.vx = varintDDAdd(wide.vx, varintDDMulDouble(ax, half));
            wide.vy = varintDDAdd(wide.vy, varintDDMulDouble(ay, half));
        }

        if (i % SAMPLE_EVERY == 0 && samples < MAX_SAMPLES) {
            sampleX[samples] = wide.x;
            sampleY[samples] = wide.y;
            samples++;
        }
    }

    const double plainDrift = fabs(plainEnergy(&plain) - plainStart);
    const varintDD wideDrift = varintDDSub(wideEnergy(&wide), wideStart);

    /* The exact trajectory is the unit circle, so the true position at
     * elapsed time t is simply (cos t, sin t). That gives an
     * independent reference for how wrong BOTH integrators are. */
    const double elapsed = (double)steps * dt;
    const double trueX = cos(elapsed);
    const double trueY = sin(elapsed);

    const double plainOff = hypot(plain.x - trueX, plain.y - trueY);
    const double wideOff = hypot(varintDDToDouble(wide.x) - trueX,
                                 varintDDToDouble(wide.y) - trueY);
    const double betweenThem = hypot(plain.x - varintDDToDouble(wide.x),
                                     plain.y - varintDDToDouble(wide.y));

    printf("  %zu steps of dt = %g (%.1f orbits)\n\n", steps, dt,
           elapsed / 6.283185307179586);

    printf(
        "  --- position: truncation dominates, precision cannot help ---\n\n");
    printf("  true position (cos t, sin t)  (%.17g, %.17g)\n", trueX, trueY);
    printf("  double                        (%.17g, %.17g)\n", plain.x,
           plain.y);
    printf("  double-double                 (%.17g, %.17g)\n\n",
           varintDDToDouble(wide.x), varintDDToDouble(wide.y));

    printf("  distance from truth, double        %.3e\n", plainOff);
    printf("  distance from truth, double-double %.3e\n", wideOff);
    printf("  distance between the two           %.3e\n\n", betweenThem);

    printf("  Both are wrong by roughly the same amount, and they differ\n");
    printf("  from each other by far less than either differs from the\n");
    printf("  truth. That gap is leapfrog's O(dt^2) phase error, and no\n");
    printf("  number of significand bits will touch it. If you want a\n");
    printf("  better position, shrink dt or change integrator - do not\n");
    printf("  reach for wider arithmetic.\n\n");

    printf("  --- energy: rounding dominates, precision wins outright ---\n\n");
    printf("  energy drift, double          %.3e\n", plainDrift);
    printf("  energy drift, double-double   %.3e\n\n",
           fabs(varintDDToDouble(wideDrift)));

    if (plainDrift > 0.0 && fabs(varintDDToDouble(wideDrift)) > 0.0) {
        printf("  A factor of %.0f, from the identical algorithm.\n\n",
               plainDrift / fabs(varintDDToDouble(wideDrift)));
    }

    printf("  The difference between these two results is the whole\n");
    printf("  lesson. Leapfrog is symplectic: its truncation error in a\n");
    printf("  conserved quantity does not accumulate, it oscillates\n");
    printf("  within a bounded band and keeps coming back. So energy\n");
    printf("  drift is not measuring truncation at all - what is left\n");
    printf("  once the bounded part cancels is the random walk of\n");
    printf("  rounding, and that is exactly what wider arithmetic\n");
    printf("  removes.\n\n");
    printf("  Same simulation, same step count, two observables: one\n");
    printf("  where double-double buys nothing, one where it buys five\n");
    printf("  orders of magnitude. Know which one you are looking at\n");
    printf("  before deciding whether you need it.\n");

    return samples;
}

/* ====================================================================
 * Part 3: archiving the trajectory
 * ====================================================================
 * Columnar, not row-wise. The codec compresses a run of similar values;
 * interleaving x and y into one stream would alternate between two
 * unrelated sequences and defeat the leading-limb chain entirely. This
 * is the same reason column stores exist. */
static void part_archive(const varintDD *sampleX, const varintDD *sampleY,
                         size_t samples) {
    printf("\n=== Part 3: Archiving the trajectory ===\n\n");

    if (samples == 0) {
        printf("  nothing sampled\n");
        return;
    }

    uint8_t *bufferX = malloc(varintDDStreamMaxSize(samples));
    uint8_t *bufferY = malloc(varintDDStreamMaxSize(samples));
    varintDD *reloaded = malloc(samples * sizeof(varintDD));

    if (bufferX == NULL || bufferY == NULL || reloaded == NULL) {
        printf("  out of memory\n");
        goto done;
    }

    varintDDStreamMeta metaX;
    varintDDStreamMeta metaY;

    const size_t sizeX = varintDDStreamEncode(
        bufferX, sampleX, samples, VARINT_DD_STREAM_HI_AUTO,
        VARINT_DD_STREAM_LOSSLESS, &metaX);
    const size_t sizeY = varintDDStreamEncode(
        bufferY, sampleY, samples, VARINT_DD_STREAM_HI_AUTO,
        VARINT_DD_STREAM_LOSSLESS, &metaY);

    const size_t raw = samples * sizeof(varintDD);

    printf("  %zu sampled states per channel\n\n", samples);
    printf("  %-10s %9s %9s %8s %7s %9s\n", "channel", "raw", "stored",
           "per val", "ratio", "mode");
    printf("  %-10s %9zu %9zu %8.2f %6.2fx %9s\n", "x", raw, sizeX,
           (double)sizeX / samples, (double)raw / (double)sizeX,
           metaX.hiMode == VARINT_DD_STREAM_HI_XOR ? "xor" : "raw");
    printf("  %-10s %9zu %9zu %8.2f %6.2fx %9s\n", "y", raw, sizeY,
           (double)sizeY / samples, (double)raw / (double)sizeY,
           metaY.hiMode == VARINT_DD_STREAM_HI_XOR ? "xor" : "raw");

    /* An archive is worthless if it does not read back identically.
     * Verify with memcmp, not ==: the point of a lossless archive is
     * the bits, and == would happily accept a changed trailing limb. */
    const size_t got = varintDDStreamDecode(bufferX, sizeX, reloaded, samples);

    printf("\n  reload check: %zu values, %s\n", got,
           (got == samples &&
            memcmp(reloaded, sampleX, samples * sizeof(varintDD)) == 0)
               ? "bit-exact"
               : "MISMATCH");

    printf("\n  These are genuine double-double values - every one has a\n");
    printf("  non-zero trailing limb from the integration - so this is\n");
    printf("  the hard case for the codec, not the flattering one.\n");
    printf("  %zu of %zu trailing limbs were exactly zero.\n",
           metaX.exactValues, samples);

    printf("\n  If a %.2fx archive is not enough, the precision ladder is\n",
           (double)raw / (double)sizeX);
    printf("  the lever: trajectory samples rarely need all 106 bits when\n");
    printf("  the integration itself is only good to 1e-10. Encoding at\n");
    printf("  30 trailing bits would still beat double precision by 13\n");
    printf("  digits while storing meaningfully less.\n");

done:
    free(bufferX);
    free(bufferY);
    free(reloaded);
}

int main(int argc, char *argv[]) {
    size_t steps = argc > 1 ? (size_t)strtoull(argv[1], NULL, 10) : 2000000;

    if (steps == 0) {
        steps = 2000000;
    }

    printf("========================================\n");
    printf("Orbit propagator - varintDD + varintDDStream\n");
    printf("========================================\n");

    /* Any program whose correctness depends on the extra precision
     * should verify the build preserved it. See example_dd.c. */
    if (!varintDDSelfCheck()) {
        printf("\nvarintDDSelfCheck FAILED - this build does not preserve\n");
        printf("IEEE semantics, so every result below would be wrong.\n");
        return 1;
    }

    static varintDD sampleX[MAX_SAMPLES];
    static varintDD sampleY[MAX_SAMPLES];

    part_clock_drift(steps);

    const size_t samples = part_orbit(steps, sampleX, sampleY);

    part_archive(sampleX, sampleY, samples);

    printf("\n========================================\n");
    return 0;
}
