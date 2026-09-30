/*
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 */

/** \file
 * \brief Non-blocking async SDO engine + fault-diagnosis service.
 *
 * Outbound SDO frames are written straight to the slave mailbox (SM0) with
 * ecx_FPWR() - exactly what SOEM's own mbxouthandler does. Inbound CoE
 * replies are delivered by SOEM's cyclic mailbox (ecx_slavembxcyclic +
 * ecx_mbxhandler) and parked in slave->coembxin; the engine consumes at
 * most one reply per cycle with ecx_mbxreceive(..., timeout=0). Nothing here
 * sleeps, loops or blocks the RT cycle. Frame formats / state transitions
 * mirror src/ec_coe.c (ecx_SDOread / ecx_SDOwrite). See ARCHITECTURE 5.2.
 */

#include "internal.h"
#include <stdlib.h>
#include <string.h>

/* ---- fault objects (CiA402 + vendor extension) ---- */
#define FAULT_CODE_603F 0x603Fu
#define FAULT_REG_1001  0x1001u
#define FAULT_VENDOR_LO 0x203Fu
#define FAULT_VENDOR_HI 0x203Eu

/* watchdog: max cycles with no progress (~1 s @ 1 ms cycle) */
#define SDO_REQ_TIMEOUT_CYCLES 1000u
/* max mailbox messages serviced per cycle */
#define MBX_SERVICE_LIMIT 8

/* ---- packed CoE SDO frame (mirrors ec_coe.c ec_SDOt) ---- */
OSAL_PACKED_BEGIN
typedef struct OSAL_PACKED
{
   ec_mbxheadert hdr;
   uint16_t canopen;
   uint8_t command;
   uint16_t index;
   uint8_t subindex;
   union
   {
      uint8_t b[0x200];
      uint32_t l[0x80];
   };
} jsdk_sdo_frame_t;
OSAL_PACKED_END

/* ==================================================================== */
/* outbound frame transmit (mirrors SOEM mbxouthandler: FPWR to SM0)     */
/* ==================================================================== */
static int
sdo_tx_frame(jsdk_context_t *ctx, jsdk_joint_t *joint, const void *frame)
{
   ec_slavet *s = &ctx->soem.slavelist[joint->slave];

   if (s->state < EC_STATE_PRE_OP || !s->mbx_l)
   {
      return 0;
   }
   return ecx_FPWR(&ctx->soem.port, s->configadr, s->mbx_wo, s->mbx_l,
                   (void *)frame, EC_TIMEOUTRET);
}

static void
fill_header(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_frame_t *f,
            uint16_t len)
{
   uint8_t cnt;

   f->hdr.length = htoes(len);
   f->hdr.address = htoes(0x0000);
   f->hdr.priority = 0x00;
   cnt = ec_nextmbxcnt(ctx->soem.slavelist[joint->slave].mbx_cnt);
   ctx->soem.slavelist[joint->slave].mbx_cnt = cnt;
   f->hdr.mbxtype = ECT_MBXT_COE + MBX_HDR_SET_CNT(cnt);
   f->canopen = htoes((uint16_t)(0x000 + (ECT_COES_SDOREQ << 12)));
}

static void
fill_upload_init(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_frame_t *f,
                 const jsdk_sdo_req *req)
{
   memset(f, 0, sizeof(*f));
   fill_header(ctx, joint, f, 0x000a);
   f->command = ECT_SDO_UP_REQ;
   f->index = htoes(req->index);
   f->subindex = req->subindex;
   f->l[0] = 0;
}

static void
fill_upload_seg(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_frame_t *f,
                const jsdk_sdo_req *req)
{
   memset(f, 0, sizeof(*f));
   fill_header(ctx, joint, f, 0x000a);
   f->command = (uint8_t)(ECT_SDO_SEG_UP_REQ + req->toggle);
   f->index = htoes(req->index);
   f->subindex = req->subindex;
   f->l[0] = 0;
}

/* expedited download (single frame, <= 4 bytes) */
static void
fill_download_exp(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_frame_t *f,
                  const jsdk_sdo_req *req)
{
   size_t n = req->bufsize > 4 ? 4 : req->bufsize;

   memset(f, 0, sizeof(*f));
   fill_header(ctx, joint, f, 0x000a);
   f->command = (uint8_t)(ECT_SDO_DOWN_EXP | (((4 - n) << 2) & 0x0c));
   f->index = htoes(req->index);
   f->subindex = req->subindex;
   memcpy(&f->b[0], req->buf, n);
}

