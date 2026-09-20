/* The arena lives in src/foundation/arena.h. This file used to be a second
 * declaration of the SAME CBMArena under the SAME include guard, with a
 * shorter API: whichever header a translation unit reached first won, and the
 * two definitions had to be kept byte-compatible by hand or the struct layout
 * silently diverged across the build. One definition now. The path is
 * relative because the lsp_all unit compiles with no -Isrc. */
#ifndef CBM_INTERNAL_ARENA_SHIM_H
#define CBM_INTERNAL_ARENA_SHIM_H
#include "../../src/foundation/arena.h"
#endif /* CBM_INTERNAL_ARENA_SHIM_H */
