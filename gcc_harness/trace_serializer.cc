#include "gcc_harness/trace_serializer.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/str_format.h"
#include "absl/types/span.h"
#include "core/mips.h"
#include "core/round_trip_test_cases.h"
#include "core/rtl_cfg.h"
#include "core/rtl_expr.h"
#include "gcc_harness/function_hooks.h"

namespace rom_nom_nom::gcc_harness {
namespace {

constexpr uint64_t kFnv1aOffsetBasis = 14695981039346656037ull;
constexpr uint64_t kFnv1aPrime = 1099511628211ull;

void MixFnvBytes(uint64_t* hash, std::string_view bytes) {
  for (unsigned char c : bytes) {
    *hash ^= static_cast<uint64_t>(c);
    *hash *= kFnv1aPrime;
  }
}

void MixFnvU64(uint64_t* hash, uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    *hash ^= (value >> (i * 8)) & 0xffu;
    *hash *= kFnv1aPrime;
  }
}

void MixFnvString(uint64_t* hash, std::string_view text) {
  MixFnvU64(hash, static_cast<uint64_t>(text.size()));
  MixFnvBytes(hash, text);
}

class BinaryWriter {
 public:
  void Reserve(size_t bytes) { buf_.reserve(bytes); }

  void WriteU8(uint8_t v) { buf_.push_back(static_cast<char>(v)); }

  void WriteU16(uint16_t v) {
    WriteU8(static_cast<uint8_t>(v & 0xffu));
    WriteU8(static_cast<uint8_t>((v >> 8) & 0xffu));
  }

  void WriteI16(int16_t v) { WriteU16(static_cast<uint16_t>(v)); }

  void WriteU32(uint32_t v) {
    WriteU8(static_cast<uint8_t>(v & 0xffu));
    WriteU8(static_cast<uint8_t>((v >> 8) & 0xffu));
    WriteU8(static_cast<uint8_t>((v >> 16) & 0xffu));
    WriteU8(static_cast<uint8_t>((v >> 24) & 0xffu));
  }

  void WriteI32(int32_t v) { WriteU32(static_cast<uint32_t>(v)); }

  void WriteU64(uint64_t v) {
    WriteU32(static_cast<uint32_t>(v & 0xffffffffu));
    WriteU32(static_cast<uint32_t>((v >> 32) & 0xffffffffu));
  }

  void WriteI64(int64_t v) { WriteU64(static_cast<uint64_t>(v)); }

  void WriteRawBytes(std::string_view bytes) { buf_.append(bytes.data(), bytes.size()); }

  void WriteLengthPrefixedBytes(std::string_view bytes) {
    WriteU32(static_cast<uint32_t>(bytes.size()));
    WriteRawBytes(bytes);
  }

  const std::string& Bytes() const { return buf_; }
  std::string TakeBytes() { return std::move(buf_); }

 private:
  std::string buf_;
};

class BinaryReader {
 public:
  explicit BinaryReader(std::string_view bytes)
      : data_(reinterpret_cast<const uint8_t*>(bytes.data())), size_(bytes.size()) {}

  size_t RemainingBytes() const { return size_ - pos_; }

  absl::Status ReadU8(uint8_t* out) {
    if (RemainingBytes() < 1) {
      return absl::InvalidArgumentError("Unexpected EOF reading uint8");
    }
    *out = data_[pos_++];
    return absl::OkStatus();
  }

  absl::Status ReadU16(uint16_t* out) {
    if (RemainingBytes() < 2) {
      return absl::InvalidArgumentError("Unexpected EOF reading uint16");
    }
    *out = static_cast<uint16_t>(data_[pos_]) | (static_cast<uint16_t>(data_[pos_ + 1]) << 8);
    pos_ += 2;
    return absl::OkStatus();
  }

  absl::Status ReadI16(int16_t* out) {
    uint16_t raw = 0;
    if (absl::Status st = ReadU16(&raw); !st.ok()) {
      return st;
    }
    *out = static_cast<int16_t>(raw);
    return absl::OkStatus();
  }

  absl::Status ReadU32(uint32_t* out) {
    if (RemainingBytes() < 4) {
      return absl::InvalidArgumentError("Unexpected EOF reading uint32");
    }
    *out = static_cast<uint32_t>(data_[pos_]) | (static_cast<uint32_t>(data_[pos_ + 1]) << 8) |
           (static_cast<uint32_t>(data_[pos_ + 2]) << 16) |
           (static_cast<uint32_t>(data_[pos_ + 3]) << 24);
    pos_ += 4;
    return absl::OkStatus();
  }

  absl::Status ReadI32(int32_t* out) {
    uint32_t raw = 0;
    if (absl::Status st = ReadU32(&raw); !st.ok()) {
      return st;
    }
    *out = static_cast<int32_t>(raw);
    return absl::OkStatus();
  }

  absl::Status ReadU64(uint64_t* out) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (absl::Status st = ReadU32(&lo); !st.ok()) {
      return st;
    }
    if (absl::Status st = ReadU32(&hi); !st.ok()) {
      return st;
    }
    *out = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return absl::OkStatus();
  }

  absl::Status ReadI64(int64_t* out) {
    uint64_t raw = 0;
    if (absl::Status st = ReadU64(&raw); !st.ok()) {
      return st;
    }
    *out = static_cast<int64_t>(raw);
    return absl::OkStatus();
  }

  absl::Status ReadLengthPrefixedSlice(std::string_view* out) {
    uint32_t len = 0;
    if (absl::Status st = ReadU32(&len); !st.ok()) {
      return st;
    }
    if (RemainingBytes() < len) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Unexpected EOF reading %u-byte slice", len));
    }
    *out = std::string_view(reinterpret_cast<const char*>(data_ + pos_), len);
    pos_ += len;
    return absl::OkStatus();
  }

 private:
  const uint8_t* data_ = nullptr;
  size_t size_ = 0;
  size_t pos_ = 0;
};

struct TraceWritePools {
  std::vector<std::string> strings;
  absl::flat_hash_map<std::string, uint32_t> string_to_id;
  std::vector<std::string> cfg_blobs;
  absl::flat_hash_map<std::string, uint32_t> cfg_blob_to_id;

  uint32_t InternString(std::string_view s) {
    auto it = string_to_id.find(s);
    if (it != string_to_id.end()) {
      return it->second;
    }
    const uint32_t id = static_cast<uint32_t>(strings.size());
    strings.emplace_back(s);
    string_to_id.emplace(strings.back(), id);
    return id;
  }
};

struct TraceReadPools {
  std::vector<std::string> strings;
  std::vector<RtlControlFlowGraph> cfgs;

  absl::Status ReadInternedString(BinaryReader* reader, std::string* out) const {
    uint32_t id = 0;
    if (absl::Status st = reader->ReadU32(&id); !st.ok()) {
      return st;
    }
    if (id >= strings.size()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Out-of-range string pool index %u (pool size %zu)", id, strings.size()));
    }
    *out = strings[id];
    return absl::OkStatus();
  }

  absl::Status ReadInternedCfg(BinaryReader* reader, RtlControlFlowGraph* out) const {
    uint32_t id = 0;
    if (absl::Status st = reader->ReadU32(&id); !st.ok()) {
      return st;
    }
    if (id >= cfgs.size()) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Out-of-range CFG pool index %u (pool size %zu)", id, cfgs.size()));
    }
    *out = cfgs[id];
    return absl::OkStatus();
  }
};

void WriteStringVec(BinaryWriter* w, TraceWritePools* pools, absl::Span<const std::string> vec) {
  w->WriteU32(static_cast<uint32_t>(vec.size()));
  for (const std::string& s : vec) {
    w->WriteU32(pools->InternString(s));
  }
}

