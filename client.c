#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <stdio.h>
#include <stdlib.h>

const int BUFFER_SIZE = 2048 * 2048;
const int MSG_SIZE = 128;

struct ctx {
  struct rdma_event_channel *ec;
  struct rdma_cm_id *cma_id;

  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_ah *ah;
  uint32_t remote_qpn;
  uint32_t remote_qkey;

  struct ibv_mr *send_mr;

  char *send_region;
  int connected;
};

int main(int argc, char **argv) {
  int ret = 0;
  struct ctx *ctx = calloc(1, sizeof *ctx);
  ctx->ec = rdma_create_event_channel();
  if (!ctx->ec) {
    fprintf(stderr, "rdma_create_event_channel failed\n");
    goto out;
  }

  ret = rdma_create_id(ctx->ec, &ctx->cma_id, ctx, RDMA_PS_UDP);
  if (ret) {
    fprintf(stderr, "rdma_create_id failed\n");
    goto out;
  }

  struct rdma_addrinfo hints = {};
  struct rdma_addrinfo *res, *rai;
  hints.ai_port_space = RDMA_PS_UDP;
  hints.ai_flags = RAI_PASSIVE;
  ret = rdma_getaddrinfo("192.168.97.2", NULL, &hints, &res);
  if (ret) {
    fprintf(stderr, "rdma_getaddrinfo failed\n");
    goto out;
  }
  hints.ai_src_addr = res->ai_src_addr;
  hints.ai_src_len = res->ai_src_len;
  hints.ai_flags &= ~RAI_PASSIVE;
  ret = rdma_getaddrinfo("192.168.97.111", "10086", &hints, &rai);
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
        ctx->pd = ibv_alloc_pd(ctx->cma_id->verbs);
        if (!ctx->pd) {
          ret = -ENOMEM;
          fprintf(stderr, "ibv_alloc_pd failed\n");
          goto out;
        }

        ctx->cq = ibv_create_cq(ctx->cma_id->verbs, 10, ctx, NULL, 0);
        if (!ctx->cq) {
          fprintf(stderr, "ibv_create_cq failed\n");
          goto out;
        }

        struct ibv_qp_init_attr init_qp_attr = {};
        init_qp_attr.cap.max_send_wr = 3;
        init_qp_attr.cap.max_recv_wr = 3;
        init_qp_attr.cap.max_send_sge = 1;
        init_qp_attr.cap.max_recv_sge = 1;
        init_qp_attr.qp_context = ctx;
        init_qp_attr.send_cq = ctx->cq;
        init_qp_attr.recv_cq = ctx->cq;
        init_qp_attr.qp_type = IBV_QPT_UD;
        init_qp_attr.sq_sig_all = 0;
        ret = rdma_create_qp(ctx->cma_id, ctx->pd, &init_qp_attr);
        if (ret) {
          fprintf(stderr, "ibv_create_qp failed\n");
          goto out;
        }

        ctx->send_region = malloc(BUFFER_SIZE);
        if (!ctx->send_region) {
          fprintf(stderr, "malloc failed\n");
          goto out;
        }

        ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_region, BUFFER_SIZE,
                                  IBV_ACCESS_LOCAL_WRITE);
        if (!ctx->send_mr) {
          fprintf(stderr, "ibv_reg_mr failed\n");
          goto out;
        }

        conn_param.private_data = rai->ai_connect;
        conn_param.private_data_len = rai->ai_connect_len;
        printf("connecting\n");
        ret = rdma_connect(ctx->cma_id, &conn_param);
        if (ret) {
          fprintf(stderr, "rdma_connect failed\n");
          goto out;
        }
        break;
      case RDMA_CM_EVENT_ESTABLISHED:
        printf("rdma connection established\n");
        ctx->remote_qpn = event->param.ud.qp_num;
        ctx->remote_qkey = event->param.ud.qkey;
        ctx->ah = ibv_create_ah(ctx->pd, &event->param.ud.ah_attr);
        if (!ctx->ah) {
          printf("failure creating address handle\n");
          goto out;
        }
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

  /* write something to send region, use safe api */
  memset(ctx->send_region, 0, MSG_SIZE);
  strncpy((char *)ctx->send_region, "Hello from client msg 0", MSG_SIZE);
  strncpy((char *)ctx->send_region + MSG_SIZE, "Hello from client msg 1",
          MSG_SIZE);
  strncpy((char *)ctx->send_region + 2 * MSG_SIZE, "Bye", MSG_SIZE);
  if (!ctx->connected)
    return 0;
  printf("Post send 0\n");
  ret = rdma_post_ud_send(ctx->cma_id, ctx, ctx->send_region, MSG_SIZE,
                          ctx->send_mr, IBV_SEND_SIGNALED, ctx->ah,
                          ctx->remote_qpn);
  if (ret) {
    fprintf(stderr, "rdma_post_send failed\n");
    goto out;
  }
  printf("Post send 1\n");
  ret = rdma_post_ud_send(ctx->cma_id, ctx, ctx->send_region + MSG_SIZE,
                          MSG_SIZE, ctx->send_mr, IBV_SEND_SIGNALED, ctx->ah,
                          ctx->remote_qpn);
  if (ret) {
    fprintf(stderr, "rdma_post_send failed\n");
    goto out;
  }
  printf("Post send bye\n");
  ret = rdma_post_ud_send(ctx->cma_id, ctx, ctx->send_region + 2 * MSG_SIZE,
                          MSG_SIZE, ctx->send_mr, IBV_SEND_SIGNALED, ctx->ah,
                          ctx->remote_qpn);
  if (ret) {
    fprintf(stderr, "rdma_post_send failed\n");
    goto out;
  }

  /* poll cq */
  int sent = 0;
  while (sent < 3) {
    struct ibv_wc wc[3];
    do {
      ret = ibv_poll_cq(ctx->cq, 3, wc);
    } while (ret == 0);
    if (ret < 0) {
      fprintf(stderr, "ibv_poll_cq failed\n");
      goto out;
    }
    for (int i = 0; i < ret; i++) {
      if (wc[i].opcode == IBV_WC_SEND && wc[i].status == IBV_WC_SUCCESS) {
        sent++;
      }
    }
    printf("Sent %d\n", sent);
  }

out:
  if (ctx->ah)
    ibv_destroy_ah(ctx->ah);
  if (ctx->send_mr)
    ibv_dereg_mr(ctx->send_mr);
  if (ctx->send_region)
    free(ctx->send_region);
  if (ctx->cma_id->qp)
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