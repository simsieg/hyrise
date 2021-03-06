#include "join_nested_loop.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "resolve_type.hpp"
#include "storage/create_iterable_from_segment.hpp"
#include "storage/segment_iterables/any_segment_iterable.hpp"
#include "storage/segment_iterate.hpp"
#include "type_comparison.hpp"
#include "utils/assert.hpp"
#include "utils/ignore_unused_variable.hpp"
#include "utils/performance_warning.hpp"

namespace {

using namespace opossum;  // NOLINT

void process_match(RowID left_row_id, RowID right_row_id, const JoinNestedLoop::JoinParams& params) {
  params.pos_list_left.emplace_back(left_row_id);
  params.pos_list_right.emplace_back(right_row_id);

  if (params.track_left_matches) {
    params.left_matches[left_row_id.chunk_offset] = true;
  }

  if (params.track_right_matches) {
    params.right_matches[right_row_id.chunk_offset] = true;
  }
}

// inner join loop that joins two segments via their iterators
// __attribute__((noinline)) to reduce compile time. As the hotloop is within this function, no performance
// loss expected
template <typename BinaryFunctor, typename LeftIterator, typename RightIterator>
void __attribute__((noinline))
join_two_typed_segments(const BinaryFunctor& func, LeftIterator left_it, LeftIterator left_end,
                        RightIterator right_begin, RightIterator right_end, const ChunkID chunk_id_left,
                        const ChunkID chunk_id_right, const JoinNestedLoop::JoinParams& params) {
  /**
   * The nested loops.
   */

  for (; left_it != left_end; ++left_it) {
    const auto left_value = *left_it;
    if (left_value.is_null()) continue;

    for (auto right_it = right_begin; right_it != right_end; ++right_it) {
      const auto right_value = *right_it;
      if (right_value.is_null()) continue;

      if (func(left_value.value(), right_value.value())) {
        process_match(RowID{chunk_id_left, left_value.chunk_offset()},
                      RowID{chunk_id_right, right_value.chunk_offset()}, params);
      }
    }
  }
}
}  // namespace

