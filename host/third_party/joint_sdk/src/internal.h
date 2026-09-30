/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief Internal structures shared between SDK translation units.
 */

#ifndef JSDK_INTERNAL_H
#define JSDK_INTERNAL_H

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>

#include "soem/soem.h"
#include <joint_sdk/joint_sdk.h>

/* --- neutral PDO entry description (index:sub, bit length).
 * A 0 index marks a padding entry: it MUST be kept so later entries stay
 * byte aligned (IgH cb324580 lesson). --- */
typedef struct
{
   uint16_t index;
   uint8_t subindex;
   uint16_t bitlen;
} jsdk_pdo_entry_t;

/* --- byte offsets of the mapped fields within slave->outputs/inputs.
 * Computed by the offset walker (includes padding). --- */
typedef struct
{
   unsigned int controlword;       /* u16  output */
   unsigned int target_position;   /* i32  output */
   unsigned int target_velocity;   /* i32  output */
   unsigned int target_torque;     /* i16  output */
   unsigned int mode_of_operation; /* u8   output */
   unsigned int statusword;        /* u16  input  */
   unsigned int actual_position;   /* i32  input  */
   unsigned int actual_velocity;   /* i32  input  */
   unsigned int actual_torque;     /* i16  input  */
   unsigned int mode_display;      /* u8   input  */
} jsdk_pdo_offsets_t;

/* --- L2 device profile (declarative; transport agnostic). --- */
struct jsdk_joint_profile
{
   const char *name;          /* points into the profile's own memory for
                                 heap (ESI) profiles */
   uint32_t vendor_id;
   uint32_t product_code;
   uint32_t revision_no;
   uint16_t dc_assign_activate; /* 0 = no DC; else e.g. 0x300 */
   uint32_t min_cycle_ns;
   int8_t mode_csp;
   int8_t mode_csv;
   int8_t mode_cst;
   unsigned int rx_entry_count;
   unsigned int tx_entry_count;
   uint16_t rx_pdo_index;             /* e.g. 0x1600 (written to 0x1C12) */
   uint16_t tx_pdo_index;             /* e.g. 0x1A00 (written to 0x1C13) */
   const jsdk_pdo_entry_t *rx_entries; /* RxPDO order, incl. pad */
   const jsdk_pdo_entry_t *tx_entries; /* TxPDO order, incl. pad */
   void *heap_block;                   /* non-NULL = heap-allocated (ESI) */
};

/* --- L3 CiA402 per-axis state (pure logic, no I/O). --- */
typedef struct
{
   int enable_requested;
   int fault_reset_requested;
   int homing_start_requested; /* mode 6: OR bit4 after OE */
   int8_t requested_mode;
   uint16_t controlword;
   jsdk_axis_state_t axis_state;
   int operation_enabled;
} jsdk_cia402_axis_t;

/* --- latest user command (written to PDO in cycle_end). --- */
typedef struct
{
   int32_t target_position;
   int32_t target_velocity;
   int16_t target_torque;
} jsdk_joint_command_t;

/* --- non-blocking SDO engine request.
 * One request is served to completion at a time (FIFO, slot 0 has priority
 * and is reserved for the fault-code auto read). The engine rides on SOEM's
 * cyclic mailbox: ecx_mbxsend() queues frames, ecx_mbxhandler() transmits
 * them and parks CoE responses in slave->coembxin, and each cycle we consume
 * one response per active request. Never blocks / sleeps. --- */
#define JSDK_MAX_SDO_REQUESTS 12

/* engine phases */
enum
{
   JSDK_REQ_ST_IDLE = 0,   /* nothing running */
   JSDK_REQ_ST_SEND,       /* (re)send current frame, then wait reply */
   JSDK_REQ_ST_WAIT,       /* awaiting the reply of the last send */
   JSDK_REQ_ST_DONE        /* finished: user state is SUCCESS/ERROR */
};

/* transfer kinds while a request runs */
enum
{
   JSDK_REQ_NONE = 0,
   JSDK_REQ_READ,
   JSDK_REQ_WRITE
};

typedef struct
{
   int used;
   uint16_t index;
   uint8_t subindex;
   uint8_t *buf;          /* caller data buffer */
   size_t bufsize;
   size_t datasize;       /* bytes transferred on the last op */
   jsdk_sdo_state_t state;/* user-facing: IDLE/BUSY/SUCCESS/ERROR */

   /* scheduler/engine */
   int op;                /* JSDK_REQ_READ / WRITE when a request is pending
                             to start (state IDLE) */
   int stage;             /* JSDK_REQ_ST_* while running */
   int kind;              /* JSDK_REQ_READ/WRITE while running */
   int phase;             /* 0 = initial transfer, 1 = segmented transfer */
   uint8_t toggle;        /* segment toggle */
   int seg_total;         /* expected total bytes (normal upload) */
   int seg_done;          /* bytes copied so far */
   int not_last;          /* segmented download: more chunks follow */
   int write_left;        /* bytes still to download */
   int write_off;         /* offset of next download chunk in buf */
   uint32_t abort_code;
   int cycles;            /* watchdog since last progress */
   uint8_t wkc;           /* last low-level wkc for diagnostics */
} jsdk_sdo_req;

