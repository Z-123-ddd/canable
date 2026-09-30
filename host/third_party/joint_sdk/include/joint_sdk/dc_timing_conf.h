#ifndef JSDK_DC_TIMING_CONF_H
#define JSDK_DC_TIMING_CONF_H

#include <stdint.h>

#include <joint_sdk/joint_sdk.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Persistent DC timing (independent of CSP/CSV/CST/homing).
 * Default path: config/dc_timing.conf, or $JSDK_DC_TIMING_CONF.
 *
 * mode=shared  — one SYNC0/shift/wait for every slave
 * mode=per_slave — [slave1] / [slave2] ... by bus position
 *
 * period_ns is always global (master task).
 */
#define JSDK_DC_TIMING_SHARED     0
#define JSDK_DC_TIMING_PER_SLAVE  1
#define JSDK_DC_TIMING_CONF_DEFAULT "config/dc_timing.conf"

const char *jsdk_dc_timing_conf_path(void);

/** Apply global period + shared defaults onto context config. */
int jsdk_dc_timing_conf_apply(jsdk_context_config_t *cfg);

/**
 * Fill joint DC from conf for bus position (1-based).
 * SHARED → shared keys; PER_SLAVE → [slaveN] then shared fallback.
 */
int jsdk_dc_timing_conf_apply_slave(uint16_t position,
                                    jsdk_joint_config_t *jcfg);

uint32_t jsdk_dc_timing_conf_period_ns(void);
int jsdk_dc_timing_conf_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* JSDK_DC_TIMING_CONF_H */
