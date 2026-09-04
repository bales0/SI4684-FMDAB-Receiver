#include "JPEGdecoder.h"

class MemoryFile {
 public:
  MemoryFile(const uint8_t* data, size_t size) : data_(data), size_(size), position_(0) {}
  explicit operator bool() const { return data_ != nullptr && size_ != 0; }
  int read() { return position_ < size_ ? data_[position_++] : -1; }
  bool seek(size_t position) {
    if (position > size_) return false;
    position_ = position;
    return true;
  }
  size_t position() const { return position_; }
  size_t remaining() const { return size_ - position_; }
  void close() {}

 private:
  const uint8_t* data_;
  size_t size_;
  size_t position_;
};

// ============================================================================
// JPEG decoder for ESP32 (no PSRAM)
//
// Uses multi-pass row-by-row decoding: for each MCU row, re-reads the file
// from the start and decodes all scans, storing only the current row's DCT
// coefficients in RAM (~15-20KB). This avoids needing the full 225KB+
// coefficient buffer that traditional progressive decoders require.
//
// A non-zero bitmap tracks which coefficient positions have been set by
// AC-first scans, so that AC-refine scans read the correct number of bits
// even when blocks are being discarded (not in the target row).
//
// Supports: SOF0 (baseline) and SOF2 (progressive DCT)
// Subsampling: YCbCr 4:4:4 / 4:2:2 / 4:2:0 / grayscale / non-standard
// Max image size: 320x240 pixels
// ============================================================================

#define PJ_MAX_COMPONENTS 3
#define PJ_MAX_HTABLES    4

// JPEG markers
#define M_SOF0  0xC0
#define M_SOF2  0xC2
#define M_DHT   0xC4
#define M_RST0  0xD0
#define M_RST7  0xD7
#define M_SOI   0xD8
#define M_EOI   0xD9
#define M_SOS   0xDA
#define M_DQT   0xDB
#define M_DRI   0xDD
#define M_APP0  0xE0
#define M_APP15 0xEF
#define M_COM   0xFE

static const uint8_t zigzag[64] = {
   0,  1,  8, 16,  9,  2,  3, 10,
  17, 24, 32, 25, 18, 11,  4,  5,
  12, 19, 26, 33, 40, 48, 41, 34,
  27, 20, 13,  6,  7, 14, 21, 28,
  35, 42, 49, 56, 57, 50, 43, 36,
  29, 22, 15, 23, 30, 37, 44, 51,
  58, 59, 52, 45, 38, 31, 39, 46,
  53, 60, 61, 54, 47, 55, 62, 63
};

static inline int pjExtend(int v, int bits) {
  int vt = 1 << (bits - 1);
  if (v < vt) v -= (1 << bits) - 1;
  return v;
}

