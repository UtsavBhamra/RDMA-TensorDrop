# TensorDrop

TensorDrop is a small RDMA learning project for staging a model checkpoint or tensor file on a training/inference worker. The server registers the file in host memory and grants remote-read permission. The client retrieves it in bounded RDMA READ chunks, verifies the received bytes, and saves a local file for an existing model process to load.

The project does not implement training, inference, CUDA, or GPUDirect RDMA. A typical GPU worker can fetch a checkpoint into host memory and then let its existing framework load or copy the weights to the GPU.

## Build

```sh
sudo apt install build-essential cmake libibverbs-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

If `libibverbs-dev` is missing, CMake still builds the simulator. The real server/fetch commands become available after installing the development files and rebuilding.

## Run the file workflow in simulator mode

The simulator exercises bounded chunking, file output, and integrity verification. Its time and throughput describe local simulator execution only; they are not RDMA or network performance results.

```sh
./build/tensordrop simulate \
  --input samples/demo_checkpoint.bin \
  --output artifacts/fetched_checkpoint.bin \
  --chunk-size 16384 \
  --metrics artifacts/simulator_metrics.json
```

The generated example files in `artifacts/` were created by this command. Compare the input and fetched file with:

```sh
cmp samples/demo_checkpoint.bin artifacts/fetched_checkpoint.bin
cat artifacts/simulator_metrics.json
```

## Run over real RDMA verbs

For a single-machine lab, first create two network namespaces and connect them with a veth pair. RXE must support per-network-namespace links; older kernels may not support this setup. Check the kernel and tools:

```sh
uname -r
sudo modprobe rdma_rxe
rdma link show
```

Create the namespaces and veth pair:

```sh
sudo ip netns add rdma-a
sudo ip netns add rdma-b
sudo ip link add veth-a type veth peer name veth-b
sudo ip link set veth-a netns rdma-a
sudo ip link set veth-b netns rdma-b
sudo ip -n rdma-a addr add 192.0.2.1/24 dev veth-a
sudo ip -n rdma-b addr add 192.0.2.2/24 dev veth-b
sudo ip -n rdma-a link set lo up
sudo ip -n rdma-b link set lo up
sudo ip -n rdma-a link set veth-a up
sudo ip -n rdma-b link set veth-b up
sudo ip netns exec rdma-a ping -c 2 192.0.2.2
```

Create and inspect the RXE devices. Find the GID index matching each namespace's veth address with `ibv_devinfo -v`:

```sh
sudo ip netns exec rdma-a rdma link add rxe-a type rxe netdev veth-a
sudo ip netns exec rdma-b rdma link add rxe-b type rxe netdev veth-b
sudo ip netns exec rdma-a ibv_devices
sudo ip netns exec rdma-b ibv_devices
sudo ip netns exec rdma-a ibv_devinfo -v
sudo ip netns exec rdma-b ibv_devinfo -v
```

Start the file server, then fetch from the other namespace (substitute the GID indexes found above):

```sh
sudo ip netns exec rdma-b ./build/tensordrop serve \
  --file samples/demo_checkpoint.bin --bind 192.0.2.2 --port 9100 \
  --device rxe-b --gid-index 0
```

```sh
sudo ip netns exec rdma-a ./build/tensordrop fetch \
  --host 192.0.2.2 --port 9100 --output artifacts/fetched_over_rdma.bin \
  --device rxe-a --gid-index 0 --chunk-size 16384 \
  --metrics artifacts/rdma_metrics.json
```

The fetch output reports bytes, chunk count, elapsed time, effective throughput, and p50/p95/p99 RDMA READ completion latency. These describe that actual transfer on the selected provider. Soft-RoCE is a software provider over Ethernet and does not represent hardware RNIC performance. Remove the lab namespaces when finished with `sudo ip netns del rdma-a` and `sudo ip netns del rdma-b`.

## Loading the fetched checkpoint

TensorDrop writes the fetched bytes unchanged. For example, a PyTorch application can load a compatible checkpoint using its normal loader:

```python
import numpy as np
import torch
weights = np.fromfile("artifacts/fetched_checkpoint.bin", dtype="<f4")
tensor = torch.from_numpy(weights)
```

The included demo file is a raw float32 tensor fixture. For a real model, serve a checkpoint in the format the training or inference application already expects, then use that framework's normal loader.

## Metrics and interpretation

The JSON report identifies its `execution_mode` and provider. Simulator output reports local simulator runtime and leaves `chunk_latency_us` null. Only the real verbs path reports RDMA READ completion latency. Throughput is the bytes fetched divided by the transfer duration, including RDMA and output-file writes. Results are descriptive transfer telemetry, not a benchmark sweep or a comparison against kernel networking.