/* --- fault-diagnosis state (rides on sdo_reqs[0]) --- */
enum
{
   JSDK_FAULT_IDLE = 0,
   JSDK_FAULT_RD_603F,
   JSDK_FAULT_RD_1001,
   JSDK_FAULT_RD_203F,
   JSDK_FAULT_RD_203E,
   JSDK_FAULT_DONE
};

/* --- joint instance --- */
struct jsdk_joint
{
   struct jsdk_context *ctx;
   const struct jsdk_joint_profile *profile;
   uint16_t alias;
   uint16_t slave; /* slave number in bus topology (1-based) */
   uint32_t sync0_cycle_ns;  /* 0 = inherit context / period */
   int32_t sync0_shift_ns;   /* 0 = inherit context / cycle/4 */
   unsigned int wait_ms;     /* 0 = inherit context / default */
   uint32_t max_motor_speed; /* 0x6080; 0 = env / default 80000 */
   jsdk_pdo_offsets_t offsets;
   jsdk_cia402_axis_t cia402;
   jsdk_joint_command_t command;
   jsdk_joint_feedback_t feedback;

   /* async SDO engine */
   int mbx_cyclic;          /* engine available (ecx_slavembxcyclic OK) */
   jsdk_sdo_req sdo_reqs[JSDK_MAX_SDO_REQUESTS];
   unsigned int sdo_req_count;

   /* fault diagnosis closed loop */
   jsdk_fault_info_t fault_info;
   int fault_seq;           /* JSDK_FAULT_* */
   int fault_notified;      /* callback fired for the current episode */
   int fault_active_prev;   /* axis was in fault last cycle */
   uint8_t fault_buf[8];    /* scratch for the 0x603F/0x1001/0x203F/0x203E */
                            /* auto reads (rides on sdo_reqs[0]) */

   /* unit conversion */
   jsdk_unit_scale_t scale;
   int64_t pos_accumulator;   /* unwrapped position (command units) */
   int32_t last_raw_position;
   int pos_accum_valid;
};

/* --- context: soem MUST be the first member so the PO2SOconfig hook can
 * recover the jsdk_context_t from the ecx_contextt* it receives. --- */
struct jsdk_context
{
   ecx_contextt soem;             /* SOEM master context (incl. slave list) */
   uint8_t io_map[4096];          /* process-data IOmap (group 0) */
   jsdk_context_config_t config;
   jsdk_joint_t **joints;
   unsigned int joint_count;
   const struct jsdk_joint_profile **loaded_profiles; /* ESI heap profiles */
   unsigned int loaded_count;
   unsigned int loaded_capacity;
   int bus_open;                  /* ecx_init succeeded */
   int configured;
   int activated;
   int expected_wkc;              /* outputsWKC*2 + inputsWKC */
   int last_wkc;
   jsdk_bus_state_t bus;
   jsdk_fault_callback_t fault_cb;
   void *fault_cb_data;
   char last_error[256];
   /* Soft-DC: PI offset so host wake aligns with slave SYNC0 (see ec_sample). */
   int64_t dc_toff_ns;
   int64_t dc_integral;
};

/* recover owning context from a PO2SOconfig callback context pointer */
#define JSDK_FROM_SOEM(soem_p) ((jsdk_context_t *)(soem_p))

/* --- L2 profile registry / ESI loader --- */
const struct jsdk_joint_profile *jsdk_profile_find(const char *name);
const struct jsdk_joint_profile *jsdk_profile_cyberbeast_joint_module(void);
/* ESI XML loader (src/esi_parser.c) */
const struct jsdk_joint_profile *esi_profile_load(
        const char *esi_path, uint32_t vendor_id, uint32_t product_code);
void esi_profile_free(const struct jsdk_joint_profile *profile);
const struct jsdk_joint_profile *esi_registry_find(const char *name);

/* --- L3 CiA402 (src/cia402.c) --- */
void jsdk_cia402_init(jsdk_cia402_axis_t *axis);
void jsdk_cia402_request_enable(jsdk_cia402_axis_t *axis, int8_t mode);
void jsdk_cia402_request_disable(jsdk_cia402_axis_t *axis);
void jsdk_cia402_request_fault_reset(jsdk_cia402_axis_t *axis);
void jsdk_cia402_request_homing_start(jsdk_cia402_axis_t *axis);
void jsdk_cia402_clear_homing_start(jsdk_cia402_axis_t *axis);
void jsdk_cia402_set_mode(jsdk_cia402_axis_t *axis, int8_t mode);
void jsdk_cia402_update(jsdk_cia402_axis_t *axis, uint16_t statusword);

/* --- misc helpers (src/transport.c) --- */
void jsdk_set_error(jsdk_context_t *ctx, const char *fmt, ...);

/* --- async SDO engine (src/sdo_async.c) --- */
int jsdk_sdo_engine_init(jsdk_context_t *ctx);      /* after configure */
void jsdk_sdo_engine_cycle(jsdk_context_t *ctx);    /* in cycle_end */
void jsdk_sdo_engine_shutdown(jsdk_context_t *ctx); /* on destroy */
void jsdk_fault_service(jsdk_context_t *ctx);       /* in cycle_end */
/* public ABI wrappers are in sdo_async.c */
/* internal fault auto-read uses sdo_reqs[0] */

#endif