namespace opossum {

/*
 * This is a Nested Loop Join implementation completely based on iterables.
 * It supports all current join and predicate conditions, as well as NULL values.
 * Because this is a Nested Loop Join, the performance is going to be far inferior to JoinHash and JoinSortMerge,
 * so only use this for testing or benchmarking purposes.
 */

JoinNestedLoop::JoinNestedLoop(const std::shared_ptr<const AbstractOperator>& left,
                               const std::shared_ptr<const AbstractOperator>& right, const JoinMode mode,
                               const ColumnIDPair& column_ids, const PredicateCondition predicate_condition)
    : AbstractJoinOperator(OperatorType::JoinNestedLoop, left, right, mode, column_ids, predicate_condition) {}

const std::string JoinNestedLoop::name() const { return "JoinNestedLoop"; }

std::shared_ptr<AbstractOperator> JoinNestedLoop::_on_deep_copy(
    const std::shared_ptr<AbstractOperator>& copied_input_left,
    const std::shared_ptr<AbstractOperator>& copied_input_right) const {
  return std::make_shared<JoinNestedLoop>(copied_input_left, copied_input_right, _mode, _column_ids,
                                          _predicate_condition);
}

void JoinNestedLoop::_on_set_parameters(const std::unordered_map<ParameterID, AllTypeVariant>& parameters) {}

std::shared_ptr<const Table> JoinNestedLoop::_on_execute() {
  PerformanceWarning("Nested Loop Join used");

  const auto output_table = _initialize_output_table();

  auto left_table = input_table_left();
  auto right_table = input_table_right();

  auto left_column_id = _column_ids.first;
  auto right_column_id = _column_ids.second;

  auto maybe_flipped_predicate_condition = _predicate_condition;

  if (_mode == JoinMode::Right) {
    // for Right Outer we swap the tables so we have the outer on the "left"
    std::swap(left_table, right_table);
    std::swap(left_column_id, right_column_id);
    maybe_flipped_predicate_condition = flip_predicate_condition(_predicate_condition);
  }

  const auto pos_list_left = std::make_shared<PosList>();
  const auto pos_list_right = std::make_shared<PosList>();

  const auto is_outer_join = (_mode == JoinMode::Left || _mode == JoinMode::Right || _mode == JoinMode::Outer);

  // for Full Outer, remember the matches on the right side
  std::vector<std::vector<bool>> right_matches(right_table->chunk_count());

  // Scan all chunks from left input
  for (ChunkID chunk_id_left = ChunkID{0}; chunk_id_left < left_table->chunk_count(); ++chunk_id_left) {
    auto segment_left = left_table->get_chunk(chunk_id_left)->get_segment(left_column_id);

    // for Outer joins, remember matches on the left side
    std::vector<bool> left_matches;

    if (is_outer_join) {
      left_matches.resize(segment_left->size());
    }

    // Scan all chunks for right input
    for (ChunkID chunk_id_right = ChunkID{0}; chunk_id_right < right_table->chunk_count(); ++chunk_id_right) {
      const auto segment_right = right_table->get_chunk(chunk_id_right)->get_segment(right_column_id);
      right_matches[chunk_id_right].resize(segment_right->size());

      const auto track_right_matches = (_mode == JoinMode::Outer);
      JoinParams params{*pos_list_left, *pos_list_right,     left_matches, right_matches[chunk_id_right],
                        is_outer_join,  track_right_matches, _mode,        maybe_flipped_predicate_condition};
      _join_two_untyped_segments(*segment_left, *segment_right, chunk_id_left, chunk_id_right, params);
    }

    if (is_outer_join) {
      // add unmatched rows on the left for Left and Full Outer joins
      for (ChunkOffset chunk_offset{0}; chunk_offset < left_matches.size(); ++chunk_offset) {
        if (!left_matches[chunk_offset]) {
          pos_list_left->emplace_back(RowID{chunk_id_left, chunk_offset});
          pos_list_right->emplace_back(NULL_ROW_ID);
        }
      }
    }
  }

  // For Full Outer we need to add all unmatched rows for the right side.
  // Unmatched rows on the left side are already added in the main loop above
  if (_mode == JoinMode::Outer) {
    for (ChunkID chunk_id_right = ChunkID{0}; chunk_id_right < right_table->chunk_count(); ++chunk_id_right) {
      const auto chunk_size = right_table->get_chunk(chunk_id_right)->size();

      for (auto chunk_offset = ChunkOffset{0}; chunk_offset < chunk_size; ++chunk_offset) {
        if (!right_matches[chunk_id_right][chunk_offset]) {
          pos_list_left->emplace_back(NULL_ROW_ID);
          pos_list_right->emplace_back(chunk_id_right, chunk_offset);
        }
      }
    }
  }

  // write output chunks
  Segments segments;

  if (_mode == JoinMode::Right) {
    _write_output_chunks(segments, right_table, pos_list_right);
    _write_output_chunks(segments, left_table, pos_list_left);
  } else {
    _write_output_chunks(segments, left_table, pos_list_left);
    _write_output_chunks(segments, right_table, pos_list_right);
  }

  output_table->append_chunk(segments);

  return output_table;
}

void JoinNestedLoop::_join_two_untyped_segments(const BaseSegment& base_segment_left,
                                                const BaseSegment& base_segment_right, const ChunkID chunk_id_left,
                                                const ChunkID chunk_id_right, JoinNestedLoop::JoinParams& params) {
  /**
   * This function dispatches `join_two_typed_segments`.
   *
   * To reduce compile time, we erase the types of Segments and the PredicateCondition/comparator if
   * `base_segment_left.data_type() != base_segment_left.data_type()` or `LeftSegmentType != RightSegmentType`. This is
   * the "SLOW PATH".
   * If data types and segment types are the same, we take the "FAST PATH", where only the SegmentType of left segment is
   * erased and inlining optimization can be performed by the compiler for the inner loop.
   *
   * Having this SLOW PATH and erasing the SegmentType even for the FAST PATH are essential for keeping the compile time
   * of the JoinNestedLoop reasonably low.
   */

  /**
   * FAST PATH
   */
  if (base_segment_left.data_type() == base_segment_right.data_type()) {
    auto fast_path_taken = false;

    resolve_data_and_segment_type(base_segment_left, [&](const auto data_type_t, const auto& segment_left) {
      using ColumnDataType = typename decltype(data_type_t)::type;
      using LeftSegmentType = std::decay_t<decltype(segment_left)>;

      if (const auto* segment_right = dynamic_cast<const LeftSegmentType*>(&base_segment_right)) {
        const auto iterable_left = create_any_segment_iterable<ColumnDataType>(segment_left);
        const auto iterable_right = create_iterable_from_segment<ColumnDataType>(*segment_right);

        iterable_left.with_iterators([&](auto left_begin, const auto& left_end) {
          iterable_right.with_iterators([&](auto right_begin, const auto& right_end) {
            with_comparator(params.predicate_condition, [&](auto comparator) {
              join_two_typed_segments(comparator, left_begin, left_end, right_begin, right_end, chunk_id_left,
                                      chunk_id_right, params);
            });
          });
        });

        fast_path_taken = true;
      }
    });

    if (fast_path_taken) {
      return;
    }
  }

  /**
   * SLOW PATH
   */
  // clang-format off
  segment_with_iterators<ResolveDataTypeTag, EraseTypes::Always>(base_segment_left, [&](auto left_it, const auto left_end) {  // NOLINT
    segment_with_iterators<ResolveDataTypeTag>(base_segment_right, [&](auto right_it, const auto right_end) {  // NOLINT
      using LeftType = typename std::decay_t<decltype(left_it)>::ValueType;
      using RightType = typename std::decay_t<decltype(right_it)>::ValueType;

      // make sure that we do not compile invalid versions of these lambdas
      constexpr auto LEFT_IS_STRING_COLUMN = (std::is_same<LeftType, pmr_string>{});
      constexpr auto RIGHT_IS_STRING_COLUMN = (std::is_same<RightType, pmr_string>{});

      constexpr auto NEITHER_IS_STRING_COLUMN = !LEFT_IS_STRING_COLUMN && !RIGHT_IS_STRING_COLUMN;
      constexpr auto BOTH_ARE_STRING_COLUMN = LEFT_IS_STRING_COLUMN && RIGHT_IS_STRING_COLUMN;

      if constexpr (NEITHER_IS_STRING_COLUMN || BOTH_ARE_STRING_COLUMN) {
        // Dirty hack to avoid https://gcc.gnu.org/bugzilla/show_bug.cgi?id=86740
        const auto left_it_copy = left_it;
        const auto left_end_copy = left_end;
        const auto right_it_copy = right_it;
        const auto right_end_copy = right_end;
        const auto params_copy = params;
        const auto chunk_id_left_copy = chunk_id_left;
        const auto chunk_id_right_copy = chunk_id_right;

        // Erase the `predicate_condition` into a std::function<>
        auto erased_comparator = std::function<bool(const LeftType&, const RightType&)>{};
        with_comparator(params_copy.predicate_condition, [&](auto comparator) { erased_comparator = comparator; });

        join_two_typed_segments(erased_comparator, left_it_copy, left_end_copy, right_it_copy, right_end_copy,
                                chunk_id_left_copy, chunk_id_right_copy, params_copy);
      } else {
        // gcc complains without these
        ignore_unused_variable(right_end);
        ignore_unused_variable(left_end);

        Fail("Cannot join String with non-String column");
      }
    });
  });
  // clang-format on
}

void JoinNestedLoop::_write_output_chunks(Segments& segments, const std::shared_ptr<const Table>& input_table,
                                          const std::shared_ptr<PosList>& pos_list) {
  // Add segments from table to output chunk
  for (ColumnID column_id{0}; column_id < input_table->column_count(); ++column_id) {
    std::shared_ptr<BaseSegment> segment;

    if (input_table->type() == TableType::References) {
      if (input_table->chunk_count() > 0) {
        auto new_pos_list = std::make_shared<PosList>();

        // de-reference to the correct RowID so the output can be used in a Multi Join
        for (const auto& row : *pos_list) {
          if (row.is_null()) {
            new_pos_list->push_back(NULL_ROW_ID);
          } else {
            auto reference_segment = std::static_pointer_cast<const ReferenceSegment>(
                input_table->get_chunk(row.chunk_id)->get_segment(column_id));
            new_pos_list->push_back((*reference_segment->pos_list())[row.chunk_offset]);
          }
        }

        auto reference_segment = std::static_pointer_cast<const ReferenceSegment>(
            input_table->get_chunk(ChunkID{0})->get_segment(column_id));

        segment = std::make_shared<ReferenceSegment>(reference_segment->referenced_table(),
                                                     reference_segment->referenced_column_id(), new_pos_list);
      } else {
        // If there are no Chunks in the input_table, we can't deduce the Table that input_table is referencing to.
        // pos_list will contain only NULL_ROW_IDs anyway, so it doesn't matter which Table the ReferenceSegment that
        // we output is referencing. HACK, but works fine: we create a dummy table and let the ReferenceSegment ref
        // it.
        const auto dummy_table = Table::create_dummy_table(input_table->column_definitions());
        segment = std::make_shared<ReferenceSegment>(dummy_table, column_id, pos_list);
      }
    } else {
      segment = std::make_shared<ReferenceSegment>(input_table, column_id, pos_list);
    }

    segments.push_back(segment);
  }
}

}  // namespace opossum
