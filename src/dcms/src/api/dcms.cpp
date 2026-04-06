#include "api/dcms.h"
#include "api/dcms_api.h"

namespace dcms {

int init(const std::string &config_path) {
    return dcms_init(config_path.empty() ? nullptr : config_path.c_str());
}

void shutdown() { dcms_shutdown(); }

} // namespace dcms
