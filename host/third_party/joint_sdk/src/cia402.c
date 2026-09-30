/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief L3 CiA402 (DS402) axis state machine - pure logic, no I/O.
 * Aligned with IgH joint-sdk cia402.c (disable ladder + CW 0x000F).
 */

#include "internal.h"

static jsdk_axis_state_t
decode_state(uint16_t statusword)
{
   if (statusword & JSDK_CIA402_SW_FAULT)
   {
      return JSDK_AXIS_FAULT;
   }
   switch (statusword & JSDK_CIA402_SW_MASK)
   {
   case JSDK_CIA402_SW_SWITCH_ON_DISABLED:
      return JSDK_AXIS_SWITCH_ON_DISABLED;
   case JSDK_CIA402_SW_READY_TO_SWITCH_ON:
      return JSDK_AXIS_READY_TO_SWITCH_ON;
   case JSDK_CIA402_SW_SWITCHED_ON:
      return JSDK_AXIS_SWITCHED_ON;
   case JSDK_CIA402_SW_OPERATION_ENABLED:
      return JSDK_AXIS_OPERATION_ENABLED;
   default:
      return JSDK_AXIS_UNKNOWN;
   }
}

void
jsdk_cia402_init(jsdk_cia402_axis_t *axis)
{
   if (!axis)
   {
      return;
   }
   axis->enable_requested = 0;
   axis->fault_reset_requested = 0;
   axis->homing_start_requested = 0;
   axis->requested_mode = 0;
   axis->controlword = JSDK_CIA402_CW_SHUTDOWN;
   axis->axis_state = JSDK_AXIS_UNKNOWN;
   axis->operation_enabled = 0;
}

void
jsdk_cia402_request_enable(jsdk_cia402_axis_t *axis, int8_t mode)
{
   if (!axis)
   {
      return;
   }
   axis->requested_mode = mode;
   axis->enable_requested = 1;
}

void
jsdk_cia402_request_disable(jsdk_cia402_axis_t *axis)
{
   if (!axis)
   {
      return;
   }
   axis->enable_requested = 0;
   axis->homing_start_requested = 0;
   axis->operation_enabled = 0;
   axis->controlword = JSDK_CIA402_CW_DISABLE_OP;
}

void
jsdk_cia402_request_homing_start(jsdk_cia402_axis_t *axis)
{
   if (!axis)
   {
      return;
   }
   axis->homing_start_requested = 1;
}

void
jsdk_cia402_clear_homing_start(jsdk_cia402_axis_t *axis)
{
   if (!axis)
   {
      return;
   }
   axis->homing_start_requested = 0;
}

void
jsdk_cia402_request_fault_reset(jsdk_cia402_axis_t *axis)
{
   if (!axis)
   {
      return;
   }
   axis->fault_reset_requested = 1;
}

void
jsdk_cia402_set_mode(jsdk_cia402_axis_t *axis, int8_t mode)
{
   if (!axis)
   {
      return;
   }
   if (!axis->enable_requested)
   {
      axis->requested_mode = mode;
   }
}

void
jsdk_cia402_update(jsdk_cia402_axis_t *axis, uint16_t statusword)
{
   if (!axis)
   {
      return;
   }
   axis->axis_state = decode_state(statusword);
   axis->operation_enabled = 0;

   if (axis->axis_state == JSDK_AXIS_FAULT)
   {
      if (axis->fault_reset_requested || axis->enable_requested)
      {
         axis->controlword = JSDK_CIA402_CW_FAULT_RESET;
         axis->fault_reset_requested = 0;
      }
      else
      {
         axis->controlword = JSDK_CIA402_CW_DISABLE_VOLTAGE;
      }
      return;
   }

   axis->fault_reset_requested = 0;

   /* IgH controlled-stop ladder */
   if (!axis->enable_requested)
   {
      switch (axis->axis_state)
      {
      case JSDK_AXIS_OPERATION_ENABLED:
         axis->controlword = JSDK_CIA402_CW_DISABLE_OP;
         break;
      case JSDK_AXIS_SWITCHED_ON:
         axis->controlword = JSDK_CIA402_CW_SHUTDOWN;
         break;
      case JSDK_AXIS_READY_TO_SWITCH_ON:
      case JSDK_AXIS_SWITCH_ON_DISABLED:
      default:
         axis->controlword = JSDK_CIA402_CW_DISABLE_VOLTAGE;
         break;
      }
      return;
   }

   switch (axis->axis_state)
   {
   case JSDK_AXIS_SWITCH_ON_DISABLED:
      axis->controlword = JSDK_CIA402_CW_SHUTDOWN;
      break;
   case JSDK_AXIS_READY_TO_SWITCH_ON:
      axis->controlword = JSDK_CIA402_CW_SWITCH_ON;
      break;
   case JSDK_AXIS_SWITCHED_ON:
      axis->controlword = JSDK_CIA402_CW_ENABLE_OP;
      break;
   case JSDK_AXIS_OPERATION_ENABLED:
      /* Homing: bit4 only after OE (rising edge of Homing operation start). */
      if (axis->requested_mode == (int8_t)JSDK_MODE_HOMING &&
          axis->homing_start_requested)
         axis->controlword = JSDK_CIA402_CW_HOMING_START;
      else
         axis->controlword = JSDK_CIA402_CW_ENABLE_OP;
      axis->operation_enabled = 1;
      break;
   default:
      axis->controlword = JSDK_CIA402_CW_SHUTDOWN;
      break;
   }
}
