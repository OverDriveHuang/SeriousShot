#pragma once
#include "core/error.hpp"
#include "core/result.hpp"
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace hdrshot::update {
struct Version {
  std::uint32_t major{}, minor{}, patch{};
  auto operator<=>(const Version &) const = default;
  static Result<Version, Error> parse(std::string_view);
  std::string text() const;
};
struct Candidate {
  Version version{}, minimum_os{};
  std::string product{"SeriousShot"}, channel, platform, architecture;
  std::string asset_url, asset_name, sha256, release_tag;
  std::uint64_t asset_size{}, release_id{}, asset_id{};
  bool draft{}, prerelease{};
  int installer_protocol{1};
  bool operator==(const Candidate &) const = default;
  std::string key() const;
};
struct Identity {
  Version version{}, os{};
  std::string channel{"stable"}, platform, architecture, install;
};
Result<std::optional<Candidate>, Error> select(const std::vector<Candidate> &, const Identity &);
enum class Phase {
  idle,
  checking,
  retry_wait,
  check_failed,
  cleaning,
  downloading,
  verifying,
  ready,
  deferred,
  preparing,
  manual_pending,
  installing,
  recovery_required,
  cleanup_blocked,
  download_failed
};
struct View {
  Phase phase{Phase::idle};
  std::optional<Candidate> candidate;
  std::uint64_t received{}, total{};
  unsigned attempts{};
  std::string detail;
  bool busy{}, prompted{};
  std::int64_t last_check{}, last_success{};
  std::uint64_t generation{};
  std::optional<Error> error;
};
class Schedule {
public:
  bool due(std::int64_t now_seconds, bool cold_start);
  void begin(std::int64_t now_seconds);
  bool retry(bool transient);
  unsigned attempts() const {
    return attempts_;
  }

private:
  std::optional<std::int64_t> anchor_;
  unsigned attempts_{};
};
} // namespace hdrshot::update