/* normal download init: total size in l[0], first chunk in b[4].. */
static void
fill_download_init(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_frame_t *f,
                   jsdk_sdo_req *req)
{
   int maxdata = ctx->soem.slavelist[joint->slave].mbx_l - 0x10;
   int first = (int)req->bufsize;

   if (first > maxdata)
   {
      first = maxdata;
   }
   req->not_last = ((int)req->bufsize > maxdata) ? 1 : 0;
   req->write_left = (int)req->bufsize - first;
   req->write_off = first;

   memset(f, 0, sizeof(*f));
   fill_header(ctx, joint, f, (uint16_t)(0x0a + first));
   f->command = ECT_SDO_DOWN_INIT;
   f->index = htoes(req->index);
   f->subindex = req->subindex;
   f->l[0] = htoel((uint32_t)req->bufsize);
   if (first > 0)
   {
      memcpy(&f->b[4], req->buf, (size_t)first);
   }
}

/* download segment (mirrors ec_coe segmented loop) */
static void
fill_download_seg(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_frame_t *f,
                  jsdk_sdo_req *req)
{
   int maxdata = ctx->soem.slavelist[joint->slave].mbx_l - 0x10;
   int framedatasize = req->write_left;
   uint8_t cmd;

   if (framedatasize > maxdata)
   {
      framedatasize = maxdata;
   }
   req->not_last = (framedatasize < req->write_left) ? 1 : 0;
   cmd = req->not_last ? 0x00 : 0x01;

   memset(f, 0, sizeof(*f));
   if (!req->not_last && (framedatasize < 7))
   {
      fill_header(ctx, joint, f, 0x000a);
      cmd = (uint8_t)(0x01 + ((7 - framedatasize) << 1));
   }
   else
   {
      fill_header(ctx, joint, f, (uint16_t)(framedatasize + 3));
   }
   f->command = (uint8_t)(cmd + req->toggle);
   if (framedatasize > 0)
   {
      memcpy(&f->index, req->buf + req->write_off, (size_t)framedatasize);
   }
   req->write_off += framedatasize;
   req->write_left -= framedatasize;
   req->toggle ^= 0x10;
}

/* ==================================================================== */
/* request helpers                                                      */
/* ==================================================================== */
static jsdk_sdo_req *
req_active(jsdk_joint_t *joint)
{
   unsigned int i;
   for (i = 0; i < joint->sdo_req_count; ++i)
   {
      if (joint->sdo_reqs[i].stage != JSDK_REQ_ST_IDLE)
      {
         return &joint->sdo_reqs[i];
      }
   }
   return NULL;
}

static jsdk_sdo_req *
req_pending(jsdk_joint_t *joint)
{
   unsigned int i;
   for (i = 0; i < joint->sdo_req_count; ++i)
   {
      jsdk_sdo_req *r = &joint->sdo_reqs[i];
      if (r->op != JSDK_REQ_NONE && r->state == JSDK_SDO_BUSY)
      {
         return r;
      }
   }
   return NULL;
}

static void
req_finish(jsdk_joint_t *joint, jsdk_sdo_req *req, jsdk_sdo_state_t st)
{
   req->state = st;
   req->op = JSDK_REQ_NONE;
   req->stage = JSDK_REQ_ST_IDLE;
   req->kind = JSDK_REQ_NONE;
   (void)joint;
}

static void
req_start(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_req *req)
{
   ec_mbxbuft *mbx = NULL;

   /* drain any stale parked reply */
   if (ctx->soem.slavelist[joint->slave].coembxinfull)
   {
      if (ecx_mbxreceive(&ctx->soem, joint->slave, &mbx, 0) > 0 && mbx)
      {
         ecx_dropmbx(&ctx->soem, mbx);
      }
   }
   req->phase = 0;
   req->toggle = 0;
   req->seg_total = 0;
   req->seg_done = 0;
   req->write_off = 0;
   req->write_left = 0;
   req->not_last = 0;
   req->cycles = 0;
   req->kind = req->op;
   req->stage = JSDK_REQ_ST_SEND;
}

