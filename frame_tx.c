#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

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
  struct ibv_comp_channel *cc;

  struct ibv_mr *frame_mr;
  struct ibv_mr *msg_mr;
  struct msg *msg;
  char *frame_region;
  size_t region_size;
  int connected;

  int num_frames;

  uint64_t request_elapsed_min;
  uint64_t request_elapsed_max;
  uint64_t request_elapsed_sum;
  uint64_t request_elapsed_cnt;
};

int main(int argc, char **argv) {
  Args parsed_args = parse_args(argc, argv);
  int ret = 0;
  int sent = 0;
  struct ctx *ctx = calloc(1, sizeof *ctx);
  ctx->num_frames = parsed_args.number_of_frames;
  ctx->request_elapsed_min = UINT64_MAX;
  ctx->request_elapsed_max = 0;
  ctx->request_elapsed_sum = 0;
  ctx->request_elapsed_cnt = 0;

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
  ret = rdma_getaddrinfo(parsed_args.local_ip_address, NULL, &hints, &res);
  if (ret) {
    fprintf(stderr, "rdma_getaddrinfo failed\n");
    goto out;
  }
  hints.ai_src_addr = res->ai_src_addr;
  hints.ai_src_len = res->ai_src_len;
  hints.ai_flags &= ~RAI_PASSIVE;
  ret = rdma_getaddrinfo(parsed_args.remote_ip_address, parsed_args.port_number,
                         &hints, &rai);
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

        ctx->pd = ibv_alloc_pd(event->id->verbs);
        if (!ctx->pd) {
          ret = -ENOMEM;
          fprintf(stderr, "ibv_alloc_pd failed\n");
          goto out;
        }

        if (parsed_args.event) {
          ctx->cc = ibv_create_comp_channel(event->id->verbs);
          if (!ctx->cc) {
            ret = -EIO;
            fprintf(stderr, "ibv_create_comp_channel failed\n");
            goto out;
          }
        }

        ctx->cq = ibv_create_cq(event->id->verbs, 10, ctx, ctx->cc, 0);
        if (!ctx->cq) {
          fprintf(stderr, "ibv_create_cq failed\n");
          goto out;
        }

        if (parsed_args.event) {
          ret = ibv_req_notify_cq(ctx->cq, 0);
          if (ret) {
            fprintf(stderr, "ibv_req_notify_cq failed\n");
            goto out;
          }
        }

        struct ibv_qp_init_attr init_qp_attr = {
            .cap.max_send_wr = 10,
            .cap.max_recv_wr = 10,
            .cap.max_send_sge = 1,
            .cap.max_recv_sge = 1,
            .cap.max_inline_data = sizeof(struct msg),
            .send_cq = ctx->cq,
            .recv_cq = ctx->cq,
            .qp_type = IBV_QPT_RC,
        };
        ret = rdma_create_qp(event->id, ctx->pd, &init_qp_attr);
        if (ret) {
          fprintf(stderr, "ibv_create_qp failed\n");
          goto out;
        }
        ctx->qp = event->id->qp;

        ctx->region_size = parsed_args.frame_width * parsed_args.frame_height *
                           (parsed_args.bit_depth == 8 ? 2 : 4);

        ctx->frame_region =
            mmap(NULL, ctx->region_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (!ctx->frame_region) {
          fprintf(stderr, "mmap failed\n");
          goto out;
        }

        ctx->frame_mr =
            ibv_reg_mr(ctx->pd, ctx->frame_region, ctx->region_size, 0);
        if (!ctx->frame_mr) {
          fprintf(stderr, "ibv_reg_mr failed\n");
          goto out;
        }

        ctx->msg = calloc(1, sizeof(*ctx->msg));
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

        struct rdma_conn_param conn_param = {
            .initiator_depth = 1,
            .responder_resources = 1,
            .rnr_retry_count = 7 /* infinite retry */,
        };
        printf("connecting\n");
        ret = rdma_connect(event->id, &conn_param);
        if (ret) {
          fprintf(stderr, "rdma_connect failed\n");
          goto out;
        }
        break;
      case RDMA_CM_EVENT_ESTABLISHED:
        ctx->connected = 1;
        printf("connected\n");
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

  /* wait for a message */
  rdma_post_recv(ctx->cma_id, NULL, ctx->msg, sizeof(*ctx->msg), ctx->msg_mr);

  struct msg rtt_msg = {
      .id = MSG_TX_MEASURE_RTT_START,
  };
  rdma_post_send(ctx->cma_id, NULL, &rtt_msg, sizeof(rtt_msg), NULL,
                 IBV_SEND_INLINE);

  /* poll cq */
  struct ibv_wc wc;
  for (;;) {
    if (ctx->cc) {
      struct ibv_cq *cq;
      void *cq_ctx = NULL;
      ret = ibv_get_cq_event(ctx->cc, &cq, &cq_ctx);
      if (ret) {
        fprintf(stderr, "ibv_get_cq_event failed\n");
        goto out;
      }
      ibv_ack_cq_events(cq, 1);
      ret = ibv_req_notify_cq(cq, 0);
      if (ret) {
        fprintf(stderr, "ibv_req_notify_cq failed\n");
        goto out;
      }
    }
    while (ibv_poll_cq(ctx->cq, 1, &wc)) {
      if (wc.status != IBV_WC_SUCCESS) {
        fprintf(stderr, "Work completion error: %s\n",
                ibv_wc_status_str(wc.status));
        /* check more info */
        fprintf(stderr, "wc.vendor_error = 0x%x, wc.qp_num = %u\n",
                wc.vendor_err, wc.qp_num);
        goto out;
      }
      if (wc.opcode == IBV_WC_RECV) {
        if (ctx->msg->id == MSG_RX_MEASURE_RTT) {
          struct msg rtt_msg = {
              .id = MSG_TX_MEASURE_RTT_DONE,
              .data.measure_rtt.timestamp =
                  ctx->msg->data.measure_rtt.timestamp,
          };
          rdma_post_send(ctx->cma_id, NULL, &rtt_msg, sizeof(rtt_msg), NULL,
                         IBV_SEND_INLINE);
          printf("Start sending:\n");
        } else if (ctx->msg->id == MSG_RX_FRAME_READY) {
          /* write the frame */
          rdma_post_write(ctx->cma_id, (void *)ctx->msg->data.frame.addr,
                          ctx->frame_region, ctx->region_size, ctx->frame_mr,
                          IBV_SEND_SIGNALED, ctx->msg->data.frame.addr,
                          ctx->msg->data.frame.rkey);
        }
        /* wait for next message */
        rdma_post_recv(ctx->cma_id, NULL, ctx->msg, sizeof(*ctx->msg),
                       ctx->msg_mr);
      } else if (wc.opcode == IBV_WC_RDMA_WRITE) {
        /* write done, send done msg */
        struct msg td_msg = {
            .id = MSG_TX_FRAME_DONE,
            .data.frame.addr = wc.wr_id,
            .data.frame.timestamp = ctx->msg->data.frame.timestamp,
        };
        rdma_post_send(ctx->cma_id, NULL, &td_msg, sizeof(td_msg), NULL,
                       IBV_SEND_INLINE);
        sent++;
        printf(".");
        fflush(stdout);
        if (sent >= ctx->num_frames)
          goto out;
      }
    }
  }

out:
  printf("\nsent %d\n", sent);
  if (ctx->msg_mr)
    ibv_dereg_mr(ctx->msg_mr);
  if (ctx->msg)
    free(ctx->msg);
  if (ctx->frame_mr)
    ibv_dereg_mr(ctx->frame_mr);
  if (ctx->frame_region)
    munmap(ctx->frame_region, ctx->region_size);
  if (ctx->cma_id && ctx->cma_id->qp)
    rdma_destroy_qp(ctx->cma_id);
  if (ctx->cq)
    ibv_destroy_cq(ctx->cq);
  if (ctx->cc)
    ibv_destroy_comp_channel(ctx->cc);
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