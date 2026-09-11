// Real implementation of parseTripUpdates — see
// include/transit/sta_feed_parser.h for the field subset and the two
// upstream-schema quirks (TranslatedString-shaped string fields,
// unreliable direction_id) this decodes around.

#include "transit/sta_feed_parser.h"

#include "transit/sta_route_table.h"

namespace transit {
namespace sta {

namespace {

// Protobuf wire types (the low 3 bits of every field tag).
constexpr uint8_t kWireVarint = 0;
constexpr uint8_t kWireFixed64 = 1;
constexpr uint8_t kWireLengthDelimited = 2;
constexpr uint8_t kWireFixed32 = 5;

// Bounds-checked cursor over one length-delimited protobuf message's bytes.
// Every read advances past exactly what it consumed and fails (returns
// false) rather than ever stepping past `end_` — this parses untrusted
// network input, so out-of-bounds reads are not an acceptable failure mode
// here even on malformed/truncated data.
class Reader {
 public:
  Reader(const uint8_t* data, size_t len) : pos_(data), end_(data + len) {}

  bool atEnd() const { return pos_ >= end_; }
  size_t remaining() const { return static_cast<size_t>(end_ - pos_); }

  bool readVarint(uint64_t& out) {
    out = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (pos_ >= end_) return false;
      const uint8_t byte = *pos_++;
      out |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) return true;
    }
    return false;  // more than 10 continuation bytes: malformed
  }

  bool readTag(uint32_t& fieldNumber, uint8_t& wireType) {
    uint64_t tag = 0;
    if (!readVarint(tag)) return false;
    fieldNumber = static_cast<uint32_t>(tag >> 3);
    wireType = static_cast<uint8_t>(tag & 0x7);
    return true;
  }

  // Length-delimited field payload: reads the length varint, then slices
  // that many bytes as a nested Reader without copying.
  bool readLengthDelimited(Reader& sub) {
    uint64_t length = 0;
    if (!readVarint(length)) return false;
    if (length > remaining()) return false;
    sub = Reader(pos_, static_cast<size_t>(length));
    pos_ += length;
    return true;
  }

  bool readString(std::string& out) {
    Reader sub(nullptr, 0);
    if (!readLengthDelimited(sub)) return false;
    out.assign(reinterpret_cast<const char*>(sub.pos_), sub.remaining());
    return true;
  }

  bool skipField(uint8_t wireType) {
    switch (wireType) {
      case kWireVarint: {
        uint64_t discard = 0;
        return readVarint(discard);
      }
      case kWireFixed64:
        if (remaining() < 8) return false;
        pos_ += 8;
        return true;
      case kWireLengthDelimited: {
        Reader discard(nullptr, 0);
        return readLengthDelimited(discard);
      }
      case kWireFixed32:
        if (remaining() < 4) return false;
        pos_ += 4;
        return true;
      default:
        return false;  // groups (wire types 3/4) are deprecated, unused here
    }
  }