absl::Status ReadStringVec(BinaryReader* r, const TraceReadPools& pools,
                           std::vector<std::string>* out) {
  uint32_t count = 0;
  if (absl::Status st = r->ReadU32(&count); !st.ok()) {
    return st;
  }
  if (count > r->RemainingBytes() / 4) {
    return absl::InvalidArgumentError("String vector count exceeds remaining buffer");
  }
  out->clear();
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (absl::Status st = pools.ReadInternedString(r, &(*out)[i]); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

void WriteU32Vec(BinaryWriter* w, absl::Span<const uint32_t> vec) {
  w->WriteU32(static_cast<uint32_t>(vec.size()));
  for (uint32_t v : vec) {
    w->WriteU32(v);
  }
}

absl::Status ReadU32Vec(BinaryReader* r, std::vector<uint32_t>* out) {
  uint32_t count = 0;
  if (absl::Status st = r->ReadU32(&count); !st.ok()) {
    return st;
  }
  if (count > r->RemainingBytes() / 4) {
    return absl::InvalidArgumentError("uint32 vector count exceeds remaining buffer");
  }
  out->clear();
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (absl::Status st = r->ReadU32(&(*out)[i]); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

void WriteI32Vec(BinaryWriter* w, absl::Span<const int> vec) {
  w->WriteU32(static_cast<uint32_t>(vec.size()));
  for (int v : vec) {
    w->WriteI32(static_cast<int32_t>(v));
  }
}

absl::Status ReadI32Vec(BinaryReader* r, std::vector<int>* out) {
  uint32_t count = 0;
  if (absl::Status st = r->ReadU32(&count); !st.ok()) {
    return st;
  }
  if (count > r->RemainingBytes() / 4) {
    return absl::InvalidArgumentError("int32 vector count exceeds remaining buffer");
  }
  out->clear();
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    int32_t v = 0;
    if (absl::Status st = r->ReadI32(&v); !st.ok()) {
      return st;
    }
    (*out)[i] = static_cast<int>(v);
  }
  return absl::OkStatus();
}

void WriteSizeVec(BinaryWriter* w, absl::Span<const size_t> vec) {
  w->WriteU32(static_cast<uint32_t>(vec.size()));
  for (size_t v : vec) {
    w->WriteU64(static_cast<uint64_t>(v));
  }
}

absl::Status ReadSizeVec(BinaryReader* r, std::vector<size_t>* out) {
  uint32_t count = 0;
  if (absl::Status st = r->ReadU32(&count); !st.ok()) {
    return st;
  }
  if (count > r->RemainingBytes() / 8) {
    return absl::InvalidArgumentError("size_t vector count exceeds remaining buffer");
  }
  out->clear();
  out->resize(count);
  for (uint32_t i = 0; i < count; ++i) {
    uint64_t v = 0;
    if (absl::Status st = r->ReadU64(&v); !st.ok()) {
      return st;
    }
    (*out)[i] = static_cast<size_t>(v);
  }
  return absl::OkStatus();
}

void WriteStringU32Map(BinaryWriter* w, TraceWritePools* pools,
                       const absl::flat_hash_map<std::string, uint32_t>& map) {
  std::vector<std::pair<std::string_view, uint32_t>> entries;
  entries.reserve(map.size());
  for (const auto& [k, v] : map) {
    entries.emplace_back(k, v);
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  w->WriteU32(static_cast<uint32_t>(entries.size()));
  for (const auto& [k, v] : entries) {
    w->WriteU32(pools->InternString(k));
    w->WriteU32(v);
  }
}

absl::Status ReadStringU32Map(BinaryReader* r, const TraceReadPools& pools,
                              absl::flat_hash_map<std::string, uint32_t>* out) {
  uint32_t count = 0;
  if (absl::Status st = r->ReadU32(&count); !st.ok()) {
    return st;
  }
  if (count > r->RemainingBytes() / 8) {
    return absl::InvalidArgumentError("String->uint32 map count exceeds remaining buffer");
  }
  out->clear();
  out->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    std::string key;
    uint32_t val = 0;
    if (absl::Status st = pools.ReadInternedString(r, &key); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadU32(&val); !st.ok()) {
      return st;
    }
    (*out)[std::move(key)] = val;
  }
  return absl::OkStatus();
}

void WriteIntIntMap(BinaryWriter* w, const absl::flat_hash_map<int, int>& map) {
  std::vector<std::pair<int, int>> entries(map.begin(), map.end());
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  w->WriteU32(static_cast<uint32_t>(entries.size()));
  for (const auto& [k, v] : entries) {
    w->WriteI32(static_cast<int32_t>(k));
    w->WriteI32(static_cast<int32_t>(v));
  }
}

absl::Status ReadIntIntMap(BinaryReader* r, absl::flat_hash_map<int, int>* out) {
  uint32_t count = 0;
  if (absl::Status st = r->ReadU32(&count); !st.ok()) {
    return st;
  }
  if (count > r->RemainingBytes() / 8) {
    return absl::InvalidArgumentError("int->int map count exceeds remaining buffer");
  }
  out->clear();
  out->reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    int32_t k = 0;
    int32_t v = 0;
    if (absl::Status st = r->ReadI32(&k); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI32(&v); !st.ok()) {
      return st;
    }
    (*out)[static_cast<int>(k)] = static_cast<int>(v);
  }
  return absl::OkStatus();
}

void WriteInstruction(BinaryWriter* w, const Instruction& inst) {
  w->WriteU32(inst.raw_word);
  w->WriteU32(inst.vram);
  w->WriteU16(static_cast<uint16_t>(inst.opcode));
  const uint8_t mask =
      (inst.rd.has_value() ? 0x01u : 0u) | (inst.rs.has_value() ? 0x02u : 0u) |
      (inst.rt.has_value() ? 0x04u : 0u) | (inst.unscheduled_clobber_reg.has_value() ? 0x08u : 0u) |
      (inst.fd.has_value() ? 0x10u : 0u) | (inst.fs.has_value() ? 0x20u : 0u) |
      (inst.ft.has_value() ? 0x40u : 0u) | (inst.was_unscheduled_delay_slot ? 0x80u : 0u);
  w->WriteU8(mask);
  if (inst.rd.has_value()) {
    w->WriteU8(static_cast<uint8_t>(*inst.rd));
  }
  if (inst.rs.has_value()) {
    w->WriteU8(static_cast<uint8_t>(*inst.rs));
  }
  if (inst.rt.has_value()) {
    w->WriteU8(static_cast<uint8_t>(*inst.rt));
  }
  if (inst.unscheduled_clobber_reg.has_value()) {
    w->WriteU8(static_cast<uint8_t>(*inst.unscheduled_clobber_reg));
  }
  if (inst.fd.has_value()) {
    w->WriteU8(static_cast<uint8_t>(*inst.fd));
  }
  if (inst.fs.has_value()) {
    w->WriteU8(static_cast<uint8_t>(*inst.fs));
  }
  if (inst.ft.has_value()) {
    w->WriteU8(static_cast<uint8_t>(*inst.ft));
  }
  w->WriteI16(inst.immediate);
  w->WriteU8(inst.shift_amount);
  w->WriteU32(inst.target);
}

absl::Status ReadInstruction(BinaryReader* r, Instruction* out) {
  *out = Instruction{};
  uint16_t opcode_raw = 0;
  uint8_t mask = 0;
  if (absl::Status st = r->ReadU32(&out->raw_word); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU32(&out->vram); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU16(&opcode_raw); !st.ok()) {
    return st;
  }
  out->opcode = static_cast<Opcode>(opcode_raw);
  if (absl::Status st = r->ReadU8(&mask); !st.ok()) {
    return st;
  }
  out->was_unscheduled_delay_slot = (mask & 0x80u) != 0;
  auto read_opt_gpr = [&](uint8_t bit, std::optional<Register>* dst) -> absl::Status {
    if ((mask & bit) == 0) {
      return absl::OkStatus();
    }
    uint8_t reg_raw = 0;
    if (absl::Status st = r->ReadU8(&reg_raw); !st.ok()) {
      return st;
    }
    *dst = static_cast<Register>(reg_raw);
    return absl::OkStatus();
  };
  auto read_opt_fpr = [&](uint8_t bit, std::optional<FpRegister>* dst) -> absl::Status {
    if ((mask & bit) == 0) {
      return absl::OkStatus();
    }
    uint8_t reg_raw = 0;
    if (absl::Status st = r->ReadU8(&reg_raw); !st.ok()) {
      return st;
    }
    *dst = static_cast<FpRegister>(reg_raw);
    return absl::OkStatus();
  };
  if (absl::Status st = read_opt_gpr(0x01u, &out->rd); !st.ok()) {
    return st;
  }
  if (absl::Status st = read_opt_gpr(0x02u, &out->rs); !st.ok()) {
    return st;
  }
  if (absl::Status st = read_opt_gpr(0x04u, &out->rt); !st.ok()) {
    return st;
  }
  if (absl::Status st = read_opt_gpr(0x08u, &out->unscheduled_clobber_reg); !st.ok()) {
    return st;
  }
  if (absl::Status st = read_opt_fpr(0x10u, &out->fd); !st.ok()) {
    return st;
  }
  if (absl::Status st = read_opt_fpr(0x20u, &out->fs); !st.ok()) {
    return st;
  }
  if (absl::Status st = read_opt_fpr(0x40u, &out->ft); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadI16(&out->immediate); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU8(&out->shift_amount); !st.ok()) {
    return st;
  }
  return r->ReadU32(&out->target);
}

void WriteRtlExpr(BinaryWriter* w, TraceWritePools* pools, const RtlExpr& expr) {
  w->WriteU32(static_cast<uint32_t>(expr.op));
  w->WriteU32(static_cast<uint32_t>(expr.mode));
  const uint8_t flags =
      (expr.flags.in_struct ? 0x01u : 0u) | (expr.flags.volatil ? 0x02u : 0u) |
      (expr.flags.unchanging ? 0x04u : 0u) | (expr.flags.integrated ? 0x08u : 0u) |
      (expr.flags.frame_related ? 0x10u : 0u) | (expr.symbol_paren_wrapped ? 0x20u : 0u) |
      (expr.bracketed_children ? 0x40u : 0u);
  w->WriteU8(flags);
  w->WriteI64(expr.int_val);
  w->WriteI64(expr.int_val_high);
  w->WriteU32(pools->InternString(expr.str_val));
  w->WriteU32(pools->InternString(expr.raw_op));
  w->WriteU32(pools->InternString(expr.raw_mode));

  w->WriteU32(static_cast<uint32_t>(expr.extra_ints.size()));
  for (int64_t val : expr.extra_ints) {
    w->WriteI64(val);
  }
  w->WriteU32(static_cast<uint32_t>(expr.children.size()));
  for (const RtlExpr& child : expr.children) {
    WriteRtlExpr(w, pools, child);
  }
  w->WriteU32(static_cast<uint32_t>(expr.generic_items.size()));
  for (const RtlExpr::GenericItem& item : expr.generic_items) {
    w->WriteU32(static_cast<uint32_t>(item.kind));
    w->WriteU64(static_cast<uint64_t>(item.child_index));
    w->WriteU64(static_cast<uint64_t>(item.child_count));
    w->WriteI64(item.int_val);
    w->WriteU32(pools->InternString(item.text));
  }
}

absl::Status ReadRtlExpr(BinaryReader* r, const TraceReadPools& pools, RtlExpr* out) {
  *out = RtlExpr{};
  uint32_t op_raw = 0;
  uint32_t mode_raw = 0;
  uint8_t flags = 0;
  if (absl::Status st = r->ReadU32(&op_raw); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU32(&mode_raw); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU8(&flags); !st.ok()) {
    return st;
  }
  out->op = static_cast<RtlOp>(op_raw);
  out->mode = static_cast<RtlMachineMode>(mode_raw);
  out->flags.in_struct = (flags & 0x01u) != 0;
  out->flags.volatil = (flags & 0x02u) != 0;
  out->flags.unchanging = (flags & 0x04u) != 0;
  out->flags.integrated = (flags & 0x08u) != 0;
  out->flags.frame_related = (flags & 0x10u) != 0;
  out->symbol_paren_wrapped = (flags & 0x20u) != 0;
  out->bracketed_children = (flags & 0x40u) != 0;

  if (absl::Status st = r->ReadI64(&out->int_val); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadI64(&out->int_val_high); !st.ok()) {
    return st;
  }
  if (absl::Status st = pools.ReadInternedString(r, &out->str_val); !st.ok()) {
    return st;
  }
  if (absl::Status st = pools.ReadInternedString(r, &out->raw_op); !st.ok()) {
    return st;
  }
  if (absl::Status st = pools.ReadInternedString(r, &out->raw_mode); !st.ok()) {
    return st;
  }

  uint32_t extra_count = 0;
  if (absl::Status st = r->ReadU32(&extra_count); !st.ok()) {
    return st;
  }
  if (extra_count > r->RemainingBytes() / 8) {
    return absl::InvalidArgumentError("RtlExpr extra_ints count exceeds remaining buffer");
  }
  out->extra_ints.resize(extra_count);
  for (uint32_t i = 0; i < extra_count; ++i) {
    if (absl::Status st = r->ReadI64(&out->extra_ints[i]); !st.ok()) {
      return st;
    }
  }

  uint32_t child_count = 0;
  if (absl::Status st = r->ReadU32(&child_count); !st.ok()) {
    return st;
  }
  if (child_count > r->RemainingBytes() / 4) {
    return absl::InvalidArgumentError("RtlExpr children count exceeds remaining buffer");
  }
  out->children.resize(child_count);
  for (uint32_t i = 0; i < child_count; ++i) {
    if (absl::Status st = ReadRtlExpr(r, pools, &out->children[i]); !st.ok()) {
      return st;
    }
  }

  uint32_t generic_count = 0;
  if (absl::Status st = r->ReadU32(&generic_count); !st.ok()) {
    return st;
  }
  if (generic_count > r->RemainingBytes() / 16) {
    return absl::InvalidArgumentError("RtlExpr generic_items count exceeds remaining buffer");
  }
  out->generic_items.resize(generic_count);
  for (uint32_t i = 0; i < generic_count; ++i) {
    uint32_t kind_raw = 0;
    uint64_t child_idx = 0;
    uint64_t child_cnt = 0;
    if (absl::Status st = r->ReadU32(&kind_raw); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadU64(&child_idx); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadU64(&child_cnt); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI64(&out->generic_items[i].int_val); !st.ok()) {
      return st;
    }
    if (absl::Status st = pools.ReadInternedString(r, &out->generic_items[i].text); !st.ok()) {
      return st;
    }
    out->generic_items[i].kind = static_cast<RtlExpr::GenericItem::Kind>(kind_raw);
    out->generic_items[i].child_index = static_cast<size_t>(child_idx);
    out->generic_items[i].child_count = static_cast<size_t>(child_cnt);
  }
  return absl::OkStatus();
}

void WriteConstantPoolEntry(BinaryWriter* w, TraceWritePools* pools,
                            const Stage0ConstantPoolEntry& entry) {
  w->WriteU32(pools->InternString(entry.label_name));
  w->WriteU32(pools->InternString(entry.section_name));
  w->WriteU64(static_cast<uint64_t>(entry.byte_size));
  WriteU32Vec(w, entry.words);
  w->WriteU32(entry.entry_vram);
}

absl::Status ReadConstantPoolEntry(BinaryReader* r, const TraceReadPools& pools,
                                   Stage0ConstantPoolEntry* out) {
  *out = Stage0ConstantPoolEntry{};
  uint64_t byte_size = 0;
  if (absl::Status st = pools.ReadInternedString(r, &out->label_name); !st.ok()) {
    return st;
  }
  if (absl::Status st = pools.ReadInternedString(r, &out->section_name); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU64(&byte_size); !st.ok()) {
    return st;
  }
  out->byte_size = static_cast<size_t>(byte_size);
  if (absl::Status st = ReadU32Vec(r, &out->words); !st.ok()) {
    return st;
  }
  return r->ReadU32(&out->entry_vram);
}

void WriteRtlInsn(BinaryWriter* w, TraceWritePools* pools, const RtlInsn& insn) {
  w->WriteU32(pools->InternString(insn.canonical_pattern));
  w->WriteU32(pools->InternString(insn.rtl_sexpr_pattern));
  w->WriteU32(static_cast<uint32_t>(insn.idiom_kind));
  w->WriteU32(insn.vram);
  const uint8_t flags =
      (insn.is_prologue_insn ? 0x01u : 0u) | (insn.is_synthetic_rtl_expansion ? 0x02u : 0u) |
      (insn.loop_note_kind.has_value() ? 0x04u : 0u) | (insn.has_explicit_call_fusage ? 0x08u : 0u);
  w->WriteU8(flags);
  if (insn.loop_note_kind.has_value()) {
    w->WriteU32(static_cast<uint32_t>(*insn.loop_note_kind));
  }
  w->WriteU32(static_cast<uint32_t>(insn.call_fusage.size()));
  for (const RtlExpr& expr : insn.call_fusage) {
    WriteRtlExpr(w, pools, expr);
  }
  w->WriteU32(static_cast<uint32_t>(insn.machine_instructions.size()));
  for (const Instruction& mi : insn.machine_instructions) {
    WriteInstruction(w, mi);
  }
}

absl::Status ReadRtlInsn(BinaryReader* r, const TraceReadPools& pools, RtlInsn* out) {
  *out = RtlInsn{};
  uint32_t idiom_raw = 0;
  uint8_t flags = 0;
  if (absl::Status st = pools.ReadInternedString(r, &out->canonical_pattern); !st.ok()) {
    return st;
  }
  if (absl::Status st = pools.ReadInternedString(r, &out->rtl_sexpr_pattern); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU32(&idiom_raw); !st.ok()) {
    return st;
  }
  out->idiom_kind = static_cast<RtlFusedIdiomKind>(idiom_raw);
  if (absl::Status st = r->ReadU32(&out->vram); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU8(&flags); !st.ok()) {
    return st;
  }
  out->is_prologue_insn = (flags & 0x01u) != 0;
  out->is_synthetic_rtl_expansion = (flags & 0x02u) != 0;
  out->has_explicit_call_fusage = (flags & 0x08u) != 0;
  if ((flags & 0x04u) != 0) {
    uint32_t note_raw = 0;
    if (absl::Status st = r->ReadU32(&note_raw); !st.ok()) {
      return st;
    }
    out->loop_note_kind = static_cast<RtlLoopNoteKind>(note_raw);
  }

  uint32_t fusage_count = 0;
  if (absl::Status st = r->ReadU32(&fusage_count); !st.ok()) {
    return st;
  }
  if (fusage_count > r->RemainingBytes() / 4) {
    return absl::InvalidArgumentError("RtlInsn call_fusage count exceeds remaining buffer");
  }
  out->call_fusage.resize(fusage_count);
  for (uint32_t i = 0; i < fusage_count; ++i) {
    if (absl::Status st = ReadRtlExpr(r, pools, &out->call_fusage[i]); !st.ok()) {
      return st;
    }
  }

  uint32_t mi_count = 0;
  if (absl::Status st = r->ReadU32(&mi_count); !st.ok()) {
    return st;
  }
  if (mi_count > r->RemainingBytes() / 16) {
    return absl::InvalidArgumentError(
        "RtlInsn machine_instructions count exceeds remaining buffer");
  }
  out->machine_instructions.resize(mi_count);
  for (uint32_t i = 0; i < mi_count; ++i) {
    if (absl::Status st = ReadInstruction(r, &out->machine_instructions[i]); !st.ok()) {
      return st;
    }
  }
  return absl::OkStatus();
}

void WriteRtlBasicBlock(BinaryWriter* w, TraceWritePools* pools, const RtlBasicBlock& blk) {
  w->WriteU32(blk.id);
  w->WriteU32(blk.start_vram);
  w->WriteU32(blk.end_vram);
  w->WriteU32(static_cast<uint32_t>(blk.rtl_insns.size()));
  for (const RtlInsn& insn : blk.rtl_insns) {
    WriteRtlInsn(w, pools, insn);
  }
  WriteStringVec(w, pools, blk.instructions);
  WriteSizeVec(w, blk.successors);
  WriteSizeVec(w, blk.predecessors);
  WriteSizeVec(w, blk.addr_vec_targets);
  const uint8_t flags = (blk.has_label ? 0x01u : 0u) | (blk.has_trailing_barrier ? 0x02u : 0u) |
                        (blk.has_unreachable_entry_label ? 0x04u : 0u);
  w->WriteU8(flags);
}

absl::Status ReadRtlBasicBlock(BinaryReader* r, const TraceReadPools& pools, RtlBasicBlock* out) {
  *out = RtlBasicBlock{};
  if (absl::Status st = r->ReadU32(&out->id); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU32(&out->start_vram); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU32(&out->end_vram); !st.ok()) {
    return st;
  }
  uint32_t insn_count = 0;
  if (absl::Status st = r->ReadU32(&insn_count); !st.ok()) {
    return st;
  }
  if (insn_count > r->RemainingBytes() / 16) {
    return absl::InvalidArgumentError("RtlBasicBlock rtl_insns count exceeds remaining buffer");
  }
  out->rtl_insns.resize(insn_count);
  for (uint32_t i = 0; i < insn_count; ++i) {
    if (absl::Status st = ReadRtlInsn(r, pools, &out->rtl_insns[i]); !st.ok()) {
      return st;
    }
  }
  if (absl::Status st = ReadStringVec(r, pools, &out->instructions); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadSizeVec(r, &out->successors); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadSizeVec(r, &out->predecessors); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadSizeVec(r, &out->addr_vec_targets); !st.ok()) {
    return st;
  }
  uint8_t flags = 0;
  if (absl::Status st = r->ReadU8(&flags); !st.ok()) {
    return st;
  }
  out->has_label = (flags & 0x01u) != 0;
  out->has_trailing_barrier = (flags & 0x02u) != 0;
  out->has_unreachable_entry_label = (flags & 0x04u) != 0;
  return absl::OkStatus();
}

void WriteRtlSidebandState(BinaryWriter* w, TraceWritePools* pools, const RtlSidebandState& sb) {
  w->WriteI32(static_cast<int32_t>(sb.max_regno));
  WriteIntIntMap(w, sb.reg_renumber);
  WriteIntIntMap(w, sb.reg_n_refs);
  WriteIntIntMap(w, sb.reg_n_sets);
  WriteIntIntMap(w, sb.reg_n_deaths);
  WriteIntIntMap(w, sb.reg_live_length);
  WriteIntIntMap(w, sb.reg_n_calls_crossed);

  std::vector<int> live_keys;
  live_keys.reserve(sb.basic_block_live_at_start.size());
  for (const auto& [bb, regs] : sb.basic_block_live_at_start) {
    live_keys.push_back(bb);
  }
  std::sort(live_keys.begin(), live_keys.end());
  w->WriteU32(static_cast<uint32_t>(live_keys.size()));
  for (int bb : live_keys) {
    w->WriteI32(static_cast<int32_t>(bb));
    const absl::flat_hash_set<int>& reg_set = sb.basic_block_live_at_start.at(bb);
    std::vector<int> sorted_regs(reg_set.begin(), reg_set.end());
    std::sort(sorted_regs.begin(), sorted_regs.end());
    WriteI32Vec(w, sorted_regs);
  }

  w->WriteI32(static_cast<int32_t>(sb.frame_size_bytes));
  w->WriteI32(static_cast<int32_t>(sb.outgoing_args_size));
  const uint8_t flags =
      (sb.is_leaf_function ? 0x01u : 0u) | (sb.frame_pointer_needed ? 0x02u : 0u) |
      (sb.epilogue_delay_pattern.has_value() ? 0x04u : 0u) | (sb.is_variadic ? 0x08u : 0u) |
      (sb.returns_aggregate_in_memory ? 0x10u : 0u) | (sb.cse_jumps_altered ? 0x20u : 0u);
  w->WriteU8(flags);
  WriteI32Vec(w, sb.saved_gpr_regnos);
  WriteI32Vec(w, sb.saved_fpr_regnos);
  if (sb.epilogue_delay_pattern.has_value()) {
    w->WriteU32(pools->InternString(*sb.epilogue_delay_pattern));
  }

  w->WriteU32(static_cast<uint32_t>(sb.prologue_args.size()));
  for (const MipsPrologueArgSpec& arg : sb.prologue_args) {
    w->WriteU32(static_cast<uint32_t>(arg.kind));
    w->WriteI32(static_cast<int32_t>(arg.byte_size));
    w->WriteI32(static_cast<int32_t>(arg.align_bits));
    w->WriteI32(static_cast<int32_t>(arg.gcc_mode));
  }

  w->WriteU32(static_cast<uint32_t>(sb.spill_slots.size()));
  for (const RtlSpillSlotInfo& slot : sb.spill_slots) {
    w->WriteI32(static_cast<int32_t>(slot.frame_offset));
    w->WriteI32(static_cast<int32_t>(slot.byte_size));
    w->WriteI32(static_cast<int32_t>(slot.recovered_pseudo_regno));
    w->WriteU32(static_cast<uint32_t>(slot.mode));
  }

  w->WriteU32(static_cast<uint32_t>(sb.loops.size()));
  for (const RtlLoopInfo& loop : sb.loops) {
    w->WriteI32(static_cast<int32_t>(loop.header_label_id));
    w->WriteI32(static_cast<int32_t>(loop.latch_block_id));
    w->WriteI32(static_cast<int32_t>(loop.preheader_block_id));
    WriteI32Vec(w, loop.body_block_ids);
    WriteI32Vec(w, loop.hoisted_invariant_regnos);
  }

  w->WriteU32(static_cast<uint32_t>(sb.constant_pool.size()));
  for (const Stage0ConstantPoolEntry& cp : sb.constant_pool) {
    WriteConstantPoolEntry(w, pools, cp);
  }
  WriteI32Vec(w, sb.pointer_regnos);
}

absl::Status ReadRtlSidebandState(BinaryReader* r, const TraceReadPools& pools,
                                  RtlSidebandState* out) {
  *out = RtlSidebandState{};
  int32_t max_regno = 0;
  if (absl::Status st = r->ReadI32(&max_regno); !st.ok()) {
    return st;
  }
  out->max_regno = static_cast<int>(max_regno);
  if (absl::Status st = ReadIntIntMap(r, &out->reg_renumber); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadIntIntMap(r, &out->reg_n_refs); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadIntIntMap(r, &out->reg_n_sets); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadIntIntMap(r, &out->reg_n_deaths); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadIntIntMap(r, &out->reg_live_length); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadIntIntMap(r, &out->reg_n_calls_crossed); !st.ok()) {
    return st;
  }

  uint32_t live_bb_count = 0;
  if (absl::Status st = r->ReadU32(&live_bb_count); !st.ok()) {
    return st;
  }
  if (live_bb_count > r->RemainingBytes() / 8) {
    return absl::InvalidArgumentError("basic_block_live_at_start count exceeds remaining buffer");
  }
  out->basic_block_live_at_start.reserve(live_bb_count);
  for (uint32_t i = 0; i < live_bb_count; ++i) {
    int32_t bb = 0;
    std::vector<int> regs;
    if (absl::Status st = r->ReadI32(&bb); !st.ok()) {
      return st;
    }
    if (absl::Status st = ReadI32Vec(r, &regs); !st.ok()) {
      return st;
    }
    absl::flat_hash_set<int> reg_set(regs.begin(), regs.end());
    out->basic_block_live_at_start[static_cast<int>(bb)] = std::move(reg_set);
  }

  int32_t frame_size = 0;
  int32_t outgoing_size = 0;
  uint8_t flags = 0;
  if (absl::Status st = r->ReadI32(&frame_size); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadI32(&outgoing_size); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU8(&flags); !st.ok()) {
    return st;
  }
  out->frame_size_bytes = static_cast<int>(frame_size);
  out->outgoing_args_size = static_cast<int>(outgoing_size);
  out->is_leaf_function = (flags & 0x01u) != 0;
  out->frame_pointer_needed = (flags & 0x02u) != 0;
  out->is_variadic = (flags & 0x08u) != 0;
  out->returns_aggregate_in_memory = (flags & 0x10u) != 0;
  out->cse_jumps_altered = (flags & 0x20u) != 0;

  if (absl::Status st = ReadI32Vec(r, &out->saved_gpr_regnos); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadI32Vec(r, &out->saved_fpr_regnos); !st.ok()) {
    return st;
  }
  if ((flags & 0x04u) != 0) {
    std::string pattern;
    if (absl::Status st = pools.ReadInternedString(r, &pattern); !st.ok()) {
      return st;
    }
    out->epilogue_delay_pattern = std::move(pattern);
  }

  uint32_t arg_count = 0;
  if (absl::Status st = r->ReadU32(&arg_count); !st.ok()) {
    return st;
  }
  if (arg_count > r->RemainingBytes() / 16) {
    return absl::InvalidArgumentError("prologue_args count exceeds remaining buffer");
  }
  out->prologue_args.resize(arg_count);
  for (uint32_t i = 0; i < arg_count; ++i) {
    uint32_t kind_raw = 0;
    int32_t byte_size = 0;
    int32_t align_bits = 0;
    int32_t gcc_mode = 0;
    if (absl::Status st = r->ReadU32(&kind_raw); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI32(&byte_size); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI32(&align_bits); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI32(&gcc_mode); !st.ok()) {
      return st;
    }
    out->prologue_args[i] = {
        .kind = static_cast<MipsPrologueArgKind>(kind_raw),
        .byte_size = static_cast<int>(byte_size),
        .align_bits = static_cast<int>(align_bits),
        .gcc_mode = static_cast<int>(gcc_mode),
    };
  }

  uint32_t spill_count = 0;
  if (absl::Status st = r->ReadU32(&spill_count); !st.ok()) {
    return st;
  }
  if (spill_count > r->RemainingBytes() / 16) {
    return absl::InvalidArgumentError("spill_slots count exceeds remaining buffer");
  }
  out->spill_slots.resize(spill_count);
  for (uint32_t i = 0; i < spill_count; ++i) {
    int32_t frame_off = 0;
    int32_t byte_size = 0;
    int32_t regno = 0;
    uint32_t mode_raw = 0;
    if (absl::Status st = r->ReadI32(&frame_off); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI32(&byte_size); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI32(&regno); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadU32(&mode_raw); !st.ok()) {
      return st;
    }
    out->spill_slots[i] = {
        .frame_offset = static_cast<int>(frame_off),
        .byte_size = static_cast<int>(byte_size),
        .recovered_pseudo_regno = static_cast<int>(regno),
        .mode = static_cast<RtlMachineMode>(mode_raw),
    };
  }

  uint32_t loop_count = 0;
  if (absl::Status st = r->ReadU32(&loop_count); !st.ok()) {
    return st;
  }
  if (loop_count > r->RemainingBytes() / 20) {
    return absl::InvalidArgumentError("loops count exceeds remaining buffer");
  }
  out->loops.resize(loop_count);
  for (uint32_t i = 0; i < loop_count; ++i) {
    int32_t header = 0;
    int32_t latch = 0;
    int32_t preheader = 0;
    if (absl::Status st = r->ReadI32(&header); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI32(&latch); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI32(&preheader); !st.ok()) {
      return st;
    }
    out->loops[i].header_label_id = static_cast<int>(header);
    out->loops[i].latch_block_id = static_cast<int>(latch);
    out->loops[i].preheader_block_id = static_cast<int>(preheader);
    if (absl::Status st = ReadI32Vec(r, &out->loops[i].body_block_ids); !st.ok()) {
      return st;
    }
    if (absl::Status st = ReadI32Vec(r, &out->loops[i].hoisted_invariant_regnos); !st.ok()) {
      return st;
    }
  }

  uint32_t cp_count = 0;
  if (absl::Status st = r->ReadU32(&cp_count); !st.ok()) {
    return st;
  }
  if (cp_count > r->RemainingBytes() / 20) {
    return absl::InvalidArgumentError("sideband constant_pool count exceeds remaining buffer");
  }
  out->constant_pool.resize(cp_count);
  for (uint32_t i = 0; i < cp_count; ++i) {
    if (absl::Status st = ReadConstantPoolEntry(r, pools, &out->constant_pool[i]); !st.ok()) {
      return st;
    }
  }
  return ReadI32Vec(r, &out->pointer_regnos);
}

void WriteRtlCfgBody(BinaryWriter* w, TraceWritePools* pools, const RtlControlFlowGraph& cfg) {
  w->WriteU32(static_cast<uint32_t>(cfg.pass));
  w->WriteU32(static_cast<uint32_t>(cfg.rpo_blocks.size()));
  for (const RtlBasicBlock& blk : cfg.rpo_blocks) {
    WriteRtlBasicBlock(w, pools, blk);
  }
  WriteRtlSidebandState(w, pools, cfg.sideband);
  w->WriteU8(cfg.has_explicit_trailing_void_return ? 1u : 0u);
  w->WriteU64(static_cast<uint64_t>(cfg.local_alloc_pseudo_reg_count));
  w->WriteU64(static_cast<uint64_t>(cfg.global_alloc_pseudo_reg_count));
}

absl::Status ReadRtlCfgBody(BinaryReader* r, const TraceReadPools& pools,
                            RtlControlFlowGraph* out) {
  *out = RtlControlFlowGraph{};
  uint32_t pass_raw = 0;
  uint32_t block_count = 0;
  if (absl::Status st = r->ReadU32(&pass_raw); !st.ok()) {
    return st;
  }
  out->pass = static_cast<RtlPassKind>(pass_raw);
  if (absl::Status st = r->ReadU32(&block_count); !st.ok()) {
    return st;
  }
  if (block_count > r->RemainingBytes() / 16) {
    return absl::InvalidArgumentError("RtlControlFlowGraph block count exceeds remaining buffer");
  }
  out->rpo_blocks.resize(block_count);
  for (uint32_t i = 0; i < block_count; ++i) {
    if (absl::Status st = ReadRtlBasicBlock(r, pools, &out->rpo_blocks[i]); !st.ok()) {
      return st;
    }
  }
  if (absl::Status st = ReadRtlSidebandState(r, pools, &out->sideband); !st.ok()) {
    return st;
  }
  uint8_t trailing_void = 0;
  uint64_t local_cnt = 0;
  uint64_t global_cnt = 0;
  if (absl::Status st = r->ReadU8(&trailing_void); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU64(&local_cnt); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU64(&global_cnt); !st.ok()) {
    return st;
  }
  out->has_explicit_trailing_void_return = (trailing_void != 0);
  out->local_alloc_pseudo_reg_count = static_cast<size_t>(local_cnt);
  out->global_alloc_pseudo_reg_count = static_cast<size_t>(global_cnt);
  return absl::OkStatus();
}

uint32_t InternRtlCfg(TraceWritePools* pools, const RtlControlFlowGraph& cfg) {
  BinaryWriter cfg_writer;
  WriteRtlCfgBody(&cfg_writer, pools, cfg);
  std::string blob = cfg_writer.TakeBytes();
  auto it = pools->cfg_blob_to_id.find(blob);
  if (it != pools->cfg_blob_to_id.end()) {
    return it->second;
  }
  const uint32_t id = static_cast<uint32_t>(pools->cfg_blobs.size());
  pools->cfg_blobs.push_back(std::move(blob));
  pools->cfg_blob_to_id.emplace(pools->cfg_blobs.back(), id);
  return id;
}

void WriteStage0FunctionInput(BinaryWriter* w, TraceWritePools* pools,
                              const Stage0FunctionInput& input) {
  WriteU32Vec(w, input.machine_words);
  w->WriteU32(input.base_vram);

  w->WriteU32(static_cast<uint32_t>(input.relocations.size()));
  for (const Stage0Relocation& rel : input.relocations) {
    w->WriteU64(static_cast<uint64_t>(rel.word_index));
    w->WriteU32(static_cast<uint32_t>(rel.kind));
    w->WriteU32(pools->InternString(rel.symbol_name));
    w->WriteI64(rel.addend);
  }

  w->WriteU32(static_cast<uint32_t>(input.jump_tables.size()));
  for (const Stage0JumpTable& jt : input.jump_tables) {
    w->WriteU32(pools->InternString(jt.label_name));
    WriteSizeVec(w, jt.target_word_indices);
    w->WriteU32(jt.table_vram);
  }

  w->WriteU32(static_cast<uint32_t>(input.constant_pool.size()));
  for (const Stage0ConstantPoolEntry& cp : input.constant_pool) {
    WriteConstantPoolEntry(w, pools, cp);
  }

  std::vector<std::pair<uint32_t, std::string_view>> vram_syms;
  vram_syms.reserve(input.vram_to_symbol.size());
  for (const auto& [vram, sym] : input.vram_to_symbol) {
    vram_syms.emplace_back(vram, sym);
  }
  std::sort(vram_syms.begin(), vram_syms.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  w->WriteU32(static_cast<uint32_t>(vram_syms.size()));
  for (const auto& [vram, sym] : vram_syms) {
    w->WriteU32(vram);
    w->WriteU32(pools->InternString(sym));
  }

  std::vector<std::pair<uint32_t, uint32_t>> vram_sizes(input.vram_to_symbol_size.begin(),
                                                        input.vram_to_symbol_size.end());
  std::sort(vram_sizes.begin(), vram_sizes.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  w->WriteU32(static_cast<uint32_t>(vram_sizes.size()));
  for (const auto& [vram, sz] : vram_sizes) {
    w->WriteU32(vram);
    w->WriteU32(sz);
  }

  w->WriteU32(input.gp_vram);
  w->WriteI32(static_cast<int32_t>(input.profile.optimize_level));
  WriteStringVec(w, pools, input.profile.c_flags);
  WriteStringU32Map(w, pools, input.profile.gp_symbol_sizes);
}

absl::Status ReadStage0FunctionInput(BinaryReader* r, const TraceReadPools& pools,
                                     Stage0FunctionInput* out) {
  *out = Stage0FunctionInput{};
  if (absl::Status st = ReadU32Vec(r, &out->machine_words); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadU32(&out->base_vram); !st.ok()) {
    return st;
  }

  uint32_t reloc_count = 0;
  if (absl::Status st = r->ReadU32(&reloc_count); !st.ok()) {
    return st;
  }
  if (reloc_count > r->RemainingBytes() / 24) {
    return absl::InvalidArgumentError("Stage0 relocations count exceeds remaining buffer");
  }
  out->relocations.resize(reloc_count);
  for (uint32_t i = 0; i < reloc_count; ++i) {
    uint64_t word_idx = 0;
    uint32_t kind_raw = 0;
    if (absl::Status st = r->ReadU64(&word_idx); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadU32(&kind_raw); !st.ok()) {
      return st;
    }
    if (absl::Status st = pools.ReadInternedString(r, &out->relocations[i].symbol_name); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadI64(&out->relocations[i].addend); !st.ok()) {
      return st;
    }
    out->relocations[i].word_index = static_cast<size_t>(word_idx);
    out->relocations[i].kind = static_cast<Stage0RelocKind>(kind_raw);
  }

  uint32_t jt_count = 0;
  if (absl::Status st = r->ReadU32(&jt_count); !st.ok()) {
    return st;
  }
  if (jt_count > r->RemainingBytes() / 12) {
    return absl::InvalidArgumentError("Stage0 jump_tables count exceeds remaining buffer");
  }
  out->jump_tables.resize(jt_count);
  for (uint32_t i = 0; i < jt_count; ++i) {
    if (absl::Status st = pools.ReadInternedString(r, &out->jump_tables[i].label_name); !st.ok()) {
      return st;
    }
    if (absl::Status st = ReadSizeVec(r, &out->jump_tables[i].target_word_indices); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadU32(&out->jump_tables[i].table_vram); !st.ok()) {
      return st;
    }
  }

  uint32_t cp_count = 0;
  if (absl::Status st = r->ReadU32(&cp_count); !st.ok()) {
    return st;
  }
  if (cp_count > r->RemainingBytes() / 20) {
    return absl::InvalidArgumentError("Stage0 constant_pool count exceeds remaining buffer");
  }
  out->constant_pool.resize(cp_count);
  for (uint32_t i = 0; i < cp_count; ++i) {
    if (absl::Status st = ReadConstantPoolEntry(r, pools, &out->constant_pool[i]); !st.ok()) {
      return st;
    }
  }

  uint32_t vram_sym_count = 0;
  if (absl::Status st = r->ReadU32(&vram_sym_count); !st.ok()) {
    return st;
  }
  if (vram_sym_count > r->RemainingBytes() / 8) {
    return absl::InvalidArgumentError("Stage0 vram_to_symbol count exceeds remaining buffer");
  }
  out->vram_to_symbol.reserve(vram_sym_count);
  for (uint32_t i = 0; i < vram_sym_count; ++i) {
    uint32_t vram = 0;
    std::string sym;
    if (absl::Status st = r->ReadU32(&vram); !st.ok()) {
      return st;
    }
    if (absl::Status st = pools.ReadInternedString(r, &sym); !st.ok()) {
      return st;
    }
    out->vram_to_symbol[vram] = std::move(sym);
  }

  uint32_t vram_sz_count = 0;
  if (absl::Status st = r->ReadU32(&vram_sz_count); !st.ok()) {
    return st;
  }
  if (vram_sz_count > r->RemainingBytes() / 8) {
    return absl::InvalidArgumentError("Stage0 vram_to_symbol_size count exceeds remaining buffer");
  }
  out->vram_to_symbol_size.reserve(vram_sz_count);
  for (uint32_t i = 0; i < vram_sz_count; ++i) {
    uint32_t vram = 0;
    uint32_t sz = 0;
    if (absl::Status st = r->ReadU32(&vram); !st.ok()) {
      return st;
    }
    if (absl::Status st = r->ReadU32(&sz); !st.ok()) {
      return st;
    }
    out->vram_to_symbol_size[vram] = sz;
  }

  if (absl::Status st = r->ReadU32(&out->gp_vram); !st.ok()) {
    return st;
  }
  int32_t opt_level = 0;
  if (absl::Status st = r->ReadI32(&opt_level); !st.ok()) {
    return st;
  }
  out->profile.optimize_level = static_cast<int>(opt_level);
  if (absl::Status st = ReadStringVec(r, pools, &out->profile.c_flags); !st.ok()) {
    return st;
  }
  return ReadStringU32Map(r, pools, &out->profile.gp_symbol_sizes);
}

void WriteSubfunctionSnapshot(BinaryWriter* w, TraceWritePools* pools,
                              const SubfunctionBoundarySnapshot& snap) {
  w->WriteU32(static_cast<uint32_t>(snap.subfunction_id));
  w->WriteI32(static_cast<int32_t>(snap.optimize_level));
  WriteStringVec(w, pools, snap.c_flags);
  WriteStringU32Map(w, pools, snap.gp_symbol_sizes);
  w->WriteU32(InternRtlCfg(pools, snap.pre_state));
  w->WriteU32(InternRtlCfg(pools, snap.post_state));
  WriteStringVec(w, pools, snap.pre_asm_lines);
  WriteStringVec(w, pools, snap.post_asm_lines);
  WriteU32Vec(w, snap.post_machine_words);
}

absl::Status ReadSubfunctionSnapshot(BinaryReader* r, const TraceReadPools& pools,
                                     SubfunctionBoundarySnapshot* out) {
  *out = SubfunctionBoundarySnapshot{};
  uint32_t sub_id_raw = 0;
  int32_t opt_level = 0;
  if (absl::Status st = r->ReadU32(&sub_id_raw); !st.ok()) {
    return st;
  }
  if (absl::Status st = r->ReadI32(&opt_level); !st.ok()) {
    return st;
  }
  out->subfunction_id = static_cast<SubfunctionId>(sub_id_raw);
  out->optimize_level = static_cast<int>(opt_level);
  if (absl::Status st = ReadStringVec(r, pools, &out->c_flags); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadStringU32Map(r, pools, &out->gp_symbol_sizes); !st.ok()) {
    return st;
  }
  if (absl::Status st = pools.ReadInternedCfg(r, &out->pre_state); !st.ok()) {
    return st;
  }
  if (absl::Status st = pools.ReadInternedCfg(r, &out->post_state); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadStringVec(r, pools, &out->pre_asm_lines); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadStringVec(r, pools, &out->post_asm_lines); !st.ok()) {
    return st;
  }
  return ReadU32Vec(r, &out->post_machine_words);
}

bool SetDiff(std::string* diff_detail, std::string msg) {
  if (diff_detail != nullptr) {
    *diff_detail = std::move(msg);
  }
  return false;
}

bool InstructionEqualsExact(const Instruction& lhs, const Instruction& rhs) {
  return lhs.raw_word == rhs.raw_word && lhs.vram == rhs.vram && lhs.opcode == rhs.opcode &&
         lhs.rd == rhs.rd && lhs.rs == rhs.rs && lhs.rt == rhs.rt &&
         lhs.unscheduled_clobber_reg == rhs.unscheduled_clobber_reg &&
         lhs.was_unscheduled_delay_slot == rhs.was_unscheduled_delay_slot && lhs.fd == rhs.fd &&
         lhs.fs == rhs.fs && lhs.ft == rhs.ft && lhs.immediate == rhs.immediate &&
         lhs.shift_amount == rhs.shift_amount && lhs.target == rhs.target;
}

bool RtlExprEqualsExact(const RtlExpr& lhs, const RtlExpr& rhs) {
  if (lhs.op != rhs.op || lhs.mode != rhs.mode || lhs.flags != rhs.flags ||
      lhs.int_val != rhs.int_val || lhs.int_val_high != rhs.int_val_high ||
      lhs.str_val != rhs.str_val || lhs.symbol_paren_wrapped != rhs.symbol_paren_wrapped ||
      lhs.bracketed_children != rhs.bracketed_children || lhs.raw_op != rhs.raw_op ||
      lhs.raw_mode != rhs.raw_mode || lhs.extra_ints != rhs.extra_ints ||
      lhs.generic_items != rhs.generic_items || lhs.children.size() != rhs.children.size()) {
    return false;
  }
  for (size_t i = 0; i < lhs.children.size(); ++i) {
    if (!RtlExprEqualsExact(lhs.children[i], rhs.children[i])) {
      return false;
    }
  }
  return true;
}

bool RtlInsnEqualsExact(const RtlInsn& lhs, const RtlInsn& rhs, std::string* diff_detail) {
  if (lhs.canonical_pattern != rhs.canonical_pattern) {
    return SetDiff(diff_detail, absl::StrCat("canonical_pattern mismatch: '", lhs.canonical_pattern,
                                             "' vs '", rhs.canonical_pattern, "'"));
  }
  if (lhs.rtl_sexpr_pattern != rhs.rtl_sexpr_pattern) {
    return SetDiff(diff_detail, "rtl_sexpr_pattern mismatch");
  }
  if (lhs.idiom_kind != rhs.idiom_kind || lhs.vram != rhs.vram ||
      lhs.is_prologue_insn != rhs.is_prologue_insn ||
      lhs.is_synthetic_rtl_expansion != rhs.is_synthetic_rtl_expansion ||
      lhs.loop_note_kind != rhs.loop_note_kind ||
      lhs.has_explicit_call_fusage != rhs.has_explicit_call_fusage) {
    return SetDiff(diff_detail, "RtlInsn metadata field mismatch");
  }
  if (lhs.call_fusage.size() != rhs.call_fusage.size()) {
    return SetDiff(diff_detail, "RtlInsn call_fusage size mismatch");
  }
  for (size_t i = 0; i < lhs.call_fusage.size(); ++i) {
    if (!RtlExprEqualsExact(lhs.call_fusage[i], rhs.call_fusage[i])) {
      return SetDiff(diff_detail, absl::StrFormat("RtlInsn call_fusage[%zu] mismatch", i));
    }
  }
  if (lhs.machine_instructions.size() != rhs.machine_instructions.size()) {
    return SetDiff(diff_detail, "RtlInsn machine_instructions size mismatch");
  }
  for (size_t i = 0; i < lhs.machine_instructions.size(); ++i) {
    if (!InstructionEqualsExact(lhs.machine_instructions[i], rhs.machine_instructions[i])) {
      return SetDiff(diff_detail, absl::StrFormat("RtlInsn machine_instructions[%zu] mismatch", i));
    }
  }
  return true;
}

bool SubfunctionSnapshotEqualsExact(const SubfunctionBoundarySnapshot& lhs,
                                    const SubfunctionBoundarySnapshot& rhs,
                                    std::string* diff_detail) {
  if (lhs.subfunction_id != rhs.subfunction_id || lhs.optimize_level != rhs.optimize_level ||
      lhs.c_flags != rhs.c_flags || lhs.gp_symbol_sizes != rhs.gp_symbol_sizes ||
      lhs.pre_asm_lines != rhs.pre_asm_lines || lhs.post_asm_lines != rhs.post_asm_lines ||
      lhs.post_machine_words != rhs.post_machine_words) {
    return SetDiff(diff_detail,
                   absl::StrCat("SubfunctionBoundarySnapshot header/asm/words mismatch for ",
                                SubfunctionIdName(lhs.subfunction_id)));
  }
  std::string sub_diff;
  if (!RtlControlFlowGraphEqualsExact(lhs.pre_state, rhs.pre_state, &sub_diff)) {
    return SetDiff(diff_detail,
                   absl::StrCat("SubfunctionBoundarySnapshot ",
                                SubfunctionIdName(lhs.subfunction_id), " pre_state: ", sub_diff));
  }
  if (!RtlControlFlowGraphEqualsExact(lhs.post_state, rhs.post_state, &sub_diff)) {
    return SetDiff(diff_detail,
                   absl::StrCat("SubfunctionBoundarySnapshot ",
                                SubfunctionIdName(lhs.subfunction_id), " post_state: ", sub_diff));
  }
  return true;
}

}  // namespace

uint64_t ComputeCorpusFingerprint(absl::Span<const RoundTripTestCase> test_cases) {
  uint64_t hash = kFnv1aOffsetBasis;
  MixFnvU64(&hash, static_cast<uint64_t>(test_cases.size()));
  for (const RoundTripTestCase& tc : test_cases) {
    MixFnvString(&hash, tc.test_name);
    MixFnvString(&hash, tc.function_name);
    MixFnvString(&hash, tc.original_c);
    MixFnvU64(&hash, static_cast<uint64_t>(tc.options.c_flags.size()));
    for (const std::string& flag : tc.options.c_flags) {
      MixFnvString(&hash, flag);
    }
    MixFnvU64(&hash, static_cast<uint64_t>(tc.options.base_vram));
    MixFnvString(&hash, tc.options.extra_types_header);
  }
  return hash;
}

std::string SerializeFunctionCompilationTrace(const FunctionCompilationTrace& trace) {
  TraceWritePools pools;
  BinaryWriter body;
  body.WriteU32(pools.InternString(trace.test_name));
  body.WriteU32(pools.InternString(trace.function_name));
  body.WriteI32(static_cast<int32_t>(trace.optimize_level));
  WriteStringVec(&body, &pools, trace.c_flags);
  WriteStringU32Map(&body, &pools, trace.gp_symbol_sizes);

  std::vector<RtlPassKind> pass_keys;
  pass_keys.reserve(trace.pass_snapshots.size());
  for (const auto& [pass, cfg] : trace.pass_snapshots) {
    pass_keys.push_back(pass);
  }
  std::sort(pass_keys.begin(), pass_keys.end(), [](RtlPassKind a, RtlPassKind b) {
    return static_cast<uint32_t>(a) < static_cast<uint32_t>(b);
  });
  body.WriteU32(static_cast<uint32_t>(pass_keys.size()));
  for (RtlPassKind pass : pass_keys) {
    body.WriteU32(static_cast<uint32_t>(pass));
    body.WriteU32(InternRtlCfg(&pools, trace.pass_snapshots.at(pass)));
  }

  body.WriteU32(static_cast<uint32_t>(trace.subfunction_snapshots.size()));
  for (const SubfunctionBoundarySnapshot& snap : trace.subfunction_snapshots) {
    WriteSubfunctionSnapshot(&body, &pools, snap);
  }

  body.WriteU32(pools.InternString(trace.final_asm_text));
  WriteStringVec(&body, &pools, trace.final_insn_asm_lines);
  WriteU32Vec(&body, trace.final_machine_words);
  WriteStage0FunctionInput(&body, &pools, trace.stage0_input);

  BinaryWriter out;
  out.WriteU32(static_cast<uint32_t>(pools.strings.size()));
  for (const std::string& s : pools.strings) {
    out.WriteLengthPrefixedBytes(s);
  }
  out.WriteU32(static_cast<uint32_t>(pools.cfg_blobs.size()));
  for (const std::string& blob : pools.cfg_blobs) {
    out.WriteLengthPrefixedBytes(blob);
  }
  out.WriteRawBytes(body.Bytes());
  return out.TakeBytes();
}

absl::StatusOr<FunctionCompilationTrace> DeserializeFunctionCompilationTrace(
    std::string_view bytes) {
  BinaryReader r(bytes);
  TraceReadPools pools;

  uint32_t str_count = 0;
  if (absl::Status st = r.ReadU32(&str_count); !st.ok()) {
    return st;
  }
  if (str_count > r.RemainingBytes() / 4) {
    return absl::InvalidArgumentError("String pool count exceeds remaining buffer");
  }
  pools.strings.resize(str_count);
  for (uint32_t i = 0; i < str_count; ++i) {
    std::string_view slice;
    if (absl::Status st = r.ReadLengthPrefixedSlice(&slice); !st.ok()) {
      return st;
    }
    pools.strings[i].assign(slice.data(), slice.size());
  }

  uint32_t cfg_count = 0;
  if (absl::Status st = r.ReadU32(&cfg_count); !st.ok()) {
    return st;
  }
  if (cfg_count > r.RemainingBytes() / 4) {
    return absl::InvalidArgumentError("CFG pool count exceeds remaining buffer");
  }
  pools.cfgs.resize(cfg_count);
  for (uint32_t i = 0; i < cfg_count; ++i) {
    std::string_view blob;
    if (absl::Status st = r.ReadLengthPrefixedSlice(&blob); !st.ok()) {
      return st;
    }
    BinaryReader cfg_reader(blob);
    if (absl::Status st = ReadRtlCfgBody(&cfg_reader, pools, &pools.cfgs[i]); !st.ok()) {
      return st;
    }
    if (cfg_reader.RemainingBytes() != 0) {
      return absl::InvalidArgumentError(absl::StrFormat("Trailing %zu bytes in CFG pool entry %u",
                                                        cfg_reader.RemainingBytes(), i));
    }
  }

  FunctionCompilationTrace trace;
  if (absl::Status st = pools.ReadInternedString(&r, &trace.test_name); !st.ok()) {
    return st;
  }
  if (absl::Status st = pools.ReadInternedString(&r, &trace.function_name); !st.ok()) {
    return st;
  }
  int32_t opt_level = 0;
  if (absl::Status st = r.ReadI32(&opt_level); !st.ok()) {
    return st;
  }
  trace.optimize_level = static_cast<int>(opt_level);
  if (absl::Status st = ReadStringVec(&r, pools, &trace.c_flags); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadStringU32Map(&r, pools, &trace.gp_symbol_sizes); !st.ok()) {
    return st;
  }

  uint32_t pass_count = 0;
  if (absl::Status st = r.ReadU32(&pass_count); !st.ok()) {
    return st;
  }
  if (pass_count > r.RemainingBytes() / 8) {
    return absl::InvalidArgumentError("pass_snapshots count exceeds remaining buffer");
  }
  trace.pass_snapshots.reserve(pass_count);
  for (uint32_t i = 0; i < pass_count; ++i) {
    uint32_t pass_raw = 0;
    RtlControlFlowGraph cfg;
    if (absl::Status st = r.ReadU32(&pass_raw); !st.ok()) {
      return st;
    }
    if (absl::Status st = pools.ReadInternedCfg(&r, &cfg); !st.ok()) {
      return st;
    }
    trace.pass_snapshots[static_cast<RtlPassKind>(pass_raw)] = std::move(cfg);
  }

  uint32_t snap_count = 0;
  if (absl::Status st = r.ReadU32(&snap_count); !st.ok()) {
    return st;
  }
  if (snap_count > r.RemainingBytes() / 16) {
    return absl::InvalidArgumentError("subfunction_snapshots count exceeds remaining buffer");
  }
  trace.subfunction_snapshots.resize(snap_count);
  for (uint32_t i = 0; i < snap_count; ++i) {
    if (absl::Status st = ReadSubfunctionSnapshot(&r, pools, &trace.subfunction_snapshots[i]);
        !st.ok()) {
      return st;
    }
  }

  if (absl::Status st = pools.ReadInternedString(&r, &trace.final_asm_text); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadStringVec(&r, pools, &trace.final_insn_asm_lines); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadU32Vec(&r, &trace.final_machine_words); !st.ok()) {
    return st;
  }
  if (absl::Status st = ReadStage0FunctionInput(&r, pools, &trace.stage0_input); !st.ok()) {
    return st;
  }
  if (r.RemainingBytes() != 0) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Trailing %zu bytes after trace payload", r.RemainingBytes()));
  }
  return trace;
}

std::string SerializeTraceShard(const SerializedTraceShardHeader& header,
                                absl::Span<const IndexedFunctionCompilationTrace> records) {
  std::vector<std::string> payloads;
  payloads.reserve(records.size());
  std::vector<TraceShardIndexEntry> index_entries;
  index_entries.reserve(records.size());

  uint64_t running_offset = 0;
  for (const IndexedFunctionCompilationTrace& rec : records) {
    std::string payload = SerializeFunctionCompilationTrace(rec.trace);
    const uint64_t len = static_cast<uint64_t>(payload.size());
    index_entries.push_back(TraceShardIndexEntry{
        .case_index = rec.case_index,
        .byte_offset = static_cast<uint32_t>(running_offset),
        .byte_length = static_cast<uint32_t>(len),
    });
    running_offset += len;
    payloads.push_back(std::move(payload));
  }

  BinaryWriter w;
  w.WriteU32(header.magic);
  w.WriteU32(header.format_version);
  w.WriteU32(header.shard_index);
  w.WriteU32(header.num_shards);
  w.WriteU32(header.total_corpus_cases);
  w.WriteU64(header.corpus_fingerprint);
  w.WriteU32(static_cast<uint32_t>(index_entries.size()));

  for (const TraceShardIndexEntry& entry : index_entries) {
    w.WriteU32(entry.case_index);
    w.WriteU32(entry.byte_offset);
    w.WriteU32(entry.byte_length);
  }
  for (const std::string& payload : payloads) {
    w.WriteRawBytes(payload);
  }
  return w.TakeBytes();
}

absl::StatusOr<ParsedTraceShardIndex> ParseTraceShardIndex(
    std::string_view bytes, std::optional<uint64_t> expected_corpus_fingerprint) {
  BinaryReader r(bytes);
  ParsedTraceShardIndex parsed;
  if (absl::Status st = r.ReadU32(&parsed.header.magic); !st.ok()) {
    return st;
  }
  if (parsed.header.magic != kTraceShardMagic) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Invalid trace shard magic: 0x%08x (expected 0x%08x)", parsed.header.magic,
                        kTraceShardMagic));
  }
  if (absl::Status st = r.ReadU32(&parsed.header.format_version); !st.ok()) {
    return st;
  }
  if (parsed.header.format_version != kTraceShardFormatVersion) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Unsupported trace shard format version %u (expected %u)",
                        parsed.header.format_version, kTraceShardFormatVersion));
  }
  if (absl::Status st = r.ReadU32(&parsed.header.shard_index); !st.ok()) {
    return st;
  }
  if (absl::Status st = r.ReadU32(&parsed.header.num_shards); !st.ok()) {
    return st;
  }
  if (parsed.header.num_shards == 0 || parsed.header.shard_index >= parsed.header.num_shards) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Invalid shard coordinates: shard_index=%u num_shards=%u",
                        parsed.header.shard_index, parsed.header.num_shards));
  }
  if (absl::Status st = r.ReadU32(&parsed.header.total_corpus_cases); !st.ok()) {
    return st;
  }
  if (absl::Status st = r.ReadU64(&parsed.header.corpus_fingerprint); !st.ok()) {
    return st;
  }
  if (expected_corpus_fingerprint.has_value() &&
      parsed.header.corpus_fingerprint != *expected_corpus_fingerprint) {
    return absl::FailedPreconditionError(
        absl::StrFormat("Trace shard corpus fingerprint mismatch: got 0x%016x != expected 0x%016x",
                        parsed.header.corpus_fingerprint, *expected_corpus_fingerprint));
  }

  uint32_t record_count = 0;
  if (absl::Status st = r.ReadU32(&record_count); !st.ok()) {
    return st;
  }
  if (record_count > r.RemainingBytes() / 12) {
    return absl::InvalidArgumentError("Trace shard index table exceeds remaining buffer");
  }
  parsed.entries.resize(record_count);
  for (uint32_t i = 0; i < record_count; ++i) {
    if (absl::Status st = r.ReadU32(&parsed.entries[i].case_index); !st.ok()) {
      return st;
    }
    if (absl::Status st = r.ReadU32(&parsed.entries[i].byte_offset); !st.ok()) {
      return st;
    }
    if (absl::Status st = r.ReadU32(&parsed.entries[i].byte_length); !st.ok()) {
      return st;
    }
  }

  const size_t payload_size = r.RemainingBytes();
  parsed.payload_base_offset = bytes.size() - payload_size;

  size_t expected_offset = 0;
  for (size_t i = 0; i < parsed.entries.size(); ++i) {
    const TraceShardIndexEntry& entry = parsed.entries[i];
    if (i > 0 && entry.case_index <= parsed.entries[i - 1].case_index) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Trace shard index case_index not strictly ascending at entry %zu: %u <= %u", i,
          entry.case_index, parsed.entries[i - 1].case_index));
    }
    if (entry.byte_offset > payload_size || entry.byte_length > payload_size - entry.byte_offset) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Trace shard index entry %zu (case_index=%u) out of bounds: offset=%u length=%u "
          "payload_size=%zu",
          i, entry.case_index, entry.byte_offset, entry.byte_length, payload_size));
    }
    if (entry.byte_offset != expected_offset) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Trace shard index entry %zu (case_index=%u) breaks contiguous payload tiling: "
          "offset=%u expected=%zu",
          i, entry.case_index, entry.byte_offset, expected_offset));
    }
    expected_offset += entry.byte_length;
  }
  if (expected_offset != payload_size) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Trace shard payload size mismatch: index covers %zu bytes, payload has %zu bytes",
        expected_offset, payload_size));
  }
  return parsed;
}

