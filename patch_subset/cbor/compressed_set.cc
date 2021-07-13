#include "patch_subset/cbor/compressed_set.h"

#include "patch_subset/cbor/cbor_utils.h"
#include "patch_subset/cbor/compressed_int_list.h"

namespace patch_subset::cbor {

using std::string;

CompressedSet::CompressedSet()
    : _sparse_bit_set_bytes(std::nullopt), _ranges(std::nullopt) {}

CompressedSet::CompressedSet(string_view sparse_bit_set_bytes,
                             const range_vector& ranges)
    : _sparse_bit_set_bytes(sparse_bit_set_bytes), _ranges(ranges) {}

StatusCode CompressedSet::Decode(const cbor_item_t& cbor_map,
                                 CompressedSet& out) {
  if (!cbor_isa_map(&cbor_map) || cbor_map_is_indefinite(&cbor_map)) {
    return StatusCode::kInvalidArgument;
  }
  CompressedSet result;
  StatusCode sc = CborUtils::GetBytesField(cbor_map, kSparseBitSetFieldNumber,
                                           result._sparse_bit_set_bytes);
  if (sc != StatusCode::kOk) {
    return StatusCode::kInvalidArgument;
  }
  sc = CompressedRangeList::GetRangeListField(
      cbor_map, kSRangeDeltasFieldNumber, result._ranges);
  if (sc != StatusCode::kOk) {
    return StatusCode::kInvalidArgument;
  }
  out = result;
  return StatusCode::kOk;
}

StatusCode CompressedSet::Encode(cbor_item_unique_ptr& map_out) const {
  int size = (_sparse_bit_set_bytes.has_value() ? 1 : 0) +
             (_ranges.has_value() ? 1 : 0);
  cbor_item_unique_ptr map = make_cbor_map(size);
  StatusCode sc = CborUtils::SetBytesField(*map, kSparseBitSetFieldNumber,
                                           _sparse_bit_set_bytes);
  if (sc != StatusCode::kOk) {
    return StatusCode::kInvalidArgument;
  }
  sc = CompressedRangeList::SetRangeListField(*map, kSRangeDeltasFieldNumber,
                                              _ranges);
  if (sc != StatusCode::kOk) {
    return StatusCode::kInvalidArgument;
  }
  map_out.swap(map);
  return StatusCode::kOk;
}

StatusCode CompressedSet::SetCompressedSetField(
    cbor_item_t& map, int field_number,
    const optional<CompressedSet>& compressed_set) {
  if (!compressed_set.has_value()) {
    return StatusCode::kOk;  // Nothing to do.
  }
  cbor_item_unique_ptr field_value = empty_cbor_ptr();
  StatusCode sc = compressed_set.value().Encode(field_value);
  if (sc != StatusCode::kOk) {
    return sc;
  }
  return CborUtils::SetField(map, field_number, move_out(field_value));
}

StatusCode CompressedSet::GetCompressedSetField(const cbor_item_t& map,
                                                int field_number,
                                                optional<CompressedSet>& out) {
  cbor_item_unique_ptr field = empty_cbor_ptr();
  StatusCode sc = CborUtils::GetField(map, field_number, field);
  if (sc == StatusCode::kNotFound) {
    out.reset();
    return StatusCode::kOk;
  } else if (sc != StatusCode::kOk) {
    return StatusCode::kInvalidArgument;
  }
  CompressedSet results;
  sc = Decode(*field, results);
  if (sc != StatusCode::kOk) {
    return StatusCode::kInvalidArgument;
  }
  out.emplace(results);
  return StatusCode::kOk;
}

bool CompressedSet::HasSparseBitSetBytes() const {
  return _sparse_bit_set_bytes.has_value();
}
CompressedSet& CompressedSet::SetSparseBitSetBytes(const string& bytes) {
  _sparse_bit_set_bytes.emplace(bytes);
  return *this;
}
CompressedSet& CompressedSet::ResetSparseBitSetBytes() {
  _sparse_bit_set_bytes.reset();
  return *this;
}
string CompressedSet::SparseBitSetBytes() const {
  return _sparse_bit_set_bytes.has_value() ? _sparse_bit_set_bytes.value() : "";
}

bool CompressedSet::HasRanges() const { return _ranges.has_value(); }
CompressedSet& CompressedSet::SetRanges(range_vector ranges) {
  _ranges.emplace(ranges);
  return *this;
}
CompressedSet& CompressedSet::ResetRanges() {
  _ranges.reset();
  return *this;
}
range_vector CompressedSet::Ranges() const {
  return _ranges.has_value() ? _ranges.value() : range_vector();
}

bool CompressedSet::operator==(const CompressedSet& other) const {
  return _sparse_bit_set_bytes == other._sparse_bit_set_bytes &&
  _ranges == other._ranges;
}
bool CompressedSet::operator!=(const CompressedSet& other) const {
  return !(*this == other);
}

}  // namespace patch_subset::cbor
