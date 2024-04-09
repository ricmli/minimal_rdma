#include <asm-generic/errno.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "frame_common.h"

enum frame_status {
  FRAME_FREE = 0,
  FRAME_IN_WRITING,
  FRAME_DONE_WRITING,
};

struct frame {
  int idx;
  void *addr;
  size_t size;
  enum frame_status status;
};

struct ctx {
  struct rdma_event_channel *ec;

  struct rdma_cm_id *cma_id;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_qp *qp;

  struct ibv_mr *frame_mr;
  struct ibv_mr *msg_mr;
  struct msg *msg;
  char *frame_region;
  int connected;
};

int main(int argc, char **argv) {
  int ret = 0;
  int sent = 0;
  struct ctx *ctx = calloc(1, sizeof *ctx);
  ctx->ec = rdma_create_event_channel();
  if (!ctx->ec) {
    fprintf(stderr, "rdma_create_event_channel failed\n");
    goto out;
  }

  ret = rdma_create_id(ctx->ec, &ctx->cma_id, ctx, RDMA_PS_TCP);
  if (ret) {
    fprintf(stderr, "rdma_create_id failed\n");
    goto out;
  }

  struct rdma_addrinfo hints = {};
  struct rdma_addrinfo *res, *rai;
  hints.ai_port_space = RDMA_PS_TCP;
  hints.ai_flags = RAI_PASSIVE;
  ret = rdma_getaddrinfo("192.168.97.2", NULL, &hints, &res);
  if (ret) {
    fprintf(stderr, "rdma_getaddrinfo failed\n");
    goto out;
  }
  hints.ai_src_addr = res->ai_src_addr;
  hints.ai_src_len = res->ai_src_len;
  hints.ai_flags &= ~RAI_PASSIVE;
  ret = rdma_getaddrinfo("192.168.97.111", "20086", &hints, &rai);
  rdma_freeaddrinfo(res);
  if (ret) {
    fprintf(stderr, "rdma_getaddrinfo failed\n");
    goto out;
  }

  /* connect to server */
  ret =
      rdma_resolve_addr(ctx->cma_id, rai->ai_src_addr, rai->ai_dst_addr, 2000);
  if (ret) {
    fprintf(stderr, "rdma_resolve_addr failed\n");
    goto out;
  }

  struct rdma_cm_event *event;
  while (!ret && !ctx->connected) {
    ret = rdma_get_cm_event(ctx->ec, &event);
    if (!ret) {
      switch (event->event) {
      case RDMA_CM_EVENT_ADDR_RESOLVED:
        printf("address resolved\n");
        ret = rdma_resolve_route(ctx->cma_id, 2000);
        if (ret) {
          fprintf(stderr, "rdma_resolve_route failed\n");
          goto out;
        }
        break;
      case RDMA_CM_EVENT_ROUTE_RESOLVED:
        printf("route resolved\n");
        struct rdma_conn_param conn_param = {};
        ctx->pd = ibv_alloc_pd(event->id->verbs);
        if (!ctx->pd) {
          ret = -ENOMEM;
          fprintf(stderr, "ibv_alloc_pd failed\n");
          goto out;
        }

        ctx->cq = ibv_create_cq(event->id->verbs, 10, ctx, NULL, 0);
        if (!ctx->cq) {
          fprintf(stderr, "ibv_create_cq failed\n");
          goto out;
        }

        struct ibv_qp_init_attr init_qp_attr = {};
        init_qp_attr.cap.max_send_wr = 10;
        init_qp_attr.cap.max_recv_wr = 10;
        init_qp_attr.cap.max_send_sge = 1;
        init_qp_attr.cap.max_recv_sge = 1;
        init_qp_attr.send_cq = ctx->cq;
        init_qp_attr.recv_cq = ctx->cq;
        init_qp_attr.qp_type = IBV_QPT_RC;
        ret = rdma_create_qp(event->id, ctx->pd, &init_qp_attr);
        if (ret) {
          fprintf(stderr, "ibv_create_qp failed\n");
          goto out;
        }
        ctx->qp = event->id->qp;

        ctx->frame_region = malloc(FRAME_BUFFER_SIZE);
        if (!ctx->frame_region) {
          fprintf(stderr, "malloc failed\n");
          goto out;
        }

        ctx->frame_mr =
            ibv_reg_mr(ctx->pd, ctx->frame_region, FRAME_BUFFER_SIZE, 0);
        if (!ctx->frame_mr) {
          fprintf(stderr, "ibv_reg_mr failed\n");
          goto out;
        }

        ctx->msg = malloc(sizeof(*ctx->msg));
        if (!ctx->msg) {
          fprintf(stderr, "malloc msg failed\n");
          goto out;
        }

        ctx->msg_mr = ibv_reg_mr(ctx->pd, ctx->msg, sizeof(*ctx->msg),
                                 IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->msg_mr) {
          fprintf(stderr, "ibv_reg_mr msg failed\n");
          goto out;
        }

        conn_param.initiator_depth = conn_param.responder_resources = 1;
        conn_param.rnr_retry_count = 7; /* infinite retry */
        printf("connecting\n");
        ret = rdma_connect(event->id, &conn_param);
        if (ret) {
          fprintf(stderr, "rdma_connect failed\n");
          goto out;
        }
        break;
      case RDMA_CM_EVENT_ESTABLISHED:
        ctx->connected = 1;
        break;
      case RDMA_CM_EVENT_ADDR_ERROR:
      case RDMA_CM_EVENT_ROUTE_ERROR:
      case RDMA_CM_EVENT_CONNECT_ERROR:
      case RDMA_CM_EVENT_UNREACHABLE:
      case RDMA_CM_EVENT_REJECTED:
        printf("event: %s, error: %d\n", rdma_event_str(event->event),
               event->status);
        ret = event->status;
        break;
      default:
        break;
      }
      rdma_ack_cm_event(event);
    }
  }

  /* wait for a ready message */
  rdma_post_recv(ctx->cma_id, NULL, ctx->msg, sizeof(*ctx->msg), ctx->msg_mr);

  /* poll cq */
  struct ibv_wc wc;
  for (;;) {
    do {
      ret = ibv_poll_cq(ctx->cq, 1, &wc);
    } while (ret == 0);
    if (ret < 0) {
      fprintf(stderr, "ibv_poll_cq failed\n");
      goto out;
    }
    if (wc.status != IBV_WC_SUCCESS) {
      fprintf(stderr, "Work completion error: %s\n",
              ibv_wc_status_str(wc.status));
      /* check more info */
      fprintf(stderr, "wc.vendor_error = 0x%x, wc.qp_num = %u\n", wc.vendor_err,
              wc.qp_num);
    }
    if (wc.opcode == IBV_WC_RECV) {
      /* check msg */
      if (ctx->msg->id == MSG_RX_FRAME_READY) {
        /* write the frame */
        struct ibv_send_wr wr = {}, *bad_wr = NULL;
        struct ibv_sge sge;

        wr.wr_id = ctx->msg->data.frame.addr;
        wr.opcode = IBV_WR_RDMA_WRITE;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = ctx->msg->data.frame.addr;
        wr.wr.rdma.rkey = ctx->msg->data.frame.rkey;
        sge.addr = (uintptr_t)ctx->frame_region;
        sge.length = FRAME_BUFFER_SIZE;
        sge.lkey = ctx->frame_mr->lkey;
        ibv_post_send(ctx->qp, &wr, &bad_wr);
      }
    }
    if (wc.opcode == IBV_WC_RDMA_WRITE) {
      /* write done, send done msg */
      ctx->msg->id = MSG_TX_FRAME_DONE;
      ctx->msg->data.frame.addr = wc.wr_id;
      rdma_post_send(ctx->cma_id, NULL, ctx->msg, sizeof(*ctx->msg),
                     ctx->msg_mr, 0);
      sent++;
      printf("sent %d\n", sent);
      /* send done, wait next ready in advance */
      rdma_post_recv(ctx->cma_id, NULL, ctx->msg, sizeof(*ctx->msg),
                     ctx->msg_mr);
    }
    if (wc.opcode == IBV_WC_SEND) {
      /* send done succ */
    }
  }

out:
  if (ctx->msg_mr)
    ibv_dereg_mr(ctx->msg_mr);
  if (ctx->msg)
    free(ctx->msg);
  if (ctx->frame_mr)
    ibv_dereg_mr(ctx->frame_mr);
  if (ctx->frame_region)
    free(ctx->frame_region);
  if (ctx->cma_id && ctx->cma_id->qp)
    rdma_destroy_qp(ctx->cma_id);
  if (ctx->pd)
    ibv_dealloc_pd(ctx->pd);
  if (rai)
    rdma_freeaddrinfo(rai);
  if (ctx->cma_id)
    rdma_destroy_id(ctx->cma_id);
  if (ctx->ec)
    rdma_destroy_event_channel(ctx->ec);

  free(ctx);
  return ret;
}