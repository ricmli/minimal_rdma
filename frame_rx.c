
#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

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
  size_t region_size;

  struct frame frames[2];
  int recv_count;

  int num_frames;

  uint64_t round_latency_min;
  uint64_t round_latency_max;
  uint64_t round_latency_sum;
  uint64_t round_latency_cnt;
};

static int post_frame_recv(struct ctx *ctx) {
  /* find a free frame */
  struct frame *f = NULL;
  for (int i = 0; i < FRAME_BUFFER_CNT; i++) {
    if (ctx->frames[i].status == FRAME_FREE)
      f = &ctx->frames[i];
  }
  if (!f) {
    return -1;
  }
  struct timespec tp = {};
  clock_gettime(CLOCK_REALTIME, &tp);
  /* send frame_ready to tx */
  struct msg rd_msg = {};
  rd_msg.id = MSG_RX_FRAME_READY;
  rd_msg.data.frame.addr = (uint64_t)f->addr;
  rd_msg.data.frame.rkey = ctx->frame_mr->rkey;
  rd_msg.data.frame.timestamp = tp.tv_sec * 1e9 + tp.tv_nsec;
  rdma_post_send(ctx->cma_id, f, &rd_msg, sizeof(rd_msg), NULL,
                 IBV_SEND_INLINE);

  /* post recv frame_done from tx */
  rdma_post_recv(ctx->cma_id, f, ctx->msg, sizeof(*ctx->msg), ctx->msg_mr);
  printf("rev next frame %d\n", ctx->recv_count);
  return 0;
}

