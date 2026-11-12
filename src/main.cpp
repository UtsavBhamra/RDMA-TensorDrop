#include "tensordrop/transfer.hpp"

#include <charconv>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void usage() {
  std::cout << "TensorDrop: stage model tensors/checkpoints with RDMA READ\n\n"
            << "Usage:\n"
            << "  tensordrop simulate --input FILE --output FILE [--chunk-size BYTES] [--metrics FILE]\n"
            << "  tensordrop serve --file FILE --bind IP --port PORT [--device NAME] [--gid-index N]\n"
            << "                   [--max-file-size BYTES]\n"
            << "  tensordrop fetch --host IP --port PORT --output FILE [--device NAME]\n"
            << "                   [--gid-index N] [--chunk-size BYTES] [--metrics FILE]\n\n"
            << "Simulator results describe local file-copy runtime only. Real RDMA requires\n"
            << "an RDMA verbs device such as an RXE link or a supported RNIC.\n";
}

std::uint64_t number(const std::string& value, const std::string& option) {
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0)
    throw std::runtime_error(option + " must be a positive integer");
  return parsed;
}

struct Options {
  std::string input, output, host, bind = "0.0.0.0", device;
  std::string metrics = "transfer_metrics.json";
  std::uint64_t port = 9000, chunk_size = 1024u * 1024u;
  std::uint64_t max_file_size = 1024ull * 1024ull * 1024ull;
  int gid_index = 0;
};

Options parse(int argc, char** argv, int begin) {
  Options o;
  for (int i = begin; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--help") { usage(); std::exit(0); }
    if (i + 1 >= argc) throw std::runtime_error("missing value for " + key);
    const std::string value = argv[++i];
    if (key == "--input" || key == "--file") o.input = value;
    else if (key == "--output") o.output = value;
    else if (key == "--host") o.host = value;
    else if (key == "--bind") o.bind = value;
    else if (key == "--device") o.device = value;
    else if (key == "--metrics") o.metrics = value;
    else if (key == "--port") o.port = number(value, key);
    else if (key == "--chunk-size") o.chunk_size = number(value, key);
    else if (key == "--max-file-size") o.max_file_size = number(value, key);
    else if (key == "--gid-index") {
      std::uint64_t n = 0;
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), n);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          n > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("--gid-index is too large");
      o.gid_index = static_cast<int>(n);
    } else throw std::runtime_error("unknown option: " + key);
  }
  if (o.port > 65535) throw std::runtime_error("--port must be between 1 and 65535");
  if (o.chunk_size > 64u * 1024u * 1024u)
    throw std::runtime_error("--chunk-size cannot exceed 64 MiB");
  return o;
}

void required(const std::string& value, const std::string& flag) {
  if (value.empty()) throw std::runtime_error("this command requires " + flag);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
      usage();
      return 0;
    }
    const std::string command = argv[1];
    if (command == "simulate") {
      const auto o = parse(argc, argv, 2);
      required(o.input, "--input"); required(o.output, "--output");
      auto metrics = tensordrop::simulate_transfer(o.input, o.output, o.chunk_size);
      tensordrop::write_metrics(metrics, o.metrics);
      tensordrop::print_metrics(metrics);
      std::cout << "Fetched file: " << o.output << "\nMetrics: " << o.metrics << "\n";
      return metrics.integrity_ok ? 0 : 2;
    }
    if (command == "serve" || command == "fetch") {
      const auto o = parse(argc, argv, 2);
#ifdef TENSORDROP_HAVE_IBVERBS
      if (command == "serve") {
        required(o.input, "--file");
        return tensordrop::serve_file(o.input, o.bind, static_cast<std::uint16_t>(o.port),
                                      o.device, o.gid_index, o.max_file_size);
      }
      required(o.host, "--host"); required(o.output, "--output");
      auto metrics = tensordrop::fetch_file(o.host, static_cast<std::uint16_t>(o.port),
          o.output, o.device, o.gid_index, o.chunk_size, o.metrics);
      tensordrop::write_metrics(metrics, o.metrics);
      tensordrop::print_metrics(metrics);
      std::cout << "Fetched file: " << o.output << "\nMetrics: " << o.metrics << "\n";
      return metrics.integrity_ok ? 0 : 2;
#else
      throw std::runtime_error("real RDMA mode needs libibverbs development files; use 'simulate' for a local workflow demo");
#endif
    }
    throw std::runtime_error("expected simulate, serve, or fetch (try --help)");
  } catch (const std::exception& e) {
    std::cerr << "tensordrop: " << e.what() << "\n";
    return 1;
  }
}
