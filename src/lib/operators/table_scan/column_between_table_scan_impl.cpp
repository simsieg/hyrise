#include "column_between_table_scan_impl.hpp"

#include <memory>
#include <string>
#include <type_traits>

#include "expression/between_expression.hpp"
#include "storage/chunk.hpp"
#include "storage/create_iterable_from_segment.hpp"
#include "storage/segment_iterables/create_iterable_from_attribute_vector.hpp"
#include "storage/segment_iterate.hpp"
#include "storage/table.hpp"

#include "utils/assert.hpp"

#include "resolve_type.hpp"
#include "type_comparison.hpp"

namespace opossum {

ColumnBetweenTableScanImpl::ColumnBetweenTableScanImpl(const std::shared_ptr<const Table>& in_table,
                                                       const ColumnID column_id, const AllTypeVariant& left_value,
                                                       const AllTypeVariant& right_value,
                                                       PredicateCondition predicate_condition)
    : AbstractSingleColumnTableScanImpl(in_table, column_id, predicate_condition),
      _left_value{left_value},
      _right_value{right_value} {}

std::string ColumnBetweenTableScanImpl::description() const { return "ColumnBetween"; }

void ColumnBetweenTableScanImpl::_scan_non_reference_segment(
    const BaseSegment& segment, const ChunkID chunk_id, PosList& matches,
    const std::shared_ptr<const PosList>& position_filter) const {
  // early outs for specific NULL semantics
  if (variant_is_null(_left_value) || variant_is_null(_right_value)) {
    /**
     * Comparing anything with NULL (without using IS [NOT] NULL) will result in NULL.
     * Therefore, these scans will always return an empty position list.
     */
    return;
  }

  // Select optimized or generic scanning implementation based on segment type
  if (const auto* dictionary_segment = dynamic_cast<const BaseDictionarySegment*>(&segment)) {
    _scan_dictionary_segment(*dictionary_segment, chunk_id, matches, position_filter);
  } else {
    _scan_generic_segment(segment, chunk_id, matches, position_filter);
  }
}

void ColumnBetweenTableScanImpl::_scan_generic_segment(const BaseSegment& segment, const ChunkID chunk_id,
                                                       PosList& matches,
                                                       const std::shared_ptr<const PosList>& position_filter) const {
  segment_with_iterators_filtered(segment, position_filter, [&](auto it, const auto end) {
    using ColumnDataType = typename decltype(it)::ValueType;

    auto typed_left_value = type_cast_variant<ColumnDataType>(_left_value);
    auto typed_right_value = type_cast_variant<ColumnDataType>(_right_value);

    with_comparator_between(_predicate_condition, [&](auto between_comparator_function) {
      auto between_comparator = [&](const auto& position) {
        return between_comparator_function(position.value(), typed_left_value, typed_right_value);
      };
      _scan_with_iterators<true>(between_comparator, it, end, chunk_id, matches);
    });
  });
}

void ColumnBetweenTableScanImpl::_scan_dictionary_segment(const BaseDictionarySegment& segment, const ChunkID chunk_id,
                                                          PosList& matches,
                                                          const std::shared_ptr<const PosList>& position_filter) const {
  // naming assumption: the left value is always the lower one (otherwise the result is empty)
  ValueID left_value_id;
  if (is_between_predicate_condition_lower_inclusive(_predicate_condition)) {
    left_value_id = segment.lower_bound(_left_value);
  } else {
    left_value_id = segment.upper_bound(_left_value);
  }

  ValueID right_value_id;
  if (is_between_predicate_condition_upper_inclusive(_predicate_condition)) {
    right_value_id = segment.upper_bound(_right_value);
  } else {
    right_value_id = segment.lower_bound(_right_value);
  }

  if (right_value_id == INVALID_VALUE_ID) {
    // lower/upper_bound returns INVALID_VALUE_ID for NULL, while the dictionary uses unique_values_count (#1283).
    right_value_id = static_cast<ValueID>(segment.unique_values_count());
  }

  auto column_iterable = create_iterable_from_attribute_vector(segment);

  // NOLINTNEXTLINE - cpplint is drunk
  if (left_value_id == ValueID{0} && right_value_id == static_cast<ValueID>(segment.unique_values_count())) {
    // all values match
    column_iterable.with_iterators(position_filter, [&](auto left_it, auto left_end) {
      static const auto always_true = [](const auto&) { return true; };
      _scan_with_iterators<true>(always_true, left_it, left_end, chunk_id, matches);
    });

    return;
  }

  if (left_value_id == INVALID_VALUE_ID || left_value_id >= static_cast<ValueID>(segment.unique_values_count()) ||
      left_value_id >= right_value_id) {
    // TODO(all)
    // if (left_value_id >= static_cast<ValueID>(segment.unique_values_count()) || left_value_id == right_value_id) {
    // no values match
    return;
  }

  const auto value_id_diff = right_value_id - left_value_id;

  const auto comparator = [left_value_id, value_id_diff](const auto& position) {
    // Using < here because the right value id is the upper_bound. Also, because the value ids are integers, we can do
    // a little hack here: (x >= a && x < b) === ((x - a) < (b - a)); cf. https://stackoverflow.com/a/17095534/2204581

    return (position.value() - left_value_id) < value_id_diff;
  };

  column_iterable.with_iterators(position_filter, [&](auto left_it, auto left_end) {
    // No need to check for NULL because NULL would be represented as a value ID outside of our range
    _scan_with_iterators<false>(comparator, left_it, left_end, chunk_id, matches);
  });
}

}  // namespace opossum
