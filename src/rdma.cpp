#include "tensordrop/transfer.hpp"

#include <arpa/inet.h>
#include <endian.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace tensordrop {
namespace {

constexpr std::uint32_t kWireMagic = 0x54445250;  // "TDRP"
constexpr std::uint64_t kHardFileLimit = 4ull * 1024ull * 1024ull * 1024ull;
using Clock = std::chrono::steady_clock;

struct WirePeer {
  std::uint32_t magic;
  std::uint32_t qpn;
  std::uint32_t psn;
  std::uint32_t rkey;
  std::uint64_t address;
  std::uint64_t bytes;
  std::uint64_t checksum;
  std::uint8_t gid[16];
};

void transfer_exact(int fd, void* data, std::size_t length, bool sending) {
  auto* bytes = static_cast<unsigned char*>(data);
  std::size_t complete = 0;
  while (complete < length) {
    const auto amount = sending
        ? ::send(fd, bytes + complete, length - complete, MSG_NOSIGNAL)
        : ::recv(fd, bytes + complete, length - complete, 0);
    if (amount <= 0)
      throw std::runtime_error(sending ? "control socket send failed" : "control peer disconnected");
    complete += static_cast<std::size_t>(amount);
  }
}

int tcp_socket(const std::string& host, std::uint16_t port, bool server) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = server ? AI_PASSIVE : 0;
  addrinfo* addresses = nullptr;
  const auto service = std::to_string(port);
  const char* node = server ? (host.empty() ? nullptr : host.c_str()) : host.c_str();
  const auto rc = getaddrinfo(node, service.c_str(), &hints, &addresses);
  if (rc != 0) throw std::runtime_error(std::string("cannot resolve control address: ") + gai_strerror(rc));

  int fd = -1;
  for (auto* address = addresses; address; address = address->ai_next) {
    fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) continue;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    const int rc_socket = server ? ::bind(fd, address->ai_addr, address->ai_addrlen)
                                 : ::connect(fd, address->ai_addr, address->ai_addrlen);
    if (rc_socket == 0) break;
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);
  if (fd < 0) throw std::runtime_error(server ? "could not bind control socket" : "could not connect to server");
  if (server && ::listen(fd, 1) < 0) {
    ::close(fd);
    throw std::runtime_error("could not listen on control socket");
  }
  return fd;
}

struct RdmaEndpoint {
  ibv_context* context = nullptr;
  ibv_pd* protection_domain = nullptr;
  ibv_cq* completion_queue = nullptr;
  ibv_qp* queue_pair = nullptr;
  ibv_mr* memory_region = nullptr;
  ibv_port_attr port_attributes{};
  std::vector<unsigned char> buffer;
  std::string device_name;
  int port_number = 1;
  int gid_index = 0;
  std::uint32_t packet_sequence = 0x348291;

