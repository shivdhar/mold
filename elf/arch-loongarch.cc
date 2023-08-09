// This file contains LoongArch-specific code. LoongArch is a clean RISC
// ISA. It supports PC-relative load/store instructions. All instructions
// are 4 bytes long.
//
// This file support LoongArch psABI v2 with relaxation not implemented.
// The reloactions from 20 to 46, 49 and 54 are deprecated in psABI v2.
//
// The TLSGD and TLSLD relocation type share GOT relocation type, which
// means can not think the symbol value as relocation value directly. It
// needs judgement like has_tlsgd(). How they share relocation types follows.
// a), TLS_{LD, GD}_PC_HI20 + GOT_PC_LO12 + GOT64_PC_LO20 + GOT64_PC_HI12,
// b), TLS_{LD, GD}_HI20 + GOT_LO12 + GOT64_LO20, GOT64_HI12.
//
// LoongArch use 2 instructions to get a 32bits address, and use 4 instruc-
// tions to get a 64bits address. It gets the 4K-page of the address plus
// 2KB at first. Then absolute instructions (ld, st, addi) get the detail.
// When the loaded address from got is local address, relaxation will
// relax it from pcalau12i+ld to pcalau12i+addi.
// When the load address range is PC ±1MB and 4bytes-align, relaxation
// will relax it from pcalau12i+addi to pcaddi.
// At present, relaxation is not implemented.
//
// https://reviews.llvm.org/D138135
// https://loongson.github.io/LoongArch-Documentation/LoongArch-ELF-ABI-EN.html

#include "mold.h"

namespace mold::elf {

static u32 hi20(u32 val) { return (val + 0x800) >> 12; }
static u32 lo12(u32 val) { return val & 0xfff; }

static i64 alau32_hi20(i64 val, i64 pc) {
  return ((val + ((val & 0x800) << 1)) & ~0xfffl) - (pc & ~0xfffl);
}

static i64 alau64_hi32(i64 val, i64 pc) {
  return (val - ((val & 0x800l) << 21)) - (pc & ~0xffffffffl);
}

static void writeJ20(u8 *loc, u32 val) {
  *(ul32 *)loc &= 0b11111110'00000000'00000000'00011111;
  *(ul32 *)loc |= (val & 0xfffff) << 5;
}

static void writeK12(u8 *loc, u32 val) {
  *(ul32 *)loc &= 0b11111111'11110000'00000011'11111111;
  *(ul32 *)loc |= (val & 0xfff) << 10;
}

static void writeD5k16(u8 *loc, u32 val) {
  u32 hi = val >> 16;
  *(ul32 *)loc &= 0b11111100'00000000'00000011'11100000;
  *(ul32 *)loc |= ((val & 0xffff) << 10) | (hi & 0x1f);
}

static void writeD10k16(u8 *loc, u32 val) {
  u32 hi = val >> 16;
  *(ul32 *)loc &= 0b11111100'00000000'00000000'00000000;
  *(ul32 *)loc |= ((val & 0xffff) << 10) | (hi & 0x3ff);
}

static void writeK16(u8 *loc, u32 val) {
  *(ul32 *)loc &= 0b11111100'00000000'00000011'11111111;
  *(ul32 *)loc |= (val & 0xffff) << 10;
}

static void overwrite_uleb(u8 *loc, u64 val) {
  while (*loc & 0b1000'0000) {
    *loc++ = 0b1000'0000 | (val & 0b0111'1111);
    val >>= 7;
  }
  *loc = val & 0b0111'1111;
}

