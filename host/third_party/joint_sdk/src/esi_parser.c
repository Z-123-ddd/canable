/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief ESI XML -> jsdk_joint_profile loader (L2, multi-model support).
 *
 * Zero-dependency mini XML scanner tailored to CiA402/CyberBeast style ESI
 * files (see ECAT_CIA402.xml). It extracts the device identity, the RxPDO /
 * TxPDO entry sets (index:sub + bit length, in mapping order, padding kept)
 * and builds a heap jsdk_joint_profile_t. Among the listed PDOs it picks the
 * "full" one (the set containing the control/status word with most entries),
 * which the transport writes to 0x1C12 / 0x1C13.
 *
 * NOTE: This parser is validated against the bundled CyberBeast ESI. Other
 * vendors' XML dialects may need small tweaks; keep the neutral structures in
 * internal.h untouched so only this file adapts.
 */

#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ESI_MAX_DEVICES 16
#define ESI_MAX_PDOS    16
#define ESI_MAX_ENTRIES 64

/* global registry of loaded (heap) profiles so that add_joint can resolve
 * them by name without tying the profile to a context */
#define ESI_REG_MAX 32
static const struct jsdk_joint_profile *esi_registry[ESI_REG_MAX];
static unsigned int esi_reg_count;

static void
esi_reg_add(const struct jsdk_joint_profile *p)
{
   if (esi_reg_count < ESI_REG_MAX)
   {
      esi_registry[esi_reg_count++] = p;
   }
}

static void
esi_reg_remove(const struct jsdk_joint_profile *p)
{
   unsigned int i;
   for (i = 0; i < esi_reg_count; ++i)
   {
      if (esi_registry[i] == p)
      {
         memmove(&esi_registry[i], &esi_registry[i + 1],
                 (esi_reg_count - i - 1) * sizeof(esi_registry[0]));
         esi_reg_count--;
         return;
      }
   }
}

const struct jsdk_joint_profile *
esi_registry_find(const char *name)
{
   unsigned int i;
   if (!name)
   {
      return NULL;
   }
   for (i = 0; i < esi_reg_count; ++i)
   {
      if (esi_registry[i]->name &&
          !strcmp(esi_registry[i]->name, name))
      {
         return esi_registry[i];
      }
   }
   return NULL;
}

/* ---- tiny text helpers (buffer is NUL terminated) ---- */
static char *
read_file(const char *path, size_t *len_out)
{
   FILE *f;
   long sz;
   char *buf;

   f = fopen(path, "rb");
   if (!f)
   {
      return NULL;
   }
   if (fseek(f, 0, SEEK_END) != 0)
   {
      fclose(f);
      return NULL;
   }
   sz = ftell(f);
   if (sz <= 0)
   {
      fclose(f);
      return NULL;
   }
   rewind(f);
   buf = (char *)malloc((size_t)sz + 1);
   if (!buf)
   {
      fclose(f);
      return NULL;
   }
   if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
   {
      free(buf);
      fclose(f);
      return NULL;
   }
   buf[sz] = '\0';
   if (len_out)
   {
      *len_out = (size_t)sz;
   }
   fclose(f);
   return buf;
}

/* parse a numeric literal: hex "#x.."/"0x.." or decimal */
static unsigned long
parse_uint(const char *s)
{
   while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
   {
      s++;
   }
   if (s[0] == '#' && (s[1] == 'x' || s[1] == 'X'))
   {
      return strtoul(s + 2, NULL, 16);
   }
   return strtoul(s, NULL, 0);
}