  RdmaEndpoint(const std::string& requested_device, int selected_gid,
               std::vector<unsigned char> memory, int access)
      : buffer(std::move(memory)), gid_index(selected_gid) {
    int count = 0;
    ibv_device** devices = ibv_get_device_list(&count);
    if (!devices || count == 0)
      throw std::runtime_error("no RDMA verbs devices found; create an RXE device or use an RDMA NIC");
    ibv_device* selected = nullptr;
    for (int i = 0; i < count; ++i) {
      if (requested_device.empty() || requested_device == ibv_get_device_name(devices[i])) {
        selected = devices[i];
        break;
      }
    }
    if (!selected) {
      ibv_free_device_list(devices);
      throw std::runtime_error("requested RDMA device was not found");
    }
    device_name = ibv_get_device_name(selected);
    context = ibv_open_device(selected);
    ibv_free_device_list(devices);
    if (!context) throw std::runtime_error("ibv_open_device failed");
    if (ibv_query_port(context, port_number, &port_attributes) != 0)
      throw std::runtime_error("ibv_query_port failed");

    union ibv_gid local_gid{};
    if (gid_index < 0 || ibv_query_gid(context, port_number, gid_index, &local_gid) != 0)
      throw std::runtime_error("cannot query the selected GID index");

    protection_domain = ibv_alloc_pd(context);
    if (!protection_domain) throw std::runtime_error("ibv_alloc_pd failed");
    completion_queue = ibv_create_cq(context, 64, nullptr, nullptr, 0);
    if (!completion_queue) throw std::runtime_error("ibv_create_cq failed");

    // The server registers the model file with REMOTE_READ; the client registers
    // its chunk buffer with LOCAL_WRITE so RDMA can place received bytes there.
    memory_region = ibv_reg_mr(protection_domain, buffer.data(), buffer.size(), access);
    if (!memory_region) throw std::runtime_error("ibv_reg_mr failed; check memory-lock limits");

    ibv_qp_init_attr init{};
    init.send_cq = completion_queue;
    init.recv_cq = completion_queue;
    init.qp_type = IBV_QPT_RC;
    init.cap.max_send_wr = 1;
    init.cap.max_recv_wr = 1;
    init.cap.max_send_sge = 1;
    init.cap.max_recv_sge = 1;
    queue_pair = ibv_create_qp(protection_domain, &init);
    if (!queue_pair) throw std::runtime_error("ibv_create_qp failed");

    ibv_qp_attr attributes{};
    attributes.qp_state = IBV_QPS_INIT;
    attributes.pkey_index = 0;
    attributes.port_num = port_number;
    attributes.qp_access_flags = IBV_ACCESS_REMOTE_READ;
    const int init_mask = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    if (ibv_modify_qp(queue_pair, &attributes, init_mask) != 0)
      throw std::runtime_error("QP INIT transition failed");
  }

  ~RdmaEndpoint() {
    if (queue_pair) ibv_destroy_qp(queue_pair);
    if (memory_region) ibv_dereg_mr(memory_region);
    if (completion_queue) ibv_destroy_cq(completion_queue);
    if (protection_domain) ibv_dealloc_pd(protection_domain);
    if (context) ibv_close_device(context);
  }

  WirePeer descriptor(std::uint64_t file_bytes, std::uint64_t checksum) {
    WirePeer peer{};
    peer.magic = htonl(kWireMagic);
    peer.qpn = htonl(queue_pair->qp_num);
    peer.psn = htonl(packet_sequence);
    peer.rkey = htonl(memory_region->rkey);
    peer.address = htobe64(reinterpret_cast<std::uintptr_t>(buffer.data()));
    peer.bytes = htobe64(file_bytes);
    peer.checksum = htobe64(checksum);
    union ibv_gid gid{};
    if (ibv_query_gid(context, port_number, gid_index, &gid) != 0)
      throw std::runtime_error("ibv_query_gid failed while building connection descriptor");
    std::memcpy(peer.gid, gid.raw, sizeof(peer.gid));
    return peer;
  }

  void connect_to(const WirePeer& peer) {
    ibv_qp_attr attributes{};
    attributes.qp_state = IBV_QPS_RTR;
    attributes.path_mtu = IBV_MTU_1024;
    attributes.dest_qp_num = ntohl(peer.qpn);
    attributes.rq_psn = ntohl(peer.psn);
    attributes.max_dest_rd_atomic = 1;
    attributes.min_rnr_timer = 12;
    attributes.ah_attr.is_global = 1;
    std::memcpy(attributes.ah_attr.grh.dgid.raw, peer.gid, sizeof(peer.gid));
    attributes.ah_attr.grh.sgid_index = gid_index;
    attributes.ah_attr.grh.hop_limit = 1;
    attributes.ah_attr.port_num = port_number;
    const int rtr_mask = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    if (ibv_modify_qp(queue_pair, &attributes, rtr_mask) != 0)
      throw std::runtime_error("QP RTR transition failed; check RXE GID indexes and links");

    attributes = {};
    attributes.qp_state = IBV_QPS_RTS;
    attributes.timeout = 14;
    attributes.retry_cnt = 7;
    attributes.rnr_retry = 7;
    attributes.sq_psn = packet_sequence;
    attributes.max_rd_atomic = 1;
    const int rts_mask = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;
    if (ibv_modify_qp(queue_pair, &attributes, rts_mask) != 0)
      throw std::runtime_error("QP RTS transition failed");
  }

