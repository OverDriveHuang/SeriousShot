#pragma once
#include "domain/update/update_policy.hpp"
#include <functional>
namespace hdrshot::update {
using Done = std::function<void(Result<bool, Error>)>;
struct Receipt {
  std::string key;
  std::uint64_t generation{};
};
struct Checked {
  std::optional<Candidate> candidate;
  std::optional<Candidate> incompatible;
};
class Effects {
public:
  virtual ~Effects() = default;
  virtual void check(const Identity &, std::function<void(Result<Checked, Error>)>) = 0;
  virtual void delay(unsigned seconds, std::function<void()>) = 0;
  virtual void acquire_and_download(const Candidate &,
                                    std::function<void(std::uint64_t, std::uint64_t)>,
                                    std::function<void(Result<Receipt, Error>)>) = 0;
  virtual void cancel_download(Done) = 0;
  virtual void persist_prompt(const std::string &, Done) = 0;
  virtual bool was_prompted(const std::string &) const = 0;
  virtual bool busy() const = 0;
  virtual void ask_install(const Candidate &, std::function<void(bool)>) = 0;
  virtual void install(const Candidate &, const Receipt &, Done) = 0;
  virtual bool manual_install() const = 0;
};
} // namespace hdrshot::update