/* send the current frame; returns 1 = queued/sent, 0 = retry later */
static int
req_send(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_req *req)
{
   jsdk_sdo_frame_t f;

   if (req->kind == JSDK_REQ_READ)
   {
      if (req->phase == 0)
      {
         fill_upload_init(ctx, joint, &f, req);
      }
      else
      {
         fill_upload_seg(ctx, joint, &f, req);
      }
   }
   else /* WRITE */
   {
      if (req->phase == 0)
      {
         if (req->bufsize <= 4)
         {
            fill_download_exp(ctx, joint, &f, req);
         }
         else
         {
            fill_download_init(ctx, joint, &f, req);
         }
      }
      else
      {
         fill_download_seg(ctx, joint, &f, req);
      }
   }

   if (!sdo_tx_frame(ctx, joint, &f))
   {
      return 0; /* retry next cycle */
   }
   req->stage = JSDK_REQ_ST_WAIT;
   req->cycles = 0;
   return 1;
}

/* consume and process one parked reply; 1 if handled, 0 if none yet */
static int
req_consume(jsdk_context_t *ctx, jsdk_joint_t *joint, jsdk_sdo_req *req)
{
   ec_slavet *s = &ctx->soem.slavelist[joint->slave];
   ec_mbxbuft *mbx = NULL;
   jsdk_sdo_frame_t *r;
   int wkc;
   uint8_t service;

   if (!s->coembxinfull)
   {
      return 0;
   }
   wkc = ecx_mbxreceive(&ctx->soem, joint->slave, &mbx, 0);
   if (wkc <= 0 || !mbx)
   {
      return 0;
   }
   r = (jsdk_sdo_frame_t *)mbx;
   service = (uint8_t)(etohs(r->canopen) >> 12);

   if (((r->hdr.mbxtype & 0x0f) != ECT_MBXT_COE) || (service != ECT_COES_SDORES))
   {
      if (r->command == ECT_SDO_ABORT)
      {
         req->abort_code = etohl(r->l[0]);
      }
      req_finish(joint, req, JSDK_SDO_ERROR);
      ecx_dropmbx(&ctx->soem, mbx);
      return 1;
   }

   if (req->kind == JSDK_REQ_READ)
   {
      if (req->phase == 0)
      {
         /* ---- reply to the upload init request ---- */
         if (r->index != htoes(req->index))
         {
            if (r->command == ECT_SDO_ABORT)
            {
               req->abort_code = etohl(r->l[0]);
            }
            req_finish(joint, req, JSDK_SDO_ERROR);
            ecx_dropmbx(&ctx->soem, mbx);
            return 1;
         }
         if (r->command & 0x02)
         {
            unsigned int bytesize = 4u - ((r->command >> 2) & 0x03u);
            if (req->bufsize >= bytesize)
            {
               memcpy(req->buf, &r->b[0], bytesize);
               req->datasize = bytesize;
               req_finish(joint, req, JSDK_SDO_SUCCESS);
            }
            else
            {
               req_finish(joint, req, JSDK_SDO_ERROR);
            }
         }
         else
         {
            uint32_t sdolen = etohl(r->l[0]);
            unsigned int framedatasize =
                (unsigned int)etohs(r->hdr.length) - 10u;

            if (sdolen > req->bufsize)
            {
               req_finish(joint, req, JSDK_SDO_ERROR);
            }
            else if (framedatasize < sdolen)
            {
               memcpy(req->buf, &r->b[4], framedatasize);
               req->seg_total = (int)sdolen;
               req->seg_done = (int)framedatasize;
               req->phase = 1;
               req->toggle = 0;
               req->stage = JSDK_REQ_ST_SEND; /* request segment 0 */
            }
            else
            {
               memcpy(req->buf, &r->b[4], sdolen);
               req->datasize = sdolen;
               req_finish(joint, req, JSDK_SDO_SUCCESS);
            }
         }
      }
      else
      {
         /* ---- reply to an upload segment request ---- */
         unsigned int framedatasize =
             (unsigned int)etohs(r->hdr.length) - 3u;

         if ((r->command & 0xe0) != 0x00)
         {
            if (r->command == ECT_SDO_ABORT)
            {
               req->abort_code = etohl(r->l[0]);
            }
            req_finish(joint, req, JSDK_SDO_ERROR);
            ecx_dropmbx(&ctx->soem, mbx);
            return 1;
         }
         if (r->command & 0x01)
         {
            if (framedatasize == 7u)
            {
               framedatasize -= (unsigned int)((r->command & 0x0e) >> 1);
            }
            if (req->seg_done + (int)framedatasize <= (int)req->bufsize)
            {
               memcpy(req->buf + req->seg_done, &r->index, framedatasize);
               req->seg_done += (int)framedatasize;
               req->datasize = (size_t)req->seg_total;
               req_finish(joint, req, JSDK_SDO_SUCCESS);
            }
            else
            {
               req_finish(joint, req, JSDK_SDO_ERROR);
            }
         }
         else
         {
            if (req->seg_done + (int)framedatasize <= (int)req->bufsize)
            {
               memcpy(req->buf + req->seg_done, &r->index, framedatasize);
               req->seg_done += (int)framedatasize;
               req->toggle ^= 0x10;
               req->stage = JSDK_REQ_ST_SEND; /* next segment request */
            }
            else
            {
               req_finish(joint, req, JSDK_SDO_ERROR);
            }
         }
      }
   }
   else /* WRITE */
   {
      if (req->phase == 0)
      {
         /* ---- reply to expedited or normal-init download ---- */
         if ((r->index != htoes(req->index)) ||
             (r->subindex != req->subindex))
         {
            if (r->command == ECT_SDO_ABORT)
            {
               req->abort_code = etohl(r->l[0]);
            }
            req_finish(joint, req, JSDK_SDO_ERROR);
            ecx_dropmbx(&ctx->soem, mbx);
            return 1;
         }
         if (req->bufsize <= 4)
         {
            req_finish(joint, req, JSDK_SDO_SUCCESS);
         }
         else if (req->not_last)
         {
            req->phase = 1;
            req->toggle = 0;
            req->stage = JSDK_REQ_ST_SEND; /* first download segment */
         }
         else
         {
            req_finish(joint, req, JSDK_SDO_SUCCESS);
         }
      }
      else
      {
         /* ---- reply to a download segment ---- */
         if ((r->command & 0xe0) != 0x20)
         {
            if (r->command == ECT_SDO_ABORT)
            {
               req->abort_code = etohl(r->l[0]);
            }
            req_finish(joint, req, JSDK_SDO_ERROR);
            ecx_dropmbx(&ctx->soem, mbx);
            return 1;
         }
         if (req->write_left <= 0)
         {
            req_finish(joint, req, JSDK_SDO_SUCCESS);
         }
         else
         {
            req->stage = JSDK_REQ_ST_SEND; /* next download segment */
         }
      }
   }
   ecx_dropmbx(&ctx->soem, mbx);
   return 1;
}

