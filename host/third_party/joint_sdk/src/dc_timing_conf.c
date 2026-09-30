#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <joint_sdk/dc_timing_conf.h>

#define JSDK_DC_SLAVE_SLOTS 16

typedef struct
{
   int valid;
   uint32_t sync0_cycle_ns;
   int32_t sync0_shift_ns;
   unsigned int wait_ms;
   int set_cycle;
   int set_shift;
   int set_wait;
} jsdk_dc_slave_slot_t;

typedef struct
{
   int loaded;
   int mode; /* JSDK_DC_TIMING_* */
   int set_period;
   int set_sync0_cycle;
   int set_sync0_shift;
   int set_wait_ms;
   uint32_t period_ns;
   uint32_t sync0_cycle_ns;
   int32_t sync0_shift_ns;
   unsigned int wait_ms;
   jsdk_dc_slave_slot_t slaves[JSDK_DC_SLAVE_SLOTS];
} jsdk_dc_timing_file_t;

static jsdk_dc_timing_file_t g_dc;

const char *
jsdk_dc_timing_conf_path(void)
{
   const char *e = getenv("JSDK_DC_TIMING_CONF");
   if (e && e[0])
      return e;
   return JSDK_DC_TIMING_CONF_DEFAULT;
}

static char *
trim_inplace(char *s)
{
   char *e;
   while (*s && isspace((unsigned char)*s))
      s++;
   if (!*s)
      return s;
   e = s + strlen(s);
   while (e > s && isspace((unsigned char)e[-1]))
      *--e = '\0';
   return s;
}

static int
try_fopen(const char *path, FILE **fp)
{
   *fp = fopen(path, "r");
   return *fp != NULL;
}

static FILE *
open_conf_read(void)
{
   FILE *fp = NULL;
   const char *primary = jsdk_dc_timing_conf_path();
   if (try_fopen(primary, &fp))
      return fp;
   /* When cwd is build/examples/RelWithDebInfo */
   if (strcmp(primary, JSDK_DC_TIMING_CONF_DEFAULT) == 0)
   {
      if (try_fopen("../config/dc_timing.conf", &fp))
         return fp;
      if (try_fopen("../../config/dc_timing.conf", &fp))
         return fp;
      if (try_fopen("../../../config/dc_timing.conf", &fp))
         return fp;
   }
   return NULL;
}

static int
parse_slave_section(const char *name, int *pos_out)
{
   unsigned long pos;
   char *end = NULL;
   if (strncmp(name, "slave", 5) != 0)
      return 0;
   pos = strtoul(name + 5, &end, 10);
   if (!end || *end || pos < 1 || pos > JSDK_DC_SLAVE_SLOTS)
      return 0;
   *pos_out = (int)pos;
   return 1;
}

