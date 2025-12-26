#pragma once
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace Plexus {
    /// @brief Unique identifier for a resource managed by the Context.
    using ResourceID = uint32_t;

    /**
     * @brief A thread-safe registry for Resources.
     *
     * The Context is responsible for mapping string names to unique integer IDs
     * (ResourceID). This allows systems to reference shared data efficiently by ID
     * rather than string comparison during runtime.
     */
    class Context {
    public:
        /**
         * @brief Registers a resource by name or retrieves its existing ID.
         *
         * This method is thread-safe.
         *
         * @param name The unique name of the resource (e.g., "TransformComponent").
         * @return ResourceID A unique identifier for the resource.
         */
        ResourceID register_resource(const std::string &name);

        /**
         * @brief Retrieves the name associated with a ResourceID.
         *
         * Useful for debugging or visualization purposes.
         *
         * @param id The ResourceID to look up.
         * @return std::string The name of the resource, or empty string if not found.
         */
        std::string get_name(ResourceID id) const;

    private:
        std::map<std::string, ResourceID> m_name_to_id;
        std::map<ResourceID, std::string> m_id_to_name;
        mutable std::mutex m_mutex;
        ResourceID m_next_id = 0;
    };
}
