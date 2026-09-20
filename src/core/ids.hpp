#pragma once

#include <compare>
#include <cstdint>

namespace hdrshot {

template <typename Tag>
struct StrongId {
  std::uint64_t value{};
  friend bool operator==(const StrongId&, const StrongId&) = default;
  friend auto operator<=>(const StrongId&, const StrongId&) = default;
};

struct SessionIdTag;
struct OperationIdTag;
struct FrameIdTag;
struct ObjectIdTag;
struct DisplayIdTag;

using SessionId = StrongId<SessionIdTag>;
using OperationId = StrongId<OperationIdTag>;
using FrameId = StrongId<FrameIdTag>;
using ObjectId = StrongId<ObjectIdTag>;
using DisplayId = StrongId<DisplayIdTag>;
using DisplayGeneration = std::uint64_t;
using DocumentRevision = std::uint64_t;
using SelectionRevision = std::uint64_t;

}  // namespace hdrshot
