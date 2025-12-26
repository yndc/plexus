#include "plexus/execution_graph.h"
#include <iomanip>
#include <iostream>

namespace Plexus {
    void ExecutionGraph::dump_debug(std::ostream &os) const {
        os << "Execution Graph Dump:\n";
        os << "Total Nodes: " << nodes.size() << "\n";
        os << "Entry Nodes: ";
        for (int id : entry_nodes) {
            os << id << " ";
        }
        os << "\n\n";

        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto &node = nodes[i];
            os << "[" << i << "] " << (node.label.empty() ? "<unnamed>" : node.label) << "\n";
            os << "  Priority: " << node.priority << "\n";
            os << "  Initial Dependencies: " << node.initial_dependencies << "\n";
            os << "  Dependents: ";
            if (node.dependents.empty()) {
                os << "None";
            } else {
                for (int dep : node.dependents) {
                    os << dep << " ";
                }
            }
            os << "\n";
            os << "  Error Policy: ";
            switch (node.error_policy) {
            case ErrorPolicy::Continue:
                os << "Continue";
                break;
            case ErrorPolicy::CancelDependents:
                os << "CancelDependents";
                break;
            case ErrorPolicy::CancelGraph:
                os << "CancelGraph";
                break;
            }
            os << "\n----------------------------------------\n";
        }
    }
}