/* content between the first "<tag" .. ">" and the first "</tag>" */
static char *
elem_body(const char *s, const char *tag, char **next)
{
   char open[80];
   char close[96];
   const char *a;
   const char *gt;
   const char *b;
   char *out;
   size_t n;

   snprintf(open, sizeof(open), "<%s", tag);
   snprintf(close, sizeof(close), "</%s>", tag);
   a = strstr(s, open);
   if (!a)
   {
      return NULL;
   }
   gt = strchr(a, '>');
   if (!gt)
   {
      return NULL;
   }
   b = strstr(gt + 1, close);
   if (!b)
   {
      return NULL;
   }
   n = (size_t)(b - (gt + 1));
   out = (char *)malloc(n + 1);
   if (!out)
   {
      return NULL;
   }
   memcpy(out, gt + 1, n);
   out[n] = '\0';
   if (next)
   {
      *next = (char *)(b + strlen(close));
   }
   return out;
}

/* value of attr="..." inside the tag starting at s (s points at '<') */
static unsigned long
tag_attr(const char *s, const char *attr)
{
   char pat[64];
   const char *p;
   const char *eq;
   const char *quote;

   snprintf(pat, sizeof(pat), "%s", attr);
   p = strstr(s, pat);
   if (!p)
   {
      return 0;
   }
   eq = strchr(p, '=');
   if (!eq)
   {
      return 0;
   }
   quote = strchr(eq + 1, '"');
   if (!quote)
   {
      return 0;
   }
   return parse_uint(quote + 1);
}

/* first element <name>..</name> inside s, parsed as a number */
static int
elem_num(const char *s, const char *name, unsigned long *out)
{
   char *body = elem_body(s, name, NULL);
   if (!body)
   {
      return 0;
   }
   *out = parse_uint(body);
   free(body);
   return 1;
}

/* ---- per-entry / per-PDO collection ---- */
typedef struct
{
   uint16_t index;
   uint8_t subindex;
   uint16_t bitlen;
} esi_entry_t;

typedef struct
{
   int present;
   uint16_t index;
   unsigned int count;
   esi_entry_t entries[ESI_MAX_ENTRIES];
} esi_pdo_t;

/* parse one "<RxPdo ...>...</RxPdo>" body: PDO index + ordered entries.
 * Entry layouts vary between vendors: either <Entry> wrappers (CyberBeast/
 * CiA402) or bare <SubIndex> elements; both are handled. Padding entries
 * (Index 0) are preserved. */
static void
parse_pdo_body(const char *body, esi_pdo_t *pdo)
{
   const char *cur = body;
   unsigned long v = 0;

   pdo->present = 1;
   pdo->count = 0;
   pdo->index = 0;
   if (elem_num(body, "Index", &v))
   {
      pdo->index = (uint16_t)v;
   }

   while (pdo->count < ESI_MAX_ENTRIES)
   {
      const char *pe = strstr(cur, "<Entry");
      const char *ps = strstr(cur, "<SubIndex");
      const char *marker;
      const char *tagname;
      char *nxt = NULL;
      char *sub;
      esi_entry_t *e;

      if (!pe && !ps)
      {
         break;
      }
      if (pe && (!ps || pe < ps))
      {
         marker = pe;
         tagname = "Entry";
      }
      else
      {
         marker = ps;
         tagname = "SubIndex";
      }
      sub = elem_body(marker, tagname, &nxt);
      if (!sub)
      {
         break;
      }
      e = &pdo->entries[pdo->count];
      e->index = 0;
      e->subindex = 0;
      e->bitlen = 0;
      if (elem_num(sub, "Index", &v))
      {
         e->index = (uint16_t)v;
      }
      if (elem_num(sub, "BitLen", &v))
      {
         e->bitlen = (uint16_t)v;
      }
      if (elem_num(sub, "SubIndex", &v))
      {
         e->subindex = (uint8_t)v;
      }
      pdo->count++;
      free(sub);
      if (!nxt || nxt == cur)
      {
         break;
      }
      cur = nxt;
   }
}

