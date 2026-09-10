#pragma once

// Dependency-free 8-bit grayscale PNG encoder for host-side render snapshots
// (see host_render_target.h). No zlib/libpng: PNG's IDAT payload is just a
// zlib stream, and zlib/DEFLATE (RFC 1950/1951) explicitly allows "stored"
// (uncompressed) deflate blocks -- a real, spec-legal deflate stream, just
// an uncompressed one -- so a valid PNG needs nothing but a few fixed
// headers, a CRC-32, and an Adler-32 checksum, all straightforward to
// hand-roll. Files come out larger than a compressed PNG would (no run
// benefit taken), which is a non-issue for small test-only snapshots.
//
// Test-only: not part of the firmware or any non-test build.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace transit_test {

namespace png_detail {

inline uint32_t crc32(const uint8_t* data, size_t len) {
  static uint32_t table[256];
  static bool tableReady = false;
  if (!tableReady) {
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      }
      table[n] = c;
    }
    tableReady = true;
  }
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; ++i) {
    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFu;
}

inline uint32_t adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1, b = 0;
  constexpr uint32_t kMod = 65521u;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % kMod;
    b = (b + a) % kMod;
  }
  return (b << 16) | a;
}

inline void appendBigEndian32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>(v & 0xFF));
}

// Appends one PNG chunk: length + type + data + CRC(type+data).
inline void appendChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
  appendBigEndian32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> typeAndData;
  typeAndData.reserve(4 + data.size());
  typeAndData.insert(typeAndData.end(), type, type + 4);
  typeAndData.insert(typeAndData.end(), data.begin(), data.end());
  out.insert(out.end(), typeAndData.begin(), typeAndData.end());
  appendBigEndian32(out, crc32(typeAndData.data(), typeAndData.size()));
}

// Wraps `raw` in a minimal zlib stream (RFC 1950 2-byte header + Adler-32
// trailer) made of RFC 1951 "stored" (uncompressed) deflate blocks, each up
// to 65535 bytes.
inline std::vector<uint8_t> zlibStoredStream(const std::vector<uint8_t>& raw) {
  std::vector<uint8_t> out;
  out.reserve(raw.size() + raw.size() / 65535 * 5 + 8);
  out.push_back(0x78);  // CMF: deflate, 32K window
  out.push_back(0x01);  // FLG: no preset dict, check bits for 0x7801 to be a multiple of 31

  size_t offset = 0;
  const size_t total = raw.size();
  do {
    const size_t remaining = total - offset;
    const size_t blockLen = remaining < 65535 ? remaining : 65535;
    const bool isFinal = (offset + blockLen) >= total;
    out.push_back(isFinal ? 0x01 : 0x00);  // BFINAL/BTYPE=00 (stored)
    out.push_back(static_cast<uint8_t>(blockLen & 0xFF));
    out.push_back(static_cast<uint8_t>((blockLen >> 8) & 0xFF));
    const uint16_t nlen = static_cast<uint16_t>(~static_cast<uint16_t>(blockLen));
    out.push_back(static_cast<uint8_t>(nlen & 0xFF));
    out.push_back(static_cast<uint8_t>((nlen >> 8) & 0xFF));
    out.insert(out.end(), raw.begin() + static_cast<long>(offset),
               raw.begin() + static_cast<long>(offset + blockLen));
    offset += blockLen;
  } while (offset < total);
  if (total == 0) {
    // Degenerate one-block stream so a zero-byte image still round-trips.
    out.push_back(0x01);
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0xFF);
    out.push_back(0xFF);
  }

  appendBigEndian32(out, adler32(raw.data(), raw.size()));
  return out;
}

}  // namespace png_detail

// Encodes `pixels` (row-major, one byte per pixel, 0=black..255=white) as an
// 8-bit grayscale (color type 0) PNG and writes it to `path`. Returns false
// only on an I/O failure opening/writing the file; the encoding itself
// cannot fail for a well-formed width/height/pixels triple.
inline bool writeGrayscalePng(const std::string& path, int width, int height,
                              const std::vector<uint8_t>& pixels) {
  // Each scanline is prefixed with a filter-type byte (0 = None -- no
  // prediction, matching the "no compression benefit taken" trade-off noted
  // above).
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(height) * (static_cast<size_t>(width) + 1));
  for (int y = 0; y < height; ++y) {
    raw.push_back(0);  // filter type: None
    const uint8_t* row = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width);
    raw.insert(raw.end(), row, row + width);
  }

  std::vector<uint8_t> file;
  static constexpr uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  file.insert(file.end(), kSignature, kSignature + 8);

  std::vector<uint8_t> ihdr;
  png_detail::appendBigEndian32(ihdr, static_cast<uint32_t>(width));
  png_detail::appendBigEndian32(ihdr, static_cast<uint32_t>(height));
  ihdr.push_back(8);  // bit depth
  ihdr.push_back(0);  // color type: grayscale
  ihdr.push_back(0);  // compression method: deflate
  ihdr.push_back(0);  // filter method: adaptive (per-scanline byte, all None here)
  ihdr.push_back(0);  // interlace method: none
  png_detail::appendChunk(file, "IHDR", ihdr);

  png_detail::appendChunk(file, "IDAT", png_detail::zlibStoredStream(raw));
  png_detail::appendChunk(file, "IEND", {});

  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  const size_t written = std::fwrite(file.data(), 1, file.size(), f);
  std::fclose(f);
  return written == file.size();
}

}  // namespace transit_test
