/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief Robot joint SDK - stable C ABI (SOEM based).
 *
 * Layers (see ARCHITECTURE.zh-CN.md):
 *   L1 transport (jsdk_context_*), L2 device profile (+ESI loading),
 *   L3 CiA402 (enable/disable/fault_reset/mode), non-blocking async SDO,
 *   fault diagnosis closed loop and physical-unit conversion.
 * The SDK owns no RT thread (form B): call cycle_begin/cycle_end from your
 * real-time loop. Everything inside the cycle is rt_safe.
 */

#ifndef JSDK_JOINT_SDK_H
#define JSDK_JOINT_SDK_H

#include <stdint.h>
#include <stddef.h>

#include <joint_sdk/cia402.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct jsdk_context jsdk_context_t;
typedef struct jsdk_joint jsdk_joint_t;
struct jsdk_joint_profile; /* opaque device profile (L2) */

/* ---- status ---- */
typedef enum
{
   JSDK_OK = 0,
   JSDK_ERR_INVALID_ARG = -1,
   JSDK_ERR_NO_MEMORY = -2,
   JSDK_ERR_NOT_FOUND = -3,
   JSDK_ERR_BAD_STATE = -4,
   JSDK_ERR_ECRT = -5,
   JSDK_ERR_UNSUPPORTED = -6
} jsdk_status_t;

/* ---- CiA402 operating modes (0x6060 encodings) ---- */
typedef enum
{
   JSDK_MODE_HOMING = 6,
   JSDK_MODE_CSP = 8,
   JSDK_MODE_CSV = 9,
   JSDK_MODE_CST = 10
} jsdk_mode_t;

/* ---- axis state (decoded from Statusword) ---- */
typedef enum
{
   JSDK_AXIS_UNKNOWN = 0,
   JSDK_AXIS_SWITCH_ON_DISABLED,
   JSDK_AXIS_READY_TO_SWITCH_ON,
   JSDK_AXIS_SWITCHED_ON,
   JSDK_AXIS_OPERATION_ENABLED,
   JSDK_AXIS_FAULT
} jsdk_axis_state_t;

/* ---- configuration ---- */
typedef struct
{
   char ifname[64];        /* NIC adapter name, e.g. "eth0" / Npcap name */
   /**
    * Master PDO / task period (ns). One per context; not per slave.
    * Cyclic loop and activate priming use this only.
    */
   uint32_t period_ns;
   /**
    * Default slave DC if a joint leaves its own fields at 0.
    * 0 = SYNC0 cycle follows period_ns.
    */
   uint32_t sync0_cycle_ns;
   /**
    * Default SYNC0 shift if a joint leaves sync0_shift_ns at 0.
    * 0 = cycle / 4.
    */
   int32_t sync0_shift_ns;
   /**
    * Default WaitBeforeSAFEOP (ms) if a joint leaves wait_ms at 0.
    * 0 at both levels → JSDK_DEFAULT_WAIT_MS.
    */
   unsigned int wait_ms;
   unsigned int max_joints;
   int auto_reference_clock; /* reserved (default 1) */
} jsdk_context_config_t;

typedef struct
{
   uint16_t alias;          /* reserved (0) */
   uint16_t position;       /* slave number in bus topology, 1-based */
   const char *profile_name;/* NULL/"" = default CyberBeast profile, a name
                               from jsdk_profile_get_name() for an ESI-loaded
                               profile, or an ESI file *path* (contains .xml
                               or '/') which is auto-loaded and owned by the
                               context (IgH-compatible) */
   /**
    * Per-slave ecx_dcsync0 CyclTime (ns) + CoE 1C32/1C33:02.
    * 0 = context sync0_cycle_ns, else period_ns.
    */
   uint32_t sync0_cycle_ns;
   /**
    * Per-slave ecx_dcsync0 CyclShift (ns).
    * 0 = context sync0_shift_ns, else this cycle / 4.
    */
   int32_t sync0_shift_ns;
   /**
    * Per-slave settle before SAFE_OP (ms). Bus wait uses the max of all
    * DC joints. 0 = context wait_ms / JSDK_DEFAULT_WAIT_MS.
    */
   unsigned int wait_ms;
   /**
    * Max motor speed written to CoE 0x6080 in PO2SO (counts/s).
    * 0 = env JSDK_MAX_MOTOR_SPEED, else default 80000.
    */
   uint32_t max_motor_speed;
} jsdk_joint_config_t;

/* ---- feedback snapshot (refreshed in jsdk_context_cycle_begin) ---- */
typedef struct
{
   uint16_t statusword;
   int8_t mode_display;
   int32_t actual_position;
   int32_t actual_velocity;
   int16_t actual_torque;
   jsdk_axis_state_t axis_state;
   int online;
   int operational;
   uint8_t al_state;
} jsdk_joint_feedback_t;

/* ---- bus health snapshot (approximate: refreshed once per cycle from the
 * last frame's WKC; slaves_responding/al_states are not refreshed with a
 * per-cycle AL read, see jsdk_context_get_bus_state) ---- */