/* scan a device body for all RxPdo/TxPdo blocks */
static void
scan_pdos(const char *body, esi_pdo_t *rx, int *rx_n, esi_pdo_t *tx,
          int *tx_n, int cap)
{
   const char *cur = body;
   int n = 0;

   while (n < cap)
   {
      const char *open = strstr(cur, "<RxPdo");
      if (!open)
      {
         break;
      }
      {
         const char *endtag = strstr(open, "</RxPdo>");
         const char *bodyend = endtag ? endtag : open + strlen(open);
         char *b = (char *)malloc((size_t)(bodyend - open) + 2);
         if (b)
         {
            size_t m = (size_t)(bodyend - open);
            memcpy(b, open, m);
            b[m] = '\0';
            parse_pdo_body(b, &rx[n]);
            if (tag_attr(open, "Index"))
            {
               rx[n].index = (uint16_t)tag_attr(open, "Index");
            }
            free(b);
         }
      }
      n++;
      cur = open + 6;
   }
   *rx_n = n;

   n = 0;
   cur = body;
   while (n < cap)
   {
      const char *open = strstr(cur, "<TxPdo");
      if (!open)
      {
         break;
      }
      {
         const char *endtag = strstr(open, "</TxPdo>");
         const char *bodyend = endtag ? endtag : open + strlen(open);
         char *b = (char *)malloc((size_t)(bodyend - open) + 2);
         if (b)
         {
            size_t m = (size_t)(bodyend - open);
            memcpy(b, open, m);
            b[m] = '\0';
            parse_pdo_body(b, &tx[n]);
            if (tag_attr(open, "Index"))
            {
               tx[n].index = (uint16_t)tag_attr(open, "Index");
            }
            free(b);
         }
      }
      n++;
      cur = open + 6;
   }
   *tx_n = n;
}

/* pick the "full" PDO: contains the control/status word and most entries */
static const char *
find_device_tag(const char *s)
{
   const char *a = s ? strstr(s, "<Device ") : NULL;
   const char *b = s ? strstr(s, "<Device>") : NULL;
   if (a && (!b || a < b))
   {
      return a;
   }
   return b;
}

static int
pick_full(const esi_pdo_t *pdo, int n, int is_tx)
{
   int best = -1;
   int i;

   for (i = 0; i < n; ++i)
   {
      int has_key = 0;
      unsigned int k;
      for (k = 0; k < pdo[i].count; ++k)
      {
         if ((!is_tx && pdo[i].entries[k].index == JSDK_CIA402_CONTROLWORD) ||
             (is_tx && pdo[i].entries[k].index == JSDK_CIA402_STATUSWORD))
         {
            has_key = 1;
            break;
         }
      }
      if (has_key)
      {
         if (best < 0 || pdo[i].count > pdo[best].count)
         {
            best = i;
         }
      }
   }
   if (best < 0 && n > 0)
   {
      best = 0; /* fallback: first PDO */
   }
   return best;
}

