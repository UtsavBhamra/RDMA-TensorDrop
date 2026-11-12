#include "tensordrop/transfer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace tensordrop {

Metrics simulate_transfer(const std::string& input, const std::string& output,
                          std::size_t chunk_size) {
  if (chunk_size == 0 || chunk_size > 64u * 1024u * 1024u)
    throw std::runtime_error("chunk size must be between 1 byte and 64 MiB");
  std::ifstream source(input, std::ios::binary);
  if (!source) throw std::runtime_error("cannot open input tensor/checkpoint: " + input);
  source.seekg(0, std::ios::end);
  const auto end = source.tellg();
  if (end <= 0) throw std::runtime_error("input file must not be empty");
  const auto total = static_cast<std::uint64_t>(end);
  source.seekg(0);

  const std::filesystem::path target(output);
  if (std::filesystem::absolute(input) == std::filesystem::absolute(output))
    throw std::runtime_error("input and output paths must be different");
  if (!target.parent_path().empty()) std::filesystem::create_directories(target.parent_path());
  const auto temporary = output + ".part";
  std::ofstream sink(temporary, std::ios::binary | std::ios::trunc);
  if (!sink) throw std::runtime_error("cannot create output file: " + temporary);

  // This path copies bounded blocks to exercise the same chunked file workflow.
  // Its wall time is simulator runtime, not an RDMA or network latency result.
  std::vector<char> block(std::min<std::uint64_t>(total, chunk_size));
  std::uint64_t chunks = 0;
  const auto start = std::chrono::steady_clock::now();
  while (source) {
    source.read(block.data(), static_cast<std::streamsize>(block.size()));
    const auto count = source.gcount();
    if (count <= 0) break;
    sink.write(block.data(), count);
    if (!sink) throw std::runtime_error("failed writing simulated transfer output");
    ++chunks;
  }
  sink.close();
  if (!source.eof()) throw std::runtime_error("failed reading input file");

  const auto expected_hash = checksum_file(input);
  const auto received_hash = checksum_file(temporary);
  if (expected_hash != received_hash || std::filesystem::file_size(temporary) != total) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("simulated transfer integrity check failed");
  }
  std::filesystem::rename(temporary, output);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  Metrics m;
  m.mode = "simulator";
  m.provider = "local chunk-copy simulator";
  m.bytes = total;
  m.chunks = chunks;
  m.elapsed_us = std::chrono::duration<double, std::micro>(elapsed).count();
  m.throughput_mib_s = m.elapsed_us > 0
      ? (static_cast<double>(total) / (1024.0 * 1024.0)) / (m.elapsed_us / 1.0e6)
      : 0.0;
  m.integrity_ok = true;
  return m;
}

}  // namespace tensordrop
