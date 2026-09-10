// Stub implementation — replaced by work unit 6 (icon cache).
// Exists so the scaffold links and runs end to end before that unit lands.

#include "transit/icon_cache.h"

namespace transit {

IconCache::IconCache(HttpTransport& transport) : transport_(transport) {}

IconBitmap IconCache::getIconBitmap(const std::string& /*imageSlug*/, uint16_t /*sizePx*/) {
  return IconBitmap{};  // null data = not available, caller falls back to text-only
}

}  // namespace transit
