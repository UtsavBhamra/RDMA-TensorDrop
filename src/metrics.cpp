#include "tensordrop/transfer.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace tensordrop {
namespace {

}  // namespace

std::uint64_t checksum_file(const std::string& path) {
  // FNV-1a is used as a lightweight integrity check, not as a security hash.
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("cannot open file for checksum: " + path);
  std::uint64_t hash = 14695981039346656037ull;
  char byte = 0;
  while (file.get(byte)) {
    hash ^= static_cast<unsigned char>(byte);
    hash *= 1099511628211ull;
  }
  if (!file.eof()) throw std::runtime_error("failed while checksumming: " + path);
  return hash;
}

void write_metrics(const Metrics& m, const std::string& path) {
  const std::filesystem::path destination(path);
  if (!destination.parent_path().empty())
    std::filesystem::create_directories(destination.parent_path());
  std::ofstream out(path, std::ios::trunc);
  if (!out) throw std::runtime_error("cannot write metrics file: " + path);
  out << std::fixed << std::setprecision(3)
      << "{\n"
      << "  \"execution_mode\": \"" << m.mode << "\",\n"
      << "  \"provider\": \"" << m.provider << "\",\n"
      << "  \"bytes_transferred\": " << m.bytes << ",\n"
      << "  \"chunks\": " << m.chunks << ",\n"
      << "  \"elapsed_us\": " << m.elapsed_us << ",\n"
      << "  \"throughput_mib_s\": " << m.throughput_mib_s << ",\n"
      << "  \"chunk_latency_us\": ";
  if (m.has_chunk_latency) {
    out << "{\"p50\": " << m.chunk_p50_us
        << ", \"p95\": " << m.chunk_p95_us
        << ", \"p99\": " << m.chunk_p99_us << "},\n";
  } else {
    out << "null,\n";
  }
  out << "  \"integrity_ok\": " << (m.integrity_ok ? "true" : "false") << "\n}\n";
  if (!out) throw std::runtime_error("failed writing metrics file: " + path);
}

void print_metrics(const Metrics& m) {
  std::cout << "Execution mode: " << m.mode << "\n"
            << "Provider: " << m.provider << "\n"
            << "Transferred: " << m.bytes << " bytes in " << m.chunks << " chunks\n"
            << std::fixed << std::setprecision(3)
            << "Elapsed: " << m.elapsed_us << " us\n"
            << "Throughput: " << m.throughput_mib_s << " MiB/s\n";
  if (m.has_chunk_latency) {
    std::cout << "RDMA READ completion latency: p50 " << m.chunk_p50_us
              << " us, p95 " << m.chunk_p95_us << " us, p99 "
              << m.chunk_p99_us << " us\n";
  } else {
    std::cout << "RDMA completion latency: not measured in simulator mode\n";
  }
  std::cout << "Integrity: " << (m.integrity_ok ? "verified" : "FAILED") << "\n";
}

}  // namespace tensordrop
