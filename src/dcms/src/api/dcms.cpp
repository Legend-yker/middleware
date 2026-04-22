#include "api/dcms.h"

#include <optional>

namespace dcms {

// 进程级唯一的默认 NodeHandle（单节点进程直接用 dcms::init/defaultNode）
static std::optional<NodeHandle> g_default_node;

int init(const std::string &node_name, const std::string & /*config_path*/) {
    if (g_default_node && g_default_node->valid())
        return kDcmsOk; // 幂等：已初始化则直接返回

    g_default_node.emplace();
    int rc = NodeHandle::create(node_name, *g_default_node);
    if (rc != kDcmsOk)
        g_default_node.reset(); // 失败时清空，防止返回无效对象
    return rc;
}

int shutdown() {
    g_default_node.reset(); // 析构 NodeHandle：关闭 daemon_fd，join notify_thread_
    return 0;
}

NodeHandle &defaultNode() {
    return *g_default_node; // 调用方须确保 init() 已成功，否则解引用 nullopt 会崩溃
}

} // namespace dcms
