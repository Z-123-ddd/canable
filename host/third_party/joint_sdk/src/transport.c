/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief L1 transport layer: SOEM v2 encapsulation.
 *
 * Implements the stable C ABI from <joint_sdk/joint_sdk.h> on top of SOEM:
 * lifecycle, PO2SOconfig-driven PDO assignment, process-data mapping with an
 * offset walker (padding-aware), DC configuration, SAFE_OP/OP transitions and
 * the cyclic send/receive boundary. See ARCHITECTURE.zh-CN.md sections 3-8.
 */

#include "internal.h"
#include <joint_sdk/dc_timing_conf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* drive DC AssignActivate value goes to ESC register 0x0980 (mirrors IgH) */
#define JSDK_REG_DC_ASSIGN_ACTIVATE 0x0980u

/* ==================================================================== */
/* little-endian process-data / CoE payload helpers                     */
/* ==================================================================== */
static void
pd_put_u16(uint8_t *p, uint16_t v)
{
   p[0] = (uint8_t)(v & 0xFF);
   p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static uint16_t
pd_get_u16(const uint8_t *p)
{
   return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void
pd_put_u32(uint8_t *p, uint32_t v)
{
   p[0] = (uint8_t)(v & 0xFF);
   p[1] = (uint8_t)((v >> 8) & 0xFF);
   p[2] = (uint8_t)((v >> 16) & 0xFF);
   p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static int32_t
pd_get_i32(const uint8_t *p)
{
   uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
   return (int32_t)v;
}

/* ==================================================================== */
/* offset walker: accumulates bit offsets of one PDO into `off`.        */
/* Padding entries MUST be kept so later fields stay byte-aligned.      */
/* Returns total size in bytes, or -1 if a field is not byte aligned.   */
/* ==================================================================== */
static int
walk_offsets(const jsdk_pdo_entry_t *entries, unsigned int count,
             jsdk_pdo_offsets_t *off)
{
   unsigned int i, bitpos = 0;

   for (i = 0; i < count; ++i)
   {
      if (entries[i].index == 0)
      {
         bitpos += entries[i].bitlen; /* padding */
         continue;
      }
      if ((bitpos % 8) != 0)
      {
         return -1;
      }
      switch (entries[i].index)
      {
      case JSDK_CIA402_CONTROLWORD: off->controlword = bitpos / 8; break;
      case JSDK_CIA402_TARGET_POSITION: off->target_position = bitpos / 8; break;
      case JSDK_CIA402_TARGET_VELOCITY: off->target_velocity = bitpos / 8; break;
      case JSDK_CIA402_TARGET_TORQUE: off->target_torque = bitpos / 8; break;
      case JSDK_CIA402_MODE_OF_OPERATION: off->mode_of_operation = bitpos / 8; break;
      case JSDK_CIA402_STATUSWORD: off->statusword = bitpos / 8; break;
      case JSDK_CIA402_ACTUAL_POSITION: off->actual_position = bitpos / 8; break;
      case JSDK_CIA402_ACTUAL_VELOCITY: off->actual_velocity = bitpos / 8; break;
      case JSDK_CIA402_ACTUAL_TORQUE: off->actual_torque = bitpos / 8; break;
      case JSDK_CIA402_MODE_DISPLAY: off->mode_display = bitpos / 8; break;
      default: break;
      }
      bitpos += entries[i].bitlen;
   }
   return (int)((bitpos + 7) / 8);
}

static unsigned int
profile_total_bits(const jsdk_pdo_entry_t *entries, unsigned int count)
{
   unsigned int i, bits = 0;
   for (i = 0; i < count; ++i)
   {
      bits += entries[i].bitlen;
   }
   return bits;
}

/* ==================================================================== */
/* error / string helpers                                               */
/* ==================================================================== */
void
jsdk_set_error(jsdk_context_t *ctx, const char *fmt, ...)
{
   va_list ap;

   if (!ctx)
   {
      return;
   }
   va_start(ap, fmt);
   vsnprintf(ctx->last_error, sizeof(ctx->last_error), fmt, ap);
   va_end(ap);
}

const char *
jsdk_context_last_error(jsdk_context_t *ctx)
{
   return ctx ? ctx->last_error : "null context";
}

const char *
jsdk_status_string(jsdk_status_t status)
{
   switch (status)
   {
   case JSDK_OK: return "ok";
   case JSDK_ERR_INVALID_ARG: return "invalid argument";
   case JSDK_ERR_NO_MEMORY: return "out of memory";
   case JSDK_ERR_NOT_FOUND: return "not found";
   case JSDK_ERR_BAD_STATE: return "bad state";
   case JSDK_ERR_ECRT: return "ethercat error";
   case JSDK_ERR_UNSUPPORTED: return "unsupported";
   default: return "unknown";
   }
}

const char *
jsdk_axis_state_string(jsdk_axis_state_t state)
{
   switch (state)
   {
   case JSDK_AXIS_UNKNOWN: return "unknown";
   case JSDK_AXIS_SWITCH_ON_DISABLED: return "switch_on_disabled";
   case JSDK_AXIS_READY_TO_SWITCH_ON: return "ready_to_switch_on";
   case JSDK_AXIS_SWITCHED_ON: return "switched_on";
   case JSDK_AXIS_OPERATION_ENABLED: return "operation_enabled";
   case JSDK_AXIS_FAULT: return "fault";
   default: return "invalid";
   }
}

/* ==================================================================== */
/* small helpers                                                        */
/* ==================================================================== */
static jsdk_joint_t *
joint_find_by_slave(jsdk_context_t *ctx, uint16_t slave)
{
   unsigned int i;
   for (i = 0; i < ctx->joint_count; ++i)
   {
      if (ctx->joints[i]->slave == slave)
      {
         return ctx->joints[i];
      }
   }
   return NULL;
}

/* one send+receive roundtrip */
static int
roundtrip(ecx_contextt *context)
{
   ecx_send_processdata(context);
   return ecx_receive_processdata(context, EC_TIMEOUTRET);
}

/* wait for one joint to reach a state (per-slave AL status read) */
static int
wait_joint_state(jsdk_context_t *ctx, jsdk_joint_t *joint, uint16_t state,
                 int attempts, int timeout)
{
   uint16_t st = 0;
   int i;

   for (i = 0; i < attempts; ++i)
   {
      st = ecx_statecheck(&ctx->soem, joint->slave, state, timeout);
      if (st == state)
      {
         return 0;
      }
   }
   jsdk_set_error(ctx,
                  "slave %u failed to reach state 0x%02X (now 0x%02X, "
                  "AL status 0x%04X %s)",
                  joint->slave, state, st,
                  ctx->soem.slavelist[joint->slave].ALstatuscode,
                  ec_ALstatuscode2string(
                      ctx->soem.slavelist[joint->slave].ALstatuscode));
   return -1;
}

/* stage the (safe) initial outputs of one joint and return its output ptr */
static void
joint_stage_outputs(jsdk_context_t *ctx, jsdk_joint_t *joint)
{
   ec_slavet *s = &ctx->soem.slavelist[joint->slave];
   uint8_t *out = s->outputs;

   if (!out)
   {
      return;
   }
   pd_put_u16(out + joint->offsets.controlword, joint->cia402.controlword);
   pd_put_u32(out + joint->offsets.target_position, 0);
   pd_put_u32(out + joint->offsets.target_velocity, 0);
   pd_put_u16(out + joint->offsets.target_torque, 0);
   /* Always drive 0x6060. Leaving the IOmap zero would PDO-write mode=0 and
    * wipe any PRE_OP SDO mode selection on every frame. */
   {
      int8_t mode = joint->cia402.requested_mode ?
                        joint->cia402.requested_mode :
                        (joint->profile ? joint->profile->mode_csp
                                        : (int8_t)JSDK_MODE_CSP);
      out[joint->offsets.mode_of_operation] = (uint8_t)mode;
   }
}

/* Per-slave DC: joint fields override context defaults; 0 inherits. */
static void
joint_dc_params(const jsdk_context_t *ctx, const jsdk_joint_t *joint,
                uint32_t *cycle_ns, int32_t *shift_ns, unsigned int *wait_ms)
{
   uint32_t cyc;
   int32_t shift;
   unsigned int wait;

   cyc = joint->sync0_cycle_ns;
   if (!cyc)
      cyc = ctx->config.sync0_cycle_ns;
   if (!cyc)
      cyc = ctx->config.period_ns;
   if (!cyc)
      cyc = JSDK_DEFAULT_PERIOD_NS;

   shift = joint->sync0_shift_ns;
   if (!shift)
      shift = ctx->config.sync0_shift_ns;
   if (!shift)
      shift = (int32_t)(cyc / 4);

   wait = joint->wait_ms;
   if (!wait)
      wait = ctx->config.wait_ms;
   if (!wait)
      wait = JSDK_DEFAULT_WAIT_MS;

   if (cycle_ns)
      *cycle_ns = cyc;
   if (shift_ns)
      *shift_ns = shift;
   if (wait_ms)
      *wait_ms = wait;
}

/* ==================================================================== */
/* PO2SOconfig hook: called by SOEM in PRE_OP during config_map_group,  */
/* before the PDO map is read back. Selects the full-featured PDO set.  */
/* ==================================================================== */
static int
joint_po2so_config(ecx_contextt *context, uint16_t slave)
{
   jsdk_context_t *ctx = JSDK_FROM_SOEM(context);
   jsdk_joint_t *joint = joint_find_by_slave(ctx, slave);
   uint8_t d8;
   uint8_t d16le[2];
   int wkc;

   if (!joint)
   {
      return 0;
   }

#define SDO_U8(IDX, SUB, VAL)                                          \
   do                                                                 \
   {                                                                  \
      d8 = (uint8_t)(VAL);                                            \
      wkc = ecx_SDOwrite(context, slave, (IDX), (SUB), FALSE, 1,      \
                         &d8, EC_TIMEOUTRXM);                         \
      if (wkc <= 0)                                                   \
      {                                                               \
         jsdk_set_error(ctx, "PO2SOconfig: SDO %04X:%u write failed " \
                             "(wkc=%d)", (IDX), (SUB), wkc);          \
         return 0;                                                    \
      }                                                               \
   } while (0)

#define SDO_U16(IDX, SUB, VAL)                                         \
   do                                                                  \
   {                                                                   \
      pd_put_u16(d16le, (uint16_t)(VAL));                              \
      wkc = ecx_SDOwrite(context, slave, (IDX), (SUB), FALSE, 2,       \
                         d16le, EC_TIMEOUTRXM);                        \
      if (wkc <= 0)                                                    \
      {                                                                \
         jsdk_set_error(ctx, "PO2SOconfig: SDO %04X:%u write failed "  \
                             "(wkc=%d)", (IDX), (SUB), wkc);           \
         return 0;                                                     \
      }                                                                \
   } while (0)

   /* RxPDO assignment: 0x1C12 <- profile rx_pdo_index (0x1600) */
   SDO_U8(JSDK_CIA402_RXPDO_ASSIGN, 0x00, 0);
   SDO_U16(JSDK_CIA402_RXPDO_ASSIGN, 0x01, joint->profile->rx_pdo_index);
   SDO_U8(JSDK_CIA402_RXPDO_ASSIGN, 0x00, 1);
   /* TxPDO assignment: 0x1C13 <- profile tx_pdo_index (0x1A00) */
   SDO_U8(JSDK_CIA402_TXPDO_ASSIGN, 0x00, 0);
   SDO_U16(JSDK_CIA402_TXPDO_ASSIGN, 0x01, joint->profile->tx_pdo_index);
   SDO_U8(JSDK_CIA402_TXPDO_ASSIGN, 0x00, 1);

   /* Default mode via SDO (many drives ignore 0x6060 PDO until this is set). */
   SDO_U8(JSDK_CIA402_MODE_OF_OPERATION, 0x00, joint->profile->mode_csp);

   /* Optional Homing params in PREOP (this drive often rejects OP mailbox SDO).
    * Set before configure: JSDK_HOMING_METHOD=17|35|37
    * Optional: JSDK_HOMING_OFFSET / JSDK_HOMING_SPEED / JSDK_HOMING_ACCEL */
   {
      const char *hm = getenv("JSDK_HOMING_METHOD");
      if (hm && hm[0])
      {
         int method = atoi(hm);
         if (method == 17 || method == 35 || method == 37)
         {
            uint8_t mode6 = 6;
            uint8_t meth = (uint8_t)method;
            int w;
            const char *off = getenv("JSDK_HOMING_OFFSET");
            const char *spd = getenv("JSDK_HOMING_SPEED");
            const char *acc = getenv("JSDK_HOMING_ACCEL");
            w = ecx_SDOwrite(context, slave, 0x6060, 0, FALSE, 1, &mode6,
                             EC_TIMEOUTRXM);
            printf("PO2SOconfig: Homing 0x6060=6 wkc=%d\n", w);
            w = ecx_SDOwrite(context, slave, 0x6098, 0, FALSE, 1, &meth,
                             EC_TIMEOUTRXM);
            printf("PO2SOconfig: Homing 0x6098=%u wkc=%d\n", meth, w);
            if (method == 17 || method == 37)
            {
               int32_t v = off ? (int32_t)strtol(off, NULL, 0) : 81920;
               uint8_t d32[4];
               pd_put_u32(d32, (uint32_t)v);
               w = ecx_SDOwrite(context, slave, 0x607C, 0, FALSE, 4, d32,
                                EC_TIMEOUTRXM);
               printf("PO2SOconfig: Homing 0x607C=%d wkc=%d\n", (int)v, w);
            }
            if (method == 17)
            {
               int32_t vs = spd ? (int32_t)strtol(spd, NULL, 0) : 8192;
               int32_t va = acc ? (int32_t)strtol(acc, NULL, 0) : 262144;
               uint8_t d32[4];
               pd_put_u32(d32, (uint32_t)vs);
               w = ecx_SDOwrite(context, slave, 0x6099, 1, FALSE, 4, d32,
                                EC_TIMEOUTRXM);
               printf("PO2SOconfig: Homing 0x6099:1=%d wkc=%d\n", (int)vs, w);
               pd_put_u32(d32, (uint32_t)va);
               w = ecx_SDOwrite(context, slave, 0x609A, 0, FALSE, 4, d32,
                                EC_TIMEOUTRXM);
               printf("PO2SOconfig: Homing 0x609A=%d wkc=%d\n", (int)va, w);
            }
         }
      }
   }

   /* 0x2002:01 CyberBeast input_mode (IgH README):
    *   3 = POS_FILTER  — CSP
    *   1 = PASSTHROUGH — CSV / CST
    * Override with env JSDK_INPUT_MODE=1|3 */
   {
      uint8_t mode_opt = 3;
      uint16_t rb = 0xFFFF;
      int sz = 2;
      int wkc2;
      const char *im = getenv("JSDK_INPUT_MODE");
      if (im && im[0])
         mode_opt = (uint8_t)atoi(im);
      wkc2 = ecx_SDOwrite(context, slave, 0x2002, 0x01, FALSE, 1,
                          &mode_opt, EC_TIMEOUTRXM);
      printf("PO2SOconfig: SDO 0x2002:01=%u wkc=%d\n", mode_opt, wkc2);
      if (ecx_SDOread(context, slave, 0x2002, 0x01, FALSE, &sz, &rb,
                      EC_TIMEOUTRXM) > 0)
         printf("PO2SOconfig: 0x2002:01 readback=%u\n", rb);
   }

   /* Note: do NOT force 1C32=DC_SYNC0 before ecx_dcsync0 (AL 0x0030).
    * For FreeRun (AssignActivate=0), explicitly set SyncType=0 so SM is not
    * left waiting for a SYNC0 that we never arm. */
   /* Arm SYNC0 before SyncType=DC so config_map's SAFE_OP request is valid. */
   if (joint->profile->dc_assign_activate == 0x0300u)
   {
      uint32_t cyc;
      int32_t shift;
      uint8_t d32[4];
      uint8_t st[2];
      int w;
      joint_dc_params(ctx, joint, &cyc, &shift, NULL);
      ecx_dcsync0(context, slave, TRUE, cyc, shift);
      (void)ecx_FPWRw(&context->port, context->slavelist[slave].configadr,
                      0x0980, htoes(0x0300), EC_TIMEOUTRET);
      pd_put_u16(st, 2);
      w = ecx_SDOwrite(context, slave, 0x1C32, 0x01, FALSE, 2, st,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 1C32 SyncType=DC_SYNC0 wkc=%d\n", w);
      w = ecx_SDOwrite(context, slave, 0x1C33, 0x01, FALSE, 2, st,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 1C33 SyncType=DC_SYNC0 wkc=%d\n", w);
      pd_put_u32(d32, cyc);
      w = ecx_SDOwrite(context, slave, 0x1C32, 0x02, FALSE, 4, d32,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 1C32:02=%u shift=%d wkc=%d\n", cyc, (int)shift, w);
   }
   else
   {
      uint8_t st[2];
      uint8_t d32[4];
      uint32_t cyc = ctx->config.period_ns ? ctx->config.period_ns
                                           : JSDK_DEFAULT_PERIOD_NS;
      int w;
      pd_put_u16(st, 1); /* SM synchron — board default */
      w = ecx_SDOwrite(context, slave, 0x1C32, 0x01, FALSE, 2, st,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 1C32 SyncType=SM wkc=%d\n", w);
      w = ecx_SDOwrite(context, slave, 0x1C33, 0x01, FALSE, 2, st,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 1C33 SyncType=SM wkc=%d\n", w);
      pd_put_u32(d32, cyc);
      w = ecx_SDOwrite(context, slave, 0x1C32, 0x02, FALSE, 4, d32,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 1C32:02=%u wkc=%d\n", cyc, w);
      (void)ecx_FPWRw(&context->port, context->slavelist[slave].configadr,
                      0x0980, htoes(0), EC_TIMEOUTRET);
   }

#undef SDO_U8
#undef SDO_U16

   /* Limits that were observed at 0 on this board and block motion. */
   {
      uint8_t d16[2];
      uint8_t d32[4];
      uint32_t max_prof_vel = 0;
      int sz = 4;
      int w;
      uint8_t per = 1;
      int8_t pidx = -3;

      if (ecx_SDOread(context, slave, JSDK_CIA402_MAX_PROFILE_VELOCITY, 0,
                      FALSE, &sz, d32, EC_TIMEOUTRXM) > 0)
      {
         max_prof_vel = (uint32_t)d32[0] | ((uint32_t)d32[1] << 8) |
                        ((uint32_t)d32[2] << 16) | ((uint32_t)d32[3] << 24);
      }
      printf("PO2SOconfig: 0x607F was %u\n", max_prof_vel);
      if (max_prof_vel == 0)
      {
         max_prof_vel = 500000;
         pd_put_u32(d32, max_prof_vel);
         wkc = ecx_SDOwrite(context, slave, JSDK_CIA402_MAX_PROFILE_VELOCITY,
                            0x00, FALSE, 4, d32, EC_TIMEOUTRXM);
         printf("PO2SOconfig: 0x607F set %u wkc=%d\n", max_prof_vel, wkc);
      }

      pd_put_u16(d16, 3000);
      w = ecx_SDOwrite(context, slave, 0x6072, 0, FALSE, 2, d16, EC_TIMEOUTRXM);
      printf("PO2SOconfig: 0x6072 max_torque=3000 wkc=%d\n", w);
      w = ecx_SDOwrite(context, slave, 0x2000, 0x0A, FALSE, 2, d16,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 0x2000:0A MaxCurrent=3000 wkc=%d\n", w);
      w = ecx_SDOwrite(context, slave, 0x2007, 0x0A, FALSE, 2, d16,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 0x2007:0A +TorqLim wkc=%d\n", w);
      w = ecx_SDOwrite(context, slave, 0x2007, 0x0B, FALSE, 2, d16,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 0x2007:0B -TorqLim wkc=%d\n", w);

      w = ecx_SDOwrite(context, slave, 0x60C2, 0x01, FALSE, 1, &per,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 0x60C2:01=%u wkc=%d\n", per, w);
      w = ecx_SDOwrite(context, slave, 0x60C2, 0x02, FALSE, 1, &pidx,
                       EC_TIMEOUTRXM);
      printf("PO2SOconfig: 0x60C2:02=%d wkc=%d\n", (int)pidx, w);

      /* Following error window/timeout — default windows (~200) stop CSP
       * tracking after a short crawl on this board. */
      {
         int32_t win = 10000000;
         uint16_t tout = 0;
         pd_put_u32(d32, (uint32_t)win);
         w = ecx_SDOwrite(context, slave, 0x6065, 0, FALSE, 4, d32,
                          EC_TIMEOUTRXM);
         printf("PO2SOconfig: 0x6065 FollowErrWin=%d wkc=%d\n", (int)win, w);
         pd_put_u16(d16, tout);
         w = ecx_SDOwrite(context, slave, 0x6066, 0, FALSE, 2, d16,
                          EC_TIMEOUTRXM);
         printf("PO2SOconfig: 0x6066 FollowErrTime=%u wkc=%d\n", tout, w);
      }
      /* Widen software position limits if present (0x607D:1/2). */
      {
         int32_t lo = (int32_t)0xC0000000;
         int32_t hi = (int32_t)0x3FFFFFFF;
         pd_put_u32(d32, (uint32_t)lo);
         w = ecx_SDOwrite(context, slave, 0x607D, 1, FALSE, 4, d32,
                          EC_TIMEOUTRXM);
         printf("PO2SOconfig: 0x607D:1 min=%d wkc=%d\n", (int)lo, w);
         pd_put_u32(d32, (uint32_t)hi);
         w = ecx_SDOwrite(context, slave, 0x607D, 2, FALSE, 4, d32,
                          EC_TIMEOUTRXM);
         printf("PO2SOconfig: 0x607D:2 max=%d wkc=%d\n", (int)hi, w);
      }
      {
         uint32_t pv = 500000u;
         uint32_t vmax = 80000u; /* default MaxMotorSpeed 0x6080 */
         int32_t dcbus = 0;
         uint8_t en_trq_lim = 1;
         const char *vmax_env = getenv("JSDK_MAX_MOTOR_SPEED");
         if (joint->max_motor_speed)
            vmax = joint->max_motor_speed;
         else if (vmax_env && vmax_env[0])
            vmax = (uint32_t)strtoul(vmax_env, NULL, 0);
         if (vmax < 1u)
            vmax = 80000u;
         sz = 4;
         pd_put_u32(d32, pv);
         w = ecx_SDOwrite(context, slave, 0x6081, 0, FALSE, 4, d32,
                          EC_TIMEOUTRXM);
         printf("PO2SOconfig: 0x6081 ProfileVel=%u wkc=%d\n", pv, w);
         pd_put_u32(d32, vmax);
         w = ecx_SDOwrite(context, slave, 0x6080, 0, FALSE, 4, d32,
                          EC_TIMEOUTRXM);
         printf("PO2SOconfig: 0x6080 MaxMotorSpeed=%u wkc=%d\n", vmax, w);
         /* CST: enable torque-mode velocity limit (IgH 0x200A:12) */
         w = ecx_SDOwrite(context, slave, 0x200A, 0x0C, FALSE, 1, &en_trq_lim,
                          EC_TIMEOUTRXM);
         printf("PO2SOconfig: 0x200A:12 torq_vel_limit=%u wkc=%d\n",
                en_trq_lim, w);
         if (ecx_SDOread(context, slave, 0x6079, 0, FALSE, &sz, d32,
                         EC_TIMEOUTRXM) > 0)
         {
            dcbus = (int32_t)(d32[0] | (d32[1] << 8) | (d32[2] << 16) |
                              (d32[3] << 24));
            printf("PO2SOconfig: 0x6079 DC_link=%d mV\n", (int)dcbus);
         }
      }

      /* Dump live 1C32 sync type */
      {
         uint16_t st = 0xFFFF;
         sz = 2;
         if (ecx_SDOread(context, slave, 0x1C32, 0x01, FALSE, &sz, &st,
                         EC_TIMEOUTRXM) > 0)
            printf("PO2SOconfig: 1C32:01 SyncType=%u\n", st);
         sz = 2;
         st = 0xFFFF;
         if (ecx_SDOread(context, slave, 0x1C33, 0x01, FALSE, &sz, &st,
                         EC_TIMEOUTRXM) > 0)
            printf("PO2SOconfig: 1C33:01 SyncType=%u\n", st);
      }

      {
         char name[64];
         int nsz = (int)sizeof(name) - 1;
         memset(name, 0, sizeof(name));
         if (ecx_SDOread(context, slave, 0x1008, 0, FALSE, &nsz, name,
                         EC_TIMEOUTRXM) > 0)
            printf("PO2SOconfig: 0x1008 '%s'\n", name);
      }
   }

   return 1;
}

/* ==================================================================== */
/* public: config / lifecycle                                           */
/* ==================================================================== */
void
jsdk_context_config_default(jsdk_context_config_t *config)
{
   if (!config)
   {
      return;
   }
   memset(config, 0, sizeof(*config));
   config->period_ns = JSDK_DEFAULT_PERIOD_NS;
   config->sync0_cycle_ns = 0; /* 0 = follow period_ns (joint may override) */
   config->sync0_shift_ns = 0; /* 0 = cycle / 4 */
   config->wait_ms = JSDK_DEFAULT_WAIT_MS;
   config->max_joints = JSDK_DEFAULT_MAX_JOINTS;
   config->auto_reference_clock = 1;
}

void
jsdk_joint_config_default(jsdk_joint_config_t *config)
{
   if (!config)
   {
      return;
   }
   memset(config, 0, sizeof(*config));
}

jsdk_context_t *
jsdk_context_create(const jsdk_context_config_t *config)
{
   jsdk_context_t *ctx;

   if (!config || config->ifname[0] == '\0' || config->period_ns == 0)
   {
      return NULL;
   }
   ctx = (jsdk_context_t *)calloc(1, sizeof(*ctx));
   if (!ctx)
   {
      return NULL;
   }
   ctx->config = *config;
   (void)jsdk_dc_timing_conf_apply(&ctx->config);
   if (ctx->config.max_joints == 0)
   {
      ctx->config.max_joints = JSDK_DEFAULT_MAX_JOINTS;
   }
   ctx->expected_wkc = -1;
   ctx->last_wkc = -1;
   return ctx;
}

void
jsdk_context_destroy(jsdk_context_t *ctx)
{
   unsigned int i;

   if (!ctx)
   {
      return;
   }
   jsdk_sdo_engine_shutdown(ctx);
   if (ctx->bus_open)
   {
      ecx_close(&ctx->soem);
   }
   for (i = 0; i < ctx->loaded_count; ++i)
   {
      esi_profile_free(ctx->loaded_profiles[i]);
   }
   free(ctx->loaded_profiles);
   for (i = 0; i < ctx->joint_count; ++i)
   {
      free(ctx->joints[i]);
   }
   free(ctx->joints);
   free(ctx);
}

/* Load an ESI file into a heap profile owned by this context (freed in
 * jsdk_context_destroy). Returns NULL and sets last_error on failure. */
static const struct jsdk_joint_profile *
ctx_register_esi(jsdk_context_t *ctx, const char *path)
{
   const struct jsdk_joint_profile *p;
   const struct jsdk_joint_profile **tmp;

   p = esi_profile_load(path, 0, 0);
   if (!p)
   {
      jsdk_set_error(ctx, "add_joint: failed to load ESI '%s'", path);
      return NULL;
   }
   if (ctx->loaded_count >= ctx->loaded_capacity)
   {
      unsigned int newcap = ctx->loaded_capacity ? ctx->loaded_capacity * 2 : 4;
      tmp = (const struct jsdk_joint_profile **)realloc(
          ctx->loaded_profiles, sizeof(*tmp) * newcap);
      if (!tmp)
      {
         esi_profile_free(p);
         jsdk_set_error(ctx, "add_joint: out of memory");
         return NULL;
      }
      ctx->loaded_profiles = tmp;
      ctx->loaded_capacity = newcap;
   }
   ctx->loaded_profiles[ctx->loaded_count++] = p;
   return p;
}

jsdk_status_t
jsdk_context_add_joint(jsdk_context_t *ctx,
                       const jsdk_joint_config_t *config,
                       jsdk_joint_t **joint_out)
{
   jsdk_joint_t *joint;
   jsdk_joint_t **newlist;

   if (!ctx || !config || !joint_out)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   if (ctx->configured)
   {
      jsdk_set_error(ctx, "add_joint: context already configured");
      return JSDK_ERR_BAD_STATE;
   }
   if (config->position == 0)
   {
      jsdk_set_error(ctx, "add_joint: position must be >= 1");
      return JSDK_ERR_INVALID_ARG;
   }
   if (ctx->joint_count >= ctx->config.max_joints)
   {
      jsdk_set_error(ctx, "add_joint: max_joints (%u) reached",
                     ctx->config.max_joints);
      return JSDK_ERR_INVALID_ARG;
   }
   joint = (jsdk_joint_t *)calloc(1, sizeof(*joint));
   if (!joint)
   {
      jsdk_set_error(ctx, "add_joint: out of memory");
      return JSDK_ERR_NO_MEMORY;
   }
   joint->ctx = ctx;
   joint->alias = config->alias;
   joint->slave = config->position;
   {
      jsdk_joint_config_t jc = *config;
      (void)jsdk_dc_timing_conf_apply_slave(jc.position, &jc);
      joint->sync0_cycle_ns = jc.sync0_cycle_ns;
      joint->sync0_shift_ns = jc.sync0_shift_ns;
      joint->wait_ms = jc.wait_ms;
      joint->max_motor_speed = jc.max_motor_speed;
   }
   joint->profile = jsdk_profile_find(config->profile_name);
   if (!joint->profile)
   {
      const char *nm = config->profile_name;
      /* IgH-compatible: a path (contains .xml, '/' or '\\') is loaded from
       * the ESI file automatically and owned by the context. */
      if (nm && (strstr(nm, ".xml") || strchr(nm, '/') || strchr(nm, '\\')))
      {
         const struct jsdk_joint_profile *p =
             ctx_register_esi(ctx, nm);
         joint->profile = p;
      }
      else
      {
         /* fall back to ESI-loaded (registered) profiles */
         joint->profile = esi_registry_find(nm);
      }
   }
   if (!joint->profile)
   {
      free(joint);
      jsdk_set_error(ctx, "add_joint: unknown profile '%s'",
                     config->profile_name ? config->profile_name : "");
      return JSDK_ERR_NOT_FOUND;
   }
   jsdk_cia402_init(&joint->cia402);

   newlist = (jsdk_joint_t **)realloc(
       ctx->joints, sizeof(jsdk_joint_t *) * (ctx->joint_count + 1));
   if (!newlist)
   {
      free(joint);
      jsdk_set_error(ctx, "add_joint: out of memory");
      return JSDK_ERR_NO_MEMORY;
   }
   ctx->joints = newlist;
   ctx->joints[ctx->joint_count++] = joint;
   *joint_out = joint;
   return JSDK_OK;
}

jsdk_status_t
jsdk_context_configure(jsdk_context_t *ctx)
{
   ec_groupt *grp;
   ec_slavet *s;
   unsigned int i;
   int wkc, size;
   int use_dc = 0;

   if (!ctx)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   if (ctx->configured)
   {
      jsdk_set_error(ctx, "configure: already configured");
      return JSDK_ERR_BAD_STATE;
   }
   ctx->last_error[0] = '\0';
   if (ctx->joint_count == 0)
   {
      jsdk_set_error(ctx, "configure: no joints added");
      return JSDK_ERR_BAD_STATE;
   }

   /* bind NIC + online scan */
   if (!ecx_init(&ctx->soem, ctx->config.ifname))
   {
      jsdk_set_error(ctx, "ecx_init('%s') failed (Npcap/adapter/privileges)",
                     ctx->config.ifname);
      return JSDK_ERR_ECRT;
   }
   ctx->bus_open = 1;

   wkc = ecx_config_init(&ctx->soem);
   if (wkc <= 0)
   {
      jsdk_set_error(ctx, "ecx_config_init: no slaves found");
      return JSDK_ERR_ECRT;
   }

   /* validate identity + register PO2SOconfig for each joint */
   for (i = 0; i < ctx->joint_count; ++i)
   {
      jsdk_joint_t *j = ctx->joints[i];
      if (j->slave > (uint16_t)ctx->soem.slavecount)
      {
         jsdk_set_error(ctx, "configure: joint position %u out of range "
                             "(slavecount=%d)", j->slave,
                        ctx->soem.slavecount);
         return JSDK_ERR_NOT_FOUND;
      }
      s = &ctx->soem.slavelist[j->slave];
      if ((s->eep_man != j->profile->vendor_id) ||
          (s->eep_id != j->profile->product_code))
      {
         jsdk_set_error(ctx, "configure: slave %u identity mismatch "
                             "(got 0x%08X/0x%08X, profile expects "
                             "0x%08X/0x%08X)",
                        j->slave, (unsigned)s->eep_man, (unsigned)s->eep_id,
                        (unsigned)j->profile->vendor_id,
                        (unsigned)j->profile->product_code);
         return JSDK_ERR_NOT_FOUND;
      }
      s->PO2SOconfig = joint_po2so_config;
   }

   /* map process data (invokes PO2SOconfig in PRE_OP) */
   wkc = ecx_config_map_group(&ctx->soem, ctx->io_map, 0);
   if (wkc <= 0)
   {
      jsdk_set_error(ctx, "ecx_config_map_group failed");
      return JSDK_ERR_ECRT;
   }

   /* per-joint: verify mapped layout and compute field offsets */
   for (i = 0; i < ctx->joint_count; ++i)
   {
      jsdk_joint_t *j = ctx->joints[i];
      unsigned int rx_bits, tx_bits;
      int rx_bytes, tx_bytes;

      s = &ctx->soem.slavelist[j->slave];
      rx_bits = profile_total_bits(j->profile->rx_entries,
                                   j->profile->rx_entry_count);
      tx_bits = profile_total_bits(j->profile->tx_entries,
                                   j->profile->tx_entry_count);
      if ((s->Obits != rx_bits) || (s->Ibits != tx_bits))
      {
         jsdk_set_error(ctx, "configure: slave %u mapping mismatch "
                             "(got O%u/I%u bit, profile O%u/I%u bit)",
                        j->slave, s->Obits, s->Ibits, rx_bits, tx_bits);
         return JSDK_ERR_ECRT;
      }
      memset(&j->offsets, 0xFF, sizeof(j->offsets));
      rx_bytes = walk_offsets(j->profile->rx_entries,
                              j->profile->rx_entry_count, &j->offsets);
      tx_bytes = walk_offsets(j->profile->tx_entries,
                              j->profile->tx_entry_count, &j->offsets);
      if ((rx_bytes != (int)((rx_bits + 7) / 8)) ||
          (tx_bytes != (int)((tx_bits + 7) / 8)))
      {
         jsdk_set_error(ctx, "configure: slave %u offset walker failed "
                             "(non byte-aligned mapping?)", j->slave);
         return JSDK_ERR_ECRT;
      }
      if (j->profile->dc_assign_activate)
      {
         use_dc = 1;
      }
      /* Always program AssignActivate (0 = SM-sync). Leaving a stale 0x300
       * from EEPROM/previous run keeps the drive waiting for SYNC0. */
      (void)ecx_FPWRw(&ctx->soem.port, s->configadr,
                      JSDK_REG_DC_ASSIGN_ACTIVATE,
                      htoes(j->profile->dc_assign_activate), EC_TIMEOUTRET);
      printf("configure: slave %u AssignActivate=0x%04X\n", j->slave,
             j->profile->dc_assign_activate);
   }

   /* DC: build the chain once, then per-joint SYNC0 timing */
   if (use_dc)
   {
      if (!ecx_configdc(&ctx->soem))
      {
         jsdk_set_error(ctx, "configure: no DC-capable slave found");
         return JSDK_ERR_ECRT;
      }
      printf("configure: DC task period=%u ns (per-slave SYNC0 below)\n",
             ctx->config.period_ns);
      for (i = 0; i < ctx->joint_count; ++i)
      {
         jsdk_joint_t *j = ctx->joints[i];
         uint32_t sync0_ns;
         int32_t shift_ns;
         unsigned int wait_ms;
         s = &ctx->soem.slavelist[j->slave];
         if (!s->hasdc)
         {
            continue;
         }
         joint_dc_params(ctx, j, &sync0_ns, &shift_ns, &wait_ms);
         ecx_dcsync0(&ctx->soem, j->slave, TRUE, sync0_ns, shift_ns);
         (void)ecx_FPWRw(&ctx->soem.port, s->configadr,
                         JSDK_REG_DC_ASSIGN_ACTIVATE,
                         htoes(j->profile->dc_assign_activate), EC_TIMEOUTRET);
         {
            uint8_t d32[4];
            int w;
            pd_put_u32(d32, sync0_ns);
            w = ecx_SDOwrite(&ctx->soem, j->slave, 0x1C32, 0x02, FALSE, 4, d32,
                             EC_TIMEOUTRXM);
            printf("configure: slave %u SYNC0 cycle=%u shift=%d wait_ms=%u "
                   "1C32:02 wkc=%d\n",
                   j->slave, sync0_ns, (int)shift_ns, wait_ms, w);
            w = ecx_SDOwrite(&ctx->soem, j->slave, 0x1C33, 0x02, FALSE, 4, d32,
                             EC_TIMEOUTRXM);
            printf("configure: slave %u 1C33:02 CycleTime=%u wkc=%d\n",
                   j->slave, sync0_ns, w);
         }
      }
   }
   else
   {
      /* FreeRun: still publish a cycle time so the app layer has a period. */
      for (i = 0; i < ctx->joint_count; ++i)
      {
         uint8_t d32[4];
         int w;
         pd_put_u32(d32, ctx->config.period_ns);
         w = ecx_SDOwrite(&ctx->soem, ctx->joints[i]->slave, 0x1C32, 0x02,
                          FALSE, 4, d32, EC_TIMEOUTRXM);
         printf("configure: FreeRun 1C32:02=%u wkc=%d\n",
                ctx->config.period_ns, w);
      }
   }

   grp = &ctx->soem.grouplist[0];
   ctx->expected_wkc = grp->outputsWKC * 2 + grp->inputsWKC;

   /* IgH uses separate LWR+LRD. SOEM defaults to LRW; some joint ESC
    * firmwares mishandle LRW (controlword works, tail PDO fields don't). */
   for (i = 0; i < ctx->joint_count; ++i)
   {
      ctx->soem.slavelist[ctx->joints[i]->slave].blockLRW = 1;
   }
   ctx->soem.slavelist[0].blockLRW = 1;
   grp->blockLRW = 1;
   printf("configure: forced LWR+LRD (blockLRW=1, IgH-like)\n");

   /* enable the cyclic-mailbox async SDO engine per joint (post map: the
    * mbxstatus FMMU mappings exist now). Engine availability is per joint. */
   (void)jsdk_sdo_engine_init(ctx);

   ctx->configured = 1;
   return JSDK_OK;
}

jsdk_status_t
jsdk_context_activate(jsdk_context_t *ctx)
{
   unsigned int i;
   int ok;

   if (!ctx)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   if (!ctx->configured)
   {
      jsdk_set_error(ctx, "activate: not configured");
      return JSDK_ERR_BAD_STATE;
   }
   if (ctx->activated)
   {
      return JSDK_OK;
   }
   ctx->last_error[0] = '\0';

   /* After DC arming: settle before SAFE_OP (max of per-slave wait_ms). */
   {
      unsigned int wait_ms = 0;
      unsigned int iwait;
      for (iwait = 0; iwait < ctx->joint_count; ++iwait)
      {
         unsigned int w = 0;
         joint_dc_params(ctx, ctx->joints[iwait], NULL, NULL, &w);
         if (w > wait_ms)
            wait_ms = w;
      }
      if (!wait_ms)
         wait_ms = JSDK_DEFAULT_WAIT_MS;
      printf("activate: wait_ms=%u before SAFE_OP (max of slaves)\n", wait_ms);
      osal_usleep(wait_ms * 1000u);
   }

   /* request + wait SAFE_OP for every joint (config_map requested it) */
   ctx->soem.slavelist[0].state = EC_STATE_SAFE_OP;
   ecx_writestate(&ctx->soem, 0);
   for (i = 0; i < ctx->joint_count; ++i)
   {
      if (wait_joint_state(ctx, ctx->joints[i], EC_STATE_SAFE_OP, 4,
                           EC_TIMEOUTSTATE) != 0)
      {
         return JSDK_ERR_ECRT;
      }
   }

   /* priming process-data while in SAFE_OP (drives + DC need a few cycles) */
   for (i = 0; i < 20; ++i)
   {
      for (unsigned int jj = 0; jj < ctx->joint_count; ++jj)
      {
         joint_stage_outputs(ctx, ctx->joints[jj]);
      }
      (void)roundtrip(&ctx->soem);
      osal_usleep(ctx->config.period_ns / 1000u ? ctx->config.period_ns / 1000u
                                                : 1000);
   }

   /* stage safe outputs (controlword = shutdown) before requesting OP so
    * the first frames applied in OP already carry a valid controlword */
   for (i = 0; i < ctx->joint_count; ++i)
   {
      joint_stage_outputs(ctx, ctx->joints[i]);
   }

   /* broadcast OPERATIONAL and confirm each joint (per-slave AL read).
    * Keep ~1 ms PD cadence to satisfy DC SYNC0 / SM watchdog on Windows. */
   ctx->soem.slavelist[0].state = EC_STATE_OPERATIONAL;
   ecx_writestate(&ctx->soem, 0);
   ok = 1;
   for (i = 0; i < 2000 && ok; ++i)
   {
      unsigned int jj;
      uint32_t period_us;

      (void)roundtrip(&ctx->soem);
      ok = 0;
      for (jj = 0; jj < ctx->joint_count; ++jj)
      {
         jsdk_joint_t *j = ctx->joints[jj];
         uint16_t st;

         ecx_readstate(&ctx->soem);
         st = ctx->soem.slavelist[j->slave].state;
         if (st != EC_STATE_OPERATIONAL)
         {
            ok = 1; /* not reached yet: retry */
         }
      }
      period_us = ctx->config.period_ns / 1000u;
      if (period_us == 0)
      {
         period_us = 1000;
      }
      osal_usleep(period_us);
   }
   if (ok)
   {
      jsdk_joint_t *j0 = ctx->joints[0];
      uint16_t al = ctx->soem.slavelist[j0->slave].ALstatuscode;
      uint16_t st = ctx->soem.slavelist[j0->slave].state;
      jsdk_set_error(ctx, "activate: slave failed to reach OPERATIONAL "
                          "(state=0x%02X AL status 0x%04X %s)",
                     st, al, ec_ALstatuscode2string(al));
      return JSDK_ERR_ECRT;
   }

   /* re-stage outputs and leave one frame in flight for the first
    * jsdk_context_cycle_begin() */
   for (i = 0; i < ctx->joint_count; ++i)
   {
      joint_stage_outputs(ctx, ctx->joints[i]);
   }
   ecx_send_processdata(&ctx->soem);
   ctx->activated = 1;
   return JSDK_OK;
}

void
jsdk_context_deactivate(jsdk_context_t *ctx)
{
   if (!ctx || !ctx->bus_open)
   {
      return;
   }
   ctx->activated = 0;
   ctx->configured = 0;
   ctx->soem.slavelist[0].state = EC_STATE_INIT;
   ecx_writestate(&ctx->soem, 0);
   ecx_close(&ctx->soem);
   ctx->bus_open = 0;
}

/* Bus health / single-joint fault isolation (mirrors SOEM simple_ng
 * fieldbus_check_state). Called only when the WKC of the last frame was too
 * low, so the hot path stays clean in normal operation. Recovers each joint:
 *   OP ok  -> nothing
 *   SAFE_OP+ERROR -> ACK the error, drive back to SAFE_OP
 *   SAFE_OP       -> request OPERATIONAL again
 *   INIT/PRE_OP   -> ecx_reconfig_slave() (re-applies PO2SOconfig)
 *   lost          -> ecx_recover_slave() / ecx_statecheck() probing */
static void
jsdk_bus_health(jsdk_context_t *ctx)
{
   unsigned int i;

   ecx_readstate(&ctx->soem);
   for (i = 0; i < ctx->joint_count; ++i)
   {
      jsdk_joint_t *j = ctx->joints[i];
      ec_slavet *s = &ctx->soem.slavelist[j->slave];
      uint16_t st = (uint16_t)(s->state & 0x0f);

      if (st == EC_STATE_OPERATIONAL)
      {
         continue;
      }
      if ((s->state & (uint16_t)(EC_STATE_SAFE_OP | EC_STATE_ERROR)) ==
          (uint16_t)(EC_STATE_SAFE_OP | EC_STATE_ERROR))
      {
         /* error in SAFE_OP: acknowledge it */
         s->state = (uint16_t)(EC_STATE_SAFE_OP + EC_STATE_ACK);
         ecx_writestate(&ctx->soem, j->slave);
         (void)roundtrip(&ctx->soem);
      }
      else if (st == EC_STATE_SAFE_OP)
      {
         s->state = EC_STATE_OPERATIONAL;
         ecx_writestate(&ctx->soem, j->slave);
         (void)roundtrip(&ctx->soem);
      }
      else if (st > EC_STATE_NONE)
      {
         if (ecx_reconfig_slave(&ctx->soem, j->slave, EC_TIMEOUTRET))
         {
            s->islost = FALSE;
         }
      }
      else if (!s->islost)
      {
         (void)ecx_statecheck(&ctx->soem, j->slave, EC_STATE_OPERATIONAL,
                              EC_TIMEOUTRET);
         if (s->state == EC_STATE_NONE)
         {
            s->islost = TRUE;
         }
      }
      else if (s->state == EC_STATE_NONE)
      {
         if (ecx_recover_slave(&ctx->soem, j->slave, EC_TIMEOUTRET))
         {
            s->islost = FALSE;
         }
      }
   }
}

jsdk_status_t
jsdk_context_cycle_begin(jsdk_context_t *ctx, uint64_t app_time_ns)
{
   unsigned int i;
   int wkc;

   (void)app_time_ns; /* wake alignment uses dc_toff_ns after receive */

   if (!ctx)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   if (!ctx->activated)
   {
      jsdk_set_error(ctx, "cycle_begin: not activated");
      return JSDK_ERR_BAD_STATE;
   }

   wkc = ecx_receive_processdata(&ctx->soem, EC_TIMEOUTRET);
   ctx->last_wkc = wkc;

   /* Align host period to DC (ec_sample ec_sync). Without this, Soft-DC
    * SYNC0 drifts vs SM2 and CSP/CSV tracking dies after a short crawl. */
   if (ctx->soem.slavelist[0].hasdc && wkc > 0)
   {
      const int64_t cyc = (int64_t)ctx->config.period_ns;
      const int64_t syncoffset = (int64_t)(ctx->config.period_ns / 2);
      int64_t delta;
      int64_t timeerror;
      delta = (ctx->soem.DCtime - syncoffset) % cyc;
      if (delta > (cyc / 2))
         delta -= cyc;
      timeerror = -delta;
      ctx->dc_integral += timeerror;
      /* clamp integral windup */
      if (ctx->dc_integral > 500000000ll)
         ctx->dc_integral = 500000000ll;
      if (ctx->dc_integral < -500000000ll)
         ctx->dc_integral = -500000000ll;
      ctx->dc_toff_ns =
          (int64_t)((double)timeerror * 0.01 + (double)ctx->dc_integral * 0.00002);
      if (ctx->dc_toff_ns > (int64_t)(ctx->config.period_ns / 4))
         ctx->dc_toff_ns = (int64_t)(ctx->config.period_ns / 4);
      if (ctx->dc_toff_ns < -(int64_t)(ctx->config.period_ns / 4))
         ctx->dc_toff_ns = -(int64_t)(ctx->config.period_ns / 4);
   }
   else
   {
      ctx->dc_toff_ns = 0;
   }

   /* bus snapshot */
   ctx->bus.working_counter = (unsigned int)wkc;
   ctx->bus.wc_state = (wkc >= ctx->expected_wkc) ? 0 : 1;
   ctx->bus.slaves_responding = (unsigned int)ctx->soem.slavecount;
   ctx->bus.al_states = ctx->soem.slavelist[0].state;
   ctx->bus.link_up = (wkc >= 0) ? 1 : 0;

   /* WKC shortfall -> run bus health / single-joint recovery */
   if (ctx->bus.wc_state)
   {
      jsdk_bus_health(ctx);
   }

   for (i = 0; i < ctx->joint_count; ++i)
   {
      jsdk_joint_t *j = ctx->joints[i];
      ec_slavet *s = &ctx->soem.slavelist[j->slave];
      uint8_t *in = s->inputs;
      uint16_t sw = 0;

      if (in)
      {
         sw = pd_get_u16(in + j->offsets.statusword);
         j->feedback.statusword = sw;
         j->feedback.mode_display = (int8_t)in[j->offsets.mode_display];
         j->feedback.actual_position = pd_get_i32(in + j->offsets.actual_position);
         j->feedback.actual_velocity = pd_get_i32(in + j->offsets.actual_velocity);
         j->feedback.actual_torque = (int16_t)pd_get_u16(
             in + j->offsets.actual_torque);
      }
      jsdk_cia402_update(&j->cia402, sw);
      j->feedback.axis_state = j->cia402.axis_state;
      j->feedback.al_state = (uint8_t)(s->ALstatuscode & 0xFF);
      j->feedback.online = 1;
      j->feedback.operational = (ctx->bus.wc_state == 0);
   }
   return JSDK_OK;
}

jsdk_status_t
jsdk_context_cycle_end(jsdk_context_t *ctx)
{
   unsigned int i;

   if (!ctx)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   if (!ctx->activated)
   {
      jsdk_set_error(ctx, "cycle_end: not activated");
      return JSDK_ERR_BAD_STATE;
   }

   for (i = 0; i < ctx->joint_count; ++i)
   {
      jsdk_joint_t *j = ctx->joints[i];
      ec_slavet *s = &ctx->soem.slavelist[j->slave];
      uint8_t *out = s->outputs;

      if (!out)
      {
         continue;
      }
      pd_put_u16(out + j->offsets.controlword, j->cia402.controlword);
      {
         int8_t mode = j->cia402.requested_mode ?
                           j->cia402.requested_mode :
                           (j->profile ? j->profile->mode_csp
                                       : (int8_t)JSDK_MODE_CSP);
         out[j->offsets.mode_of_operation] = (uint8_t)mode;
      }
      /* IgH: never stream target=0 before OE (multi-million step → runaway).
       * Mirror actual when disabled; for CSV/CST also keep 607A = actual. */
      if (!j->cia402.operation_enabled)
      {
         pd_put_u32(out + j->offsets.target_position,
                    (uint32_t)j->feedback.actual_position);
         pd_put_u32(out + j->offsets.target_velocity, 0);
         pd_put_u16(out + j->offsets.target_torque, 0);
      }
      else if (j->cia402.requested_mode == (int8_t)JSDK_MODE_CSV ||
               j->cia402.requested_mode == (int8_t)JSDK_MODE_CST ||
               j->cia402.requested_mode == (int8_t)JSDK_MODE_HOMING)
      {
         /* Homing: mirror pos, do not stream CSP targets over run_homing(). */
         pd_put_u32(out + j->offsets.target_position,
                    (uint32_t)j->feedback.actual_position);
         pd_put_u32(out + j->offsets.target_velocity,
                    (uint32_t)j->command.target_velocity);
         pd_put_u16(out + j->offsets.target_torque,
                    (uint16_t)j->command.target_torque);
      }
      else
      {
         pd_put_u32(out + j->offsets.target_position,
                    (uint32_t)j->command.target_position);
         pd_put_u32(out + j->offsets.target_velocity,
                    (uint32_t)j->command.target_velocity);
         pd_put_u16(out + j->offsets.target_torque,
                    (uint16_t)j->command.target_torque);
      }
   }
   ecx_send_processdata(&ctx->soem);

   /* non-blocking async SDO engine + fault-code auto read */
   jsdk_sdo_engine_cycle(ctx);
   jsdk_fault_service(ctx);
   return JSDK_OK;
}

jsdk_status_t
jsdk_context_get_bus_state(jsdk_context_t *ctx, jsdk_bus_state_t *state)
{
   if (!ctx || !state)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   *state = ctx->bus;
   return JSDK_OK;
}

int64_t
jsdk_context_dc_toff_ns(const jsdk_context_t *ctx)
{
   return ctx ? ctx->dc_toff_ns : 0;
}

/* ==================================================================== */
/* joint command / feedback API                                         */
/* ==================================================================== */
void
jsdk_joint_request_enable(jsdk_joint_t *joint, jsdk_mode_t mode)
{
   if (!joint)
   {
      return;
   }
   jsdk_cia402_request_enable(&joint->cia402, (int8_t)mode);
}

void
jsdk_joint_request_disable(jsdk_joint_t *joint)
{
   if (!joint)
   {
      return;
   }
   jsdk_cia402_request_disable(&joint->cia402);
}

void
jsdk_joint_request_fault_reset(jsdk_joint_t *joint)
{
   if (!joint)
   {
      return;
   }
   jsdk_cia402_request_fault_reset(&joint->cia402);
}

void
jsdk_joint_set_mode(jsdk_joint_t *joint, jsdk_mode_t mode)
{
   if (!joint)
   {
      return;
   }
   jsdk_cia402_set_mode(&joint->cia402, (int8_t)mode);
}

void
jsdk_joint_request_homing_start(jsdk_joint_t *joint)
{
   if (!joint)
   {
      return;
   }
   jsdk_cia402_request_homing_start(&joint->cia402);
}

void
jsdk_joint_clear_homing_start(jsdk_joint_t *joint)
{
   if (!joint)
   {
      return;
   }
   jsdk_cia402_clear_homing_start(&joint->cia402);
}

void
jsdk_joint_set_target_position(jsdk_joint_t *joint, int32_t value)
{
   if (joint)
   {
      joint->command.target_position = value;
   }
}

void
jsdk_joint_set_target_velocity(jsdk_joint_t *joint, int32_t value)
{
   if (joint)
   {
      joint->command.target_velocity = value;
   }
}

void
jsdk_joint_set_target_torque(jsdk_joint_t *joint, int16_t value)
{
   if (joint)
   {
      joint->command.target_torque = value;
   }
}

jsdk_status_t
jsdk_joint_get_feedback(jsdk_joint_t *joint, jsdk_joint_feedback_t *feedback)
{
   if (!joint || !feedback)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   *feedback = joint->feedback;
   return JSDK_OK;
}

int
jsdk_joint_is_enabled(jsdk_joint_t *joint)
{
   return (joint && joint->cia402.operation_enabled) ? 1 : 0;
}

int
jsdk_joint_is_fault(jsdk_joint_t *joint)
{
   return (joint && joint->cia402.axis_state == JSDK_AXIS_FAULT) ? 1 : 0;
}

/* ==================================================================== */
/* ESI dynamic profile loading (L2)                                     */
/* ==================================================================== */
const struct jsdk_joint_profile *
jsdk_profile_load_from_esi(const char *esi_path, uint32_t vendor_id,
                           uint32_t product_code)
{
   return esi_profile_load(esi_path, vendor_id, product_code);
}

void
jsdk_profile_destroy(const struct jsdk_joint_profile *profile)
{
   esi_profile_free(profile);
}

const char *
jsdk_profile_get_name(const struct jsdk_joint_profile *profile)
{
   return (profile && profile->name) ? profile->name : "";
}

/* ==================================================================== */
/* fault diagnosis closed loop (public)                                 */
/* ==================================================================== */
void
jsdk_context_set_fault_callback(jsdk_context_t *ctx,
                                jsdk_fault_callback_t cb, void *user_data)
{
   if (!ctx)
   {
      return;
   }
   ctx->fault_cb = cb;
   ctx->fault_cb_data = user_data;
}

int
jsdk_joint_get_fault_info(jsdk_joint_t *joint, jsdk_fault_info_t *info)
{
   if (!joint || !info)
   {
      return 0;
   }
   *info = joint->fault_info;
   return joint->fault_info.valid;
}

/* ==================================================================== */
/* unit conversion (command units <-> physical)                         */
/* ==================================================================== */
#define JSDK_PI 3.14159265358979323846

void
jsdk_unit_scale_default(jsdk_unit_scale_t *scale, uint32_t rated_trq)
{
   if (!scale)
   {
      return;
   }
   /* default: encoder 16384 counts/rev, gear 8:1 -> 131072 counts/rev out */
   jsdk_unit_scale_calc(scale, 16384, 8, 1, rated_trq ? rated_trq : 10000);
}

void
jsdk_unit_scale_calc(jsdk_unit_scale_t *scale, uint32_t encoder_resolution,
                     uint32_t motor_rev, uint32_t shaft_rev,
                     uint32_t rated_torque)
{
   double shaft_counts_per_rev;

   if (!scale)
   {
      return;
   }
   if (!encoder_resolution || !motor_rev)
   {
      memset(scale, 0, sizeof(*scale));
      return;
   }
   shaft_counts_per_rev = (double)encoder_resolution *
                          (double)motor_rev /
                          (double)(shaft_rev ? shaft_rev : 1);
   scale->pos_counts_to_rad = 2.0 * JSDK_PI / shaft_counts_per_rev;
   scale->vel_counts_to_rad_s = 2.0 * JSDK_PI / shaft_counts_per_rev;
   /* 1 torque unit = 0.1% of rated torque */
   scale->trq_to_Nm = rated_torque ? ((double)rated_torque / 1000000.0)
                                   : 0.01;
   scale->valid = 1;
}

void
jsdk_joint_set_scale(jsdk_joint_t *joint, const jsdk_unit_scale_t *scale)
{
   if (joint && scale)
   {
      joint->scale = *scale;
   }
}

void
jsdk_joint_get_scale(jsdk_joint_t *joint, jsdk_unit_scale_t *scale)
{
   if (joint && scale)
   {
      *scale = joint->scale;
   }
}

/* position unwrap: integrate 16-bit wrapped deltas into an unbounded
 * accumulator (IgH equivalent behaviour). */
static void
pos_unwrap(jsdk_joint_t *joint, int32_t raw)
{
   uint16_t cur;
   uint16_t prev;
   int16_t diff;

   if (!joint->pos_accum_valid)
   {
      joint->pos_accumulator = (int64_t)raw;
      joint->last_raw_position = raw;
      joint->pos_accum_valid = 1;
      return;
   }
   cur = (uint16_t)(raw & 0xFFFF);
   prev = (uint16_t)(joint->last_raw_position & 0xFFFF);
   diff = (int16_t)(cur - prev);
   joint->pos_accumulator += diff;
   joint->last_raw_position = raw;
}

double
jsdk_joint_actual_position_rad(jsdk_joint_t *joint)
{
   if (!joint || !joint->scale.valid)
   {
      return 0.0;
   }
   pos_unwrap(joint, joint->feedback.actual_position);
   return (double)joint->pos_accumulator * joint->scale.pos_counts_to_rad;
}

double
jsdk_joint_actual_velocity_rad_s(jsdk_joint_t *joint)
{
   if (!joint || !joint->scale.valid)
   {
      return 0.0;
   }
   return (double)joint->feedback.actual_velocity *
          joint->scale.vel_counts_to_rad_s;
}

double
jsdk_joint_actual_torque_Nm(jsdk_joint_t *joint)
{
   if (!joint || !joint->scale.valid)
   {
      return 0.0;
   }
   return (double)joint->feedback.actual_torque * joint->scale.trq_to_Nm;
}

void
jsdk_joint_set_target_position_rad(jsdk_joint_t *joint, double rad)
{
   if (!joint || !joint->scale.valid)
   {
      return;
   }
   jsdk_joint_set_target_position(
       joint, (int32_t)(rad / joint->scale.pos_counts_to_rad));
}

void
jsdk_joint_set_target_velocity_rad_s(jsdk_joint_t *joint, double rad_s)
{
   if (!joint || !joint->scale.valid)
   {
      return;
   }
   jsdk_joint_set_target_velocity(
       joint, (int32_t)(rad_s / joint->scale.vel_counts_to_rad_s));
}

void
jsdk_joint_set_target_torque_Nm(jsdk_joint_t *joint, double Nm)
{
   if (!joint || !joint->scale.valid)
   {
      return;
   }
   jsdk_joint_set_target_torque(
       joint, (int16_t)(Nm / joint->scale.trq_to_Nm));
}
