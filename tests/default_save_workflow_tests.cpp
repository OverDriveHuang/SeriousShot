#include "application/default_save_workflow.hpp"
#include "test_support.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using hdrshot::ClockPort;
using hdrshot::DefaultSaveRequest;
using hdrshot::DefaultSaveWorkflow;
using hdrshot::Error;
using hdrshot::ErrorCode;
using hdrshot::FileReceipt;
using hdrshot::FileStorePort;
using hdrshot::FilenamePolicy;
using hdrshot::LocalDateTime;
using hdrshot::Result;
using hdrshot::Retryability;
using hdrshot::SaveFormat;
using hdrshot::WriteFileRequest;

class FixedClock final : public ClockPort {
 public:
  LocalDateTime value{2026, 8, 28, 14, 31, 9, 480};
  mutable std::size_t calls{};

  LocalDateTime now_local() const override {
    ++calls;
    return value;
  }
};

class RecordingFileStore final : public FileStorePort {
 public:
  std::map<std::string, std::vector<std::uint8_t>> files;
  std::vector<std::string> attempts;
  std::optional<ErrorCode> injected_error;

  Result<FileReceipt, Error> write(const WriteFileRequest& request) override {
    attempts.push_back(request.exact_path);
    HDRSHOT_CHECK(!request.overwrite);
    if (injected_error.has_value()) {
      return Result<FileReceipt, Error>::failure(
          Error{*injected_error, "RecordingFileStore", Retryability::after_user_action, {}});
    }
    if (files.contains(request.exact_path)) {
      return Result<FileReceipt, Error>::failure(Error{
          ErrorCode::path_already_exists,
          "RecordingFileStore",
          Retryability::same_input,
          {}});
    }
    files.emplace(
        request.exact_path,
        std::vector<std::uint8_t>(request.bytes.begin(), request.bytes.end()));
    return Result<FileReceipt, Error>::success(
        FileReceipt{request.exact_path, request.bytes.size()});
  }
};

void exact_filename_and_join_policy() {
  const LocalDateTime timestamp{2026, 1, 2, 3, 4, 5, 480};
  const auto base = FilenamePolicy::make_png_name(timestamp, 0);
  const auto collision = FilenamePolicy::make_png_name(timestamp, 2);
  HDRSHOT_CHECK(base.has_value());
  HDRSHOT_CHECK(base.value() == "SeriousShot_2026-01-02_03-04-05.png");
  HDRSHOT_CHECK(collision.has_value());
  HDRSHOT_CHECK(collision.value() == "SeriousShot_2026-01-02_03-04-05_2.png");
  const auto jpeg = FilenamePolicy::make_name(
      timestamp, 0, SaveFormat::ultra_hdr_jpeg);
  HDRSHOT_CHECK(jpeg.has_value());
  HDRSHOT_CHECK(jpeg.value() == "SeriousShot_2026-01-02_03-04-05.jpg");
  HDRSHOT_CHECK(FilenamePolicy::join_folder("/Pictures", base.value()).value() ==
                "/Pictures/SeriousShot_2026-01-02_03-04-05.png");
  HDRSHOT_CHECK(FilenamePolicy::join_folder("C:\\Pictures", base.value()).value() ==
                "C:\\Pictures\\SeriousShot_2026-01-02_03-04-05.png");
}

void default_save_without_collision() {
  FixedClock clock;
  RecordingFileStore store;
  const std::vector<std::uint8_t> png{0x89, 0x50, 0x4E, 0x47};
  const auto result = DefaultSaveWorkflow::save(DefaultSaveRequest{png, "/Pictures", 10}, clock, store);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().exact_path == "/Pictures/SeriousShot_2026-08-28_14-31-09.png");
  HDRSHOT_CHECK(clock.calls == 1);
  HDRSHOT_CHECK(store.attempts.size() == 1);
}

void collisions_increment_without_overwrite() {
  FixedClock clock;
  RecordingFileStore store;
  const std::vector<std::uint8_t> existing{9};
  store.files["/Pictures/SeriousShot_2026-08-28_14-31-09.png"] = existing;
  store.files["/Pictures/SeriousShot_2026-08-28_14-31-09_1.png"] = existing;
  const std::vector<std::uint8_t> png{1, 2, 3};
  const auto result = DefaultSaveWorkflow::save(DefaultSaveRequest{png, "/Pictures", 10}, clock, store);
  HDRSHOT_CHECK(result.has_value());
  HDRSHOT_CHECK(result.value().exact_path == "/Pictures/SeriousShot_2026-08-28_14-31-09_2.png");
  HDRSHOT_CHECK(store.attempts.size() == 3);
  HDRSHOT_CHECK(store.files.at("/Pictures/SeriousShot_2026-08-28_14-31-09.png") == existing);
}

void non_collision_error_is_not_retried() {
  FixedClock clock;
  RecordingFileStore store;
  store.injected_error = ErrorCode::storage_full;
  const std::vector<std::uint8_t> png{1};
  const auto result = DefaultSaveWorkflow::save(DefaultSaveRequest{png, "/Pictures", 10}, clock, store);
  HDRSHOT_CHECK(!result.has_value());
  HDRSHOT_CHECK(result.error().code == ErrorCode::storage_full);
  HDRSHOT_CHECK(store.attempts.size() == 1);
}

void invalid_input_never_reaches_file_store() {
  FixedClock clock;
  RecordingFileStore store;
  clock.value.month = 13;
  const std::vector<std::uint8_t> png{1};
  const auto bad_time = DefaultSaveWorkflow::save(
      DefaultSaveRequest{png, "/Pictures", 10}, clock, store);
  HDRSHOT_CHECK(!bad_time.has_value());
  HDRSHOT_CHECK(bad_time.error().code == ErrorCode::invalid_input);
  HDRSHOT_CHECK(store.attempts.empty());

  const std::vector<std::uint8_t> empty;
  const auto empty_png = DefaultSaveWorkflow::save(
      DefaultSaveRequest{empty, "/Pictures", 10}, clock, store);
  HDRSHOT_CHECK(!empty_png.has_value());
  HDRSHOT_CHECK(clock.calls == 1);
}

}  // namespace

int main() {
  using hdrshot::test::TestCase;
  return hdrshot::test::run(std::vector<TestCase>{
      {"exact filename and path join", exact_filename_and_join_policy},
      {"default save without collision", default_save_without_collision},
      {"collision increments without overwrite", collisions_increment_without_overwrite},
      {"non-collision error is not retried", non_collision_error_is_not_retried},
      {"invalid input stops before write", invalid_input_never_reaches_file_store},
  });
}