  void complete_one() {
    ibv_wc completion{};
    for (;;) {
      const int count = ibv_poll_cq(completion_queue, 1, &completion);
      if (count < 0) throw std::runtime_error("ibv_poll_cq failed");
      if (count == 0) continue;
      if (completion.status != IBV_WC_SUCCESS)
        throw std::runtime_error(std::string("RDMA work completion failed: ") +
                                 ibv_wc_status_str(completion.status));
      return;
    }
  }

  void read_remote(std::uint64_t remote_address, std::uint32_t remote_key,
                   std::size_t bytes) {
    ibv_sge scatter{};
    scatter.addr = reinterpret_cast<std::uintptr_t>(buffer.data());
    scatter.length = static_cast<std::uint32_t>(bytes);
    scatter.lkey = memory_region->lkey;
    ibv_send_wr request{};
    request.wr_id = 1;
    request.sg_list = &scatter;
    request.num_sge = 1;
    request.opcode = IBV_WR_RDMA_READ;
    request.send_flags = IBV_SEND_SIGNALED;
    request.wr.rdma.remote_addr = remote_address;
    request.wr.rdma.rkey = remote_key;
    ibv_send_wr* bad = nullptr;
    if (ibv_post_send(queue_pair, &request, &bad) != 0)
      throw std::runtime_error("ibv_post_send RDMA READ failed");
    complete_one();
  }
};

WirePeer receive_peer(int fd) {
  WirePeer peer{};
  transfer_exact(fd, &peer, sizeof(peer), false);
  if (ntohl(peer.magic) != kWireMagic) throw std::runtime_error("invalid control handshake");
  return peer;
}

void send_peer(int fd, const WirePeer& peer) {
  auto copy = peer;
  transfer_exact(fd, &copy, sizeof(copy), true);
}

double percentile(std::vector<double> values, double p) {
  if (values.empty()) return 0.0;
  const auto index = static_cast<std::size_t>(p * (values.size() - 1));
  std::nth_element(values.begin(), values.begin() + index, values.end());
  return values[index];
}

}  // namespace