typedef struct
{
   unsigned int working_counter;    /* WKC of last received frame */
   unsigned int wc_state;           /* 0 = ok, 1 = wkc shortfall */
   unsigned int slaves_responding;
   unsigned int al_states;
   int link_up;
} jsdk_bus_state_t;

#define JSDK_DEFAULT_PERIOD_NS  1000000u
#define JSDK_DEFAULT_WAIT_MS    100u
#define JSDK_DEFAULT_MAX_JOINTS 16u

/* built-in profile name (static CyberBeast Joint Module) */
#define JSDK_PROFILE_CYBERBEAST_JOINT_MODULE "CyberBeast Joint Module"

/* ---- context lifecycle ---- */
void jsdk_context_config_default(jsdk_context_config_t *config);
void jsdk_joint_config_default(jsdk_joint_config_t *config);

/** Load config/dc_timing.conf — also called inside jsdk_context_create(). */
int jsdk_dc_timing_conf_apply(jsdk_context_config_t *cfg);
int jsdk_dc_timing_conf_apply_slave(uint16_t position,
                                    jsdk_joint_config_t *jcfg);

jsdk_context_t *jsdk_context_create(const jsdk_context_config_t *config);
void jsdk_context_destroy(jsdk_context_t *ctx);

jsdk_status_t jsdk_context_add_joint(jsdk_context_t *ctx,
        const jsdk_joint_config_t *config, jsdk_joint_t **joint);

/* Bus is scanned/validated/mapped and left requesting SAFE_OP. */
jsdk_status_t jsdk_context_configure(jsdk_context_t *ctx);
/* Brings the bus to OPERATIONAL (SAFE_OP wait + priming + OP). */
jsdk_status_t jsdk_context_activate(jsdk_context_t *ctx);
/* Requests INIT and closes the socket. */
void jsdk_context_deactivate(jsdk_context_t *ctx);

/* Cycle hooks (form B). cycle_begin: receive + feedback + DS402 + fault
 * detection. cycle_end: write commands + send + service the async SDO
 * engine (non-blocking, mailbox based). */
jsdk_status_t jsdk_context_cycle_begin(jsdk_context_t *ctx,
        uint64_t app_time_ns);
jsdk_status_t jsdk_context_cycle_end(jsdk_context_t *ctx);
/* After cycle_begin: add this (ns) to the next period wait to track SYNC0. */
int64_t jsdk_context_dc_toff_ns(const jsdk_context_t *ctx);

/* Latest bus snapshot. Note: slaves_responding/al_states are approximations
 * kept from configuration time; authoritative per-slave AL state is only
 * refreshed when the bus health path runs (on a WKC shortfall). */
jsdk_status_t jsdk_context_get_bus_state(jsdk_context_t *ctx,
        jsdk_bus_state_t *state);
const char *jsdk_context_last_error(jsdk_context_t *ctx);

/* ---- joint commands / feedback ---- */
void jsdk_joint_request_enable(jsdk_joint_t *joint, jsdk_mode_t mode);
void jsdk_joint_request_disable(jsdk_joint_t *joint);
void jsdk_joint_request_fault_reset(jsdk_joint_t *joint);
void jsdk_joint_set_mode(jsdk_joint_t *joint, jsdk_mode_t mode);
/* Homing (mode 6): after Operation Enabled, assert CW bit4 (0x001F). */
void jsdk_joint_request_homing_start(jsdk_joint_t *joint);
void jsdk_joint_clear_homing_start(jsdk_joint_t *joint);

void jsdk_joint_set_target_position(jsdk_joint_t *joint, int32_t value);
void jsdk_joint_set_target_velocity(jsdk_joint_t *joint, int32_t value);
void jsdk_joint_set_target_torque(jsdk_joint_t *joint, int16_t value);

jsdk_status_t jsdk_joint_get_feedback(jsdk_joint_t *joint,
        jsdk_joint_feedback_t *feedback);
int jsdk_joint_is_enabled(jsdk_joint_t *joint);
int jsdk_joint_is_fault(jsdk_joint_t *joint);

const char *jsdk_status_string(jsdk_status_t status);
const char *jsdk_axis_state_string(jsdk_axis_state_t state);

/* === unit conversion: command units <-> physical (rad / rad/s / N·m) === */

typedef struct
{
   double pos_counts_to_rad;    /* 1 command unit = N rad (output shaft) */
   double vel_counts_to_rad_s;  /* 1 command unit/s = N rad/s (output) */
   double trq_to_Nm;            /* 1 torque unit = N N·m */
   int valid;                   /* 1 = coefficients computed */
} jsdk_unit_scale_t;

/* Defaults: encoder 16384 counts/rev, gear 8:1, rated_torque = rated_trq
 * (default 10 N·m). */
void jsdk_unit_scale_default(jsdk_unit_scale_t *scale, uint32_t rated_trq);

/* Precise coefficients from ESI parameters. */
void jsdk_unit_scale_calc(jsdk_unit_scale_t *scale,
        uint32_t encoder_resolution,   /* 0x608F:1 */
        uint32_t motor_rev,            /* 0x6091:1 */
        uint32_t shaft_rev,            /* 0x6091:2 */
        uint32_t rated_torque);        /* 0x6076 */

