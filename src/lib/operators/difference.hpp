#pragma once

#include "abstract_read_only_operator.hpp" // NEEDEDINCLUDE

namespace opossum {

/**
 * Note: Difference does not support null values at the moment
 */
class Difference : public AbstractReadOnlyOperator {
 public:
  Difference(const std::shared_ptr<const AbstractOperator>& left_in,
             const std::shared_ptr<const AbstractOperator>& right_in);

  const std::string name() const override;

 protected:
  std::shared_ptr<const Table> _on_execute() override;
  std::shared_ptr<AbstractOperator> _on_deep_copy(
      const std::shared_ptr<AbstractOperator>& copied_input_left,
      const std::shared_ptr<AbstractOperator>& copied_input_right) const override;
  void _on_set_parameters(const std::unordered_map<ParameterID, AllTypeVariant>& parameters) override;

 private:
  void _append_string_representation(std::ostream& row_string_buffer, const AllTypeVariant& value);
};
}  // namespace opossum