int serve_file(const std::string& file_path, const std::string& bind_ip,
               std::uint16_t port, const std::string& device, int gid_index,
               std::uint64_t max_file_size) {
  std::ifstream input(file_path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open model file: " + file_path);
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length <= 0) throw std::runtime_error("model file must not be empty");
  const auto file_bytes = static_cast<std::uint64_t>(length);
  if (file_bytes > max_file_size || file_bytes > kHardFileLimit)
    throw std::runtime_error("model file exceeds the configured or 4 GiB safety limit");
  input.seekg(0);
  std::vector<unsigned char> file(static_cast<std::size_t>(file_bytes));
  input.read(reinterpret_cast<char*>(file.data()), static_cast<std::streamsize>(file.size()));
  if (!input) throw std::runtime_error("failed reading model file into host memory");
  const auto checksum = checksum_file(file_path);

  // Register the server's immutable model buffer with remote-read permission.
  RdmaEndpoint endpoint(device, gid_index, std::move(file),
                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
  const int listener = tcp_socket(bind_ip == "0.0.0.0" ? "" : bind_ip, port, true);
  std::cerr << "Serving " << file_bytes << " bytes on " << bind_ip << ':' << port
            << " using RDMA device " << endpoint.device_name << "\n";
  const int fd = ::accept(listener, nullptr, nullptr);
  ::close(listener);
  if (fd < 0) throw std::runtime_error("control connection accept failed");
  try {
    const auto local = endpoint.descriptor(file_bytes, checksum);
    send_peer(fd, local);
    const auto remote = receive_peer(fd);
    endpoint.connect_to(remote);
    std::uint8_t client_ok = 0;
    transfer_exact(fd, &client_ok, sizeof(client_ok), false);
    if (client_ok != 1) throw std::runtime_error("client reported a failed file integrity check");
    std::uint8_t server_ok = 1;
    transfer_exact(fd, &server_ok, sizeof(server_ok), true);
    ::close(fd);
  } catch (...) {
    ::close(fd);
    throw;
  }
  return 0;
}

Metrics fetch_file(const std::string& host, std::uint16_t port,
                   const std::string& output, const std::string& device,
                   int gid_index, std::size_t chunk_size,
                   const std::string& metrics_path) {
  (void)metrics_path;
  if (chunk_size == 0 || chunk_size > 64u * 1024u * 1024u)
    throw std::runtime_error("chunk size must be between 1 byte and 64 MiB");
  const int fd = tcp_socket(host, port, false);
  try {
    const auto remote = receive_peer(fd);
    const auto remote_bytes = be64toh(remote.bytes);
    if (remote_bytes == 0 || remote_bytes > kHardFileLimit)
      throw std::runtime_error("server advertised an invalid or oversized file");
    const auto remote_address = be64toh(remote.address);
    const auto remote_key = ntohl(remote.rkey);
    std::vector<unsigned char> buffer(std::min<std::uint64_t>(remote_bytes, chunk_size));
    // LOCAL_WRITE is required because the RNIC writes RDMA READ results here.
    RdmaEndpoint endpoint(device, gid_index, std::move(buffer), IBV_ACCESS_LOCAL_WRITE);
    const auto local = endpoint.descriptor(0, 0);
    send_peer(fd, local);
    endpoint.connect_to(remote);

    const std::filesystem::path output_path(output);
    if (!output_path.parent_path().empty())
      std::filesystem::create_directories(output_path.parent_path());
    const auto temporary = output + ".part";
    std::ofstream sink(temporary, std::ios::binary | std::ios::trunc);
    if (!sink) throw std::runtime_error("cannot create output file: " + temporary);

    // Each bounded RDMA READ is completed before the next chunk reuses the MR.
    std::vector<double> chunk_latencies;
    const auto started = Clock::now();
    std::uint64_t offset = 0;
    std::uint64_t chunks = 0;
    while (offset < remote_bytes) {
      const auto amount = static_cast<std::size_t>(
          std::min<std::uint64_t>(endpoint.buffer.size(), remote_bytes - offset));
      const auto chunk_start = Clock::now();
      endpoint.read_remote(remote_address + offset, remote_key, amount);
      const auto chunk_end = Clock::now();
      sink.write(reinterpret_cast<const char*>(endpoint.buffer.data()),
                 static_cast<std::streamsize>(amount));
      if (!sink) throw std::runtime_error("failed writing fetched model data");
      chunk_latencies.push_back(
          std::chrono::duration<double, std::micro>(chunk_end - chunk_start).count());
      offset += amount;
      ++chunks;
    }
    sink.close();
    if (!sink) throw std::runtime_error("failed closing fetched model file");

    const auto expected_checksum = be64toh(remote.checksum);
    const auto actual_checksum = checksum_file(temporary);
    if (actual_checksum != expected_checksum ||
        std::filesystem::file_size(temporary) != remote_bytes) {
      std::filesystem::remove(temporary);
      throw std::runtime_error("fetched file failed checksum or size verification");
    }
    std::filesystem::rename(temporary, output);
    const auto finished = Clock::now();

    Metrics result;
    result.mode = "rdma";
    result.provider = endpoint.device_name;
    result.bytes = remote_bytes;
    result.chunks = chunks;
    result.elapsed_us = std::chrono::duration<double, std::micro>(finished - started).count();
    result.throughput_mib_s = result.elapsed_us > 0
        ? (static_cast<double>(remote_bytes) / (1024.0 * 1024.0)) / (result.elapsed_us / 1.0e6)
        : 0.0;
    result.chunk_p50_us = percentile(chunk_latencies, 0.50);
    result.chunk_p95_us = percentile(chunk_latencies, 0.95);
    result.chunk_p99_us = percentile(chunk_latencies, 0.99);
    result.has_chunk_latency = true;
    result.integrity_ok = true;

    std::uint8_t client_ok = 1;
    transfer_exact(fd, &client_ok, sizeof(client_ok), true);
    std::uint8_t server_ok = 0;
    transfer_exact(fd, &server_ok, sizeof(server_ok), false);
    if (server_ok != 1) throw std::runtime_error("server rejected transfer completion");
    ::close(fd);
    return result;
  } catch (...) {
    ::close(fd);
    throw;
  }
}

}  // namespace tensordrop
