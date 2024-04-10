# minimal_rdma

Minimal examples of how to code with RDMA.

## Install Dependencies

Ubuntu:

```bash
sudo apt-get update
sudo apt-get install -y libibverbs-dev librdmacm-dev libibumad-dev libpci-dev rdma-core infiniband-diags ibverbs-utils
```

Arch Linux:

```bash
sudo pacman -Syu --needed rdma-core
```

## Build

Install gcc and make environment.

```bash
make
```

## Run

### RC frame transport

This example shows how to transport raw frame data through RDMA RC path. It will measure the overall latency.

```bash
Usage: ./frame_* [options]

Options:
  -l <ip>            Set the local IP address
  -r <ip>            Set the remote IP address
  -p <number>        Set the port number
  -n <count>         Set the number of frames
  -w <pixels>        Set the frame width in pixels
  -h <pixels>        Set the frame height in pixels
  -d <bits>          Set the bit depth of the frames
  -H                  Display this help message
```

Start frame_rx:

```bash
./frame_rx -l 192.168.97.111 -p 20086 -w 3840 -h 2160 -d 10 -n 100
```

Start frame_tx:

```bash
./frame_tx -l 192.168.97.2 -r 192.168.97.111 -p 20086 -w 3840 -h 2160 -d 10 -n 100
```