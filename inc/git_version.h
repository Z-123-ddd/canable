/**
 * @file    git_version.h
 * @brief   Firmware identification strings for the Keil (MDK-ARM) build.
 *
 * The GCC build passes these via -DGIT_VERSION="..." / -DGIT_REMOTE="..."
 * (see Makefile). uVision's "Define" field strips embedded quotes, so the
 * Keil project instead force-includes this header (-include git_version.h)
 * to provide the two string macros used by main.c.
 */

#ifndef GIT_VERSION_H
#define GIT_VERSION_H

#define GIT_VERSION  "v20.0.3-keil"
#define GIT_REMOTE   "github.com/Z-123-ddd/canable.git"

#endif /* GIT_VERSION_H */