/* ==================================================================== */
/* per-joint poll (one step per cycle)                                  */
/* ==================================================================== */
static void
joint_sdo_poll(jsdk_context_t *ctx, jsdk_joint_t *joint)
{
   jsdk_sdo_req *req;

   if (!joint->mbx_cyclic)
   {
      req = req_pending(joint);
      if (req)
      {
         req_finish(joint, req, JSDK_SDO_ERROR);
      }
      return;
   }

   req = req_active(joint);
   if (!req)
   {
      req = req_pending(joint);
      if (req)
      {
         req_start(ctx, joint, req);
      }
      req = req_active(joint);
   }
   if (!req)
   {
      return;
   }

   if (req->stage == JSDK_REQ_ST_WAIT)
   {
      if (!req_consume(ctx, joint, req) && (++req->cycles > SDO_REQ_TIMEOUT_CYCLES))
      {
         req_finish(joint, req, JSDK_SDO_ERROR);
      }
   }
   else if (req->stage == JSDK_REQ_ST_SEND)
   {
      if (!req_send(ctx, joint, req) && (++req->cycles > SDO_REQ_TIMEOUT_CYCLES))
      {
         req_finish(joint, req, JSDK_SDO_ERROR);
      }
   }
}

/* ==================================================================== */
/* engine lifecycle                                                     */
/* ==================================================================== */
int
jsdk_sdo_engine_init(jsdk_context_t *ctx)
{
   unsigned int i;
   int any = 0;

   for (i = 0; i < ctx->joint_count; ++i)
   {
      jsdk_joint_t *j = ctx->joints[i];
      j->mbx_cyclic = ecx_slavembxcyclic(&ctx->soem, j->slave);
      if (j->mbx_cyclic)
      {
         any = 1;
      }
   }
   return any;
}

