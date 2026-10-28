#pragma once

#include "flex.h"
/* ====================================================================
 * Types
 * ==================================================================== */
/* A Small list has no header: its multilist pointer is its flex.  This
 * is the view each call builds on the stack from that pointer; a call
 * that changes the elements leaves the (maybe moved) flex here for the
 * caller to store back in the pointer. */
struct multilistSmall {
    flex *fl;
};
