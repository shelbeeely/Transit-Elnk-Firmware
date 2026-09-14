// The hardware half of ota_update.h: streaming an image into the inactive
// OTA partition, verifying it, and switching which slot boots. Excluded
// from [env:native] (Update.h / esp_ota_ops.h / WiFiClientSecure are all
// ESP32-only), the same split power_scheduler.cpp and time_keeper.cpp use.
//
// The decision of *whether* to do any of this lives in ota_update.cpp and is
// host-tested; this file only carries it out.

#include "transit/ota_update.h"

#ifndef FREEINK_HOST_NATIVE

#include <HTTPClient.h>
#include <Update.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>

#include <cstdio>
#include <memory>

namespace transit {

namespace {

// Read granularity. Deliberately heap-allocated rather than a stack buffer:
// setup() runs on the Arduino loop task (8 KB of stack by default) and this
// is called several frames deep, so a few kilobytes on the stack here is a
// real overflow risk, while a few kilobytes of heap for the duration of one
// download is not.
constexpr size_t kChunkBytes = 2048;

// Same reasoning as http_transport.cpp's kStallTimeoutMs: a server that
// stays connected but stops sending would otherwise spin here forever, and
// a battery device blocked in setup() never reaches enterDeepSleep().
constexpr uint32_t kStallTimeoutMs = 20000;

std::string toLowerHex(const uint8_t* digest, size_t length) {
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(length * 2);
  for (size_t i = 0; i < length; ++i) {
    out.push_back(kHex[(digest[i] >> 4) & 0x0F]);
    out.push_back(kHex[digest[i] & 0x0F]);
  }
  return out;
}

OtaApplyResult failure(const std::string& message, size_t bytesWritten = 0) {
  OtaApplyResult result;
  result.ok = false;
  result.error = message;
  result.bytesWritten = bytesWritten;
  return result;
}

// An ESP32 application image starts with the 0xE9 magic byte. Not a
// signature check -- it just distinguishes a slot holding a real app from
// one that has never been written, which is the difference between a
// rollback that boots and one that bricks until a cable arrives.
bool partitionHoldsAnApp(const esp_partition_t* partition) {
  if (partition == nullptr) return false;
  uint8_t magic = 0;
  if (esp_partition_read(partition, 0, &magic, 1) != ESP_OK) return false;
  return magic == 0xE9;
}

}  // namespace

OtaApplyResult applyOtaFromUrl(const OtaManifest& manifest, OtaProgressFn progress) {
  if (manifest.sizeBytes <= 0 || manifest.sizeBytes > kMaxImageBytes) {
    return failure("manifest size out of range");
  }
  const size_t expectedBytes = static_cast<size_t>(manifest.sizeBytes);

  WiFiClientSecure client;
  // Matches http_transport.cpp's existing posture rather than quietly
  // introducing a second, different one. This is the weakest link in the
  // chain: without a pinned CA, an attacker who can terminate TLS can serve
  // both the manifest and a matching image, and the SHA-256 check verifies
  // only that the two agree with each other. Pinning is tracked in
  // docs/OTA_UPDATES.md; until then, point ota_url at a host you control.
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, manifest.url.c_str())) return failure("could not open " + manifest.url);
  // GitHub release assets (and most object stores) answer with a redirect to
  // a signed URL, so following redirects isn't optional here.
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

  const int statusCode = http.GET();
  if (statusCode != HTTP_CODE_OK) {
    http.end();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "HTTP %d fetching image", statusCode);
    return failure(buf);
  }

  // Content-Length is a hint, not the authority: the manifest's size is what
  // was signed for by whoever published it, and it is what the digest covers.
  // A server claiming a different length is a mismatch worth refusing before
  // an erase cycle rather than discovering after one.
  const int contentLength = http.getSize();
  if (contentLength > 0 && static_cast<size_t>(contentLength) != expectedBytes) {
    http.end();
    return failure("server length disagrees with the manifest");
  }

  auto* stream = http.getStreamPtr();
  if (stream == nullptr) {
    http.end();
    return failure("no response stream");
  }

  // U_FLASH, sized from the manifest. Update::begin() erases as it goes, so
  // from here on the inactive slot's previous contents are gone -- which is
  // fine (it is not the slot currently running) and is exactly why the
  // battery gate in decideOtaUpdate() runs before this point.
  if (!Update.begin(expectedBytes, U_FLASH)) {
    http.end();
    return failure(std::string("Update.begin failed: ") + Update.errorString());
  }

  mbedtls_sha256_context sha;
  mbedtls_sha256_init(&sha);
  mbedtls_sha256_starts(&sha, /*is224=*/0);

  std::unique_ptr<uint8_t[]> buffer(new uint8_t[kChunkBytes]);
  size_t written = 0;
  uint32_t lastProgressMs = millis();
  size_t lastReportedAt = 0;

  while (written < expectedBytes) {
    const size_t available = stream->available();
    if (available == 0) {
      if (!http.connected()) break;
      if (millis() - lastProgressMs > kStallTimeoutMs) break;
      delay(1);
      continue;
    }
    size_t toRead = available > kChunkBytes ? kChunkBytes : available;
    if (toRead > expectedBytes - written) toRead = expectedBytes - written;

    const int readBytes = stream->readBytes(reinterpret_cast<char*>(buffer.get()), toRead);
    if (readBytes <= 0) break;

    const size_t chunk = static_cast<size_t>(readBytes);
    if (Update.write(buffer.get(), chunk) != chunk) {
      const std::string error = Update.errorString();
      Update.abort();
      mbedtls_sha256_free(&sha);
      http.end();
      return failure("flash write failed: " + error, written);
    }
    mbedtls_sha256_update(&sha, buffer.get(), chunk);
    written += chunk;
    lastProgressMs = millis();

    // Every ~64 KB. Often enough that a forty-second download doesn't look
    // like a hang on the serial console, rare enough not to be the thing
    // slowing it down.
    if (progress != nullptr && written - lastReportedAt >= 64 * 1024) {
      progress(written, expectedBytes);
      lastReportedAt = written;
    }
  }
  http.end();

  if (written != expectedBytes) {
    Update.abort();
    mbedtls_sha256_free(&sha);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "short read: %u of %u bytes",
                  static_cast<unsigned>(written), static_cast<unsigned>(expectedBytes));
    return failure(buf, written);
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&sha, digest);
  mbedtls_sha256_free(&sha);
  const std::string actual = toLowerHex(digest, sizeof(digest));

  // Verified BEFORE Update.end(), which is what actually switches the boot
  // partition. Ending first and checking after would leave a failed
  // verification pointing the bootloader at the bad image -- the rollback
  // machinery would eventually recover it, but only after three failed
  // boots, for a problem that was detectable here.
  if (actual != manifest.sha256) {
    Update.abort();
    return failure("sha256 mismatch: got " + actual, written);
  }

  if (!Update.end()) {
    return failure(std::string("Update.end failed: ") + Update.errorString(), written);
  }
  if (!Update.isFinished()) {
    return failure("update did not finish cleanly", written);
  }

  OtaApplyResult result;
  result.ok = true;
  result.bytesWritten = written;
  return result;
}

bool rollBackToPreviousSlot(std::string& error) {
  // The "next update" partition is by definition the one that isn't running
  // -- which, on a board that just took an update, is the slot the previous
  // (working) image is still sitting in.
  const esp_partition_t* previous = esp_ota_get_next_update_partition(nullptr);
  if (previous == nullptr) {
    error = "no alternate OTA slot on this partition table";
    return false;
  }
  if (!partitionHoldsAnApp(previous)) {
    // A board whose other slot has never been written has nothing to go
    // back to. Saying so is strictly better than pointing the bootloader at
    // blank flash, which would turn a bad update into a dead board.
    error = std::string("slot ") + previous->label + " holds no application image";
    return false;
  }
  const esp_err_t err = esp_ota_set_boot_partition(previous);
  if (err != ESP_OK) {
    char buf[80];
    std::snprintf(buf, sizeof(buf), "esp_ota_set_boot_partition failed (%d)",
                  static_cast<int>(err));
    error = buf;
    return false;
  }
  return true;
}

std::string runningPartitionLabel() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running == nullptr) return "unknown";
  return running->label;
}

}  // namespace transit

#endif  // FREEINK_HOST_NATIVE