void
jsdk_sdo_engine_cycle(jsdk_context_t *ctx)
{
   unsigned int i;
   int mbx_any = 0;

   for (i = 0; i < ctx->joint_count; ++i)
   {
      jsdk_joint_t *j = ctx->joints[i];
      joint_sdo_poll(ctx, j);
      if (j->mbx_cyclic)
      {
         mbx_any = 1;
      }
   }
   /* Outbound is sent by direct FPWR, so this handler only collects the
    * inbound replies into slave->coembxin for the next cycle. */
   if (mbx_any)
   {
      (void)ecx_mbxhandler(&ctx->soem, 0, MBX_SERVICE_LIMIT);
   }
}

void
jsdk_sdo_engine_shutdown(jsdk_context_t *ctx)
{
   unsigned int i, k;

   for (i = 0; i < ctx->joint_count; ++i)
   {
      jsdk_joint_t *j = ctx->joints[i];
      for (k = 1; k < j->sdo_req_count; ++k)
      {
         free(j->sdo_reqs[k].buf);
         j->sdo_reqs[k].buf = NULL;
         j->sdo_reqs[k].used = 0;
      }
      j->sdo_reqs[0].buf = NULL;
      j->sdo_req_count = 0;
   }
}

/* ==================================================================== */
/* public ABI (async SDO)                                               */
/* ==================================================================== */
jsdk_sdo_handle_t
jsdk_joint_sdo_create(jsdk_joint_t *joint, uint16_t index, uint8_t subindex,
                      size_t size)
{
   jsdk_sdo_req *req;
   unsigned int i;

   if (!joint || !size)
   {
      return -1;
   }
   /* slot 0 is reserved for the fault auto-reader */
   for (i = 1; i < JSDK_MAX_SDO_REQUESTS; ++i)
   {
      if (i < joint->sdo_req_count && joint->sdo_reqs[i].used)
      {
         continue;
      }
      break;
   }
   if (i >= JSDK_MAX_SDO_REQUESTS)
   {
      return -1;
   }
   if (i >= joint->sdo_req_count)
   {
      joint->sdo_req_count = i + 1;
   }
   req = &joint->sdo_reqs[i];
   memset(req, 0, sizeof(*req));
   req->buf = (uint8_t *)malloc(size);
   if (!req->buf)
   {
      return -1;
   }
   req->index = index;
   req->subindex = subindex;
   req->bufsize = size;
   req->state = JSDK_SDO_IDLE;
   req->used = 1;
   return (jsdk_sdo_handle_t)i;
}

static jsdk_sdo_req *
sdo_req_get(jsdk_joint_t *joint, jsdk_sdo_handle_t handle)
{
   if (!joint || handle < 1 || (unsigned int)handle >= joint->sdo_req_count)
   {
      return NULL;
   }
   if (!joint->sdo_reqs[handle].used)
   {
      return NULL;
   }
   return &joint->sdo_reqs[handle];
}

jsdk_sdo_state_t
jsdk_joint_sdo_state(jsdk_joint_t *joint, jsdk_sdo_handle_t handle)
{
   jsdk_sdo_req *req = sdo_req_get(joint, handle);
   return req ? req->state : JSDK_SDO_ERROR;
}

uint8_t *
jsdk_joint_sdo_data(jsdk_joint_t *joint, jsdk_sdo_handle_t handle)
{
   jsdk_sdo_req *req = sdo_req_get(joint, handle);
   return req ? req->buf : NULL;
}

size_t
jsdk_joint_sdo_data_size(jsdk_joint_t *joint, jsdk_sdo_handle_t handle)
{
   jsdk_sdo_req *req = sdo_req_get(joint, handle);
   return req ? req->datasize : 0;
}