absl::StatusOr<DeserializedTraceShard> DeserializeTraceShard(
    std::string_view bytes, std::optional<uint64_t> expected_corpus_fingerprint) {
  absl::StatusOr<ParsedTraceShardIndex> index =
      ParseTraceShardIndex(bytes, expected_corpus_fingerprint);
  if (!index.ok()) {
    return index.status();
  }

  DeserializedTraceShard shard;
  shard.header = index->header;
  shard.records.resize(index->entries.size());
  for (size_t i = 0; i < index->entries.size(); ++i) {
    const TraceShardIndexEntry& entry = index->entries[i];
    const std::string_view payload =
        bytes.substr(index->payload_base_offset + entry.byte_offset, entry.byte_length);
    absl::StatusOr<FunctionCompilationTrace> trace = DeserializeFunctionCompilationTrace(payload);
    if (!trace.ok()) {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Failed to deserialize case_index=%u: %s", entry.case_index, trace.status().message()));
    }
    shard.records[i].case_index = entry.case_index;
    shard.records[i].trace = std::move(*trace);
  }
  return shard;
}

bool RtlControlFlowGraphEqualsExact(const RtlControlFlowGraph& lhs, const RtlControlFlowGraph& rhs,
                                    std::string* diff_detail) {
  if (lhs.pass != rhs.pass) {
    return SetDiff(diff_detail, absl::StrCat("pass mismatch: ", RtlPassExtension(lhs.pass), " vs ",
                                             RtlPassExtension(rhs.pass)));
  }
  if (lhs.has_explicit_trailing_void_return != rhs.has_explicit_trailing_void_return ||
      lhs.local_alloc_pseudo_reg_count != rhs.local_alloc_pseudo_reg_count ||
      lhs.global_alloc_pseudo_reg_count != rhs.global_alloc_pseudo_reg_count) {
    return SetDiff(diff_detail, "RtlControlFlowGraph metadata mismatch");
  }
  if (lhs.sideband != rhs.sideband) {
    return SetDiff(diff_detail, "RtlControlFlowGraph sideband mismatch");
  }
  if (lhs.rpo_blocks.size() != rhs.rpo_blocks.size()) {
    return SetDiff(diff_detail, absl::StrFormat("rpo_blocks size mismatch: %zu vs %zu",
                                                lhs.rpo_blocks.size(), rhs.rpo_blocks.size()));
  }
  for (size_t b = 0; b < lhs.rpo_blocks.size(); ++b) {
    const RtlBasicBlock& lb = lhs.rpo_blocks[b];
    const RtlBasicBlock& rb = rhs.rpo_blocks[b];
    if (lb.id != rb.id || lb.start_vram != rb.start_vram || lb.end_vram != rb.end_vram ||
        lb.instructions != rb.instructions || lb.successors != rb.successors ||
        lb.predecessors != rb.predecessors || lb.addr_vec_targets != rb.addr_vec_targets ||
        lb.has_label != rb.has_label || lb.has_trailing_barrier != rb.has_trailing_barrier ||
        lb.has_unreachable_entry_label != rb.has_unreachable_entry_label) {
      return SetDiff(diff_detail, absl::StrFormat("rpo_blocks[%zu] field mismatch", b));
    }
    if (lb.rtl_insns.size() != rb.rtl_insns.size()) {
      return SetDiff(diff_detail, absl::StrFormat("rpo_blocks[%zu].rtl_insns size mismatch", b));
    }
    for (size_t i = 0; i < lb.rtl_insns.size(); ++i) {
      std::string insn_diff;
      if (!RtlInsnEqualsExact(lb.rtl_insns[i], rb.rtl_insns[i], &insn_diff)) {
        return SetDiff(diff_detail,
                       absl::StrFormat("rpo_blocks[%zu].rtl_insns[%zu]: %s", b, i, insn_diff));
      }
    }
  }
  return true;
}