template <typename E>
void write_plt_header(Context<E> &ctx, u8 *buf) {
  static const ul32 insn_64[] = {
    0x1c00'000e, // pcaddu12i $t2, %hi(%pcrel(.got.plt))
    0x0011'bdad, // sub.d     $t1, $t1, $t3
    0x28c0'01cf, // ld.d      $t3, $t2, %lo(%pcrel(.got.plt)) # _dl_runtime_resolve
    0x02ff'51ad, // addi.d    $t1, $t1, -44                   # .plt entry
    0x02c0'01cc, // addi.d    $t0, $t2, %lo(%pcrel(.got.plt)) # &.got.plt
    0x0045'05ad, // srli.d    $t1, $t1, 1                     # .plt entry offset
    0x28c0'218c, // ld.d      $t0, $t0, 8                     # link map
    0x4c00'01e0, // jr        $t3
  };

  static const ul32 insn_32[] = {
    0x1c00'000e, // pcaddu12i $t2, %hi(%pcrel(.got.plt))
    0x0011'3dad, // sub.w     $t1, $t1, $t3
    0x2880'01cf, // ld.w      $t3, $t2, %lo(%pcrel(.got.plt)) # _dl_runtime_resolve
    0x02bf'51ad, // addi.w    $t1, $t1, -44                   # .plt entry
    0x0280'01cc, // addi.w    $t0, $t2, %lo(%pcrel(.got.plt)) # &.got.plt
    0x0044'89ad, // srli.w    $t1, $t1, 2                     # .plt entry offset
    0x2880'118c, // ld.w      $t0, $t0, 4                     # link map
    0x4c00'01e0, // jr        $t3
  };

  u64 gotplt = ctx.gotplt->shdr.sh_addr;
  u64 plt = ctx.plt->shdr.sh_addr;
  u32 offset = gotplt - plt;

  if constexpr (E::is_64)
    if (gotplt - plt + 0x8000'0800 > 0xffff'ffff)
      Error(ctx) << "Overflow when make plt header";

  if constexpr (E::is_64)
    memcpy(buf, insn_64, sizeof(insn_64));
  else
    memcpy(buf, insn_32, sizeof(insn_32));

  writeJ20(buf, hi20(offset));
  writeK12(buf + 8, lo12(offset));
  writeK12(buf + 16, lo12(offset));
}

static const ul32 plt_entry_64[] = {
  0x1c00000f, // pcaddu12i $t3, %hi(%pcrel(func@.got.plt))
  0x28c001ef, // ld.d      $t3, $t3, %lo(%pcrel(func@.got.plt))
  0x4c0001ed, // jirl      $t1, $t3, 0
  0x03400000, // nop
};

static const ul32 plt_entry_32[] = {
  0x1c00000f, // pcaddu12i $t3, %hi(%pcrel(func@.got.plt))
  0x288001ef, // ld.w      $t3, $t3, %lo(%pcrel(func@.got.plt))
  0x4c0001ed, // jirl      $t1, $t3, 0
  0x03400000, // nop
};

template <typename E>
void write_plt_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {
  u64 gotplt = sym.get_gotplt_addr(ctx);
  u64 plt = sym.get_plt_addr(ctx);
  u32 offset = gotplt - plt;

  if constexpr (E::is_64)
    if (gotplt - plt + 0x8000'0800 > 0xffff'ffff)
      Error(ctx) << "Overflow when make plt entry";

  if constexpr (E::is_64)
    memcpy(buf, plt_entry_64, sizeof(plt_entry_64));
  else
    memcpy(buf, plt_entry_32, sizeof(plt_entry_32));

  writeJ20(buf, hi20(offset));
  writeK12(buf + 4, lo12(offset));
}

template <typename E>
void write_pltgot_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym) {
  u64 got = sym.get_got_addr(ctx);
  u64 plt = sym.get_plt_addr(ctx);
  u32 offset = got - plt;

  if constexpr (E::is_64)
    if (got - plt + 0x8000'0800 > 0xffff'ffff)
      Error(ctx) << "Overflow when make pltgot entry";

  if constexpr (E::is_64)
    memcpy(buf, plt_entry_64, sizeof(plt_entry_64));
  else
    memcpy(buf, plt_entry_32, sizeof(plt_entry_32));

  writeJ20(buf, hi20(offset));
  writeK12(buf + 4, lo12(offset));
}

