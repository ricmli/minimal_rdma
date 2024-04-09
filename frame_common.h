#ifndef FRAME_COMMON_H
#define FRAME_COMMON_H

#include <stdint.h>
const int FRAME_BUFFER_SIZE = 1920 * 1080 * 4;
const int FRAME_BUFFER_CNT = 2;

enum msg_id { MSG_NONE = 0, MSG_RX_FRAME_READY, MSG_TX_FRAME_DONE };

struct msg {
  int id;
  union {
    /* send rx frame data info to tx when ready */
    struct {
      uint64_t addr;
      uint32_t rkey;
      uint64_t timestamp;
    } frame;
  } data;
};

#endif