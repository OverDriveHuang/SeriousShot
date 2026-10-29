#pragma once
namespace hdrshot {
// Owner-thread diagnostic seam for deterministic resource failure/recovery
// tests. Normal operation never sets a failure; snapshots remain immutable.
enum class MacAnnotationPackFailure { none, allocation, encoder, completion };
inline thread_local MacAnnotationPackFailure mac_annotation_pack_failure =
    MacAnnotationPackFailure::none;
inline bool
consume_mac_annotation_pack_failure(MacAnnotationPackFailure point) {
  if (mac_annotation_pack_failure != point)
    return false;
  mac_annotation_pack_failure = MacAnnotationPackFailure::none;
  return true;
}
} // namespace hdrshot
