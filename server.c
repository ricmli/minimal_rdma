#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdio.h>
#include <stdlib.h>

const int BUFFER_SIZE = 100;

struct ctx {
  struct rdma_event_channel *ec;

  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_qp *qp;

  struct ibv_mr *recv_mr;

  char *recv_region;
};

int main(int argc, char **argv) {
  int ret = 0;
  struct ctx *ctx = calloc(1, sizeof *ctx);
  ctx->ec = rdma_create_event_channel();
  if (!ctx->ec) {
    fprintf(stderr, "rdma_create_event_channel failed\n");
    goto out;
  }

  struct rdma_cm_id *listen_id = NULL;
  ret = rdma_create_id(ctx->ec, &listen_id, ctx, RDMA_PS_UDP);
  if (ret) {
    fprintf(stderr, "rdma_create_id failed\n");
    goto out;
  }

  struct rdma_addrinfo hints = {};
  hints.ai_port_space = RDMA_PS_UDP;
  hints.ai_flags = RAI_PASSIVE;
  struct rdma_addrinfo *rai;
  ret = rdma_getaddrinfo("192.168.97.111", "10086", &hints, &rai);
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
        init_qp_attr.cap.max_send_wr = 1;
        init_qp_attr.cap.max_recv_wr = 1;
        init_qp_attr.cap.max_send_sge = 1;
        init_qp_attr.cap.max_recv_sge = 1;
        init_qp_attr.qp_context = ctx;
        init_qp_attr.send_cq = ctx->cq;
        init_qp_attr.recv_cq = ctx->cq;
        init_qp_attr.qp_type = IBV_QPT_UD;
        init_qp_attr.sq_sig_all = 0;
        ret = rdma_create_qp(event->id, ctx->pd, &init_qp_attr);
        if (ret) {
          fprintf(stderr, "ibv_create_qp failed\n");
          goto out;
        }
        ctx->qp = event->id->qp;

        ctx->recv_region = malloc(BUFFER_SIZE + sizeof(struct ibv_grh));
        if (!ctx->recv_region) {
          fprintf(stderr, "malloc failed\n");
          goto out;
        }

        ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_region,
                                  BUFFER_SIZE + sizeof(struct ibv_grh),
                                  IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->recv_mr) {
          fprintf(stderr, "ibv_reg_mr failed\n");
          goto out;
        }

        ret =
            rdma_post_recv(event->id, ctx, ctx->recv_region,
                           BUFFER_SIZE + sizeof(struct ibv_grh), ctx->recv_mr);
        if (ret) {
          fprintf(stderr, "rdma_post_recv failed\n");
          goto out;
        }

        struct rdma_conn_param conn_param = {};
        conn_param.qp_num = event->id->qp->qp_num;
        ret = rdma_accept(event->id, &conn_param);
        if (ret) {
          fprintf(stderr, "rdma_accept failed\n");
          goto out;
        }

        printf("Accepted, start receiving...\n");

        /* poll cq */
        struct ibv_wc wc;
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
          /* read the buffer */
          char *buf = (char *)ctx->recv_region + sizeof(struct ibv_grh);
          printf("Received: %s\n", buf);
          goto out;
        }
      }
      rdma_ack_cm_event(event);
    }
  }

out:
  rdma_ack_cm_event(event);
  if (ctx->recv_mr)
    ibv_dereg_mr(ctx->recv_mr);
  if (ctx->recv_region)
    free(ctx->recv_region);
  if (ctx->qp)
    ibv_destroy_qp(ctx->qp);
  if (ctx->pd)
    ibv_dealloc_pd(ctx->pd);
  if (rai)
    rdma_freeaddrinfo(rai);
  if (listen_id)
    rdma_destroy_id(listen_id);
  if (ctx->ec)
    rdma_destroy_event_channel(ctx->ec);
  return ret;
}