void jsdk_joint_set_scale(jsdk_joint_t *joint, const jsdk_unit_scale_t *scale);
void jsdk_joint_get_scale(jsdk_joint_t *joint, jsdk_unit_scale_t *scale);

/* Physical-unit setters/getters (rt_safe). Mix freely with the command-unit
 * API. Position is unwrapped into an unbounded accumulator. */
void jsdk_joint_set_target_position_rad(jsdk_joint_t *joint, double rad);
void jsdk_joint_set_target_velocity_rad_s(jsdk_joint_t *joint, double rad_s);
void jsdk_joint_set_target_torque_Nm(jsdk_joint_t *joint, double Nm);
double jsdk_joint_actual_position_rad(jsdk_joint_t *joint);
double jsdk_joint_actual_velocity_rad_s(jsdk_joint_t *joint);
double jsdk_joint_actual_torque_Nm(jsdk_joint_t *joint);

/* === fault diagnosis closed loop === */

typedef struct
{
   int valid;               /* 1 = fault info present */
   uint16_t code_603f;      /* CiA402 error code  (0x603F) */
   uint8_t  error_register; /* error register     (0x1001) */
   uint32_t vendor_lo;      /* vendor code low 32 (0x203F) */
   uint32_t vendor_hi;      /* vendor code high   (0x203E) */
} jsdk_fault_info_t;

/* Fault callback, invoked from the rt-safe cycle context: no sleep/malloc/
 * locks inside. */
typedef void (*jsdk_fault_callback_t)(jsdk_joint_t *joint,
        const jsdk_fault_info_t *info, void *user_data);

/* Register the fault callback (before activate is typical). */
void jsdk_context_set_fault_callback(jsdk_context_t *ctx,
        jsdk_fault_callback_t cb, void *user_data);

/* Synchronous last-fault query (rt_safe). Returns 1 if info.valid. */
int jsdk_joint_get_fault_info(jsdk_joint_t *joint, jsdk_fault_info_t *info);

/* === async SDO read/write (runtime, non-blocking, rt_safe) === */

typedef int jsdk_sdo_handle_t;

typedef enum
{
   JSDK_SDO_IDLE = 0,   /* ready to start a request */
   JSDK_SDO_BUSY,       /* request in flight */
   JSDK_SDO_SUCCESS,    /* last request succeeded */
   JSDK_SDO_ERROR       /* last request failed (see last_error) */
} jsdk_sdo_state_t;

/* Create an async SDO request handle (before activate is typical).
 *   index/subindex  CoE object to access
 *   size            data buffer size (>= object byte size)
 * Returns handle >= 0, or -1 on failure. */
jsdk_sdo_handle_t jsdk_joint_sdo_create(jsdk_joint_t *joint,
        uint16_t index, uint8_t subindex, size_t size);

/* Request state (rt_safe). */
jsdk_sdo_state_t jsdk_joint_sdo_state(jsdk_joint_t *joint,
        jsdk_sdo_handle_t handle);

/* Data buffer: fill before write(), read after SUCCESS on read(). */
uint8_t *jsdk_joint_sdo_data(jsdk_joint_t *joint, jsdk_sdo_handle_t handle);

/* Bytes actually read (valid when state == SUCCESS on read). */
size_t jsdk_joint_sdo_data_size(jsdk_joint_t *joint,
        jsdk_sdo_handle_t handle);

/* Start an async read/write (rt_safe; only when state != BUSY). */
int jsdk_joint_sdo_read(jsdk_joint_t *joint, jsdk_sdo_handle_t handle);
int jsdk_joint_sdo_write(jsdk_joint_t *joint, jsdk_sdo_handle_t handle);

/* Blocking CoE SDO (diagnostic / config use; NOT rt_safe).
 * size: in = buffer capacity, out = bytes transferred. Returns JSDK_OK. */
jsdk_status_t jsdk_joint_sdo_upload(jsdk_joint_t *joint, uint16_t index,
                                    uint8_t subindex, void *data, int *size);
jsdk_status_t jsdk_joint_sdo_download(jsdk_joint_t *joint, uint16_t index,
                                      uint8_t subindex, const void *data,
                                      int size);

/* === ESI XML dynamic profile loading (L2, multi-model, no recompile) === */

/* Parse an ESI XML file into a heap-allocated profile. If vendor_id /
 * product_code are non-zero they are used to pick a matching <Device>;
 * pass its <Name> (jsdk_profile_get_name) as joint_config.profile_name.
 * Returns NULL on failure. Free with jsdk_profile_destroy(). */
const struct jsdk_joint_profile *jsdk_profile_load_from_esi(
        const char *esi_path, uint32_t vendor_id, uint32_t product_code);
void jsdk_profile_destroy(const struct jsdk_joint_profile *profile);
const char *jsdk_profile_get_name(const struct jsdk_joint_profile *profile);

#ifdef __cplusplus
}
#endif

#endif