int main(int argc, char **argv) {
  Args parsed_args = parse_args(argc, argv);
  int ret = 0;
  struct ctx *ctx = calloc(1, sizeof *ctx);
  ctx->num_frames = parsed_args.number_of_frames;
  ctx->round_latency_min = UINT64_MAX;
  ctx->round_latency_max = 0;
  ctx->round_latency_sum = 0;
  ctx->round_latency_cnt = 0;

  ctx->ec = rdma_create_event_channel();
  if (!ctx->ec) {
    fprintf(stderr, "rdma_create_event_channel failed\n");
    goto out;
  }

  struct rdma_cm_id *listen_id = NULL;
  ret = rdma_create_id(ctx->ec, &listen_id, ctx, RDMA_PS_TCP);
  if (ret) {
    fprintf(stderr, "rdma_create_id failed\n");
    goto out;
  }

  struct rdma_addrinfo hints = {};
  hints.ai_port_space = RDMA_PS_TCP;
  hints.ai_flags = RAI_PASSIVE;
  struct rdma_addrinfo *rai;
  ret = rdma_getaddrinfo(parsed_args.local_ip_address, parsed_args.port_number,
                         &hints, &rai);
  if (ret) {
    fprintf(stderr, "rdma_getaddrinfo failed\n");
    goto out;
  }

  ret = rdma_bind_addr(listen_id, rai->ai_src_addr);
  if (ret) {
    fprintf(stderr, "rdma_bind_addr failed\n");
    goto out;
  }

  ret = rdma_listen(listen_id, 0);
  if (ret) {
    fprintf(stderr, "rdma_listen failed\n");
    goto out;
  }

  printf("Listening...\n");

  struct rdma_cm_event *event;
  while (!ret) {
    ret = rdma_get_cm_event(ctx->ec, &event);
    if (!ret) {
      // handle connect
      if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
        printf("got connect request\n");
        // init queues
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
        init_qp_attr.cap.max_inline_data = sizeof(struct msg);
        init_qp_attr.send_cq = ctx->cq;
        init_qp_attr.recv_cq = ctx->cq;
        init_qp_attr.qp_type = IBV_QPT_RC;
        ret = rdma_create_qp(event->id, ctx->pd, &init_qp_attr);
        if (ret) {
          fprintf(stderr, "ibv_create_qp failed\n");
          goto out;
        }
        ctx->qp = event->id->qp;

        size_t frame_size = parsed_args.frame_width * parsed_args.frame_height *
                            (parsed_args.bit_depth == 8 ? 2 : 4);

        ctx->region_size = FRAME_BUFFER_CNT * frame_size;
        ctx->frame_region =
            mmap(NULL, ctx->region_size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
        if (!ctx->frame_region) {
          fprintf(stderr, "mmap failed\n");
          goto out;
        }

        for (int i = 0; i < FRAME_BUFFER_CNT; i++) {
          struct frame f = {
              .idx = i,
              .addr = ctx->frame_region + i * frame_size,
              .size = frame_size,
              .status = FRAME_FREE,
          };
          ctx->frames[i] = f;
        }

        ctx->frame_mr =
            ibv_reg_mr(ctx->pd, ctx->frame_region, ctx->region_size,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
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

        struct rdma_conn_param conn_param = {};
        conn_param.initiator_depth = conn_param.responder_resources = 1;
        conn_param.rnr_retry_count = 7; /* infinite retry */
        ret = rdma_accept(event->id, &conn_param);
        if (ret) {
          fprintf(stderr, "rdma_accept failed\n");
          goto out;
        }
        ctx->cma_id = event->id;

        printf("Accepted, start receiving...\n");

        post_frame_recv(ctx);

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
            fprintf(stderr, "wc.vendor_error = 0x%x, wc.qp_num = %u\n",
                    wc.vendor_err, wc.qp_num);
          }
          if (wc.opcode == IBV_WC_RECV) {
            /* check msg */
            if (ctx->msg->id == MSG_TX_FRAME_DONE) {
              /* done */
              struct timespec tp = {};
              clock_gettime(CLOCK_REALTIME, &tp);
              uint64_t now = tp.tv_sec * 1e9 + tp.tv_nsec;
              uint64_t request_time = ctx->msg->data.frame.timestamp;
              uint64_t round_latency = now - request_time;
              if (ctx->recv_count > 0 && round_latency < 1e9 /* 1s */) {
                ctx->round_latency_min = round_latency < ctx->round_latency_min
                                             ? round_latency
                                             : ctx->round_latency_min;
                ctx->round_latency_max = round_latency > ctx->round_latency_max
                                             ? round_latency
                                             : ctx->round_latency_max;
                ctx->round_latency_sum += round_latency;
                ctx->round_latency_cnt++;
                printf("frame %d, round latency %lu ns\n", ctx->recv_count,
                       round_latency);
              }
              struct frame *f = (struct frame *)wc.wr_id;
              f->status = FRAME_DONE_WRITING;
              ctx->recv_count++;

              /* pretend to use it */
              usleep(200000);
              f->status = FRAME_FREE;
              if (ctx->recv_count == ctx->num_frames)
                goto out;
              post_frame_recv(ctx);
            }
          }
        }
      }
      rdma_ack_cm_event(event);
    }
  }

out:
  /* print statistics */
  printf("round_latency: avg %lu, min %lu, max %lu\n",
         ctx->round_latency_sum / ctx->round_latency_cnt,
         ctx->round_latency_min, ctx->round_latency_max);

  rdma_ack_cm_event(event);
  if (ctx->msg_mr)
    ibv_dereg_mr(ctx->msg_mr);
  if (ctx->msg)
    free(ctx->msg);
  if (ctx->frame_mr)
    ibv_dereg_mr(ctx->frame_mr);
  if (ctx->frame_region)
    munmap(ctx->frame_region, ctx->region_size);
  if (ctx->qp)
    ibv_destroy_qp(ctx->qp);
  if (ctx->cq)
    ibv_destroy_cq(ctx->cq);
  if (ctx->pd)
    ibv_dealloc_pd(ctx->pd);
  if (rai)
    rdma_freeaddrinfo(rai);
  if (listen_id)
    rdma_destroy_id(listen_id);
  if (ctx->ec)
    rdma_destroy_event_channel(ctx->ec);

  free(ctx);
  return ret;
}