static int
load_file(void)
{
   FILE *fp;
   char line[256];
   int cur_slave = 0; /* 1-based; 0 = global */

   if (g_dc.loaded)
      return 0;

   memset(&g_dc, 0, sizeof(g_dc));
   g_dc.mode = JSDK_DC_TIMING_SHARED;
   g_dc.period_ns = JSDK_DEFAULT_PERIOD_NS;
   g_dc.sync0_cycle_ns = JSDK_DEFAULT_PERIOD_NS;
   g_dc.sync0_shift_ns = (int32_t)(JSDK_DEFAULT_PERIOD_NS / 4);
   g_dc.wait_ms = JSDK_DEFAULT_WAIT_MS;

   fp = open_conf_read();
   if (!fp)
   {
      g_dc.loaded = 1;
      return 0;
   }

   while (fgets(line, sizeof(line), fp))
   {
      char *p = trim_inplace(line);
      char *eq;
      char key[64];
      char val[128];
      int pos;

      if (!p[0] || p[0] == '#')
         continue;
      if (p[0] == '[' && p[strlen(p) - 1] == ']')
      {
         size_t n = strlen(p) - 2;
         char name[32];
         if (n >= sizeof(name))
            n = sizeof(name) - 1;
         memcpy(name, p + 1, n);
         name[n] = '\0';
         if (parse_slave_section(name, &pos))
         {
            cur_slave = pos;
            g_dc.slaves[pos - 1].valid = 1;
            g_dc.mode = JSDK_DC_TIMING_PER_SLAVE;
         }
         else
            cur_slave = 0;
         continue;
      }
      eq = strchr(p, '=');
      if (!eq)
         continue;
      *eq = '\0';
      strncpy(key, trim_inplace(p), sizeof(key) - 1);
      key[sizeof(key) - 1] = '\0';
      strncpy(val, trim_inplace(eq + 1), sizeof(val) - 1);
      val[sizeof(val) - 1] = '\0';

      if (!strcmp(key, "mode"))
      {
         if (!strcmp(val, "shared") || !strcmp(val, "0"))
            g_dc.mode = JSDK_DC_TIMING_SHARED;
         else if (!strcmp(val, "per_slave") || !strcmp(val, "1"))
            g_dc.mode = JSDK_DC_TIMING_PER_SLAVE;
         continue;
      }

      if (cur_slave > 0)
      {
         jsdk_dc_slave_slot_t *s = &g_dc.slaves[cur_slave - 1];
         s->valid = 1;
         if (!strcmp(key, "sync0_cycle_ns"))
         {
            s->sync0_cycle_ns = (uint32_t)strtoul(val, NULL, 0);
            s->set_cycle = 1;
         }
         else if (!strcmp(key, "sync0_shift_ns"))
         {
            s->sync0_shift_ns = (int32_t)strtol(val, NULL, 0);
            s->set_shift = 1;
         }
         else if (!strcmp(key, "wait_ms"))
         {
            s->wait_ms = (unsigned int)strtoul(val, NULL, 0);
            s->set_wait = 1;
         }
         continue;
      }

      if (!strcmp(key, "period_ns"))
      {
         g_dc.period_ns = (uint32_t)strtoul(val, NULL, 0);
         g_dc.set_period = 1;
      }
      else if (!strcmp(key, "sync0_cycle_ns"))
      {
         g_dc.sync0_cycle_ns = (uint32_t)strtoul(val, NULL, 0);
         g_dc.set_sync0_cycle = 1;
      }
      else if (!strcmp(key, "sync0_shift_ns"))
      {
         g_dc.sync0_shift_ns = (int32_t)strtol(val, NULL, 0);
         g_dc.set_sync0_shift = 1;
      }
      else if (!strcmp(key, "wait_ms"))
      {
         g_dc.wait_ms = (unsigned int)strtoul(val, NULL, 0);
         g_dc.set_wait_ms = 1;
      }
   }
   fclose(fp);
   g_dc.loaded = 1;
   return 0;
}

int
jsdk_dc_timing_conf_apply(jsdk_context_config_t *cfg)
{
   if (!cfg)
      return -1;
   load_file();
   if (g_dc.set_period && g_dc.period_ns)
      cfg->period_ns = g_dc.period_ns;
   if (g_dc.set_sync0_cycle)
      cfg->sync0_cycle_ns = g_dc.sync0_cycle_ns;
   if (g_dc.set_sync0_shift)
      cfg->sync0_shift_ns = g_dc.sync0_shift_ns;
   if (g_dc.set_wait_ms && g_dc.wait_ms)
      cfg->wait_ms = g_dc.wait_ms;
   return 0;
}

int
jsdk_dc_timing_conf_apply_slave(uint16_t position, jsdk_joint_config_t *jcfg)
{
   if (!jcfg || position == 0 || position > JSDK_DC_SLAVE_SLOTS)
      return -1;
   load_file();

   /* Shared / global defaults first */
   if (g_dc.set_sync0_cycle)
      jcfg->sync0_cycle_ns = g_dc.sync0_cycle_ns;
   if (g_dc.set_sync0_shift)
      jcfg->sync0_shift_ns = g_dc.sync0_shift_ns;
   if (g_dc.set_wait_ms)
      jcfg->wait_ms = g_dc.wait_ms;

   if (g_dc.mode == JSDK_DC_TIMING_PER_SLAVE)
   {
      const jsdk_dc_slave_slot_t *s = &g_dc.slaves[position - 1];
      if (s->valid)
      {
         if (s->set_cycle)
            jcfg->sync0_cycle_ns = s->sync0_cycle_ns;
         if (s->set_shift)
            jcfg->sync0_shift_ns = s->sync0_shift_ns;
         if (s->set_wait)
            jcfg->wait_ms = s->wait_ms;
      }
   }
   return 0;
}

uint32_t
jsdk_dc_timing_conf_period_ns(void)
{
   load_file();
   if (g_dc.set_period && g_dc.period_ns)
      return g_dc.period_ns;
   return 0;
}

int
jsdk_dc_timing_conf_mode(void)
{
   load_file();
   return g_dc.mode;
}
