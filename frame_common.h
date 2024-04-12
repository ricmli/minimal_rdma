#ifndef FRAME_COMMON_H
#define FRAME_COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const int FRAME_BUFFER_CNT = 2;

enum msg_id {
  MSG_NONE = 0,
  MSG_RX_FRAME_READY,
  MSG_TX_FRAME_DONE,
  MSG_RX_MEASURE_RTT,
  MSG_TX_MEASURE_RTT_START,
  MSG_TX_MEASURE_RTT_DONE,
};

struct msg {
  int id;
  union {
    /* send rx frame data info to tx when ready */
    struct {
      uint64_t addr;
      uint32_t rkey;
      uint64_t timestamp;
    } frame;
    struct {
      uint64_t timestamp;
    } measure_rtt;
  } data;
};

// Define the Args structure
typedef struct {
  char *local_ip_address;  // Local IP address
  char *remote_ip_address; // Remote IP address
  char *port_number;       // Port number
  int number_of_frames;    // Number of frames
  int frame_width;         // Frame width
  int frame_height;        // Frame height
  int bit_depth;           // Bit depth
} Args;

// Function to print help message and exit
static void print_help_and_exit(const char *argv0) {
  printf("Usage: %s [options]\n\nOptions:\n"
         "  -l <ip>            Set the local IP address\n"
         "  -r <ip>            Set the remote IP address\n"
         "  -p <number>        Set the port number\n"
         "  -n <count>         Set the number of frames\n"
         "  -w <pixels>        Set the frame width in pixels\n"
         "  -h <pixels>        Set the frame height in pixels\n"
         "  -d <bits>          Set the bit depth of the frames\n"
         "  -H                  Display this help message\n",
         argv0);
  exit(EXIT_SUCCESS);
}

// Parse command line arguments function
static Args parse_args(int argc, char *argv[]) {
  Args args = {NULL, NULL, 0, 0, 0, 0, 0}; // Initialize Args struct

  for (int i = 1; i < argc; i++) {
    if (argv[i][0] == '-') {
      switch (argv[i][1]) {
      case 'l': // -l for local IP address
        if (i + 1 < argc) {
          args.local_ip_address = argv[++i];
        } else {
          fprintf(stderr, "Error: Option '-l' requires an argument.\n");
          print_help_and_exit(argv[0]);
        }
        break;
      case 'r': // -r for remote IP address
        if (i + 1 < argc) {
          args.remote_ip_address = argv[++i];
        } else {
          fprintf(stderr, "Error: Option '-r' requires an argument.\n");
          print_help_and_exit(argv[0]);
        }
        break;
      case 'p': // -p for port number
        if (i + 1 < argc) {
          args.port_number = argv[++i];
        } else {
          fprintf(stderr, "Error: Option '-p' requires an argument.\n");
          print_help_and_exit(argv[0]);
        }
        break;
      case 'n': // -n for number of frames
        if (i + 1 < argc) {
          args.number_of_frames = atoi(argv[++i]);
        } else {
          fprintf(stderr, "Error: Option '-n' requires an argument.\n");
          print_help_and_exit(argv[0]);
        }
        break;
      case 'w': // -w for frame width
        if (i + 1 < argc) {
          args.frame_width = atoi(argv[++i]);
        } else {
          fprintf(stderr, "Error: Option '-w' requires an argument.\n");
          print_help_and_exit(argv[0]);
        }
        break;
      case 'h': // -h for frame height
        if (i + 1 < argc) {
          args.frame_height = atoi(argv[++i]);
        } else {
          fprintf(stderr, "Error: Option '-h' requires an argument.\n");
          print_help_and_exit(argv[0]);
        }
        break;
      case 'd': // -d for bit depth
        if (i + 1 < argc) {
          args.bit_depth = atoi(argv[++i]);
        } else {
          fprintf(stderr, "Error: Option '-d' requires an argument.\n");
          print_help_and_exit(argv[0]);
        }
        break;
      case 'H': // -H for help
        print_help_and_exit(argv[0]);
        break;
      default:
        fprintf(stderr, "Error: Unknown option '-%c'.\n", argv[i][1]);
        print_help_and_exit(argv[0]);
        break;
      }
    } else {
      fprintf(stderr, "Error: Unexpected argument: %s\n", argv[i]);
      print_help_and_exit(argv[0]);
    }
  }

  return args;
}

#endif