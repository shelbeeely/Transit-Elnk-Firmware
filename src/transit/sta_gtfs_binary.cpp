// Real implementation of the sta_gtfs_binary.h reader — see that header for
// the file format this decodes.

#include "transit/sta_gtfs_binary.h"

#include <cmath>
#include <cstring>

namespace transit {
namespace sta {

namespace {

constexpr char kMagic[4] = {'S', 'T', 'A', '1'};

uint32_t readU32LE(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

float readF32LE(const uint8_t* p) {
  uint32_t bits = readU32LE(p);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

}  // namespace

bool readTableHeader(BinaryTableReader& reader, TableHeader& out) {
  uint8_t buf[kTableHeaderSize];
  if (!reader.readAt(0, buf, kTableHeaderSize)) return false;
  if (std::memcmp(buf, kMagic, sizeof(kMagic)) != 0) return false;

  out.recordCount = readU32LE(buf + 4);
  out.recordSize = readU32LE(buf + 8);
  out.blobOffset = readU32LE(buf + 12);
  // A record must hold at least its own 4-byte key, and the blob can't
  // start before the record table it follows ends -- reject a header that
  // fails either, rather than let a corrupt file drive reads to wherever
  // its bogus offsets point.
  if (out.recordSize < 4) return false;
  if (out.blobOffset < kTableHeaderSize + static_cast<uint64_t>(out.recordCount) * out.recordSize) {
    return false;
  }
  return true;
}

bool findRecordByKey(BinaryTableReader& reader, const TableHeader& header, uint32_t key,
                     std::vector<uint8_t>& outRecord) {
  if (header.recordCount == 0) return false;

  uint32_t lo = 0;
  uint32_t hi = header.recordCount;
  while (lo < hi) {
    const uint32_t mid = lo + (hi - lo) / 2;
    const uint32_t recordOffset = kTableHeaderSize + mid * header.recordSize;

    uint8_t keyBuf[4];
    if (!reader.readAt(recordOffset, keyBuf, sizeof(keyBuf))) return false;
    const uint32_t candidateKey = readU32LE(keyBuf);

    if (candidateKey == key) {
      outRecord.resize(header.recordSize);
      return reader.readAt(recordOffset, outRecord.data(), header.recordSize);
    }
    if (candidateKey < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return false;
}

bool readBlobString(BinaryTableReader& reader, const TableHeader& header, uint32_t offset,
                    std::string& out, size_t maxLen) {
  std::vector<uint8_t> buf(maxLen);

  // Common case: the string is nowhere near EOF, so the full maxLen window
  // just works -- one read, no searching. Only a blob's very last string
  // (whose full maxLen window would run past EOF, which a strict reader
  // reports as a failed read rather than a short one -- see
  // BinaryTableReader::readAt's "never partially fill and report success"
  // contract) needs the fallback below.
  if (reader.readAt(header.blobOffset + offset, buf.data(), maxLen)) {
    const void* nul = std::memchr(buf.data(), '\0', maxLen);
    if (nul == nullptr) return false;  // string exceeds maxLen, or the data is corrupt
    const size_t len = static_cast<const uint8_t*>(nul) - buf.data();
    out.assign(reinterpret_cast<const char*>(buf.data()), len);
    return true;
  }

  // Fallback: binary-search the largest window (up to maxLen) that
  // readAt() still accepts, then read exactly that much once. A blob
  // string's terminator always falls within that maximal window (every
  // string legitimately ends before or exactly at EOF), so this can't
  // land on a window too small to contain it the way a blind halving
  // retry could.
  size_t lo = 0;           // largest window size confirmed to succeed so far
  size_t hi = maxLen + 1;  // smallest window size confirmed to fail (exclusive)
  while (lo + 1 < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    if (reader.readAt(header.blobOffset + offset, buf.data(), mid)) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  if (lo == 0) return false;

  if (!reader.readAt(header.blobOffset + offset, buf.data(), lo)) return false;
  const void* nul = std::memchr(buf.data(), '\0', lo);
  if (nul == nullptr) return false;
  const size_t len = static_cast<const uint8_t*>(nul) - buf.data();
  out.assign(reinterpret_cast<const char*>(buf.data()), len);
  return true;
}

bool lookupSdRoute(BinaryTableReader& reader, uint32_t routeId, SdRouteInfo& out) {
  TableHeader header;
  if (!readTableHeader(reader, header)) return false;
  if (header.recordSize != 16) return false;

  std::vector<uint8_t> record;
  if (!findRecordByKey(reader, header, routeId, record)) return false;

  out.routeId = readU32LE(record.data());
  const uint32_t nameOffset = readU32LE(record.data() + 4);
  out.color = readU32LE(record.data() + 8);
  out.textColor = readU32LE(record.data() + 12);
  return readBlobString(reader, header, nameOffset, out.shortName);
}

void decodeStopRecordFixedFields(const uint8_t* record, SdStopInfo& out, uint32_t& outNameOffset) {
  out.stopCode = readU32LE(record);
  out.stopId.assign(reinterpret_cast<const char*>(record + 4), 8);
  outNameOffset = readU32LE(record + 12);
  out.lat = readF32LE(record + 16);
  out.lon = readF32LE(record + 20);
}

bool lookupSdStop(BinaryTableReader& reader, uint32_t stopCode, SdStopInfo& out) {
  TableHeader header;
  if (!readTableHeader(reader, header)) return false;
  if (header.recordSize != 24) return false;

  std::vector<uint8_t> record;
  if (!findRecordByKey(reader, header, stopCode, record)) return false;

  uint32_t nameOffset = 0;
  decodeStopRecordFixedFields(record.data(), out, nameOffset);
  return readBlobString(reader, header, nameOffset, out.stopName);
}

bool lookupSdTrip(BinaryTableReader& reader, uint32_t tripId, SdTripInfo& out) {
  TableHeader header;
  if (!readTableHeader(reader, header)) return false;
  if (header.recordSize != 16) return false;

  std::vector<uint8_t> record;
  if (!findRecordByKey(reader, header, tripId, record)) return false;

  out.tripId = readU32LE(record.data());
  out.routeId = readU32LE(record.data() + 4);
  out.directionId = record[8];
  const uint32_t headsignOffset = readU32LE(record.data() + 12);
  return readBlobString(reader, header, headsignOffset, out.headsign);
}

double haversineMeters(double lat1, double lon1, double lat2, double lon2) {
  constexpr double kEarthRadiusMeters = 6371000.0;
  constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

  const double phi1 = lat1 * kDegToRad;
  const double phi2 = lat2 * kDegToRad;
  const double dPhi = (lat2 - lat1) * kDegToRad;
  const double dLambda = (lon2 - lon1) * kDegToRad;

  const double a = std::sin(dPhi / 2) * std::sin(dPhi / 2) +
                   std::cos(phi1) * std::cos(phi2) * std::sin(dLambda / 2) * std::sin(dLambda / 2);
  const double c = 2 * std::atan2(std::sqrt(a), std::sqrt(1 - a));
  return kEarthRadiusMeters * c;
}

}  // namespace sta
}  // namespace transit
