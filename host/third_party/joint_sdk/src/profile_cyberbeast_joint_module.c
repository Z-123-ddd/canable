/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief L2 static device profile: CyberBeast Joint Module.
 *
 * Facts fixed from the drive ESI (ECAT_CIA402.xml) and the protocol manual:
 *   vendor 0x000C0B00 / product 0x00080117 / rev 0x00000001
 *   DC AssignActivate 0x300, min cycle 50 us
 *   full-featured PDO set 0x1600 / 0x1A00 (5 objects + 8-bit padding each)
 */

#include "internal.h"
#include <string.h>

#define CB_VENDOR_ID   0x000C0B00u
#define CB_PRODUCT_ID  0x00080117u
#define CB_REVISION    0x00000001u
#define CB_DC_ASSIGN   0x0300u
#define CB_MIN_CYCLE_NS 50000u

static const jsdk_pdo_entry_t cb_rx_entries[] =
{
   { JSDK_CIA402_CONTROLWORD, 0, 16 },
   { JSDK_CIA402_TARGET_POSITION, 0, 32 },
   { JSDK_CIA402_TARGET_VELOCITY, 0, 32 },
   { JSDK_CIA402_TARGET_TORQUE, 0, 16 },
   { JSDK_CIA402_MODE_OF_OPERATION, 0, 8 },
   { 0x0000, 0, 8 }, /* padding */
};

static const jsdk_pdo_entry_t cb_tx_entries[] =
{
   { JSDK_CIA402_STATUSWORD, 0, 16 },
   { JSDK_CIA402_ACTUAL_POSITION, 0, 32 },
   { JSDK_CIA402_ACTUAL_VELOCITY, 0, 32 },
   { JSDK_CIA402_ACTUAL_TORQUE, 0, 16 },
   { JSDK_CIA402_MODE_DISPLAY, 0, 8 },
   { 0x0000, 0, 8 }, /* padding */
};

static const struct jsdk_joint_profile cb_profile =
{
   JSDK_PROFILE_CYBERBEAST_JOINT_MODULE,
   CB_VENDOR_ID,
   CB_PRODUCT_ID,
   CB_REVISION,
   CB_DC_ASSIGN,
   CB_MIN_CYCLE_NS,
   JSDK_MODE_CSP,
   JSDK_MODE_CSV,
   JSDK_MODE_CST,
   (unsigned)(sizeof(cb_rx_entries) / sizeof(cb_rx_entries[0])),
   (unsigned)(sizeof(cb_tx_entries) / sizeof(cb_tx_entries[0])),
   0x1600u,
   0x1A00u,
   cb_rx_entries,
   cb_tx_entries,
   NULL /* heap_block: static profile */
};

const struct jsdk_joint_profile *
jsdk_profile_cyberbeast_joint_module(void)
{
   return &cb_profile;
}

const struct jsdk_joint_profile *
jsdk_profile_find(const char *name)
{
   if (!name || !name[0] ||
       !strcmp(name, JSDK_PROFILE_CYBERBEAST_JOINT_MODULE) ||
       !strcmp(name, "cyberbeast_joint_module") ||
       !strcmp(name, "default") ||
       !strcmp(name, "CyberBeast"))
   {
      return &cb_profile;
   }
   return NULL;
}
