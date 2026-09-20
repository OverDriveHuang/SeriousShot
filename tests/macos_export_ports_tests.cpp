#include "platform/macos/macos_export_ports.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

using namespace hdrshot;

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::array<char, 64> path{};
    const std::string pattern = "/tmp/hdrshot-file-store-XXXXXX";
    std::copy(pattern.begin(), pattern.end(), path.begin());
    path_ = ::mkdtemp(path.data());
    HDRSHOT_CHECK(!path_.empty());
  }

  ~TemporaryDirectory() {
    if (!file_path_.empty()) {
      ::unlink(file_path_.c_str());
    }
    if (!path_.empty()) {
      ::rmdir(path_.c_str());
    }
  }

  std::string file_path() {
    file_path_ = path_ + "/capture.png";
    return file_path_;
  }

 private:
  std::string path_;
  std::string file_path_;
};

std::vector<std::uint8_t> read_all(const std::string& path) {
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  HDRSHOT_CHECK(descriptor >= 0);
  std::vector<std::uint8_t> bytes(64U);
  const auto count = ::read(descriptor, bytes.data(), bytes.size());
  HDRSHOT_CHECK(count >= 0);
  HDRSHOT_CHECK(::close(descriptor) == 0);
  bytes.resize(static_cast<std::size_t>(count));
  return bytes;
}

void create_new_never_overwrites_and_confirmed_overwrite_is_atomic() {
  TemporaryDirectory directory;
  const auto path = directory.file_path();
  PosixFileStorePort port;
  const std::array<std::uint8_t, 3> first{1U, 2U, 3U};
  const std::array<std::uint8_t, 4> replacement{9U, 8U, 7U, 6U};

  const auto created = port.write(WriteFileRequest{first, path, false});
  HDRSHOT_CHECK(created.has_value());
  HDRSHOT_CHECK(read_all(path) == std::vector<std::uint8_t>(first.begin(), first.end()));

  const auto collision = port.write(WriteFileRequest{replacement, path, false});
  HDRSHOT_CHECK(!collision.has_value());
  HDRSHOT_CHECK(collision.error().code == ErrorCode::path_already_exists);
  HDRSHOT_CHECK(read_all(path) == std::vector<std::uint8_t>(first.begin(), first.end()));

  const auto overwritten = port.write(WriteFileRequest{replacement, path, true});
  HDRSHOT_CHECK(overwritten.has_value());
  HDRSHOT_CHECK(read_all(path) ==
      std::vector<std::uint8_t>(replacement.begin(), replacement.end()));
}

void streaming_sink_is_invisible_until_commit_and_abort_removes_pending_file() {
  TemporaryDirectory directory;
  const auto base = directory.file_path();
  const auto committed_path = base + ".streamed";
  const auto aborted_path = base + ".aborted";
  PosixFileStorePort port;
  const std::array<std::uint8_t, 3> first{1U, 2U, 3U};
  const std::array<std::uint8_t, 2> second{4U, 5U};

  auto sink = port.open_atomic(OpenAtomicFileRequest{committed_path, false});
  HDRSHOT_CHECK(sink.has_value());
  HDRSHOT_CHECK(::access(committed_path.c_str(), F_OK) != 0);
  HDRSHOT_CHECK(sink.value()->write(first).has_value());
  HDRSHOT_CHECK(sink.value()->write(second).has_value());
  HDRSHOT_CHECK(::access(committed_path.c_str(), F_OK) != 0);
  const auto receipt = sink.value()->commit();
  HDRSHOT_CHECK(receipt.has_value());
  HDRSHOT_CHECK(receipt.value().bytes_written == 5U);
  HDRSHOT_CHECK(read_all(committed_path) ==
                (std::vector<std::uint8_t>{1U, 2U, 3U, 4U, 5U}));

  auto aborted = port.open_atomic(OpenAtomicFileRequest{aborted_path, false});
  HDRSHOT_CHECK(aborted.has_value());
  HDRSHOT_CHECK(aborted.value()->write(first).has_value());
  aborted.value()->abort();
  HDRSHOT_CHECK(::access(aborted_path.c_str(), F_OK) != 0);
  ::unlink(committed_path.c_str());
}

}  // namespace

int main() {
  return hdrshot::test::run({
      {"macOS file store create-new and confirmed atomic overwrite",
       create_new_never_overwrites_and_confirmed_overwrite_is_atomic},
      {"macOS streaming file sink commits atomically and aborts cleanly",
       streaming_sink_is_invisible_until_commit_and_abort_removes_pending_file},
  });
}
