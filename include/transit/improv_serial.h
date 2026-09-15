#pragma once

// Transit-Elnk-Firmware — Improv Wi-Fi (serial transport).
//
// Lets the browser tab that just flashed the board over Web Serial
// (site/flash.html's <esp-web-install-button>, ESP Web Tools) hand it Wi-Fi
// credentials over the same still-open USB connection, instead of the user
// having to separately join the board's own "TransitBoard-Setup" AP on a
// phone. ESP Web Tools speaks Improv natively and shows its own "Connect to
// Wi-Fi" step automatically once it sees a device answer this protocol — see
// docs/OTA_UPDATES.md's neighbor doc (browser flashing) for the rest of that
// flow. The API key and stop are NOT part of Improv (it only standardizes
// Wi-Fi) — the captive portal still runs afterward for those, same as today.
//
// Protocol summary (see https://www.improv-wifi.com/serial/ for the source
// of truth — verify against it, and against a real ESP Web Tools session,
// before trusting this over serial in the field; some byte values below
// were reconstructed from a fetched copy of that page rather than read
// byte-for-byte from a reference implementation):
//
//   Frame: 'I' 'M' 'P' 'R' 'O' 'V'  version(1)  type(1)  length(1)  data
//          checksum(1) = (sum of every byte above, magic through data) & 0xFF
//
// This class is deliberately Arduino-free: it consumes bytes one at a time
// (handleByte()) and produces bytes to write back (takeOutgoingBytes()), so
// it builds and is fully tested under [env:native]. The ESP32-only glue
// (reading Serial, calling WiFi.begin() with the credentials it surfaces,
// and reporting the outcome back) lives in setup_flow.cpp, which already
// owns that exact state machine for the captive portal's own Wi-Fi form —
// this feeds the same pendingSsid_/pendingPassword_/WiFi.begin() path
// rather than duplicating it.
//
// Scope: implements only what ESP Web Tools' own UI actually exercises —
// Request Current State, Request Device Information, and Send Wi-Fi
// Settings. Request Scanned Wi-Fi Networks and the newer hostname/device-
// name/network-state commands are answered with "unknown command" rather
// than guessed at; the board's Wi-Fi scan already has a working UI via the
// captive portal's own /scan endpoint, so nothing needs this path to scan.

#include <cstdint>
#include <string>
#include <vector>

namespace transit {

enum class ImprovState : uint8_t {
  kStopped = 0x00,
  kAuthorized = 0x02,  // ready to accept an RPC command
  kProvisioning = 0x03,  // credentials received, attempting to connect
  kProvisioned = 0x04,  // connected
};

enum class ImprovError : uint8_t {
  kNone = 0x00,
  kInvalidPacket = 0x01,
  kUnknownCommand = 0x02,
  kUnableToConnect = 0x03,
  kUnknown = 0xFF,
};

struct ImprovDeviceInfo {
  std::string firmwareName;
  std::string firmwareVersion;
  std::string hardwareVariant;
  std::string deviceName;
};

struct ImprovWifiCredentials {
  std::string ssid;
  std::string password;
};

class ImprovSerial {
 public:
  explicit ImprovSerial(ImprovDeviceInfo deviceInfo);

  // Feed one byte read from the serial port. Advances the frame parser;
  // may append to the pending outgoing-bytes queue (a Request Current
  // State/Device Information command answers itself immediately) and/or
  // set hasPendingWifiCredentials() (a Wi-Fi Settings command — the caller
  // still has to actually attempt the connection and report the outcome
  // via reportProvisioned()/reportConnectFailed()).
  void handleByte(uint8_t b);

  bool hasPendingWifiCredentials() const { return hasPendingCredentials_; }
  // Clears the pending flag. Calling this without pending credentials
  // returns an empty SSID — check hasPendingWifiCredentials() first.
  ImprovWifiCredentials takePendingWifiCredentials();

  // Caller reports what actually happened after acting on credentials this
  // surfaced (or after any other Wi-Fi connect attempt worth reflecting to
  // an attached Improv client, e.g. a resumed saved-credentials attempt).
  // redirectUrl may be empty — Improv permits a Wi-Fi Settings result with
  // no follow-up URL.
  void reportProvisioned(const std::string& redirectUrl);
  void reportConnectFailed();

  // Pushes an unprompted Current State frame — used once at setup-portal
  // start (kAuthorized) so a browser tab that's already watching doesn't
  // have to ask first, in addition to answering Request Current State.
  void setState(ImprovState state);

  // Bytes queued for the host since the last call; the caller (setup_flow's
  // ESP32-only glue) writes these to Serial and discards them. Clears the
  // internal queue.
  std::vector<uint8_t> takeOutgoingBytes();

 private:
  enum class ParseState {
    kMagic,
    kVersion,
    kType,
    kLength,
    kData,
    kChecksum,
  };

  void resetParser();
  void onFrameComplete();
  void onRpcCommand(const std::vector<uint8_t>& data);

  void sendCurrentState(ImprovState state);
  void sendError(ImprovError error);
  // strings encoded as this RPC command's result payload: [command][dataLen]
  // then each string as [len][bytes]. Truncates rather than overflowing if
  // the combined payload can't fit a byte-length field — every caller here
  // passes short, fixed strings, so this is a defensive bound, not a path
  // this firmware expects to hit.
  void sendRpcResult(uint8_t command, const std::vector<std::string>& strings);
  void appendFrame(uint8_t type, const std::vector<uint8_t>& data);

  ImprovDeviceInfo deviceInfo_;
  ImprovState currentState_ = ImprovState::kStopped;

  ParseState parseState_ = ParseState::kMagic;
  size_t magicIndex_ = 0;
  uint8_t version_ = 0;
  uint8_t type_ = 0;
  uint8_t length_ = 0;
  std::vector<uint8_t> data_;
  std::vector<uint8_t> frameBytesForChecksum_;  // magic..data, to verify against the checksum byte

  bool hasPendingCredentials_ = false;
  ImprovWifiCredentials pendingCredentials_;

  std::vector<uint8_t> outgoing_;
};

}  // namespace transit
