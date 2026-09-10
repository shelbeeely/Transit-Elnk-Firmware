// Stub implementation — replaced by work unit 8 (on-device setup flow).
// Exists so the scaffold links and runs end to end before that unit lands.

#include "transit/setup_flow.h"

namespace transit {

SetupFlow::SetupFlow(ConfigStore& configStore, TransitApiClient& apiClient,
                     RenderEngine& renderEngine)
    : configStore_(configStore), apiClient_(apiClient), renderEngine_(renderEngine) {}

bool SetupFlow::runFirstTimeSetup() {
  renderEngine_.renderSetupPrompt("Setup", "Setup flow not yet implemented.");
  return configStore_.isProvisioned();
}

}  // namespace transit
