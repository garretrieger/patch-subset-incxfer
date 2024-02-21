#include "ift/proto/format_2_patch_map.h"

#include <cstdint>

#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "common/font_helper.h"
#include "common/hb_set_unique_ptr.h"
#include "common/sparse_bit_set.h"
#include "ift/proto/IFT.pb.h"
#include "ift/proto/patch_map.h"

using absl::ClippedSubstr;
using absl::Span;
using absl::Status;
using absl::StatusOr;
using absl::StrCat;
using absl::string_view;
using common::FontHelper;
using common::hb_set_unique_ptr;
using common::make_hb_set;
using common::SparseBitSet;

#define READ_STRING(OUT, D, O, L)                                \
  string_view OUT = ClippedSubstr(D, O, L);                      \
  if (OUT.length() != L) {                                       \
    return absl::InvalidArgumentError("Not enough input data."); \
  }

#define READ_UINT8(OUT, D, OFF)                    \
  uint8_t OUT = 0;                                 \
  {                                                \
    auto v = FontHelper::ReadUInt8(D.substr(OFF)); \
    if (!v.ok()) {                                 \
      return v.status();                           \
    }                                              \
    OUT = *v;                                      \
  }

#define READ_UINT16(OUT, D, OFF)                    \
  uint16_t OUT = 0;                                 \
  {                                                 \
    auto v = FontHelper::ReadUInt16(D.substr(OFF)); \
    if (!v.ok()) {                                  \
      return v.status();                            \
    }                                               \
    OUT = *v;                                       \
  }

#define READ_UINT24(OUT, D, OFF)                    \
  uint32_t OUT = 0;                                 \
  {                                                 \
    auto v = FontHelper::ReadUInt24(D.substr(OFF)); \
    if (!v.ok()) {                                  \
      return v.status();                            \
    }                                               \
    OUT = *v;                                       \
  }

#define READ_UINT32(OUT, D, OFF)                    \
  uint32_t OUT = 0;                                 \
  {                                                 \
    auto v = FontHelper::ReadUInt16(D.substr(OFF)); \
    if (!v.ok()) {                                  \
      return v.status();                            \
    }                                               \
    OUT = *v;                                       \
  }

#define READ_INT16(OUT, D, OFF)                    \
  int16_t OUT = 0;                                 \
  {                                                \
    auto v = FontHelper::ReadInt16(D.substr(OFF)); \
    if (!v.ok()) {                                 \
      return v.status();                           \
    }                                              \
    OUT = *v;                                      \
  }

#define WRITE_UINT8(V, O, M)                     \
  if (FontHelper::WillIntOverflow<uint8_t>(V)) { \
    return absl::InvalidArgumentError(M);        \
  }                                              \
  FontHelper::WriteUInt8(V, O);

#define WRITE_UINT16(V, O, M)                     \
  if (FontHelper::WillIntOverflow<uint16_t>(V)) { \
    return absl::InvalidArgumentError(M);         \
  }                                               \
  FontHelper::WriteUInt16(V, O);

#define WRITE_INT16(V, O, M)                      \
  if (FontHelper::WillIntOverflow<uint16_t>(V)) { \
    return absl::InvalidArgumentError(M);         \
  }                                               \
  FontHelper::WriteUInt16(V, O);

