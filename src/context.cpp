#include "plexus/context.h"

namespace Plexus {

    ResourceID Context::register_resource(const std::string &name) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_name_to_id.find(name); it != m_name_to_id.end()) {
            return it->second;
        }

        auto id = m_next_id++;
        m_name_to_id[name] = id;
        m_id_to_name[id] = name;
        return id;
    }

    std::string Context::get_name(ResourceID id) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (auto it = m_id_to_name.find(id); it != m_id_to_name.end()) {
            return it->second;
        }
        return ""; // Or throw, but for now empty string
    }

}
