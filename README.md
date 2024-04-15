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
  -e                 Use cq event notify for messages
  -H                 Display this help message
```

Start frame_rx:

```bash
./frame_rx -l 192.168.97.111 -p 20086 -w 3840 -h 2160 -d 10 -n 100 -e
```

Start frame_tx:

```bash
./frame_tx -l 192.168.97.2 -r 192.168.97.111 -p 20086 -w 3840 -h 2160 -d 10 -n 100 -e
```

Enable `-e` option to use CQ event will significanyly reduce CPU usage but introduce some notify latency. 

Example compare data, with TX & RX running on same machine:

| Method | Msg RTT (us) | Frame Latency (ms) | CPU Usage (%) |
| ------------ | ------------ | ------------ | ------------ |
| CQ event | 77.6 | 7.8 | 0.0 |
| Polling | 18.3 | 7.2 | 100.0 |
