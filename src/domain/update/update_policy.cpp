#include "domain/update/update_policy.hpp"
#include <charconv>
#include <limits>
namespace hdrshot::update {
Result<Version, Error> Version::parse(std::string_view input) {
  Version result;
  std::uint32_t *fields[]{&result.major, &result.minor, &result.patch};
  for (unsigned n = 0; n < 3; ++n) {
    const auto dot = input.find('.');
    auto segment = input.substr(0, dot);
    if (segment.empty() || (segment.size() > 1 && segment.front() == '0') ||
        (n < 2 && dot == input.npos) || (n == 2 && dot != input.npos))
      return Result<Version, Error>::failure(
          {ErrorCode::invalid_input, "update.version", Retryability::never, {}});
    for (const auto ch : segment)
      if (ch < '0' || ch > '9')
        return Result<Version, Error>::failure(
            {ErrorCode::invalid_input, "update.version", Retryability::never, {}});
    auto parsed = std::from_chars(segment.data(), segment.data() + segment.size(), *fields[n]);
    if (parsed.ec != std::errc{} || parsed.ptr != segment.data() + segment.size())
      return Result<Version, Error>::failure(
          {ErrorCode::invalid_input, "update.version", Retryability::never, {}});
    if (n < 2)
      input.remove_prefix(dot + 1);
  }
  return Result<Version, Error>::success(result);
}
std::string Version::text() const {
  return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
}
std::string Candidate::key() const {
  return product + "|" + channel + "|" + platform + "|" + architecture + "|" + version.text() +
         "|" + sha256;
}
Result<std::optional<Candidate>, Error> select(const std::vector<Candidate> &candidates,
                                               const Identity &identity) {
  std::optional<Candidate> best;
  for (const auto &c : candidates) {
    if (c.product != "SeriousShot" || c.channel != identity.channel ||
        c.platform != identity.platform || c.architecture != identity.architecture || c.draft ||
        c.prerelease || c.minimum_os > identity.os)
      continue;
    if (c.installer_protocol != 1 || c.sha256.size() != 64 || c.asset_size == 0 ||
        c.asset_size > 1073741824ULL || c.release_id == 0 || c.asset_id == 0 ||
        c.release_tag != "v" + c.version.text())
      return Result<std::optional<Candidate>, Error>::failure(
          {ErrorCode::invalid_input, "update.candidate", Retryability::never, {}});
    for (const auto ch : c.sha256)
      if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')))
        return Result<std::optional<Candidate>, Error>::failure(
            {ErrorCode::invalid_input, "update.digest", Retryability::never, {}});
    if (!best || c.version > best->version)
      best = c;
    else if (c.version == best->version && c != *best)
      return Result<std::optional<Candidate>, Error>::failure(
          {ErrorCode::metadata_conflict, "update.candidate", Retryability::never, {}});
  }
  return Result<std::optional<Candidate>, Error>::success(best);
}
bool Schedule::due(std::int64_t now, bool cold) {
  return cold || !anchor_ || now < *anchor_ || now - *anchor_ >= 7 * 24 * 60 * 60;
}
void Schedule::begin(std::int64_t now) {
  anchor_ = now;
  attempts_ = 1;
}
bool Schedule::retry(bool transient) {
  if (!transient || attempts_ >= 3)
    return false;
  ++attempts_;
  return true;
}
} // namespace hdrshot::update