static inline uint8_t pjClamp(int64_t v) {
  return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

// --- Non-zero coefficient bitmap ---
// Tracks which spectral positions (0-63) are non-zero for each block.
// Needed so AC refine scans read the correct number of refinement bits
// when discarding blocks not in the target MCU row.

static inline void nzSet(uint8_t* bm, int blockIdx, int k) {
  int bit = blockIdx * 64 + k;
  bm[bit >> 3] |= (1 << (bit & 7));
}

static inline bool nzGet(uint8_t* bm, int blockIdx, int k) {
  int bit = blockIdx * 64 + k;
  return (bm[bit >> 3] >> (bit & 7)) & 1;
}

// --- Huffman table ---
struct PJHuffTable {
  uint8_t bits[17];
  uint8_t vals[256];
  uint8_t lookLen[256];
  uint8_t lookSym[256];
  int32_t maxcode[18];
  int32_t mincode[18];
  int16_t valptr[18];
  int total;
  bool defined;
};

// Pre-compute the canonical Huffman lookup tables (mincode/maxcode/valptr)
// from the per-bit-length counts found in a DHT segment.
static bool pjBuildHuff(PJHuffTable* ht) {
  int code = 0, p = 0;
  uint16_t huffcode[256];
  uint8_t huffsize[256];

  for (int l = 1; l <= 16; l++) {
    if (p + ht->bits[l] > 256 ||
        code + ht->bits[l] > (1 << l)) return false;
    for (int i = 0; i < ht->bits[l]; i++) {
      huffsize[p] = l;
      huffcode[p] = code;
      p++;
      code++;
    }
    code <<= 1;
  }
  ht->total = p;

  p = 0;
  for (int l = 1; l <= 16; l++) {
    if (ht->bits[l]) {
      ht->valptr[l] = p;
      ht->mincode[l] = huffcode[p];
      ht->maxcode[l] = huffcode[p + ht->bits[l] - 1];
      p += ht->bits[l];
    } else {
      ht->mincode[l] = -1;
      ht->maxcode[l] = -1;
    }
  }
  ht->maxcode[17] = 0x7FFFF;

  memset(ht->lookLen, 0, sizeof(ht->lookLen));
  for (int i = 0; i < ht->total; i++) {
    if (huffsize[i] <= 8) {
      int prefix = huffcode[i] << (8 - huffsize[i]);
      int count = 1 << (8 - huffsize[i]);
      for (int j = 0; j < count; j++) {
        ht->lookLen[prefix + j] = huffsize[i];
        ht->lookSym[prefix + j] = ht->vals[i];
      }
    }
  }
  ht->defined = ht->total > 0;
  return ht->defined;
}

// --- Component info ---
struct PJComponent {
  uint8_t id;
  uint8_t hSamp, vSamp;
  uint8_t qtSel;
  int32_t dcPred;
};

// --- Bit reader ---
struct PJBitReader {
  MemoryFile* file;
  uint32_t buf;
  int bits;
  bool pendingMarker;
  uint8_t markerVal;
  bool eof;

  void init(MemoryFile* f) {
    file = f; buf = 0; bits = 0;
    pendingMarker = false; markerVal = 0; eof = false;
  }

  void resetBits() {
    buf = 0; bits = 0;
  }

  bool readEntropyByte(uint8_t& value) {
    if (pendingMarker || eof) return false;
    int c = file->read();
    if (c < 0) { eof = true; return false; }
    if (c != 0xFF) {
      value = static_cast<uint8_t>(c);
      return true;
    }

    int c2;
    do {
      c2 = file->read();
      if (c2 < 0) { eof = true; return false; }
    } while (c2 == 0xFF); // legal fill bytes before a marker

    if (c2 == 0x00) {
      value = 0xFF;
      return true;
    }

    pendingMarker = true;
    markerVal = static_cast<uint8_t>(c2);
    return false;
  }

  bool fillBits(int required) {
    if (required < 0 || required > 16) return false;
    while (bits < required) {
      uint8_t c;
      if (!readEntropyByte(c)) return false;
      if (bits > 24) return false;
      buf = (buf << 8) | c;
      bits += 8;
    }
    return true;
  }

  bool getBits(int n, uint32_t& value) {
    if (n == 0) { value = 0; return true; }
    if (!fillBits(n)) return false;
    bits -= n;
    value = (buf >> bits) & ((1U << n) - 1U);
    return true;
  }

  bool getBit(uint32_t& value) { return getBits(1, value); }

  bool peekBits(int n, uint32_t& value) {
    if (!fillBits(n)) return false;
    value = (buf >> (bits - n)) & ((1U << n) - 1U);
    return true;
  }

  bool skipBits(int n) {
    if (n < 0 || bits < n) return false;
    bits -= n;
    return true;
  }

  bool readBoundaryMarker(uint8_t& marker) {
    // A completed scan unit may leave at most seven mandatory 1-padding bits.
    // Never discard a complete, still-valid entropy byte at a marker boundary.
    if (bits > 7) return false;
    if (bits > 0) {
      const uint32_t mask = (1U << bits) - 1U;
      if ((buf & mask) != mask) return false;
    }
    resetBits();
    if (pendingMarker) {
      marker = markerVal;
      pendingMarker = false;
      markerVal = 0;
      return true;
    }
    if (eof) return false;

    int c = file->read();
    if (c != 0xFF) return false;
    do {
      c = file->read();
      if (c < 0) { eof = true; return false; }
    } while (c == 0xFF);
    if (c == 0x00) return false;
    marker = static_cast<uint8_t>(c);
    return true;
  }

  bool takePendingMarker(uint8_t& marker) {
    if (!pendingMarker) return false;
    marker = markerVal;
    pendingMarker = false;
    markerVal = 0;
    return true;
  }

};

// --- Huffman decode ---
static bool pjHuffDecode(PJBitReader* br, PJHuffTable* ht, uint8_t& symbol) {
  if (!ht->defined) return false;

  uint32_t look;
  if (br->peekBits(8, look) && ht->lookLen[look]) {
    if (!br->skipBits(ht->lookLen[look])) return false;
    symbol = ht->lookSym[look];
    return true;
  }

  int code = 0;
  for (int l = 1; l <= 16; l++) {
    uint32_t bit;
    if (!br->getBit(bit)) return false;
    code = (code << 1) | static_cast<int>(bit);
    if (ht->maxcode[l] >= 0 && code <= ht->maxcode[l]) {
      const int index = ht->valptr[l] + code - ht->mincode[l];
      if (index < 0 || index >= ht->total) return false;
      symbol = ht->vals[index];
      return true;
    }
  }
  return false;
}

static bool pjReceive(PJBitReader* br, int nbits, int& value) {
  if (nbits <= 0 || nbits > 16) return false;
  uint32_t bits;
  if (!br->getBits(nbits, bits)) return false;
  value = pjExtend(static_cast<int>(bits), nbits);
  return true;
}

// --- JPEG decoder state ---
struct PJDecoder {
  uint16_t width, height;
  uint8_t nComp;
  PJComponent comp[PJ_MAX_COMPONENTS];
  uint8_t maxH, maxV;
  uint16_t mcuW, mcuH;
  uint16_t mcuCntX, mcuCntY;
  uint8_t blocksPerMCU;

  uint16_t qtable[4][64];
  bool qtableDefined[4];
  PJHuffTable dcHuff[PJ_MAX_HTABLES];
  PJHuffTable acHuff[PJ_MAX_HTABLES];
  uint16_t restartInterval;
  uint8_t nextRestart;
  bool progressive;

  PJBitReader br;

  uint8_t scanNComp;
  uint8_t scanCompIdx[PJ_MAX_COMPONENTS];
  uint8_t scanDcTbl[PJ_MAX_COMPONENTS];
  uint8_t scanAcTbl[PJ_MAX_COMPONENTS];
  uint8_t ss, se, ah, al;

  int eobRun;
  int mcuCount;

  // Global block indexing for bitmap
  int compBlockOffset[PJ_MAX_COMPONENTS];
  int totalImageBlocks;
};

// --- Compute global block offsets after SOF parsing ---
static void pjComputeBlockOffsets(PJDecoder* d) {
  int offset = 0;
  for (int c = 0; c < d->nComp; c++) {
    d->compBlockOffset[c] = offset;
    offset += (d->mcuCntX * d->comp[c].hSamp) * (d->mcuCntY * d->comp[c].vSamp);
  }
  d->totalImageBlocks = offset;
}

// Compute global block index for a block at (blockCol, blockRow) of component ci
static inline int pjGlobalBlockIdx(PJDecoder* d, int ci, int blockCol, int blockRow) {
  return d->compBlockOffset[ci] + blockRow * (d->mcuCntX * d->comp[ci].hSamp) + blockCol;
}

// --- Marker reading helpers ---
static bool pjRead8(MemoryFile& f, uint8_t& value) {
  const int c = f.read();
  if (c < 0) return false;
  value = static_cast<uint8_t>(c);
  return true;
}

static bool pjRead16(MemoryFile& f, uint16_t& value) {
  uint8_t hi, lo;
  if (!pjRead8(f, hi) || !pjRead8(f, lo)) return false;
  value = (static_cast<uint16_t>(hi) << 8) | lo;
  return true;
}

static bool pjSkip(MemoryFile& f, size_t count) {
  if (count > f.remaining()) return false;
  return f.seek(f.position() + count);
}

static bool pjSegmentPayloadLength(MemoryFile& f, size_t& payloadLength) {
  uint16_t length;
  if (!pjRead16(f, length) || length < 2U) return false;
  payloadLength = static_cast<size_t>(length - 2U);
  return payloadLength <= f.remaining();
}

// --- Parse DQT ---
// DQT (Define Quantization Table) parser - up to 4 tables, 8 or 16 bit each.
static bool pjParseDQT(MemoryFile& f, PJDecoder* d) {
  size_t len;
  if (!pjSegmentPayloadLength(f, len)) return false;
  while (len != 0U) {
    uint8_t info;
    if (len < 1U || !pjRead8(f, info)) return false;
    --len;
    const uint8_t tblIdx = info & 0x0FU;
    const uint8_t prec = info >> 4;
    if (tblIdx >= 4U || prec > 1U) return false;
    const size_t tableBytes = prec ? 128U : 64U;
    if (len < tableBytes) return false;
    for (int i = 0; i < 64; i++) {
      uint16_t value;
      if (prec) {
        if (!pjRead16(f, value)) return false;
      } else {
        uint8_t value8;
        if (!pjRead8(f, value8)) return false;
        value = value8;
      }
      if (value == 0U) return false;
      d->qtable[tblIdx][zigzag[i]] = value;
    }
    len -= tableBytes;
    d->qtableDefined[tblIdx] = true;
  }
  return true;
}

// --- Parse DHT ---
// DHT (Define Huffman Table) parser - stores DC/AC tables for each component.
static bool pjParseDHT(MemoryFile& f, PJDecoder* d) {
  size_t len;
  if (!pjSegmentPayloadLength(f, len)) return false;
  while (len != 0U) {
    if (len < 17U) return false;
    uint8_t info;
    if (!pjRead8(f, info)) return false;
    --len;
    const uint8_t cls = info >> 4;
    const uint8_t tblIdx = info & 0x0FU;
    if (cls > 1U || tblIdx >= PJ_MAX_HTABLES) return false;
    PJHuffTable* ht = (cls == 0) ? &d->dcHuff[tblIdx] : &d->acHuff[tblIdx];
    ht->defined = false;
    int total = 0;
    for (int i = 1; i <= 16; i++) {
      if (!pjRead8(f, ht->bits[i])) return false;
      --len;
      total += ht->bits[i];
    }
    if (total <= 0 || total > 256 || len < static_cast<size_t>(total))
      return false;
    for (int i = 0; i < total; i++) {
      if (!pjRead8(f, ht->vals[i])) return false;
      --len;
    }
    if (!pjBuildHuff(ht)) return false;
  }
  return true;
}

// --- Parse SOF0/SOF2 ---
// SOF (Start Of Frame) parser - extracts image dimensions and subsampling info.
static bool pjParseSOF(MemoryFile& f, PJDecoder* d, bool progressive) {
  size_t len;
  uint8_t precision, nComp;
  uint16_t height, width;
  if (!pjSegmentPayloadLength(f, len) || len < 6U ||
      !pjRead8(f, precision) || !pjRead16(f, height) ||
      !pjRead16(f, width) || !pjRead8(f, nComp)) return false;
  if (precision != 8U || width == 0U || height == 0U ||
      width > 320U || height > 240U ||
      (nComp != 1U && nComp != 3U) ||
      len != 6U + 3U * nComp) return false;

  d->width = width;
  d->height = height;
  d->nComp = nComp;
  d->progressive = progressive;

  d->maxH = 0; d->maxV = 0;
  uint8_t blocksPerMCU = 0;
  for (int i = 0; i < d->nComp; i++) {
    uint8_t id, samp, qtSel;
    if (!pjRead8(f, id) || !pjRead8(f, samp) || !pjRead8(f, qtSel))
      return false;
    for (int prior = 0; prior < i; ++prior)
      if (d->comp[prior].id == id) return false;
    d->comp[i].id = id;
    d->comp[i].hSamp = (samp >> 4) & 0x0F;
    d->comp[i].vSamp = samp & 0x0F;
    d->comp[i].qtSel = qtSel;
    if (d->comp[i].hSamp == 0U || d->comp[i].hSamp > 4U ||
        d->comp[i].vSamp == 0U || d->comp[i].vSamp > 4U ||
        qtSel >= 4U) return false;
    blocksPerMCU += d->comp[i].hSamp * d->comp[i].vSamp;
    if (d->comp[i].hSamp > d->maxH) d->maxH = d->comp[i].hSamp;
    if (d->comp[i].vSamp > d->maxV) d->maxV = d->comp[i].vSamp;
  }
  if (d->maxH == 0U || d->maxV == 0U || blocksPerMCU == 0U ||
      blocksPerMCU > 10U) return false;

  d->mcuW = d->maxH * 8;
  d->mcuH = d->maxV * 8;
  d->mcuCntX = (d->width + d->mcuW - 1) / d->mcuW;
  d->mcuCntY = (d->height + d->mcuH - 1) / d->mcuH;

  d->blocksPerMCU = blocksPerMCU;

  pjComputeBlockOffsets(d);
  return true;
}

// --- Parse SOS ---
// SOS (Start Of Scan) parser - reads component selectors and the band/Ah/Al
// fields that drive progressive-mode pass dispatch.
static bool pjParseSOS(MemoryFile& f, PJDecoder* d) {
  size_t len;
  uint8_t scanNComp;
  if (!pjSegmentPayloadLength(f, len) || !pjRead8(f, scanNComp) ||
      scanNComp == 0U || scanNComp > d->nComp ||
      len != 4U + 2U * scanNComp) return false;
  d->scanNComp = scanNComp;

  for (int i = 0; i < d->scanNComp; i++) {
    uint8_t id, tbl;
    if (!pjRead8(f, id) || !pjRead8(f, tbl)) return false;
    int found = -1;
    for (int c = 0; c < d->nComp; c++) {
      if (d->comp[c].id == id) { found = c; break; }
    }
    if (found < 0) return false;
    for (int prior = 0; prior < i; ++prior)
      if (d->scanCompIdx[prior] == found) return false;
    d->scanCompIdx[i] = static_cast<uint8_t>(found);
    d->scanDcTbl[i] = (tbl >> 4) & 0x0F;
    d->scanAcTbl[i] = tbl & 0x0F;
    if (d->scanDcTbl[i] >= PJ_MAX_HTABLES ||
        d->scanAcTbl[i] >= PJ_MAX_HTABLES) return false;
  }
  uint8_t approx;
  if (!pjRead8(f, d->ss) || !pjRead8(f, d->se) ||
      !pjRead8(f, approx)) return false;
  d->ah = (approx >> 4) & 0x0F;
  d->al = approx & 0x0F;

  if (!d->progressive) {
    if (d->scanNComp != d->nComp || d->ss != 0U || d->se != 63U ||
        d->ah != 0U || d->al != 0U) return false;
  } else {
    if (d->ss > d->se || d->se > 63U || d->ah > 13U || d->al > 13U ||
        (d->ss == 0U && d->se != 0U) ||
        (d->ss != 0U && d->scanNComp != 1U) ||
        (d->ah != 0U && d->ah != d->al + 1U)) return false;
  }

  for (int i = 0; i < d->scanNComp; ++i) {
    const uint8_t ci = d->scanCompIdx[i];
    if (!d->qtableDefined[d->comp[ci].qtSel]) return false;
    if (!d->progressive) {
      if (!d->dcHuff[d->scanDcTbl[i]].defined ||
          !d->acHuff[d->scanAcTbl[i]].defined) return false;
    } else {
      if (d->ss == 0U && d->ah == 0U &&
          !d->dcHuff[d->scanDcTbl[i]].defined) return false;
      if (d->ss != 0U && !d->acHuff[d->scanAcTbl[i]].defined)
        return false;
    }
  }
  return true;
}

// --- Parse DRI ---
static bool pjParseDRI(MemoryFile& f, PJDecoder* d) {
  size_t len;
  uint16_t interval;
  if (!pjSegmentPayloadLength(f, len) || len != 2U ||
      !pjRead16(f, interval)) return false;
  d->restartInterval = interval;
  return true;
}

// --- Entropy decoders ---

static bool pjDecodeDCFirst(PJDecoder* d, int16_t* coef, int compScanIdx) {
  PJHuffTable* ht = &d->dcHuff[d->scanDcTbl[compScanIdx]];
  uint8_t s;
  if (!pjHuffDecode(&d->br, ht, s) || s > 11U) return false;
  int diff = 0;
  if (s > 0U && !pjReceive(&d->br, s, diff)) return false;
  int ci = d->scanCompIdx[compScanIdx];
  d->comp[ci].dcPred += diff;
  const int32_t value = d->comp[ci].dcPred * (1L << d->al);
  if (value < INT16_MIN || value > INT16_MAX) return false;
  coef[0] = static_cast<int16_t>(value);
  return true;
}

static bool pjDecodeDCRefine(PJDecoder* d, int16_t* coef) {
  uint32_t bit;
  if (!d->br.getBit(bit)) return false;
  coef[0] |= static_cast<int16_t>(bit << d->al);
  return true;
}

static bool pjDecodeACFirst(PJDecoder* d, int16_t* coef, int compScanIdx) {
  PJHuffTable* ht = &d->acHuff[d->scanAcTbl[compScanIdx]];

  if (d->eobRun > 0) { d->eobRun--; return true; }

  for (int k = d->ss; k <= d->se; k++) {
    uint8_t rs;
    if (!pjHuffDecode(&d->br, ht, rs)) return false;
    int s = rs & 0x0F;
    int r = rs >> 4;
    if (s > 10) return false;

    if (s == 0) {
      if (r == 15) {
        if (k + 15 > d->se) return false;
        k += 15;
      } else {
        d->eobRun = (1 << r);
        if (r > 0) {
          uint32_t extra;
          if (!d->br.getBits(r, extra)) return false;
          d->eobRun += static_cast<int>(extra);
        }
        d->eobRun--;
        return true;
      }
    } else {
      k += r;
      if (k > d->se) return false;
      int v;
      if (!pjReceive(&d->br, s, v)) return false;
      const int32_t value = static_cast<int32_t>(v) * (1L << d->al);
      if (value < INT16_MIN || value > INT16_MAX) return false;
      coef[zigzag[k]] = static_cast<int16_t>(value);
    }
  }
  return true;
}

static bool pjDecodeACRefine(PJDecoder* d, int16_t* coef, int compScanIdx) {
  PJHuffTable* ht = &d->acHuff[d->scanAcTbl[compScanIdx]];
  int p1 = 1 << d->al;
  int m1 = -(1 << d->al);
  int k = d->ss;

  if (d->eobRun == 0) {
    while (k <= d->se) {
      uint8_t rs;
      if (!pjHuffDecode(&d->br, ht, rs)) return false;
      int s = rs & 0x0F;
      int r = rs >> 4;

      if (s == 0) {
        if (r < 15) {
          d->eobRun = (1 << r);
          if (r > 0) {
            uint32_t extra;
            if (!d->br.getBits(r, extra)) return false;
            d->eobRun += static_cast<int>(extra);
          }
          break;
        }
      } else if (s != 1) {
        return false;
      }

      int newVal = 0;
      if (s == 1) {
        uint32_t bit;
        if (!d->br.getBit(bit)) return false;
        newVal = bit ? p1 : m1;
      }

      bool runCompleted = false;
      while (k <= d->se) {
        int zz = zigzag[k];
        if (coef[zz] != 0) {
          uint32_t bit;
          if (!d->br.getBit(bit)) return false;
          if (bit) {
            const int value = coef[zz] + (coef[zz] > 0 ? p1 : m1);
            if (value < INT16_MIN || value > INT16_MAX) return false;
            coef[zz] = static_cast<int16_t>(value);
          }
        } else {
          if (r == 0) {
            if (s == 1) coef[zz] = newVal;
            runCompleted = true;
            k++;
            break;
          }
          r--;
        }
        k++;
      }
      if (!runCompleted) return false;
    }
  }

  if (d->eobRun > 0) {
    while (k <= d->se) {
      int zz = zigzag[k];
      if (coef[zz] != 0) {
        uint32_t bit;
        if (!d->br.getBit(bit)) return false;
        if (bit) {
          const int value = coef[zz] + (coef[zz] > 0 ? p1 : m1);
          if (value < INT16_MIN || value > INT16_MAX) return false;
          coef[zz] = static_cast<int16_t>(value);
        }
      }
      k++;
    }
    d->eobRun--;
  }
  return true;
}

// --- Baseline block decode (DC + all AC in one call) ---
static bool pjDecodeBaseline(PJDecoder* d, int16_t* coef, int compScanIdx) {
  PJHuffTable* dcHt = &d->dcHuff[d->scanDcTbl[compScanIdx]];
  uint8_t s;
  if (!pjHuffDecode(&d->br, dcHt, s) || s > 11U) return false;
  int diff = 0;
  if (s > 0U && !pjReceive(&d->br, s, diff)) return false;
  int ci = d->scanCompIdx[compScanIdx];
  d->comp[ci].dcPred += diff;
  if (d->comp[ci].dcPred < INT16_MIN || d->comp[ci].dcPred > INT16_MAX)
    return false;
  coef[0] = d->comp[ci].dcPred;

  PJHuffTable* acHt = &d->acHuff[d->scanAcTbl[compScanIdx]];
  for (int k = 1; k <= 63; k++) {
    uint8_t rs;
    if (!pjHuffDecode(&d->br, acHt, rs)) return false;
    int r = rs >> 4;
    s = rs & 0x0F;
    if (s > 10U) return false;
    if (s == 0) {
      if (r == 15) {
        if (k + 15 > 63) return false;
        k += 15;
        continue;
      }
      break;
    }
    k += r;
    if (k > 63) return false;
    int value;
    if (!pjReceive(&d->br, s, value)) return false;
    coef[zigzag[k]] = static_cast<int16_t>(value);
  }
  return true;
}

// --- Decode one block ---
// When store=true, writes to coef (target row buffer).
// When store=false, uses local dummy; bitmap tracks non-zero positions
// so AC refine reads the correct number of bits.
static bool pjDecodeBlock(PJDecoder* d, int16_t* coef, int compScanIdx,
                          bool store, uint8_t* nzBitmap, int globalBlockIdx) {
  int16_t dummy[64];
  int16_t* target;

  if (store) {
    target = coef;
  } else {
    memset(dummy, 0, sizeof(dummy));
    // AC refine in discard mode: restore non-zero pattern from bitmap
    if (d->ss > 0 && d->ah > 0 && nzBitmap) {
      for (int k = d->ss; k <= d->se; k++) {
        if (nzGet(nzBitmap, globalBlockIdx, k)) {
          dummy[zigzag[k]] = 1; // any non-zero value
        }
      }
    }
    target = dummy;
  }

  if (d->ss == 0 && d->se == 0) {
    if (d->ah == 0) {
      if (!pjDecodeDCFirst(d, target, compScanIdx)) return false;
    } else if (!pjDecodeDCRefine(d, target)) return false;
  } else {
    if (d->ah == 0) {
      if (!pjDecodeACFirst(d, target, compScanIdx)) return false;
    } else if (!pjDecodeACRefine(d, target, compScanIdx)) return false;
  }

  // Update bitmap for discarded AC blocks
  if (!store && nzBitmap && d->ss > 0) {
    for (int k = d->ss; k <= d->se; k++) {
      if (target[zigzag[k]] != 0) {
        nzSet(nzBitmap, globalBlockIdx, k);
      }
    }
  }
  return true;
}

// --- Handle restart marker ---
static bool pjHandleRestart(PJDecoder* d, bool moreScanUnits) {
  if (d->restartInterval == 0U) return true;
  ++d->mcuCount;
  if (d->mcuCount < d->restartInterval) return true;
  if (d->mcuCount != d->restartInterval) return false;

  if (!moreScanUnits) return true; // JPEG has no RST after the final interval.

  uint8_t marker;
  if (!d->br.readBoundaryMarker(marker) ||
      marker != static_cast<uint8_t>(M_RST0 + d->nextRestart)) return false;

  d->nextRestart = static_cast<uint8_t>((d->nextRestart + 1U) & 7U);
  d->mcuCount = 0;
  for (int i = 0; i < d->nComp; ++i) d->comp[i].dcPred = 0;
  d->eobRun = 0;
  return true;
}

// --- Get block index within MCU row buffer ---
static int pjRowBlockIndex(PJDecoder* d, int mcuX, int compIdx, int bh, int bv) {
  int offset = 0;
  for (int c = 0; c < compIdx; c++) {
    offset += d->comp[c].hSamp * d->comp[c].vSamp;
  }
  offset += bv * d->comp[compIdx].hSamp + bh;
  return mcuX * d->blocksPerMCU + offset;
}

// --- Decode a scan's entropy data for the target MCU row ---
static bool pjDecodeScan(PJDecoder* d, int16_t* rowCoefs, int targetMCURow,
                         uint8_t* nzBitmap, bool& completeScan) {
  d->br.resetBits();
  d->eobRun = 0;
  d->mcuCount = 0;
  d->nextRestart = 0;
  completeScan = false;
  for (int i = 0; i < d->nComp; i++) d->comp[i].dcPred = 0;

  if (d->scanNComp > 1) {
    // --- Interleaved scan ---
    const int scanMCUs = d->mcuCntX * d->mcuCntY;
    const int totalMCUs = d->mcuCntX * (targetMCURow + 1);
    int startMCU = d->mcuCntX * targetMCURow;

    for (int mcu = 0; mcu < totalMCUs; mcu++) {
      bool store = (mcu >= startMCU);
      int mcuX = mcu % d->mcuCntX;
      int mcuY = mcu / d->mcuCntX;

      for (int si = 0; si < d->scanNComp; si++) {
        int ci = d->scanCompIdx[si];
        for (int bv = 0; bv < d->comp[ci].vSamp; bv++) {
          for (int bh = 0; bh < d->comp[ci].hSamp; bh++) {
            int16_t* coef = nullptr;
            if (store) {
              int idx = pjRowBlockIndex(d, mcuX, ci, bh, bv);
              coef = &rowCoefs[idx * 64];
            }
            int blockCol = mcuX * d->comp[ci].hSamp + bh;
            int blockRow = mcuY * d->comp[ci].vSamp + bv;
            int gbi = pjGlobalBlockIdx(d, ci, blockCol, blockRow);
            if (!pjDecodeBlock(d, coef, si, store, nzBitmap, gbi))
              return false;
          }
        }
      }
      if (!pjHandleRestart(d, mcu + 1 < scanMCUs)) return false;
    }
    completeScan = totalMCUs == scanMCUs;
  } else {
    // --- Non-interleaved scan (single component) ---
    int ci = d->scanCompIdx[0];
    const int blockCols =
        (static_cast<int>(d->width) * d->comp[ci].hSamp +
         d->maxH * 8 - 1) / (d->maxH * 8);
    const int blockRows =
        (static_cast<int>(d->height) * d->comp[ci].vSamp +
         d->maxV * 8 - 1) / (d->maxV * 8);
    int startBlockRow = targetMCURow * d->comp[ci].vSamp;
    int endBlockRow = startBlockRow + d->comp[ci].vSamp - 1;
    if (endBlockRow >= blockRows) endBlockRow = blockRows - 1;
    if (startBlockRow > endBlockRow) return false;
    const int scanBlocks = blockCols * blockRows;
    const int totalBlocks = blockCols * (endBlockRow + 1);

    for (int blk = 0; blk < totalBlocks; blk++) {
      int bCol = blk % blockCols;
      int bRow = blk / blockCols;
      bool store = (bRow >= startBlockRow && bRow <= endBlockRow);

      int16_t* coef = nullptr;
      if (store) {
        int mcuX = bCol / d->comp[ci].hSamp;
        int bh = bCol % d->comp[ci].hSamp;
        int bv = bRow - startBlockRow;
        int idx = pjRowBlockIndex(d, mcuX, ci, bh, bv);
        coef = &rowCoefs[idx * 64];
      }
      int gbi = pjGlobalBlockIdx(d, ci, bCol, bRow);
      if (!pjDecodeBlock(d, coef, 0, store, nzBitmap, gbi)) return false;
      if (!pjHandleRestart(d, blk + 1 < scanBlocks)) return false;
    }
    completeScan = totalBlocks == scanBlocks;
  }
  return true;
}

// --- Integer IDCT (LLM algorithm, 13-bit fixed point) ---
#define FIX_0_298  2446
#define FIX_0_390  3196
#define FIX_0_541  4433
#define FIX_0_765  6270
#define FIX_0_899  7373
#define FIX_1_175  9633
#define FIX_1_501 12299
#define FIX_1_847 15137
#define FIX_1_961 16069
#define FIX_2_053 16819
#define FIX_2_562 20995
#define FIX_3_072 25172

#define IDCT_BITS  13
#define PASS1_BITS 2

// 8x8 inverse DCT (AAN scaled algorithm). Reads dequantized coefficients,
// writes back 64 spatial-domain samples clipped to 0..255.
static void pjIDCT(int16_t* coef, const uint16_t* qt, uint8_t* out) {
  int64_t ws[64];

  // Pass 1: columns (dequantize + butterfly)
  for (int col = 0; col < 8; col++) {
    int64_t s0 = static_cast<int64_t>(coef[0*8+col]) * qt[0*8+col];
    int64_t s1 = static_cast<int64_t>(coef[1*8+col]) * qt[1*8+col];
    int64_t s2 = static_cast<int64_t>(coef[2*8+col]) * qt[2*8+col];
    int64_t s3 = static_cast<int64_t>(coef[3*8+col]) * qt[3*8+col];
    int64_t s4 = static_cast<int64_t>(coef[4*8+col]) * qt[4*8+col];
    int64_t s5 = static_cast<int64_t>(coef[5*8+col]) * qt[5*8+col];
    int64_t s6 = static_cast<int64_t>(coef[6*8+col]) * qt[6*8+col];
    int64_t s7 = static_cast<int64_t>(coef[7*8+col]) * qt[7*8+col];

    if (!(s1 | s2 | s3 | s4 | s5 | s6 | s7)) {
      int64_t dc = s0 * (1 << PASS1_BITS);
      for (int i = 0; i < 8; i++) ws[i*8+col] = dc;
      continue;
    }

    int64_t z2 = s2, z3 = s6;
    int64_t z1 = (z2 + z3) * FIX_0_541;
    int64_t t2 = z1 - z3 * FIX_1_847;
    int64_t t3 = z1 + z2 * FIX_0_765;
    int64_t t0 = (s0 + s4) * (1 << IDCT_BITS);
    int64_t t1 = (s0 - s4) * (1 << IDCT_BITS);
    int64_t t10 = t0 + t3, t13 = t0 - t3;
    int64_t t11 = t1 + t2, t12 = t1 - t2;

    z1 = s7 + s1; z2 = s5 + s3; z3 = s7 + s3; int64_t z4 = s5 + s1;
    int64_t z5 = (z3 + z4) * FIX_1_175;
    t0 = s7 * FIX_0_298; t1 = s5 * FIX_2_053;
    t2 = s3 * FIX_3_072; t3 = s1 * FIX_1_501;
    z1 *= -FIX_0_899; z2 *= -FIX_2_562; z3 *= -FIX_1_961; z4 *= -FIX_0_390;
    z3 += z5; z4 += z5;
    t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;

    int64_t rnd = 1 << (IDCT_BITS - PASS1_BITS - 1);
    int shift = IDCT_BITS - PASS1_BITS;
    ws[0*8+col] = (t10 + t3 + rnd) >> shift;
    ws[7*8+col] = (t10 - t3 + rnd) >> shift;
    ws[1*8+col] = (t11 + t2 + rnd) >> shift;
    ws[6*8+col] = (t11 - t2 + rnd) >> shift;
    ws[2*8+col] = (t12 + t1 + rnd) >> shift;
    ws[5*8+col] = (t12 - t1 + rnd) >> shift;
    ws[3*8+col] = (t13 + t0 + rnd) >> shift;
    ws[4*8+col] = (t13 - t0 + rnd) >> shift;
  }

  // Pass 2: rows (butterfly + output 8-bit pixels)
  for (int row = 0; row < 8; row++) {
    int64_t* w = ws + row * 8;

    if (!(w[1] | w[2] | w[3] | w[4] | w[5] | w[6] | w[7])) {
      uint8_t v = pjClamp(((w[0] + (1 << (PASS1_BITS + 2))) >> (PASS1_BITS + 3)) + 128);
      for (int i = 0; i < 8; i++) out[row*8+i] = v;
      continue;
    }

    int64_t z2 = w[2], z3 = w[6];
    int64_t z1 = (z2 + z3) * FIX_0_541;
    int64_t t2 = z1 - z3 * FIX_1_847;
    int64_t t3 = z1 + z2 * FIX_0_765;
    int64_t t0 = (w[0] + w[4]) * (1 << IDCT_BITS);
    int64_t t1 = (w[0] - w[4]) * (1 << IDCT_BITS);
    int64_t t10 = t0 + t3, t13 = t0 - t3;
    int64_t t11 = t1 + t2, t12 = t1 - t2;

    z1 = w[7] + w[1]; z2 = w[5] + w[3]; z3 = w[7] + w[3]; int64_t z4 = w[5] + w[1];
    int64_t z5 = (z3 + z4) * FIX_1_175;
    t0 = w[7] * FIX_0_298; t1 = w[5] * FIX_2_053;
    t2 = w[3] * FIX_3_072; t3 = w[1] * FIX_1_501;
    z1 *= -FIX_0_899; z2 *= -FIX_2_562; z3 *= -FIX_1_961; z4 *= -FIX_0_390;
    z3 += z5; z4 += z5;
    t0 += z1 + z3; t1 += z2 + z4; t2 += z2 + z3; t3 += z1 + z4;

    int64_t rnd = 1 << (IDCT_BITS + PASS1_BITS + 2);
    int shift = IDCT_BITS + PASS1_BITS + 3;
    out[row*8+0] = pjClamp(((t10 + t3 + rnd) >> shift) + 128);
    out[row*8+7] = pjClamp(((t10 - t3 + rnd) >> shift) + 128);
    out[row*8+1] = pjClamp(((t11 + t2 + rnd) >> shift) + 128);
    out[row*8+6] = pjClamp(((t11 - t2 + rnd) >> shift) + 128);
    out[row*8+2] = pjClamp(((t12 + t1 + rnd) >> shift) + 128);
    out[row*8+5] = pjClamp(((t12 - t1 + rnd) >> shift) + 128);
    out[row*8+3] = pjClamp(((t13 + t0 + rnd) >> shift) + 128);
    out[row*8+4] = pjClamp(((t13 - t0 + rnd) >> shift) + 128);
  }
}

// --- YCbCr to RGB565 ---
static inline uint16_t pjYCbCrToRGB565(int y, int cb, int cr) {
  cb -= 128; cr -= 128;
  int r = y + ((91881 * cr + 32768) >> 16);
  int g = y - ((22554 * cb + 46802 * cr + 32768) >> 16);
  int b = y + ((116130 * cb + 32768) >> 16);
  uint16_t pixel = ((pjClamp(r) >> 3) << 11) | ((pjClamp(g) >> 2) << 5) | (pjClamp(b) >> 3);
  return pixel;
}

// --- Render one MCU row to TFT ---
// IDCT every block in one MCU row, then convert YCbCr→RGB565 and push the
// resulting pixel rows to the TFT (centered horizontally and vertically).
static void pjOutputMCURow(PJDecoder* d, int16_t* rowCoefs, int mcuRow,
                           TFT_eSPI& tft, int offsetX, int offsetY,
                           uint8_t* allBlocks) {
  int totalBlocks = d->mcuCntX * d->blocksPerMCU;
  uint16_t lineBuffer[320];

  // IDCT all blocks in this row
  for (int b = 0; b < totalBlocks; b++) {
    int blockInMCU = b % d->blocksPerMCU;
    int compIdx = 0, acc = 0;
    for (int c = 0; c < d->nComp; c++) {
      int nb = d->comp[c].hSamp * d->comp[c].vSamp;
      if (blockInMCU < acc + nb) { compIdx = c; break; }
      acc += nb;
    }
    pjIDCT(&rowCoefs[b * 64], d->qtable[d->comp[compIdx].qtSel], &allBlocks[b * 64]);
  }

  // Output pixel rows
  for (int py = 0; py < (int)d->mcuH; py++) {
    int absY = mcuRow * d->mcuH + py;
    if (absY >= d->height) break;

    for (int mcuX = 0; mcuX < d->mcuCntX; mcuX++) {
      int mcuBase = mcuX * d->blocksPerMCU;

      for (int px = 0; px < (int)d->mcuW; px++) {
        int absX = mcuX * d->mcuW + px;
        if (absX >= d->width) break;

        int yVal, cbVal, crVal;

        if (d->nComp == 1) {
          const int sx = px * d->comp[0].hSamp / d->maxH;
          const int sy = py * d->comp[0].vSamp / d->maxV;
          int bi = mcuBase + (sy / 8) * d->comp[0].hSamp + (sx / 8);
          yVal = allBlocks[bi * 64 + (sy % 8) * 8 + (sx % 8)];
          cbVal = crVal = 128;
        } else {
          // Y
          int yPx = px * d->comp[0].hSamp / d->maxH;
          int yPy = py * d->comp[0].vSamp / d->maxV;
          int yBi = mcuBase + (yPy / 8) * d->comp[0].hSamp + (yPx / 8);
          yVal = allBlocks[yBi * 64 + (yPy % 8) * 8 + (yPx % 8)];
          // Cb
          int cbOff = d->comp[0].hSamp * d->comp[0].vSamp;
          int cbPx = px * d->comp[1].hSamp / d->maxH;
          int cbPy = py * d->comp[1].vSamp / d->maxV;
          int cbBi = mcuBase + cbOff + (cbPy / 8) * d->comp[1].hSamp + (cbPx / 8);
          cbVal = allBlocks[cbBi * 64 + (cbPy % 8) * 8 + (cbPx % 8)];
          // Cr
          int crOff = cbOff + d->comp[1].hSamp * d->comp[1].vSamp;
          int crPx = px * d->comp[2].hSamp / d->maxH;
          int crPy = py * d->comp[2].vSamp / d->maxV;
          int crBi = mcuBase + crOff + (crPy / 8) * d->comp[2].hSamp + (crPx / 8);
          crVal = allBlocks[crBi * 64 + (crPy % 8) * 8 + (crPx % 8)];
        }

        lineBuffer[absX] = pjYCbCrToRGB565(yVal, cbVal, crVal);
      }
    }

    tft.pushImage(offsetX, offsetY + absY, d->width, 1, lineBuffer);
  }
}

// --- Skip to next marker ---
static bool pjNextMarker(MemoryFile& f, uint8_t& marker) {
  int c = f.read();
  if (c != 0xFF) return false;
  do {
    c = f.read();
    if (c < 0) return false;
  } while (c == 0xFF); // marker fill bytes
  if (c == 0x00) return false; // stuffing is only valid in entropy data
  marker = static_cast<uint8_t>(c);
  return true;
}

// --- Skip entropy data to next marker ---
static bool pjSkipEntropy(MemoryFile& f, uint8_t& marker) {
  while (true) {
    int c = f.read();
    if (c < 0) return false;
    if (c == 0xFF) {
      int c2;
      do {
        c2 = f.read();
        if (c2 < 0) return false;
      } while (c2 == 0xFF);
      if (c2 != 0x00) {
        marker = static_cast<uint8_t>(c2);
        return true;
      }
    }
  }
}

static bool pjFinishOrSkipScan(PJDecoder* d, bool completeScan,
                               uint8_t& marker) {
  if (completeScan) {
    if (!d->br.readBoundaryMarker(marker)) return false;
    return marker < M_RST0 || marker > M_RST7;
  }

  d->br.resetBits();
  if (!d->br.takePendingMarker(marker) &&
      !pjSkipEntropy(*d->br.file, marker)) return false;

  // We deliberately skipped the part of this scan below the requested output
  // row. Cross any remaining restart intervals until the real scan terminator.
  while (marker >= M_RST0 && marker <= M_RST7) {
    if (!pjSkipEntropy(*d->br.file, marker)) return false;
  }
  return true;
}

static bool pjSkipVariableSegment(MemoryFile& f) {
  size_t len;
  return pjSegmentPayloadLength(f, len) && pjSkip(f, len);
}

static bool pjIsSOFMarker(uint8_t marker) {
  return marker >= 0xC0U && marker <= 0xCFU &&
         marker != M_DHT && marker != 0xC8U && marker != 0xCCU;
}

// A complete baseline scan normally ends directly at EOI. Accept legal
// length-coded marker segments between the scan and EOI, but never accept a
// second scan or another frame process in the baseline-only path.
static bool pjConsumeToEOI(MemoryFile& f, uint8_t marker) {
  while (true) {
    if (marker == M_EOI) return true;
    if (marker == 0x01U) {
      if (!pjNextMarker(f, marker)) return false;
      continue;
    }
    if (marker == M_SOI || marker == M_SOS || pjIsSOFMarker(marker) ||
        (marker >= M_RST0 && marker <= M_RST7) ||
        !pjSkipVariableSegment(f) || !pjNextMarker(f, marker)) return false;
  }
}

static bool pjValidateProgression(PJDecoder* d,
                                  int8_t coefficientBits[PJ_MAX_COMPONENTS][64]) {
  for (int si = 0; si < d->scanNComp; ++si) {
    const uint8_t ci = d->scanCompIdx[si];
    for (int k = d->ss; k <= d->se; ++k) {
      if (d->ah == 0U) {
        if (coefficientBits[ci][k] >= 0) return false;
      } else if (coefficientBits[ci][k] != static_cast<int8_t>(d->ah)) {
        return false;
      }
    }
  }
  for (int si = 0; si < d->scanNComp; ++si) {
    const uint8_t ci = d->scanCompIdx[si];
    for (int k = d->ss; k <= d->se; ++k)
      coefficientBits[ci][k] = static_cast<int8_t>(d->al);
  }
  return true;
}

// --- Process entire file for one MCU row ---
// One pass over the file for the progressive decoder: replays every scan,
// only retaining coefficients that belong to the target MCU row.
static bool pjProcessFileForRow(MemoryFile& f, PJDecoder* d, int16_t* rowCoefs,
                                 int targetRow, uint8_t* nzBitmap) {
  if (!f.seek(0)) return false;
  uint8_t first, second;
  if (!pjRead8(f, first) || !pjRead8(f, second) ||
      first != 0xFF || second != M_SOI) return false;

  memset(d->qtableDefined, 0, sizeof(d->qtableDefined));
  for (int i = 0; i < PJ_MAX_HTABLES; ++i) {
    d->dcHuff[i].defined = false;
    d->acHuff[i].defined = false;
  }
  d->restartInterval = 0;
  d->progressive = true;

  bool sofDone = false;
  bool sawScan = false;
  bool haveMarker = false;
  uint8_t marker = 0;
  int8_t coefficientBits[PJ_MAX_COMPONENTS][64];
  memset(coefficientBits, -1, sizeof(coefficientBits));

  while (true) {
    if (!haveMarker && !pjNextMarker(f, marker)) return false;
    haveMarker = false;
    if (marker == M_EOI) {
      if (!sofDone || !sawScan) return false;
      for (int ci = 0; ci < d->nComp; ++ci)
        if (coefficientBits[ci][0] < 0) return false;
      return true;
    }
    if (marker == 0x01) continue; // stand-alone TEM marker
    if (marker == M_SOI ||
        (marker >= M_RST0 && marker <= M_RST7)) return false;

    switch (marker) {
      case M_SOF2:
        if (!sofDone) {
          if (!pjParseSOF(f, d, true)) return false;
          sofDone = true;
        } else return false;
        break;
      case M_SOF0:
        return false;
      case M_DHT:
        if (!pjParseDHT(f, d)) return false;
        break;
      case M_DQT:
        if (!pjParseDQT(f, d)) return false;
        break;
      case M_DRI:
        if (!pjParseDRI(f, d)) return false;
        break;
      case M_SOS:
        if (!sofDone || !pjParseSOS(f, d) ||
            !pjValidateProgression(d, coefficientBits)) return false;
        d->br.init(&f);
        {
          bool completeScan;
          if (!pjDecodeScan(d, rowCoefs, targetRow, nzBitmap,
                            completeScan) ||
              !pjFinishOrSkipScan(d, completeScan, marker)) return false;
        }
        sawScan = true;
        haveMarker = true;
        break;
      default:
        if (pjIsSOFMarker(marker) || !pjSkipVariableSegment(f)) return false;
        break;
    }
  }
}

// --- Baseline single-pass decode ---
// Single-pass baseline decoder: walks the file once, decoding and rendering
// each MCU row on the fly. Used for SOF0 images where no multi-pass needed.
static bool pjDecodeBaselinePass(MemoryFile& f, PJDecoder* d, TFT_eSPI& tft,
                                  int offsetX, int offsetY) {
  if (!f.seek(0)) return false;
  uint8_t first, second;
  if (!pjRead8(f, first) || !pjRead8(f, second) ||
      first != 0xFF || second != M_SOI) return false;

  memset(d->qtableDefined, 0, sizeof(d->qtableDefined));
  for (int i = 0; i < PJ_MAX_HTABLES; ++i) {
    d->dcHuff[i].defined = false;
    d->acHuff[i].defined = false;
  }
  d->restartInterval = 0;
  d->progressive = false;
  bool sofDone = false;

  while (true) {
    uint8_t marker;
    if (!pjNextMarker(f, marker) || marker == M_EOI || marker == M_SOI ||
        (marker >= M_RST0 && marker <= M_RST7))
      return false;
    if (marker == 0x01) continue;

    switch (marker) {
      case M_SOF0:
        if (sofDone || !pjParseSOF(f, d, false)) return false;
        sofDone = true;
        break;
      case M_SOF2:
        return false;
      case M_DHT:
        if (!pjParseDHT(f, d)) return false;
        break;
      case M_DQT:
        if (!pjParseDQT(f, d)) return false;
        break;
      case M_DRI:
        if (!pjParseDRI(f, d)) return false;
        break;
      case M_SOS: {
        if (!sofDone || !pjParseSOS(f, d)) return false;
        d->br.init(&f);

        const size_t blocksPerRow =
            static_cast<size_t>(d->mcuCntX) * d->blocksPerMCU;
        const size_t coefSize = blocksPerRow * 64U * sizeof(int16_t);
        const size_t pixelBufSize = blocksPerRow * 64U;
        const size_t totalSize = coefSize + pixelBufSize;
        uint8_t* baseBuf = (uint8_t*)malloc(totalSize);
        if (!baseBuf) return false;
        int16_t* rowCoefs = (int16_t*)baseBuf;
        uint8_t* allBlocks = baseBuf + coefSize;

        d->mcuCount = 0;
        d->nextRestart = 0;
        for (int i = 0; i < d->nComp; i++) d->comp[i].dcPred = 0;

        const int scanMCUs = d->mcuCntX * d->mcuCntY;
        for (int row = 0; row < d->mcuCntY; row++) {
          memset(rowCoefs, 0, coefSize);
          bool rowComplete = true;

          for (int mcuX = 0; mcuX < d->mcuCntX; mcuX++) {
            for (int si = 0; si < d->scanNComp; si++) {
              int ci = d->scanCompIdx[si];
              for (int bv = 0; bv < d->comp[ci].vSamp; bv++) {
                for (int bh = 0; bh < d->comp[ci].hSamp; bh++) {
                  int idx = pjRowBlockIndex(d, mcuX, ci, bh, bv);
                  if (!pjDecodeBaseline(d, &rowCoefs[idx * 64], si)) {
                    rowComplete = false;
                    break;
                  }
                }
                if (!rowComplete) break;
              }
              if (!rowComplete) break;
            }
            if (!rowComplete) break;
            const int decodedMCU = row * d->mcuCntX + mcuX;
            if (!pjHandleRestart(d, decodedMCU + 1 < scanMCUs)) {
              rowComplete = false;
              break;
            }
          }

          if (!rowComplete) {
            free(baseBuf);
            return false;
          }
          pjOutputMCURow(d, rowCoefs, row, tft, offsetX, offsetY, allBlocks);
        }

        uint8_t terminalMarker;
        const bool endedCleanly = d->br.readBoundaryMarker(terminalMarker) &&
                                  pjConsumeToEOI(f, terminalMarker);
        free(baseBuf);
        return endedCleanly;
      }
      default:
        if (pjIsSOFMarker(marker) || !pjSkipVariableSegment(f)) return false;
        break;
    }
  }
  return false;
}

// --- Main entry point ---
// Public entry point: opens the file, parses headers, then dispatches to
// either the single-pass baseline decoder (SOF0) or the multi-pass
// progressive decoder (SOF2). Returns true on success.
bool JPEGdecoder(const uint8_t* data, size_t size, TFT_eSPI& tft,
                 int displayWidth, int displayHeight) {
  MemoryFile f(data, size);
  if (!f) return false;

  PJDecoder* d = (PJDecoder*)calloc(1, sizeof(PJDecoder));
  if (!d) { f.close(); return false; }

  // Pre-scan to get dimensions and the supported frame process.
  if (!f.seek(0)) { free(d); return false; }
  uint8_t first, second;
  if (!pjRead8(f, first) || !pjRead8(f, second) ||
      first != 0xFF || second != M_SOI) {
    free(d); f.close(); return false;
  }
  bool foundSOF = false;
  bool isBaseline = false;
  while (!foundSOF) {
    uint8_t marker;
    if (!pjNextMarker(f, marker) || marker == M_EOI) break;
    if (marker == M_SOF0) {
      if (!pjParseSOF(f, d, false)) break;
      isBaseline = true;
      foundSOF = true;
    } else if (marker == M_SOF2) {
      if (!pjParseSOF(f, d, true)) break;
      foundSOF = true;
    } else if (marker == 0x01) {
      continue;
    } else if (marker == M_SOI || pjIsSOFMarker(marker) ||
               (marker >= M_RST0 && marker <= M_RST7) ||
               !pjSkipVariableSegment(f)) break;
  }

  if (!foundSOF || displayWidth <= 0 || displayHeight <= 0 ||
      d->width > static_cast<uint16_t>(displayWidth) ||
      d->height > static_cast<uint16_t>(displayHeight)) {
    free(d); f.close(); return false;
  }

  int offsetX = (displayWidth - d->width) / 2;
  int offsetY = (displayHeight - d->height) / 2;

  bool result;
  if (isBaseline) {
    result = pjDecodeBaselinePass(f, d, tft, offsetX, offsetY);
  } else {
    // Progressive: multi-pass row-by-row decode
    // Single allocation to reduce heap fragmentation on ESP32
    const size_t blocksPerRow =
        static_cast<size_t>(d->mcuCntX) * d->blocksPerMCU;
    const size_t coefSize = blocksPerRow * 64U * sizeof(int16_t);
    const size_t pixelBufSize = blocksPerRow * 64U;
    const size_t bitmapSize = static_cast<size_t>(d->totalImageBlocks) * 8U;
    const size_t totalSize = coefSize + pixelBufSize + bitmapSize;

    uint8_t* progBuf = (uint8_t*)calloc(1, totalSize);
    if (!progBuf) {
      free(d); f.close();
      return false;
    }
    uint8_t* nzBitmap = progBuf + coefSize + pixelBufSize;

    int16_t* rowCoefs = (int16_t*)progBuf;
    uint8_t* allBlocks = progBuf + coefSize;

    result = true;
    for (int row = 0; row < d->mcuCntY; row++) {
      memset(rowCoefs, 0, coefSize);
      memset(nzBitmap, 0, bitmapSize);
      if (!pjProcessFileForRow(f, d, rowCoefs, row, nzBitmap)) {
        result = false;
        break;
      }
      pjOutputMCURow(d, rowCoefs, row, tft, offsetX, offsetY, allBlocks);
    }

    free(progBuf);
  }

  free(d);
  f.close();
  return result;
}
