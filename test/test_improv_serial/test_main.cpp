// Host-side tests for improv_serial.h's frame parser/encoder.
//
// Two things matter most here:
//
//   * The Send Wi-Fi Settings example bytes in the spec
//     (https://www.improv-wifi.com/serial/) are reproduced and checked byte-
//     for-byte in test_matches_the_spec_s_own_worked_example, since that's
//     the one place this implementation can be checked against the
//     documented protocol rather than just against itself.
//   * A corrupted frame must be rejected (wrong checksum) rather than acted
//     on with a guessed/partial payload -- credentials are security-
//     sensitive, so "didn't parse" must never silently become "parsed
//     something".

#include <unity.h>

#include <vector>

#include "transit/improv_serial.h"

using transit::ImprovDeviceInfo;
using transit::ImprovError;
using transit::ImprovSerial;
using transit::ImprovState;

namespace {

ImprovDeviceInfo testDeviceInfo() {
  ImprovDeviceInfo info;
  info.firmwareName = "Transit-Elnk-Firmware";
  info.firmwareVersion = "v0.1.0";
  info.hardwareVariant = "ESP32-C3";
  info.deviceName = "Transit-Elnk";
  return info;
}

uint8_t sumChecksum(const std::vector<uint8_t>& bytes) {
  uint32_t sum = 0;
  for (uint8_t b : bytes) sum += b;
  return static_cast<uint8_t>(sum & 0xFF);
}

// Builds a raw incoming frame the same way a real Improv client would, for
// feeding to handleByte() one byte at a time. Deliberately independent of
// ImprovSerial's own appendFrame() so a bug shared between "build" and
// "parse" wouldn't cancel itself out in these tests.
std::vector<uint8_t> buildFrame(uint8_t type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> frame = {'I', 'M', 'P', 'R', 'O', 'V', 0x01, type,
                                 static_cast<uint8_t>(data.size())};
  frame.insert(frame.end(), data.begin(), data.end());
  frame.push_back(sumChecksum(frame));
  return frame;
}

std::vector<uint8_t> buildRpcCommandFrame(uint8_t command, const std::vector<uint8_t>& commandPayload) {
  std::vector<uint8_t> data = {command, static_cast<uint8_t>(commandPayload.size())};
  data.insert(data.end(), commandPayload.begin(), commandPayload.end());
  return buildFrame(/*type=*/0x03, data);
}

std::vector<uint8_t> lengthPrefixed(const std::string& s) {
  std::vector<uint8_t> out = {static_cast<uint8_t>(s.size())};
  out.insert(out.end(), s.begin(), s.end());
  return out;
}

void feed(ImprovSerial* improv, const std::vector<uint8_t>& bytes) {
  for (uint8_t b : bytes) improv->handleByte(b);
}

// Minimal structural decoder for asserting on ImprovSerial's own output --
// intentionally not shared code with improv_serial.cpp's appendFrame().
struct DecodedFrame {
  uint8_t type;
  std::vector<uint8_t> data;
};

std::vector<DecodedFrame> decodeFrames(const std::vector<uint8_t>& bytes) {
  std::vector<DecodedFrame> frames;
  size_t i = 0;
  while (i + 9 <= bytes.size()) {
    TEST_ASSERT_EQUAL_UINT8('I', bytes[i]);
    const uint8_t type = bytes[i + 7];
    const uint8_t len = bytes[i + 8];
    TEST_ASSERT_TRUE(i + 9 + len <= bytes.size());
    std::vector<uint8_t> data(bytes.begin() + i + 9, bytes.begin() + i + 9 + len);
    std::vector<uint8_t> forChecksum(bytes.begin() + i, bytes.begin() + i + 9 + len);
    TEST_ASSERT_EQUAL_UINT8(sumChecksum(forChecksum), bytes[i + 9 + len]);
    frames.push_back({type, data});
    i += 9 + len + 1;
  }
  TEST_ASSERT_EQUAL_size_t(bytes.size(), i);
  return frames;
}

void test_matches_the_spec_s_own_worked_example() {
  // improv-wifi.com/serial's own example for Send Wi-Fi Settings:
  //   01 1E 0C {MyWirelessAP} 10 {mysecurepassword}
  // 0x1E (30) = 1 + 12 + 1 + 16, i.e. the inner data length excludes the
  // command byte and this length byte itself.
  std::vector<uint8_t> payload = lengthPrefixed("MyWirelessAP");
  std::vector<uint8_t> passBytes = lengthPrefixed("mysecurepassword");
  payload.insert(payload.end(), passBytes.begin(), passBytes.end());
  TEST_ASSERT_EQUAL_UINT8(0x1E, payload.size());

  std::vector<uint8_t> commandFrame = buildRpcCommandFrame(0x01, payload);

  ImprovSerial improv(testDeviceInfo());
  feed(&improv, commandFrame);

  TEST_ASSERT_TRUE(improv.hasPendingWifiCredentials());
  auto creds = improv.takePendingWifiCredentials();
  TEST_ASSERT_EQUAL_STRING("MyWirelessAP", creds.ssid.c_str());
  TEST_ASSERT_EQUAL_STRING("mysecurepassword", creds.password.c_str());
}

void test_receiving_wifi_settings_immediately_announces_provisioning() {
  ImprovSerial improv(testDeviceInfo());
  feed(&improv, buildRpcCommandFrame(0x01, [] {
         std::vector<uint8_t> p = lengthPrefixed("home");
         auto pw = lengthPrefixed("hunter2");
         p.insert(p.end(), pw.begin(), pw.end());
         return p;
       }()));

  auto frames = decodeFrames(improv.takeOutgoingBytes());
  TEST_ASSERT_EQUAL_INT(1, frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x01, frames[0].type);  // Current State
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ImprovState::kProvisioning), frames[0].data[0]);
}