int
jsdk_joint_sdo_read(jsdk_joint_t *joint, jsdk_sdo_handle_t handle)
{
   jsdk_sdo_req *req = sdo_req_get(joint, handle);

   if (!req || !joint->mbx_cyclic || req->state == JSDK_SDO_BUSY)
   {
      return -1;
   }
   req->datasize = 0;
   req->op = JSDK_REQ_READ;
   req->state = JSDK_SDO_BUSY;
   return 0;
}

int
jsdk_joint_sdo_write(jsdk_joint_t *joint, jsdk_sdo_handle_t handle)
{
   jsdk_sdo_req *req = sdo_req_get(joint, handle);

   if (!req || !joint->mbx_cyclic || req->state == JSDK_SDO_BUSY)
   {
      return -1;
   }
   req->op = JSDK_REQ_WRITE;
   req->state = JSDK_SDO_BUSY;
   return 0;
}

jsdk_status_t
jsdk_joint_sdo_upload(jsdk_joint_t *joint, uint16_t index, uint8_t subindex,
                      void *data, int *size)
{
   int wkc;
   int sz;

   if (!joint || !joint->ctx || !data || !size || *size <= 0)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   sz = *size;
   wkc = ecx_SDOread(&joint->ctx->soem, joint->slave, index, subindex, FALSE,
                     &sz, data, EC_TIMEOUTRXM);
   if (wkc <= 0)
   {
      jsdk_set_error(joint->ctx, "SDO upload 0x%04X:%u failed (wkc=%d)",
                     index, subindex, wkc);
      return JSDK_ERR_ECRT;
   }
   *size = sz;
   return JSDK_OK;
}

jsdk_status_t
jsdk_joint_sdo_download(jsdk_joint_t *joint, uint16_t index, uint8_t subindex,
                        const void *data, int size)
{
   int wkc;

   if (!joint || !joint->ctx || !data || size <= 0)
   {
      return JSDK_ERR_INVALID_ARG;
   }
   wkc = ecx_SDOwrite(&joint->ctx->soem, joint->slave, index, subindex, FALSE,
                      size, (void *)data, EC_TIMEOUTRXM);
   if (wkc <= 0)
   {
      jsdk_set_error(joint->ctx, "SDO download 0x%04X:%u failed (wkc=%d)",
                     index, subindex, wkc);
      return JSDK_ERR_ECRT;
   }
   return JSDK_OK;
}

/* ==================================================================== */
/* internal fault auto-read (slot 0)                                    */
/* ==================================================================== */
int
jsdk_sdo_fault_read_start(jsdk_joint_t *joint, uint16_t index, uint8_t subindex,
                          uint8_t *dst, size_t n)
{
   jsdk_sdo_req *req;

   if (!joint->mbx_cyclic)
   {
      return -1;
   }
   if (joint->sdo_req_count == 0)
   {
      joint->sdo_req_count = 1;
   }
   req = &joint->sdo_reqs[0];
   if (req->state == JSDK_SDO_BUSY)
   {
      return -1;
   }
   if (!req->used)
   {
      memset(req, 0, sizeof(*req));
      req->used = 1;
      req->state = JSDK_SDO_IDLE;
   }
   req->index = index;
   req->subindex = subindex;
   req->buf = dst;
   req->bufsize = n;
   req->datasize = 0;
   req->op = JSDK_REQ_READ;
   req->state = JSDK_SDO_BUSY;
   return 0;
}

jsdk_sdo_state_t
jsdk_sdo_fault_read_state(jsdk_joint_t *joint)
{
   return joint->sdo_reqs[0].state;
}

size_t
jsdk_sdo_fault_read_size(jsdk_joint_t *joint)
{
   return joint->sdo_reqs[0].datasize;
}

/* ==================================================================== */
/* fault diagnosis closed loop                                          */
/* -------------------------------------------------------------------- */
/* Called every cycle (from cycle_end, after the engine). When Statusword
 * reports FAULT it reads 0x603F / 0x1001 / 0x203F / 0x203E through the
 * engine (slot 0), packs them into fault_info and fires the callback once
 * per fault episode. */
