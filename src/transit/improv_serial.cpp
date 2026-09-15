#include "transit/improv_serial.h"

#include <algorithm>

namespace transit {

namespace {
constexpr uint8_t kProtocolVersion = 0x01;
constexpr char kMagic[6] = {'I', 'M', 'P', 'R', 'O', 'V'};

constexpr uint8_t kTypeCurrentState = 0x01;
constexpr uint8_t kTypeErrorState = 0x02;
constexpr uint8_t kTypeRpcCommand = 0x03;
constexpr uint8_t kTypeRpcResult = 0x04;

constexpr uint8_t kCmdSendWifiSettings = 0x01;
constexpr uint8_t kCmdRequestCurrentState = 0x02;
constexpr uint8_t kCmdRequestDeviceInfo = 0x03;

uint8_t sumChecksum(const std::vector<uint8_t>& bytes) {
  uint32_t sum = 0;
  for (uint8_t b : bytes) sum += b;
  return static_cast<uint8_t>(sum & 0xFF);
}

// Reads one [len][bytes] string starting at data[*pos], advancing *pos past
// it. Returns false (leaving *pos unchanged) if the declared length runs
// past the end of data — a malformed/truncated command, not a crash.
bool readLengthPrefixedString(const std::vector<uint8_t>& data, size_t* pos, std::string* out) {
  if (*pos >= data.size()) return false;
  const uint8_t len = data[*pos];
  const size_t start = *pos + 1;
  if (start + len > data.size()) return false;
  out->assign(reinterpret_cast<const char*>(data.data() + start), len);
  *pos = start + len;
  return true;
}
}  // namespace

ImprovSerial::ImprovSerial(ImprovDeviceInfo deviceInfo) : deviceInfo_(std::move(deviceInfo)) {}

void ImprovSerial::resetParser() {
  parseState_ = ParseState::kMagic;
  magicIndex_ = 0;
  data_.clear();
  frameBytesForChecksum_.clear();
}

void ImprovSerial::handleByte(uint8_t b) {
  switch (parseState_) {
    case ParseState::kMagic:
      if (b == static_cast<uint8_t>(kMagic[magicIndex_])) {
        frameBytesForChecksum_.push_back(b);
        ++magicIndex_;
        if (magicIndex_ == sizeof(kMagic)) parseState_ = ParseState::kVersion;
      } else {
        // Resync: the mismatching byte might itself be the start of a fresh
        // magic sequence (e.g. noise before a real frame), so re-check it
        // at index 0 rather than discarding it outright.
        frameBytesForChecksum_.clear();
        magicIndex_ = (b == static_cast<uint8_t>(kMagic[0])) ? 1 : 0;
        if (magicIndex_ == 1) frameBytesForChecksum_.push_back(b);
      }
      break;

    case ParseState::kVersion:
      version_ = b;
      frameBytesForChecksum_.push_back(b);
      parseState_ = ParseState::kType;
      break;

    case ParseState::kType:
      type_ = b;
      frameBytesForChecksum_.push_back(b);
      parseState_ = ParseState::kLength;
      break;

    case ParseState::kLength:
      length_ = b;
      frameBytesForChecksum_.push_back(b);
      data_.clear();
      parseState_ = (length_ == 0) ? ParseState::kChecksum : ParseState::kData;
      break;

    case ParseState::kData:
      data_.push_back(b);
      frameBytesForChecksum_.push_back(b);
      if (data_.size() >= length_) parseState_ = ParseState::kChecksum;
      break;

    case ParseState::kChecksum: {
      const uint8_t expected = sumChecksum(frameBytesForChecksum_);
      if (b == expected) {
        onFrameComplete();
      } else {
        sendError(ImprovError::kInvalidPacket);
      }
      resetParser();
      break;
    }
  }
}

void ImprovSerial::onFrameComplete() {
  // Only RPC Command frames (type 0x03) are ever sent to a device — the
  // other three types are this class's own outgoing vocabulary. A client
  // that (incorrectly) echoes one of those back is silently ignored rather
  // than treated as an error, since it isn't malformed, just not ours to
  // act on.
  if (type_ == kTypeRpcCommand) onRpcCommand(data_);
}

void ImprovSerial::onRpcCommand(const std::vector<uint8_t>& data) {
  if (data.empty()) {
    sendError(ImprovError::kInvalidPacket);
    return;
  }
  const uint8_t command = data[0];
  // data[1] is the command's own data-length byte; trusted implicitly here
  // since readLengthPrefixedString()/data.size() bound every read against
  // the real buffer regardless of what that byte claims.
  const std::vector<uint8_t> commandData(data.begin() + (data.size() > 1 ? 2 : 1), data.end());

  switch (command) {
    case kCmdSendWifiSettings: {
      size_t pos = 0;
      std::string ssid;
      std::string password;
      if (!readLengthPrefixedString(commandData, &pos, &ssid) ||
          !readLengthPrefixedString(commandData, &pos, &password) || ssid.empty()) {
        sendError(ImprovError::kInvalidPacket);
        return;
      }
      pendingCredentials_.ssid = ssid;
      pendingCredentials_.password = password;
      hasPendingCredentials_ = true;
      // Spec semantics: "Provisioning" means credentials were received and
      // a connection attempt is underway — true the moment this is parsed,
      // not once the caller gets around to polling for it.
      sendCurrentState(ImprovState::kProvisioning);
      break;
    }
    case kCmdRequestCurrentState:
      sendCurrentState(currentState_);
      break;
    case kCmdRequestDeviceInfo:
      sendRpcResult(kCmdRequestDeviceInfo, {deviceInfo_.firmwareName, deviceInfo_.firmwareVersion,
                                             deviceInfo_.hardwareVariant, deviceInfo_.deviceName});
      break;
    default:
      // Covers Request Scanned Wi-Fi Networks and the hostname/device-name/
      // network-state commands from later spec revisions — see this file's
      // header comment on why those are out of scope here.
      sendError(ImprovError::kUnknownCommand);
      break;
  }
}

ImprovWifiCredentials ImprovSerial::takePendingWifiCredentials() {
  hasPendingCredentials_ = false;
  ImprovWifiCredentials result = pendingCredentials_;
  pendingCredentials_ = ImprovWifiCredentials{};
  return result;
}

void ImprovSerial::reportProvisioned(const std::string& redirectUrl) {
  sendCurrentState(ImprovState::kProvisioned);
  sendRpcResult(kCmdSendWifiSettings, redirectUrl.empty() ? std::vector<std::string>{}
                                                           : std::vector<std::string>{redirectUrl});
}

void ImprovSerial::reportConnectFailed() {
  sendError(ImprovError::kUnableToConnect);
  // Back to ready rather than staying on kProvisioning/kStopped, so the
  // same browser tab can offer to retry without a power cycle.
  sendCurrentState(ImprovState::kAuthorized);
}

void ImprovSerial::setState(ImprovState state) { sendCurrentState(state); }

void ImprovSerial::sendCurrentState(ImprovState state) {
  currentState_ = state;
  appendFrame(kTypeCurrentState, {static_cast<uint8_t>(state)});
}

void ImprovSerial::sendError(ImprovError error) {
  appendFrame(kTypeErrorState, {static_cast<uint8_t>(error)});
}

void ImprovSerial::sendRpcResult(uint8_t command, const std::vector<std::string>& strings) {
  std::vector<uint8_t> payload;
  payload.push_back(command);
  payload.push_back(0);  // placeholder for the result's own data-length byte, filled in below
  size_t stringBytes = 0;
  for (const std::string& s : strings) {
    const uint8_t len = static_cast<uint8_t>(std::min<size_t>(s.size(), 255));
    payload.push_back(len);
    payload.insert(payload.end(), s.begin(), s.begin() + len);
    stringBytes += 1 + len;
    if (stringBytes > 250) break;  // stay well clear of the outer length byte's 255 ceiling
  }
  payload[1] = static_cast<uint8_t>(std::min<size_t>(stringBytes, 255));
  appendFrame(kTypeRpcResult, payload);
}

void ImprovSerial::appendFrame(uint8_t type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> frame;
  frame.reserve(6 + 3 + data.size() + 1);
  for (char c : kMagic) frame.push_back(static_cast<uint8_t>(c));
  frame.push_back(kProtocolVersion);
  frame.push_back(type);
  frame.push_back(static_cast<uint8_t>(data.size()));
  frame.insert(frame.end(), data.begin(), data.end());
  frame.push_back(sumChecksum(frame));
  outgoing_.insert(outgoing_.end(), frame.begin(), frame.end());
}

std::vector<uint8_t> ImprovSerial::takeOutgoingBytes() {
  std::vector<uint8_t> result;
  result.swap(outgoing_);
  return result;
}

}  // namespace transit