  const uint8_t* pos_;
  const uint8_t* end_;
};

// StopTimeUpdate.schedule_relationship (google/transit/realtime/gtfs-realtime.proto).
constexpr uint64_t kScheduleRelationshipSkipped = 1;

struct StopTimeUpdateData {
  std::string stopId;
  int64_t departureTime = 0;
  int64_t arrivalTime = 0;
  bool hasDeparture = false;
  bool hasArrival = false;
  bool skipped = false;
};

struct TripUpdateData {
  std::string tripId;
  std::string routeId;
  std::string destination;
  std::vector<StopTimeUpdateData> stopTimeUpdates;
};

// TranslatedString{repeated Translation{text=1, language=2} translation=1} —
// see this file's header comment on why stop_headsign/trip_short_name need
// this instead of a plain readString(). Returns the first Translation
// entry's text regardless of language (every real STA capture examined so
// far carries exactly one, tagged "en"); "" if the submessage is empty or
// carries no translations, same as an absent field would give.
bool decodeTranslatedString(Reader reader, std::string& out) {
  while (!reader.atEnd()) {
    uint32_t fieldNumber = 0;
    uint8_t wireType = 0;
    if (!reader.readTag(fieldNumber, wireType)) return false;
    if (fieldNumber == 1 && wireType == kWireLengthDelimited) {
      Reader translation(nullptr, 0);
      if (!reader.readLengthDelimited(translation)) return false;
      if (!out.empty()) continue;  // already have the first entry
      while (!translation.atEnd()) {
        uint32_t tf = 0;
        uint8_t tw = 0;
        if (!translation.readTag(tf, tw)) return false;
        if (tf == 1 && tw == kWireLengthDelimited) {
          if (!translation.readString(out)) return false;
        } else {
          if (!translation.skipField(tw)) return false;
        }
      }
    } else {
      if (!reader.skipField(wireType)) return false;
    }
  }
  return true;
}

bool parseTripDescriptor(Reader reader, TripUpdateData& trip) {
  while (!reader.atEnd()) {
    uint32_t fieldNumber = 0;
    uint8_t wireType = 0;
    if (!reader.readTag(fieldNumber, wireType)) return false;
    if (fieldNumber == 1 && wireType == kWireLengthDelimited) {
      if (!reader.readString(trip.tripId)) return false;
    } else if (fieldNumber == 5 && wireType == kWireLengthDelimited) {
      if (!reader.readString(trip.routeId)) return false;
    } else {
      if (!reader.skipField(wireType)) return false;
    }
  }
  return true;
}

bool parseTripProperties(Reader reader, TripUpdateData& trip) {
  while (!reader.atEnd()) {
    uint32_t fieldNumber = 0;
    uint8_t wireType = 0;
    if (!reader.readTag(fieldNumber, wireType)) return false;
    if (fieldNumber == 6 && wireType == kWireLengthDelimited) {
      Reader sub(nullptr, 0);
      if (!reader.readLengthDelimited(sub)) return false;
      if (!decodeTranslatedString(sub, trip.destination)) return false;
    } else {
      if (!reader.skipField(wireType)) return false;
    }
  }
  return true;
}

bool parseStopTimeEvent(Reader reader, int64_t& time) {
  while (!reader.atEnd()) {
    uint32_t fieldNumber = 0;
    uint8_t wireType = 0;
    if (!reader.readTag(fieldNumber, wireType)) return false;
    if (fieldNumber == 2 && wireType == kWireVarint) {
      uint64_t raw = 0;
      if (!reader.readVarint(raw)) return false;
      time = static_cast<int64_t>(raw);
    } else {
      if (!reader.skipField(wireType)) return false;
    }
  }
  return true;
}

bool parseStopTimeUpdate(Reader reader, StopTimeUpdateData& stu) {
  while (!reader.atEnd()) {
    uint32_t fieldNumber = 0;
    uint8_t wireType = 0;
    if (!reader.readTag(fieldNumber, wireType)) return false;
    if (fieldNumber == 4 && wireType == kWireLengthDelimited) {
      if (!reader.readString(stu.stopId)) return false;
    } else if (fieldNumber == 2 && wireType == kWireLengthDelimited) {
      Reader sub(nullptr, 0);
      if (!reader.readLengthDelimited(sub)) return false;
      if (!parseStopTimeEvent(sub, stu.arrivalTime)) return false;
      stu.hasArrival = true;
    } else if (fieldNumber == 3 && wireType == kWireLengthDelimited) {
      Reader sub(nullptr, 0);
      if (!reader.readLengthDelimited(sub)) return false;
      if (!parseStopTimeEvent(sub, stu.departureTime)) return false;
      stu.hasDeparture = true;
    } else if (fieldNumber == 5 && wireType == kWireVarint) {
      uint64_t relationship = 0;
      if (!reader.readVarint(relationship)) return false;
      stu.skipped = (relationship == kScheduleRelationshipSkipped);
    } else {
      if (!reader.skipField(wireType)) return false;
    }
  }
  return true;
}

bool parseTripUpdate(Reader reader, TripUpdateData& trip) {
  while (!reader.atEnd()) {
    uint32_t fieldNumber = 0;
    uint8_t wireType = 0;
    if (!reader.readTag(fieldNumber, wireType)) return false;
    if (fieldNumber == 1 && wireType == kWireLengthDelimited) {
      Reader sub(nullptr, 0);
      if (!reader.readLengthDelimited(sub)) return false;
      if (!parseTripDescriptor(sub, trip)) return false;
    } else if (fieldNumber == 2 && wireType == kWireLengthDelimited) {
      Reader sub(nullptr, 0);
      if (!reader.readLengthDelimited(sub)) return false;
      StopTimeUpdateData stu;
      if (!parseStopTimeUpdate(sub, stu)) return false;
      trip.stopTimeUpdates.push_back(std::move(stu));
    } else if (fieldNumber == 6 && wireType == kWireLengthDelimited) {
      Reader sub(nullptr, 0);
      if (!reader.readLengthDelimited(sub)) return false;
      if (!parseTripProperties(sub, trip)) return false;
    } else {
      if (!reader.skipField(wireType)) return false;
    }
  }
  return true;
}

void emitMatchingDepartures(const TripUpdateData& trip, const std::string& targetStopId,
                            std::vector<StaDeparture>& out) {
  for (const auto& stu : trip.stopTimeUpdates) {
    if (stu.stopId != targetStopId) continue;
    if (stu.skipped) continue;
    if (!stu.hasDeparture && !stu.hasArrival) continue;

    StaDeparture dep;
    dep.routeId = trip.routeId;
    if (const RouteInfo* info = lookupStaRoute(trip.routeId.c_str())) {
      dep.routeShortName = info->shortName;
      dep.routeColor = info->color;
      dep.routeTextColor = info->textColor;
    } else {
      dep.routeShortName = trip.routeId;  // table miss: fall back to the raw id
    }
    dep.tripId = trip.tripId;
    dep.destination = trip.destination;
    dep.departureEpoch = stu.hasDeparture ? stu.departureTime : stu.arrivalTime;
    out.push_back(std::move(dep));
  }
}

bool parseFeedEntity(Reader reader, const std::string& targetStopId,
                     std::vector<StaDeparture>& out) {
  while (!reader.atEnd()) {
    uint32_t fieldNumber = 0;
    uint8_t wireType = 0;
    if (!reader.readTag(fieldNumber, wireType)) return false;
    if (fieldNumber == 3 && wireType == kWireLengthDelimited) {
      Reader sub(nullptr, 0);
      if (!reader.readLengthDelimited(sub)) return false;
      TripUpdateData trip;
      if (!parseTripUpdate(sub, trip)) return false;
      emitMatchingDepartures(trip, targetStopId, out);
    } else {
      if (!reader.skipField(wireType)) return false;
    }
  }
  return true;
}

}  // namespace

bool parseTripUpdates(const uint8_t* data, size_t len, const std::string& targetStopId,
                      std::vector<StaDeparture>& out) {
  Reader reader(data, len);
  while (!reader.atEnd()) {
    uint32_t fieldNumber = 0;
    uint8_t wireType = 0;
    if (!reader.readTag(fieldNumber, wireType)) return false;
    if (fieldNumber == 2 && wireType == kWireLengthDelimited) {
      Reader sub(nullptr, 0);
      if (!reader.readLengthDelimited(sub)) return false;
      if (!parseFeedEntity(sub, targetStopId, out)) return false;
    } else {
      if (!reader.skipField(wireType)) return false;
    }
  }
  return true;
}

}  // namespace sta
}  // namespace transit