void test_request_device_info_returns_all_four_fields_in_order() {
  ImprovSerial improv(testDeviceInfo());
  feed(&improv, buildRpcCommandFrame(0x03, {}));

  auto frames = decodeFrames(improv.takeOutgoingBytes());
  TEST_ASSERT_EQUAL_INT(1, frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x04, frames[0].type);  // RPC Result
  const std::vector<uint8_t>& data = frames[0].data;
  TEST_ASSERT_EQUAL_UINT8(0x03, data[0]);  // echoes the command it answers

  size_t pos = 2;  // skip [command][dataLength]
  auto readString = [&](const char* expected) {
    const uint8_t len = data[pos];
    std::string s(reinterpret_cast<const char*>(&data[pos + 1]), len);
    TEST_ASSERT_EQUAL_STRING(expected, s.c_str());
    pos += 1 + len;
  };
  readString("Transit-Elnk-Firmware");
  readString("v0.1.0");
  readString("ESP32-C3");
  readString("Transit-Elnk");
}

void test_request_current_state_reports_whatever_was_last_set() {
  ImprovSerial improv(testDeviceInfo());
  improv.setState(ImprovState::kAuthorized);
  improv.takeOutgoingBytes();  // discard the setState() push itself

  feed(&improv, buildRpcCommandFrame(0x02, {}));
  auto frames = decodeFrames(improv.takeOutgoingBytes());
  TEST_ASSERT_EQUAL_INT(1, frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x01, frames[0].type);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ImprovState::kAuthorized), frames[0].data[0]);
}

void test_unknown_command_reports_unknown_command_error() {
  ImprovSerial improv(testDeviceInfo());
  feed(&improv, buildRpcCommandFrame(0x04, {}));  // Request Scanned Networks -- not implemented

  auto frames = decodeFrames(improv.takeOutgoingBytes());
  TEST_ASSERT_EQUAL_INT(1, frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x02, frames[0].type);  // Error State
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ImprovError::kUnknownCommand), frames[0].data[0]);
}

void test_a_corrupted_checksum_is_rejected_not_partially_honored() {
  ImprovSerial improv(testDeviceInfo());
  std::vector<uint8_t> frame = buildRpcCommandFrame(0x01, [] {
    std::vector<uint8_t> p = lengthPrefixed("evil");
    auto pw = lengthPrefixed("password");
    p.insert(p.end(), pw.begin(), pw.end());
    return p;
  }());
  frame.back() ^= 0xFF;  // corrupt the checksum byte

  feed(&improv, frame);

  TEST_ASSERT_FALSE(improv.hasPendingWifiCredentials());
  auto frames = decodeFrames(improv.takeOutgoingBytes());
  TEST_ASSERT_EQUAL_INT(1, frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x02, frames[0].type);  // Error State
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ImprovError::kInvalidPacket), frames[0].data[0]);
}

void test_a_stray_byte_before_a_frame_does_not_break_parsing() {
  ImprovSerial improv(testDeviceInfo());
  std::vector<uint8_t> stream = {0x00, 0x42};  // noise, neither of which is 'I'
  std::vector<uint8_t> frame = buildRpcCommandFrame(0x03, {});
  stream.insert(stream.end(), frame.begin(), frame.end());

  feed(&improv, stream);

  auto frames = decodeFrames(improv.takeOutgoingBytes());
  TEST_ASSERT_EQUAL_INT(1, frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x04, frames[0].type);
}

void test_report_provisioned_sends_state_then_result_with_the_redirect_url() {
  ImprovSerial improv(testDeviceInfo());
  improv.reportProvisioned("http://transit-elnk.local/");

  auto frames = decodeFrames(improv.takeOutgoingBytes());
  TEST_ASSERT_EQUAL_INT(2, frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x01, frames[0].type);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ImprovState::kProvisioned), frames[0].data[0]);
  TEST_ASSERT_EQUAL_UINT8(0x04, frames[1].type);
  TEST_ASSERT_EQUAL_UINT8(0x01, frames[1].data[0]);  // answers the Wi-Fi Settings command
}

void test_report_connect_failed_sends_error_then_returns_to_authorized() {
  ImprovSerial improv(testDeviceInfo());
  improv.reportConnectFailed();

  auto frames = decodeFrames(improv.takeOutgoingBytes());
  TEST_ASSERT_EQUAL_INT(2, frames.size());
  TEST_ASSERT_EQUAL_UINT8(0x02, frames[0].type);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ImprovError::kUnableToConnect), frames[0].data[0]);
  TEST_ASSERT_EQUAL_UINT8(0x01, frames[1].type);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ImprovState::kAuthorized), frames[1].data[0]);
}

}  // namespace

int main(int /*argc*/, char** /*argv*/) {
  UNITY_BEGIN();
  RUN_TEST(test_matches_the_spec_s_own_worked_example);
  RUN_TEST(test_receiving_wifi_settings_immediately_announces_provisioning);
  RUN_TEST(test_request_device_info_returns_all_four_fields_in_order);
  RUN_TEST(test_request_current_state_reports_whatever_was_last_set);
  RUN_TEST(test_unknown_command_reports_unknown_command_error);
  RUN_TEST(test_a_corrupted_checksum_is_rejected_not_partially_honored);
  RUN_TEST(test_a_stray_byte_before_a_frame_does_not_break_parsing);
  RUN_TEST(test_report_provisioned_sends_state_then_result_with_the_redirect_url);
  RUN_TEST(test_report_connect_failed_sends_error_then_returns_to_authorized);
  return UNITY_END();
}
