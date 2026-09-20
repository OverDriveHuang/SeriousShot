#pragma once

#include <utility>
#include <variant>

namespace hdrshot {

template <typename T, typename E>
class Result {
 public:
  static Result success(T value) { return Result(std::move(value)); }
  static Result failure(E error) { return Result(std::move(error)); }

  [[nodiscard]] bool has_value() const noexcept { return std::holds_alternative<T>(storage_); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const T& value() const { return std::get<T>(storage_); }
  [[nodiscard]] T& value() { return std::get<T>(storage_); }
  [[nodiscard]] const E& error() const { return std::get<E>(storage_); }

 private:
  explicit Result(T value) : storage_(std::move(value)) {}
  explicit Result(E error) : storage_(std::move(error)) {}

  std::variant<T, E> storage_;
};

}  // namespace hdrshot