template <typename E>
void EhFrameSection<E>::apply_eh_reloc(Context<E> &ctx, const ElfRel<E> &rel,
                                    u64 offset, u64 val) {
  u8 *loc = ctx.buf + this->shdr.sh_offset + offset;

  switch (rel.r_type) {
  case R_NONE:
    break;
  case R_LARCH_ADD6:
    *loc = (*loc & 0b1100'0000) | (((*loc & 0b0011'1111) + val) & 0b0011'1111);
    break;
  case R_LARCH_ADD8:
    *loc += val;
    break;
  case R_LARCH_ADD16:
    *(U16<E> *)loc += val;
    break;
  case R_LARCH_ADD32:
    *(U32<E> *)loc += val;
    break;
  case R_LARCH_ADD64:
    *(U64<E> *)loc += val;
    break;
  case R_LARCH_SUB6:
    *loc = (*loc & 0b1100'0000) | (((*loc & 0b0011'1111) - val) & 0b0011'1111);
    break;
  case R_LARCH_SUB8:
    *loc -= val;
    break;
  case R_LARCH_SUB16:
    *(U16<E> *)loc -= val;
    break;
  case R_LARCH_SUB32:
    *(U32<E> *)loc -= val;
    break;
  case R_LARCH_SUB64:
    *(U64<E> *)loc -= val;
    break;
  case R_LARCH_32_PCREL:
    *(U32<E> *)loc = val - this->shdr.sh_addr - offset;
    break;
  case R_LARCH_64_PCREL:
    *(U64<E> *)loc = val - this->shdr.sh_addr - offset;
    break;
  default:
    Fatal(ctx) << "unsupported relocation in .eh_frame: " << rel;
  }
}

