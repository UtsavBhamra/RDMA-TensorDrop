#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tensordrop {

struct Metrics {
  std::string mode;
  std::string provider;
  std::uint64_t bytes = 0;
  std::uint64_t chunks = 0;
  double elapsed_us = 0.0;
  double throughput_mib_s = 0.0;
  double chunk_p50_us = 0.0;
  double chunk_p95_us = 0.0;
  double chunk_p99_us = 0.0;
  bool has_chunk_latency = false;
  bool integrity_ok = false;
};

std::uint64_t checksum_file(const std::string& path);
void write_metrics(const Metrics& metrics, const std::string& path);
void print_metrics(const Metrics& metrics);
Metrics simulate_transfer(const std::string& input, const std::string& output,
                          std::size_t chunk_size);

#ifdef TENSORDROP_HAVE_IBVERBS
int serve_file(const std::string& file, const std::string& bind_ip,
               std::uint16_t port, const std::string& device, int gid_index,
               std::uint64_t max_file_size);
Metrics fetch_file(const std::string& host, std::uint16_t port,
                   const std::string& output, const std::string& device,
                   int gid_index, std::size_t chunk_size,
                   const std::string& metrics_path);
#endif

}  // namespace tensordrop
