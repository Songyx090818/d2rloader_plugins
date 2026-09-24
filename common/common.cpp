#include "common.h"

namespace common {

auto SdkApiVersion() noexcept -> unsigned {
        return static_cast<unsigned>(D2RL_PLUGIN_ABI_VERSION);
}

}  // namespace common
