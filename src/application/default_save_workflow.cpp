#include "application/default_save_workflow.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

namespace hdrshot {
namespace {

Error invalid_input(std::string field) {
  return Error{
      ErrorCode::invalid_input,
      "DefaultSaveWorkflow",
      Retryability::never,
      {{"field", std::move(field)}}};
}

bool valid_timestamp(const LocalDateTime& value) {
  const auto date = std::chrono::year_month_day{
      std::chrono::year{value.year},
      std::chrono::month{value.month},
      std::chrono::day{value.day}};
  return date.ok() && value.hour <= 23 && value.minute <= 59 && value.second <= 59 &&
         value.utc_offset_minutes >= -14 * 60 && value.utc_offset_minutes <= 14 * 60;
}

}  // namespace

Result<std::string, Error> FilenamePolicy::make_png_name(
    const LocalDateTime& timestamp,
    const std::size_t collision_index) {
  return make_name(
      timestamp, collision_index, SaveFormat::png_display_p3_dual_range);
}

Result<std::string, Error> FilenamePolicy::make_name(
    const LocalDateTime& timestamp,
    const std::size_t collision_index,
    const SaveFormat save_format) {
  if (!valid_timestamp(timestamp)) {
    return Result<std::string, Error>::failure(invalid_input("timestamp"));
  }

  std::ostringstream name;
  name << "SeriousShot_" << std::setfill('0') << std::setw(4) << timestamp.year << '-' << std::setw(2)
       << static_cast<unsigned>(timestamp.month) << '-' << std::setw(2)
       << static_cast<unsigned>(timestamp.day) << '_' << std::setw(2)
       << static_cast<unsigned>(timestamp.hour) << '-' << std::setw(2)
       << static_cast<unsigned>(timestamp.minute) << '-' << std::setw(2)
       << static_cast<unsigned>(timestamp.second);
  if (collision_index > 0) {
    name << '_' << collision_index;
  }
  switch (save_format) {
    case SaveFormat::png_display_p3_dual_range:
      name << ".png";
      break;
    case SaveFormat::ultra_hdr_jpeg:
      name << ".jpg";
      break;
    default:
      return Result<std::string, Error>::failure(invalid_input("save_format"));
  }
  return Result<std::string, Error>::success(name.str());
}

Result<std::string, Error> FilenamePolicy::join_folder(
    const std::string& folder,
    const std::string& file_name) {
  if (folder.empty()) {
    return Result<std::string, Error>::failure(invalid_input("default_folder"));
  }
  if (file_name.empty()) {
    return Result<std::string, Error>::failure(invalid_input("file_name"));
  }
  const auto last = folder.back();
  if (last == '/' || last == '\\') {
    return Result<std::string, Error>::success(folder + file_name);
  }
  const auto separator = folder.find('\\') != std::string::npos &&
                                 folder.find('/') == std::string::npos
                             ? '\\'
                             : '/';
  return Result<std::string, Error>::success(folder + separator + file_name);
}

Result<FileReceipt, Error> DefaultSaveWorkflow::save(
    const DefaultSaveRequest& request,
    const ClockPort& clock,
    FileStorePort& file_store) {
  if (request.encoded_bytes.empty()) {
    return Result<FileReceipt, Error>::failure(invalid_input("encoded_bytes"));
  }
  if (request.maximum_collision_attempts == 0) {
    return Result<FileReceipt, Error>::failure(invalid_input("maximum_collision_attempts"));
  }

  const auto timestamp = clock.now_local();
  for (std::size_t collision_index = 0;
       collision_index < request.maximum_collision_attempts;
       ++collision_index) {
    const auto file_name = FilenamePolicy::make_name(
        timestamp, collision_index, request.save_format);
    if (!file_name) {
      return Result<FileReceipt, Error>::failure(file_name.error());
    }
    const auto exact_path = FilenamePolicy::join_folder(request.default_folder, file_name.value());
    if (!exact_path) {
      return Result<FileReceipt, Error>::failure(exact_path.error());
    }

    const auto receipt = file_store.write(
        WriteFileRequest{request.encoded_bytes, exact_path.value(), false});
    if (receipt) {
      return receipt;
    }
    if (receipt.error().code != ErrorCode::path_already_exists) {
      return Result<FileReceipt, Error>::failure(receipt.error());
    }
  }

  return Result<FileReceipt, Error>::failure(Error{
      ErrorCode::path_already_exists,
      "DefaultSaveWorkflow",
      Retryability::after_user_action,
      {{"attempts", std::to_string(request.maximum_collision_attempts)}}});
}

}  // namespace hdrshot