static void
joint_fault_service(jsdk_context_t *ctx, jsdk_joint_t *joint)
{
   int fault = (joint->cia402.axis_state == JSDK_AXIS_FAULT);

   if (!fault)
   {
      joint->fault_seq = JSDK_FAULT_IDLE;
      joint->fault_notified = 0;
      return;
   }

   switch (joint->fault_seq)
   {
   case JSDK_FAULT_IDLE:
      if (joint->fault_notified)
      {
         break; /* already reported this episode */
      }
      memset(&joint->fault_info, 0, sizeof(joint->fault_info));
      joint->fault_seq = JSDK_FAULT_RD_603F;
      jsdk_sdo_fault_read_start(joint, FAULT_CODE_603F, 0, joint->fault_buf, 2);
      break;

   case JSDK_FAULT_RD_603F:
      if (jsdk_sdo_fault_read_state(joint) == JSDK_SDO_SUCCESS)
      {
         joint->fault_info.code_603f = (uint16_t)joint->fault_buf[0] |
                                       ((uint16_t)joint->fault_buf[1] << 8);
         joint->fault_seq = JSDK_FAULT_RD_1001;
         jsdk_sdo_fault_read_start(joint, FAULT_REG_1001, 0, joint->fault_buf, 1);
      }
      else if (jsdk_sdo_fault_read_state(joint) == JSDK_SDO_ERROR)
      {
         joint->fault_seq = JSDK_FAULT_RD_1001;
         jsdk_sdo_fault_read_start(joint, FAULT_REG_1001, 0, joint->fault_buf, 1);
      }
      break;

   case JSDK_FAULT_RD_1001:
      if (jsdk_sdo_fault_read_state(joint) == JSDK_SDO_SUCCESS)
      {
         joint->fault_info.error_register = joint->fault_buf[0];
         joint->fault_seq = JSDK_FAULT_RD_203F;
         jsdk_sdo_fault_read_start(joint, FAULT_VENDOR_LO, 0, joint->fault_buf, 4);
      }
      else if (jsdk_sdo_fault_read_state(joint) == JSDK_SDO_ERROR)
      {
         joint->fault_seq = JSDK_FAULT_RD_203F;
         jsdk_sdo_fault_read_start(joint, FAULT_VENDOR_LO, 0, joint->fault_buf, 4);
      }
      break;

   case JSDK_FAULT_RD_203F:
      if (jsdk_sdo_fault_read_state(joint) == JSDK_SDO_SUCCESS)
      {
         uint32_t v = (uint32_t)joint->fault_buf[0] |
                      ((uint32_t)joint->fault_buf[1] << 8) |
                      ((uint32_t)joint->fault_buf[2] << 16) |
                      ((uint32_t)joint->fault_buf[3] << 24);
         joint->fault_info.vendor_lo = v;
         joint->fault_seq = JSDK_FAULT_RD_203E;
         jsdk_sdo_fault_read_start(joint, FAULT_VENDOR_HI, 0, joint->fault_buf, 4);
      }
      else if (jsdk_sdo_fault_read_state(joint) == JSDK_SDO_ERROR)
      {
         joint->fault_seq = JSDK_FAULT_RD_203E;
         jsdk_sdo_fault_read_start(joint, FAULT_VENDOR_HI, 0, joint->fault_buf, 4);
      }
      break;

   case JSDK_FAULT_RD_203E:
      if (jsdk_sdo_fault_read_state(joint) == JSDK_SDO_SUCCESS)
      {
         uint32_t v = (uint32_t)joint->fault_buf[0] |
                      ((uint32_t)joint->fault_buf[1] << 8) |
                      ((uint32_t)joint->fault_buf[2] << 16) |
                      ((uint32_t)joint->fault_buf[3] << 24);
         joint->fault_info.vendor_hi = v;
      }
      joint->fault_info.valid = 1;
      joint->fault_seq = JSDK_FAULT_DONE;
      if (!joint->fault_notified && ctx->fault_cb)
      {
         joint->fault_notified = 1;
         ctx->fault_cb(joint, &joint->fault_info, ctx->fault_cb_data);
      }
      break;

   case JSDK_FAULT_DONE:
   default:
      break;
   }
}

void
jsdk_fault_service(jsdk_context_t *ctx)
{
   unsigned int i;

   for (i = 0; i < ctx->joint_count; ++i)
   {
      joint_fault_service(ctx, ctx->joints[i]);
   }
}