/* ==================================================================== */
/* loader                                                               */
/* ==================================================================== */
const struct jsdk_joint_profile *
esi_profile_load(const char *esi_path, uint32_t vendor_id,
                 uint32_t product_code)
{
   char *buf;
   esi_pdo_t rx[ESI_MAX_PDOS];
   esi_pdo_t tx[ESI_MAX_PDOS];
   int rx_n = 0;
   int tx_n = 0;
   uint32_t prod_match = 0;
   uint32_t mvid = 0;
   uint32_t mprod = 0;
   uint32_t mrev = 0;
   unsigned long top_vendor = 0;
   char *vendor_body;
   struct jsdk_joint_profile *prof = NULL;
   char *name = NULL;
   jsdk_pdo_entry_t *rx_entries = NULL;
   jsdk_pdo_entry_t *tx_entries = NULL;
   const char *cur;
   int i;
   int rx_idx = -1;
   int tx_idx = -1;

   buf = read_file(esi_path, NULL);
   if (!buf)
   {
      return NULL;
   }

   /* top level vendor id */
   vendor_body = elem_body(buf, "Vendor", NULL);
   if (vendor_body)
   {
      elem_num(vendor_body, "Id", &top_vendor);
      free(vendor_body);
   }

   /* iterate <Device> regions (bounded by sibling <Device> or end) */
   cur = buf;
   for (i = 0; i < ESI_MAX_DEVICES; ++i)
   {
      const char *dev = find_device_tag(cur);
      const char *dev_end;
      char *dbody;
      char *namebuf;
      uint32_t pcode = 0, rcode = 0, vid = 0;
      const char *type;

      if (!dev)
      {
         break;
      }
      dev_end = find_device_tag(dev + 1);
      {
         const char *next = strstr(dev + 1, "</Descriptions>");
         if (next && (!dev_end || next < dev_end))
         {
            dev_end = next;
         }
      }
      if (!dev_end)
      {
         dev_end = dev + strlen(dev);
      }
      dbody = (char *)malloc((size_t)(dev_end - dev) + 2);
      if (!dbody)
      {
         break;
      }
      {
         size_t m = (size_t)(dev_end - dev);
         memcpy(dbody, dev, m);
         dbody[m] = '\0';
      }
      cur = dev + 7;

      /* device identity: <Type ProductCode=.. RevisionNo=..> and optional
       * <VendorId> element */
      type = strstr(dbody, "<Type");
      if (type)
      {
         pcode = (uint32_t)tag_attr(type, "ProductCode");
         rcode = (uint32_t)tag_attr(type, "RevisionNo");
         vid = (uint32_t)tag_attr(type, "VendorId");
      }
      if (!vid)
      {
         elem_num(dbody, "VendorId", (unsigned long *)&vid);
      }
      if (!vid)
      {
         vid = (uint32_t)top_vendor;
      }
      if (!pcode)
      {
         free(dbody);
         continue;
      }
      if ((vendor_id && vid && vid != vendor_id) ||
          (product_code && pcode != product_code))
      {
         free(dbody);
         continue;
      }
      /* matched device */
      prod_match = pcode;
      mvid = vid;
      mprod = pcode;
      mrev = rcode;

      namebuf = elem_body(dbody, "Name", NULL);
      if (namebuf)
      {
         /* trim */
         name = namebuf;
         {
            char *p = name;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            {
               p++;
            }
            if (p != name)
            {
               memmove(name, p, strlen(p) + 1);
            }
            p = name + strlen(name);
            while (p > name && (p[-1] == ' ' || p[-1] == '\t' ||
                                p[-1] == '\r' || p[-1] == '\n'))
            {
               *--p = '\0';
            }
         }
      }

      scan_pdos(dbody, rx, &rx_n, tx, &tx_n, ESI_MAX_PDOS);
      rx_idx = pick_full(rx, rx_n, 0);
      tx_idx = pick_full(tx, tx_n, 1);
      free(dbody);
      break; /* matched one device */
   }

   if (!prod_match || rx_idx < 0 || tx_idx < 0)
   {
      free(buf);
      if (name)
      {
         free(name);
      }
      return NULL;
   }

   /* allocate single heap block: entries + name + profile struct */
   {
      size_t rx_cnt = rx[rx_idx].count;
      size_t tx_cnt = tx[tx_idx].count;
      char *block;

      rx_entries = (jsdk_pdo_entry_t *)calloc(rx_cnt ? rx_cnt : 1,
                                              sizeof(jsdk_pdo_entry_t));
      tx_entries = (jsdk_pdo_entry_t *)calloc(tx_cnt ? tx_cnt : 1,
                                              sizeof(jsdk_pdo_entry_t));
      if (!rx_entries || !tx_entries)
      {
         free(rx_entries);
         free(tx_entries);
         free(buf);
         if (name)
         {
            free(name);
         }
         return NULL;
      }
      for (i = 0; i < (int)rx_cnt; ++i)
      {
         rx_entries[i].index = rx[rx_idx].entries[i].index;
         rx_entries[i].subindex = rx[rx_idx].entries[i].subindex;
         rx_entries[i].bitlen = rx[rx_idx].entries[i].bitlen;
      }
      for (i = 0; i < (int)tx_cnt; ++i)
      {
         tx_entries[i].index = tx[tx_idx].entries[i].index;
         tx_entries[i].subindex = tx[tx_idx].entries[i].subindex;
         tx_entries[i].bitlen = tx[tx_idx].entries[i].bitlen;
      }

      prof = (struct jsdk_joint_profile *)calloc(1, sizeof(*prof));
      if (!prof)
      {
         free(rx_entries);
         free(tx_entries);
         free(buf);
         if (name)
         {
            free(name);
         }
         return NULL;
      }
      /* one block so it can be released with a single free later: pack name,
       * entries and struct into a contiguous allocation via heap_block */
      block = (char *)malloc(sizeof(*prof) +
                             (name ? strlen(name) + 1 : 1) +
                             (rx_cnt ? rx_cnt * sizeof(jsdk_pdo_entry_t) : 0) +
                             (tx_cnt ? tx_cnt * sizeof(jsdk_pdo_entry_t) : 0));
      if (!block)
      {
         free(rx_entries);
         free(tx_entries);
         free(prof);
         free(buf);
         if (name)
         {
            free(name);
         }
         return NULL;
      }
      {
         char *bp = block;
         memcpy(bp, prof, sizeof(*prof));
         free(prof);
         prof = (struct jsdk_joint_profile *)bp;
         bp += sizeof(*prof);
         if (name)
         {
            size_t nl = strlen(name) + 1;
            memcpy(bp, name, nl);
            prof->name = bp;
            bp += nl;
         }
         else
         {
            prof->name = "esi-profile";
         }
         if (rx_cnt)
         {
            memcpy(bp, rx_entries, rx_cnt * sizeof(jsdk_pdo_entry_t));
            prof->rx_entries = (const jsdk_pdo_entry_t *)bp;
            bp += rx_cnt * sizeof(jsdk_pdo_entry_t);
         }
         else
         {
            prof->rx_entries = NULL;
         }
         if (tx_cnt)
         {
            memcpy(bp, tx_entries, tx_cnt * sizeof(jsdk_pdo_entry_t));
            prof->tx_entries = (const jsdk_pdo_entry_t *)bp;
            bp += tx_cnt * sizeof(jsdk_pdo_entry_t);
         }
         else
         {
            prof->tx_entries = NULL;
         }
         prof->heap_block = block;
      }

      prof->vendor_id = mvid;
      prof->product_code = mprod;
      prof->revision_no = mrev;
      prof->rx_entry_count = (unsigned int)rx_cnt;
      prof->tx_entry_count = (unsigned int)tx_cnt;
      prof->rx_pdo_index = rx[rx_idx].index;
      prof->tx_pdo_index = tx[tx_idx].index;
      /* Default DC SYNC0 (IgH). Arm SYNC0 inside PO2SO before SyncType=2.
       * JSDK_DC=0 → SM-sync / AssignActivate 0. */
      {
         const char *dc_env = getenv("JSDK_DC");
         if (dc_env && dc_env[0] == '0')
            prof->dc_assign_activate = 0x0000u;
         else
            prof->dc_assign_activate = 0x0300u;
      }
      prof->min_cycle_ns = 50000u;
      prof->mode_csp = JSDK_MODE_CSP;
      prof->mode_csv = JSDK_MODE_CSV;
      prof->mode_cst = JSDK_MODE_CST;

      free(rx_entries);
      free(tx_entries);
   }

   free(buf);
   if (name)
   {
      free(name);
   }
   esi_reg_add(prof);
   return prof;
}

void
esi_profile_free(const struct jsdk_joint_profile *profile)
{
   if (profile && profile->heap_block)
   {
      esi_reg_remove(profile);
      free(profile->heap_block);
   }
}