template <typename E>
void InputSection<E>::apply_reloc_alloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  ElfRel<E> *dynrel = nullptr;
  if (ctx.reldyn)
    dynrel = (ElfRel<E> *)(ctx.buf + ctx.reldyn->shdr.sh_offset +
                           file.reldyn_offset + this->reldyn_offset);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE)
      continue;

    // Maybe do something in the future.
    if (rel.r_type == R_LARCH_RELAX)
      continue;

    // Relocation notes, ignore them.
    if (rel.r_type == R_LARCH_MARK_LA || rel.r_type == R_LARCH_MARK_PCREL)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    auto checkrange = [&](i64 val, i64 lo, i64 hi) {
      if (val < lo || hi <= val)
        Error(ctx) << *this << ": relocation " << rel << " against "
                   << sym << " out of range: " << val << " is not in ["
                   << lo << ", " << hi << ")";
    };

    auto checkalign = [&](i64 val, i64 align) {
      if (val & (align - 1))
        Error(ctx) << *this << ": relocation " << rel << " against "
                   << sym << " unaligned: " << val << " needs "
                   << align << " bytes aligned";
    };

    u64 S = sym.get_addr(ctx);
    u64 A = rel.r_addend;
    u64 P = get_addr() + rel.r_offset;
    u64 G = sym.get_got_idx(ctx) * sizeof(Word<E>);
    u64 GP = ctx.got->shdr.sh_addr;
    u64 TP = ctx.tp_addr;
    u64 GD = sym.has_tlsgd(ctx) ? sym.get_tlsgd_addr(ctx) : 0;
    u64 IE = sym.has_gottp(ctx) ? sym.get_gottp_addr(ctx) : 0;

    auto get_got_or_gd = [&]() {
      if (sym.has_tlsgd(ctx))
        return GD;
      else
        return GP + G;
    };

    switch (rel.r_type) {
    case R_LARCH_32: {
      if constexpr (E::is_64)
        *(U32<E> *)loc = S + A;
      else
        apply_dyn_absrel(ctx, sym, rel, loc, S, A, P, &dynrel);
      break;
    }
    case R_LARCH_64: {
      assert(E::is_64);
      apply_dyn_absrel(ctx, sym, rel, loc, S, A, P, &dynrel);
      break;
    }
    case R_LARCH_B16: {
      i64 val = S + A - P;
      checkrange(val, -(1ll << 17), 1ll << 17);
      checkalign(val, 4);
      writeK16(loc, val >> 2);
      break;
    }
    case R_LARCH_B21: {
      i64 val = S + A - P;
      checkrange(val, -(1ll << 22), 1ll << 22);
      checkalign(val, 4);
      writeD5k16(loc, val >> 2);
      break;
    }
    case R_LARCH_B26: {
      i64 val = S + A - P;
      checkrange(val, -(1ll << 27), 1ll << 27);
      checkalign(val, 4);
      writeD10k16(loc, val >> 2);
      break;
    }
    case R_LARCH_ABS_HI20: {
      i64 val = S + A;
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_ABS_LO12: {
      i64 val = S + A;
      writeK12(loc, val);
      break;
    }
    case R_LARCH_ABS64_LO20: {
      i64 val = S + A;
      writeJ20(loc, val >> 32);
      break;
    }
    case R_LARCH_ABS64_HI12: {
      i64 val = S + A;
      writeK12(loc, val >> 52);
      break;
    }
    case R_LARCH_PCALA_HI20: {
      i64 val = alau32_hi20(S + A, P);
      checkrange(val, -(1ll << 31), 1ll << 31);
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_PCALA_LO12: {
      i64 val = S + A;
      writeK12(loc, val);
      break;
    }
    case R_LARCH_PCALA64_LO20: {
      i64 val = alau64_hi32(S + A, P);
      writeJ20(loc, val >> 32);
      break;
    }
    case R_LARCH_PCALA64_HI12: {
      i64 val = alau64_hi32(S + A, P);
      writeK12(loc, val >> 52);
      break;
    }
    case R_LARCH_GOT_PC_HI20: {
      i64 val = alau32_hi20(GP + G + A, P);
      checkrange(val, -(1ll << 31), 1ll << 31);
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_GOT_PC_LO12: {
      i64 val = get_got_or_gd() + A;
      writeK12(loc, val);
      break;
    }
    case R_LARCH_GOT64_PC_LO20: {
      i64 val = alau64_hi32(get_got_or_gd() + A, P);
      writeJ20(loc, val >> 32);
      break;
    }
    case R_LARCH_GOT64_PC_HI12: {
      i64 val = alau64_hi32(get_got_or_gd() + A, P);
      writeK12(loc, val >> 52);
      break;
    }
    case R_LARCH_GOT_HI20: {
      i64 val = GP + G + A;
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_GOT_LO12: {
      i64 val = get_got_or_gd() + A;
      writeK12(loc, val);
      break;
    }
    case R_LARCH_GOT64_LO20: {
      i64 val = get_got_or_gd() + A;
      writeJ20(loc, val >> 32);
      break;
    }
    case R_LARCH_GOT64_HI12: {
      i64 val = get_got_or_gd() + A;
      writeK12(loc, val >> 52);
      break;
    }
    case R_LARCH_TLS_LE_HI20: {
      i64 val = S + A - TP;
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_TLS_LE_LO12: {
      i64 val = S + A - TP;
      writeK12(loc, val);
      break;
    }
    case R_LARCH_TLS_LE64_LO20: {
      i64 val = S + A - TP;
      writeJ20(loc, val >> 32);
      break;
    }
    case R_LARCH_TLS_LE64_HI12: {
      i64 val = S + A - TP;
      writeK12(loc, val >> 52);
      break;
    }
    case R_LARCH_TLS_IE_PC_HI20: {
      i64 val = alau32_hi20(IE + A, P);
      checkrange(val, -(1ll << 31), 1ll << 31);
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_TLS_IE_PC_LO12: {
      i64 val = IE + A;
      writeK12(loc, val);
      break;
    }
    case R_LARCH_TLS_IE64_PC_LO20: {
      i64 val = alau64_hi32(IE + A, P);
      writeJ20(loc, val >> 32);
      break;
    }
    case R_LARCH_TLS_IE64_PC_HI12: {
      i64 val = alau64_hi32(IE + A, P);
      writeK12(loc, val >> 52);
      break;
    }
    case R_LARCH_TLS_IE_HI20: {
      i64 val = IE + A;
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_TLS_IE_LO12: {
      i64 val = IE + A;
      writeK12(loc, val);
      break;
    }
    case R_LARCH_TLS_IE64_LO20: {
      i64 val = IE + A;
      writeJ20(loc, val >> 32);
      break;
    }
    case R_LARCH_TLS_IE64_HI12: {
      i64 val = IE + A;
      writeK12(loc, val >> 52);
      break;
    }
    case R_LARCH_TLS_LD_PC_HI20:
    case R_LARCH_TLS_GD_PC_HI20: {
      i64 val = alau32_hi20(GD + A, P);
      checkrange(val, -(1ll << 31), 1ll << 31);
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_TLS_LD_HI20:
    case R_LARCH_TLS_GD_HI20: {
      i64 val = GD + A;
      writeJ20(loc, val >> 12);
      break;
    }
    case R_LARCH_ADD6:
      *loc = (*loc & 0b1100'0000) | (((*loc & 0b0011'1111) + S + A) & 0b0011'1111);
      break;
    case R_LARCH_ADD8:
      *loc += S + A;
      break;
    case R_LARCH_ADD16:
      *(U16<E> *)loc += S + A;
      break;
    case R_LARCH_ADD32:
      *(U32<E> *)loc += S + A;
      break;
    case R_LARCH_ADD64:
      *(U64<E> *)loc += S + A;
      break;
    case R_LARCH_SUB6:
      *loc = (*loc & 0b1100'0000) | (((*loc & 0b0011'1111) - S - A) & 0b0011'1111);
      break;
    case R_LARCH_SUB8:
      *loc -= S + A;
      break;
    case R_LARCH_SUB16:
      *(U16<E> *)loc -= S + A;
      break;
    case R_LARCH_SUB32:
      *(U32<E> *)loc -= S + A;
      break;
    case R_LARCH_SUB64:
      *(U64<E> *)loc -= S + A;
      break;
    case R_LARCH_32_PCREL:
      *(U32<E> *)loc = S + A - P;
      break;
    case R_LARCH_64_PCREL:
      *(U64<E> *)loc = S + A - P;
      break;
    case R_LARCH_ADD_ULEB128:
      overwrite_uleb(loc, read_uleb(loc) + S + A);
      break;
    case R_LARCH_SUB_ULEB128:
      overwrite_uleb(loc, read_uleb(loc) - S - A);
      break;
    default:
      unreachable();
    }

  }
}

template <typename E>
void InputSection<E>::apply_reloc_nonalloc(Context<E> &ctx, u8 *base) {
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    if (!sym.file) {
      record_undef_error(ctx, rel);
      continue;
    }

    SectionFragment<E> *frag;
    i64 frag_addend;
    std::tie(frag, frag_addend) = get_fragment(ctx, rel);

    u64 S = frag ? frag->get_addr(ctx) : sym.get_addr(ctx);
    u64 A = frag ? frag_addend : (i64)rel.r_addend;

    switch (rel.r_type) {
    case R_LARCH_32:
      *(U32<E> *)loc = S + A;
      break;
    case R_LARCH_64:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(U64<E> *)loc = *val;
      else
        *(U64<E> *)loc = S + A;
      break;
    case R_LARCH_ADD6:
      *loc = (*loc & 0b1100'0000) | (((*loc & 0b0011'1111) + S + A) & 0b0011'1111);
      break;
    case R_LARCH_ADD8:
      *loc += S + A;
      break;
    case R_LARCH_ADD16:
      *(U16<E> *)loc += S + A;
      break;
    case R_LARCH_ADD32:
      *(U32<E> *)loc += S + A;
      break;
    case R_LARCH_ADD64:
      *(U64<E> *)loc += S + A;
      break;
    case R_LARCH_SUB6:
      *loc = (*loc & 0b1100'0000) | (((*loc & 0b0011'1111) - S - A) & 0b0011'1111);
      break;
    case R_LARCH_SUB8:
      *loc -= S + A;
      break;
    case R_LARCH_SUB16:
      *(U16<E> *)loc -= S + A;
      break;
    case R_LARCH_SUB32:
      *(U32<E> *)loc -= S + A;
      break;
    case R_LARCH_SUB64:
      *(U64<E> *)loc -= S + A;
      break;
    case R_LARCH_TLS_DTPREL32:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(U32<E> *)loc = *val;
      else
        *(U32<E> *)loc = S + A - ctx.dtp_addr;
      break;
    case R_LARCH_TLS_DTPREL64:
      if (std::optional<u64> val = get_tombstone(sym, frag))
        *(U64<E> *)loc = *val;
      else
        *(U64<E> *)loc = S + A - ctx.dtp_addr;
      break;
    case R_LARCH_ADD_ULEB128:
      overwrite_uleb(loc, read_uleb(loc) + S + A);
      break;
    case R_LARCH_SUB_ULEB128:
      overwrite_uleb(loc, read_uleb(loc) - S - A);
      break;
    default:
      Fatal(ctx) << *this << ": invalid relocation for non-allocated sections: "
                 << rel;
      break;
    }

  }
}

template <typename E>
void InputSection<E>::scan_relocations(Context<E> &ctx) {
  assert(shdr().sh_flags & SHF_ALLOC);

  this->reldyn_offset = file.num_dynrel * sizeof(ElfRel<E>);
  std::span<const ElfRel<E>> rels = get_rels(ctx);

  // Scan relocations
  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_NONE)
      continue;

    if (rel.r_type == R_LARCH_RELAX)
      continue;

    if (rel.r_type == R_LARCH_MARK_LA || rel.r_type == R_LARCH_MARK_PCREL)
      continue;

    if (record_undef_error(ctx, rel))
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];

    if (!sym.file) {
      record_undef_error(ctx, rel);
      continue;
    }

    if (sym.is_ifunc())
      sym.flags |= (NEEDS_GOT | NEEDS_PLT);

    switch (rel.r_type) {
    case R_LARCH_32:
      if constexpr (E::is_64)
        scan_absrel(ctx, sym, rel);
      else
        scan_dyn_absrel(ctx, sym, rel);
      break;
    case R_LARCH_64:
      assert(E::is_64);
      scan_dyn_absrel(ctx, sym, rel);
      break;
    case R_LARCH_B26:
      if (sym.is_imported)
        sym.flags |= NEEDS_PLT;
      break;
    case R_LARCH_GOT_HI20:
    case R_LARCH_GOT_PC_HI20:
      sym.flags |= NEEDS_GOT;
      break;
    case R_LARCH_TLS_IE_HI20:
    case R_LARCH_TLS_IE_PC_HI20:
      sym.flags |= NEEDS_GOTTP;
      break;
    case R_LARCH_TLS_LD_PC_HI20:
    case R_LARCH_TLS_GD_PC_HI20:
    case R_LARCH_TLS_LD_HI20:
    case R_LARCH_TLS_GD_HI20:
      sym.flags |= NEEDS_TLSGD;
      break;
    case R_LARCH_32_PCREL:
    case R_LARCH_64_PCREL:
      scan_pcrel(ctx, sym, rel);
      break;
    case R_LARCH_TLS_LE_HI20:
    case R_LARCH_TLS_LE_LO12:
    case R_LARCH_TLS_LE64_LO20:
    case R_LARCH_TLS_LE64_HI12:
      check_tlsle(ctx, sym, rel);
      break;
    case R_LARCH_B16:
    case R_LARCH_B21:
    case R_LARCH_ABS_HI20:
    case R_LARCH_ABS_LO12:
    case R_LARCH_ABS64_LO20:
    case R_LARCH_ABS64_HI12:
    case R_LARCH_PCALA_HI20:
    case R_LARCH_PCALA_LO12:
    case R_LARCH_PCALA64_LO20:
    case R_LARCH_PCALA64_HI12:
    case R_LARCH_GOT_PC_LO12:
    case R_LARCH_GOT64_PC_LO20:
    case R_LARCH_GOT64_PC_HI12:
    case R_LARCH_GOT_LO12:
    case R_LARCH_GOT64_LO20:
    case R_LARCH_GOT64_HI12:
    case R_LARCH_TLS_IE_PC_LO12:
    case R_LARCH_TLS_IE64_PC_LO20:
    case R_LARCH_TLS_IE64_PC_HI12:
    case R_LARCH_TLS_IE_LO12:
    case R_LARCH_TLS_IE64_LO20:
    case R_LARCH_TLS_IE64_HI12:
    case R_LARCH_ADD6:
    case R_LARCH_SUB6:
    case R_LARCH_ADD8:
    case R_LARCH_SUB8:
    case R_LARCH_ADD16:
    case R_LARCH_SUB16:
    case R_LARCH_ADD32:
    case R_LARCH_SUB32:
    case R_LARCH_ADD64:
    case R_LARCH_SUB64:
    case R_LARCH_ADD_ULEB128:
    case R_LARCH_SUB_ULEB128:
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
}

#define INSTANTIATE(E)                                                          \
  template void write_plt_header(Context<E> &, u8 *);                           \
  template void write_plt_entry(Context<E> &, u8 *, Symbol<E> &);               \
  template void write_pltgot_entry(Context<E> &, u8 *, Symbol<E> &);            \
  template void                                                                 \
  EhFrameSection<E>::apply_eh_reloc(Context<E> &, const ElfRel<E> &, u64, u64); \
  template void InputSection<E>::apply_reloc_alloc(Context<E> &, u8 *);         \
  template void InputSection<E>::apply_reloc_nonalloc(Context<E> &, u8 *);      \
  template void InputSection<E>::scan_relocations(Context<E> &);

INSTANTIATE(LOONGARCH64);
INSTANTIATE(LOONGARCH32);

} // namespace mold::elf