bool FunctionCompilationTraceEqualsExact(const FunctionCompilationTrace& lhs,
                                         const FunctionCompilationTrace& rhs,
                                         std::string* diff_detail) {
  if (lhs.test_name != rhs.test_name) {
    return SetDiff(diff_detail, absl::StrCat("test_name mismatch: '", lhs.test_name, "' vs '",
                                             rhs.test_name, "'"));
  }
  if (lhs.function_name != rhs.function_name) {
    return SetDiff(diff_detail, absl::StrCat("function_name mismatch: '", lhs.function_name,
                                             "' vs '", rhs.function_name, "'"));
  }
  if (lhs.optimize_level != rhs.optimize_level) {
    return SetDiff(diff_detail, absl::StrFormat("optimize_level mismatch: %d vs %d",
                                                lhs.optimize_level, rhs.optimize_level));
  }
  if (lhs.c_flags != rhs.c_flags) {
    return SetDiff(diff_detail, "c_flags mismatch");
  }
  if (lhs.gp_symbol_sizes != rhs.gp_symbol_sizes) {
    return SetDiff(diff_detail, "gp_symbol_sizes mismatch");
  }
  if (lhs.final_asm_text != rhs.final_asm_text) {
    return SetDiff(diff_detail, absl::StrCat("final_asm_text mismatch:\nlhs:\n", lhs.final_asm_text,
                                             "\nrhs:\n", rhs.final_asm_text));
  }
  if (lhs.final_insn_asm_lines != rhs.final_insn_asm_lines) {
    return SetDiff(diff_detail, "final_insn_asm_lines mismatch");
  }
  if (lhs.final_machine_words != rhs.final_machine_words) {
    return SetDiff(diff_detail, "final_machine_words mismatch");
  }
  if (lhs.stage0_input != rhs.stage0_input) {
    return SetDiff(diff_detail, "stage0_input mismatch");
  }
  if (lhs.pass_snapshots.size() != rhs.pass_snapshots.size()) {
    return SetDiff(diff_detail,
                   absl::StrFormat("pass_snapshots size mismatch: %zu vs %zu",
                                   lhs.pass_snapshots.size(), rhs.pass_snapshots.size()));
  }
  for (const auto& [pass, lhs_cfg] : lhs.pass_snapshots) {
    auto it = rhs.pass_snapshots.find(pass);
    if (it == rhs.pass_snapshots.end()) {
      return SetDiff(diff_detail,
                     absl::StrCat("Missing pass_snapshot ", RtlPassExtension(pass), " in rhs"));
    }
    std::string cfg_diff;
    if (!RtlControlFlowGraphEqualsExact(lhs_cfg, it->second, &cfg_diff)) {
      return SetDiff(diff_detail,
                     absl::StrCat("pass_snapshots[", RtlPassExtension(pass), "]: ", cfg_diff));
    }
  }
  if (lhs.subfunction_snapshots.size() != rhs.subfunction_snapshots.size()) {
    return SetDiff(diff_detail, absl::StrFormat("subfunction_snapshots size mismatch: %zu vs %zu",
                                                lhs.subfunction_snapshots.size(),
                                                rhs.subfunction_snapshots.size()));
  }
  for (size_t i = 0; i < lhs.subfunction_snapshots.size(); ++i) {
    std::string snap_diff;
    if (!SubfunctionSnapshotEqualsExact(lhs.subfunction_snapshots[i], rhs.subfunction_snapshots[i],
                                        &snap_diff)) {
      return SetDiff(diff_detail, absl::StrFormat("subfunction_snapshots[%zu]: %s", i, snap_diff));
    }
  }
  return true;
}

}  // namespace rom_nom_nom::gcc_harness
