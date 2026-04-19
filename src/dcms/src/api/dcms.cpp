#include "api/dcms.h"

#include <optional>

namespace dcms {

static std::optional<NodeHandle> g_default_node;

int init(const std::string &node_name, const std::string & /*config_path*/) {
    if (g_default_node && g_default_node->valid())
        return kDcmsOk;  // idempotent

    g_default_node.emplace();
    int rc = NodeHandle::create(node_name, *g_default_node);
    if (rc != kDcmsOk)
        g_default_node.reset();
    return rc;
}

void shutdown() {
    g_default_node.reset();
}

NodeHandle &defaultNode() {
    return *g_default_node;
}

} // namespace dcms
