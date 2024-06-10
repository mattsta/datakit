#pragma once

#include "flex.h"

/* ====================================================================
 * Types
 * ==================================================================== */

/* fl[0] and fl[1] always exist.
 * head element is always head(fl[0]) is always the head of the ml.
 * tail element is either tail(fl[0]) _or_ tail(fl[1])
 *  (depending on fl[1] having elements). */
struct multilistMedium {
    flex *fl[2];
};