namespace ift::proto {

constexpr uint8_t features_bit_mask = 1;
constexpr uint8_t design_space_bit_mask = 1 << 1;
constexpr uint8_t copy_mappings_bit_mask = 1 << 2;
constexpr uint8_t index_delta_bit_mask = 1 << 3;
constexpr uint8_t encoding_bit_mask = 1 << 4;
constexpr uint8_t codepoint_bit_mask = 1 << 5;
constexpr uint8_t ignore_bit_mask = 1 << 6;

static StatusOr<uint8_t> EncodingToInt(PatchEncoding encoding) {
  if (encoding == IFTB_ENCODING) {
    return 0;
  }

  if (encoding == SHARED_BROTLI_ENCODING) {
    return 1;
  }

  if (encoding == PER_TABLE_SHARED_BROTLI_ENCODING) {
    return 2;
  }

  return absl::InvalidArgumentError(
      StrCat("Unknown patch encoding, ", encoding));
}

static StatusOr<PatchEncoding> IntToEncoding(uint8_t value) {
  switch (value) {
    case 0:
      return IFTB_ENCODING;
    case 1:
      return SHARED_BROTLI_ENCODING;
    case 2:
      return PER_TABLE_SHARED_BROTLI_ENCODING;
    case 3:
      // fall through.
    default:
      return absl::InvalidArgumentError("Unrecognized encoding value.");
  }
}

static PatchEncoding PickDefaultEncoding(const PatchMap& patch_map) {
  uint32_t counts[3] = {0, 0, 0};
  for (const auto& e : patch_map.GetEntries()) {
    auto i = EncodingToInt(e.encoding);
    if (!i.ok()) {
      // Ignore.
      continue;
    }
    counts[*i]++;
  }

  if (counts[0] >= counts[1] && counts[0] >= counts[2]) {
    return IFTB_ENCODING;
  }

  if (counts[1] >= counts[0] && counts[1] >= counts[2]) {
    return SHARED_BROTLI_ENCODING;
  }

  return PER_TABLE_SHARED_BROTLI_ENCODING;
}

static Status EncodeEntries(Span<const PatchMap::Entry> entries, bool is_ext,
                            PatchEncoding default_encoding, std::string& out);

static Status EncodeEntry(const PatchMap::Entry& entry,
                          uint32_t last_entry_index,
                          PatchEncoding default_encoding, std::string& out);

static Status DecodeEntries(absl::string_view data, uint16_t count,
                            PatchEncoding default_encoding, PatchMap& out);
static StatusOr<absl::string_view> DecodeEntry(absl::string_view data,
                                               PatchEncoding default_encoding,
                                               PatchMap& out);

Status Format2PatchMap::Deserialize(absl::string_view data, PatchMap& out,
                                    std::string& uri_template_out) {
  constexpr int format_offset = 0;
  constexpr int id_offset = 5;
  constexpr int default_patch_encoding_offset = 9;
  constexpr int mapping_count_offset = 10;
  constexpr int mappings_field_offset = 12;
  constexpr int uri_template_length_offset = 20;
  constexpr int uri_template_offset = 22;

  READ_UINT8(format, data, format_offset);
  if (format != 2) {
    return absl::InvalidArgumentError("Invalid format number (!= 2).");
  }

  READ_UINT8(default_encoding_int, data, default_patch_encoding_offset);
  auto default_encoding = IntToEncoding(default_encoding_int);
  if (!default_encoding.ok()) {
    return default_encoding.status();
  }

  READ_UINT16(mapping_count, data, mapping_count_offset);
  READ_UINT32(mappings_offset, data, mappings_field_offset);
  auto s = DecodeEntries(ClippedSubstr(data, mappings_offset), mapping_count,
                         *default_encoding, out);

  READ_UINT16(uri_template_length, data, uri_template_length_offset);
  READ_STRING(uri_template, data, uri_template_offset, uri_template_length);
  uri_template_out = uri_template;

  return absl::OkStatus();
}

StatusOr<std::string> Format2PatchMap::Serialize(const PatchMap& patch_map,
                                                 bool is_ext,
                                                 string_view uri_template) {
  // TODO(garretrieger): prereserve estimated capacity based on patch_map.
  std::string out;

  FontHelper::WriteUInt8(0x02, out);  // Format = 2
  FontHelper::WriteUInt32(0x0, out);  // Reserved = 0x00000000

  // TODO(garretrieger): actually add the Id.
  FontHelper::WriteUInt32(0x0, out);  // Id[0] = 0x00000000
  FontHelper::WriteUInt32(0x0, out);  // Id[1] = 0x00000000
  FontHelper::WriteUInt32(0x0, out);  // Id[2] = 0x00000000
  FontHelper::WriteUInt32(0x0, out);  // Id[3] = 0x00000000

  // defaultPatchEncoding
  PatchEncoding default_encoding = PickDefaultEncoding(patch_map);
  auto encoding_value = EncodingToInt(default_encoding);
  if (!encoding_value.ok()) {
    return encoding_value.status();
  }
  FontHelper::WriteUInt8(*encoding_value, out);

  // mappingCount
  WRITE_UINT16(patch_map.GetEntries().size(), out,
               "Exceeded maximum number of entries (0xFFFF).");

  // mappings
  constexpr int header_min_length = 22;
  FontHelper::WriteUInt32(header_min_length + uri_template.length(), out);

  // idStrings
  FontHelper::WriteUInt32(0, out);

  // uriTemplateLength
  WRITE_UINT16(uri_template.length(), out,
               "Exceeded maximum uri template size (0xFFFF)");

  // uriTemplate
  out.append(uri_template);

  return EncodeEntries(patch_map.GetEntries(), is_ext, default_encoding, out);
}

Status DecodeEntries(absl::string_view data, uint16_t count,
                     PatchEncoding default_encoding, PatchMap& out) {
  for (uint32_t i = 0; i < count; i++) {
    auto s = DecodeEntry(data, default_encoding, out);
    if (!s.ok()) {
      return s.status();
    }
    data = *s;
  }
  return absl::OkStatus();
}

StatusOr<absl::string_view> DecodeEntry(absl::string_view data,
                                        PatchEncoding default_encoding,
                                        uint32_t& entry_index, PatchMap& out) {
  if (data.empty()) {
    return absl::InvalidArgumentError(
        "Not enough input data to decode mapping entry.");
  }

  PatchMap::Coverage coverage;

  READ_UINT8(format, data, 0);
  uint32_t offset = 1;

  if (format & features_bit_mask) {
    READ_UINT8(feature_count, data, offset++);
    for (unsigned i = 0; i < feature_count; i++) {
      READ_UINT32(next_tag, data, offset);
      coverage.features.insert(next_tag);
      offset += 4;
    }
  }

  if (format & design_space_bit_mask) {
    READ_UINT16(segment_count, data, offset);
    offset += 2 + segment_count * 12;
    // TODO(garretrieger): read in segments.
  }

  if (format & copy_mappings_bit_mask) {
    READ_UINT16(copy_count, data, offset);
    offset += 2 + copy_count * 2;
    // TODO(garretrieger): read in copies
  }

  entry_index++;
  if (format & index_delta_bit_mask) {
    READ_INT16(delta, data, offset);
    entry_index += delta;
    offset += 2;
  }

  if (format & encoding_bit_mask) {
    READ_UINT8(encoding_int, data, offset++);
    auto new_encoding = IntToEncoding(encoding_int);
    if (!new_encoding.ok()) {
      return new_encoding.status();
    }
    default_encoding = *new_encoding;
  }

  if (format & codepoint_bit_mask) {
    READ_UINT24(bias, data, offset);
    offset += 3;
    hb_set_unique_ptr codepoint_set = make_hb_set();
    auto s = SparseBitSet::Decode(data.substr(offset), codepoint_set.get());
    // TODO(garretrieger): have decode report how much of 'data' it consumed.
    //                     Advance 'offset' as needed.
    if (!s.ok()) {
      return s;
    }

    hb_codepoint_t cp = HB_SET_VALUE_INVALID;
    while (hb_set_next(codepoint_set.get(), &cp)) {
      coverage.codepoints.insert(cp + bias);
    }
  }

  if (!(format & ignore_bit_mask)) {
    out.AddEntry(coverage, entry_index, default_encoding);
  }

  return absl::OkStatus();
}

Status EncodeEntries(Span<const PatchMap::Entry> entries, bool is_ext,
                     PatchEncoding default_encoding, std::string& out) {
  // TODO(garretrieger): identify and copy existing entries when possible.
  uint32_t last_entry_index = 0;
  for (const auto& entry : entries) {
    if (entry.extension_entry != is_ext) {
      continue;
    }

    auto s = EncodeEntry(entry, last_entry_index, default_encoding, out);
    if (!s.ok()) {
      return s;
    }
    last_entry_index = entry.patch_index;
  }

  return absl::OkStatus();
}

Status EncodeEntry(const PatchMap::Entry& entry, uint32_t last_entry_index,
                   PatchEncoding default_encoding, std::string& out) {
  const auto& coverage = entry.coverage;
  bool has_codepoints = !coverage.codepoints.empty();
  bool has_features = !coverage.features.empty();
  bool has_design_space = !coverage.design_space.empty();
  int64_t delta = ((int64_t)entry.patch_index) - ((int64_t)last_entry_index);
  bool has_delta = delta != 1;
  bool has_patch_encoding = entry.encoding != default_encoding;

  // format
  uint8_t format = (has_features ? features_bit_mask : 0) |
                   (has_design_space ? design_space_bit_mask : 0) |
                   // not set, has copy mapping indices (bit 2)
                   (has_delta ? index_delta_bit_mask : 0) |
                   (has_patch_encoding ? encoding_bit_mask : 0) |
                   (has_codepoints ? codepoint_bit_mask : 0);
  // not set, ignore (bit 6)
  FontHelper::WriteUInt8(format, out);

  if (has_features) {
    WRITE_UINT8(coverage.features.size(), out,
                "Exceed max number of feature tags (0xFF).");
    for (hb_tag_t tag : coverage.features) {
      FontHelper::WriteUInt32(tag, out);
    }
  }

  if (has_design_space) {
    // TODO(garretrieger): implement me.
    return absl::UnimplementedError("Not implemented yet.");
  }

  if (has_delta) {
    WRITE_INT16(delta, out, "Exceed max entry index delta (int16).");
  }

  if (has_patch_encoding) {
    auto encoding_value = EncodingToInt(entry.encoding);
    if (!encoding_value.ok()) {
      return encoding_value.status();
    }
    FontHelper::WriteUInt8(*encoding_value, out);
  }

  if (has_codepoints) {
    uint32_t bias = coverage.SmallestCodepoint();
    hb_set_unique_ptr biased_set = make_hb_set();
    for (uint32_t cp : coverage.codepoints) {
      hb_set_add(biased_set.get(), cp - bias);
    }

    std::string sparse_bit_set = SparseBitSet::Encode(*biased_set);
    out.append(sparse_bit_set);
  }

  return absl::OkStatus();
}

}  // namespace ift::proto