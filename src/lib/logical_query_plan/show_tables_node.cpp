#include "show_tables_node.hpp"

#include <string>

namespace opossum {

ShowTablesNode::ShowTablesNode() : AbstractLQPNode(LQPNodeType::ShowTables) {}

std::string ShowTablesNode::description() const { return "[ShowTables]"; }

std::shared_ptr<AbstractLQPNode> ShowTablesNode::_shallow_copy_impl(LQPNodeMapping & node_mapping) const {
  return ShowTablesNode::make();
}

bool ShowTablesNode::_shallow_equals_impl(const AbstractLQPNode& rhs, const LQPNodeMapping & node_mapping) const {
  return true;
}

}  // namespace opossum
