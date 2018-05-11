#include "table.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "resolve_type.hpp"
#include "types.hpp"
#include "utils/assert.hpp"
#include "value_column.hpp"

namespace opossum {

std::shared_ptr<Table> Table::create_dummy_table(const TableColumnDefinitions& column_definitions) {
  return std::make_shared<Table>(column_definitions, TableType::Data);
}

Table::Table(const TableColumnDefinitions& column_definitions, const TableType type, const uint32_t max_chunk_size,
             const UseMvcc use_mvcc)
    : _column_definitions(column_definitions),
      _type(type),
      _use_mvcc(use_mvcc),
      _max_chunk_size(max_chunk_size),
      _append_mutex(std::make_unique<std::mutex>()),
      _read_too_much(false) {
  Assert(max_chunk_size > 0, "Table must have a chunk size greater than 0.");
}

const TableColumnDefinitions& Table::column_definitions() const { return _column_definitions; }

TableType Table::type() const { return _type; }

UseMvcc Table::has_mvcc() const { return _use_mvcc; }

size_t Table::column_count() const { return _column_definitions.size(); }

const std::string& Table::column_name(const ColumnID column_id) const {
  DebugAssert(column_id < _column_definitions.size(), "ColumnID out of range");
  return _column_definitions[column_id].name;
}

std::vector<std::string> Table::column_names() const {
  std::vector<std::string> names;
  names.reserve(_column_definitions.size());
  for (const auto& column_definition : _column_definitions) {
    names.emplace_back(column_definition.name);
  }
  return names;
}

DataType Table::column_data_type(const ColumnID column_id) const {
  DebugAssert(column_id < _column_definitions.size(), "ColumnID out of range");
  return _column_definitions[column_id].data_type;
}

std::vector<DataType> Table::column_data_types() const {
  std::vector<DataType> data_types;
  data_types.reserve(_column_definitions.size());
  for (const auto& column_definition : _column_definitions) {
    data_types.emplace_back(column_definition.data_type);
  }
  return data_types;
}

bool Table::column_is_nullable(const ColumnID column_id) const {
  DebugAssert(column_id < _column_definitions.size(), "ColumnID out of range");
  return _column_definitions[column_id].nullable;
}

std::vector<bool> Table::columns_are_nullable() const {
  std::vector<bool> nullable(column_count());
  for (size_t column_idx = 0; column_idx < column_count(); ++column_idx) {
    nullable[column_idx] = _column_definitions[column_idx].nullable;
  }
  return nullable;
}

ColumnID Table::column_id_by_name(const std::string& column_name) const {
  const auto iter = std::find_if(_column_definitions.begin(), _column_definitions.end(),
                                 [&](const auto& column_definition) { return column_definition.name == column_name; });
  Assert(iter != _column_definitions.end(), "Couldn't find column '" + column_name + "'");
  return static_cast<ColumnID>(std::distance(_column_definitions.begin(), iter));
}

void Table::append(std::vector<AllTypeVariant> values) {
  if (_chunks.empty() || _chunks.back()->size() >= _max_chunk_size) {
    append_mutable_chunk();
  }

  _chunks.back()->append(values);
}

void Table::append_mutable_chunk() {
  ChunkColumns columns;
  for (const auto& column_definition : _column_definitions) {
    resolve_data_type(column_definition.data_type, [&](auto type) {
      using ColumnDataType = typename decltype(type)::type;
      columns.push_back(std::make_shared<ValueColumn<ColumnDataType>>(column_definition.nullable));
    });
  }
  append_chunk(columns);
}

uint64_t Table::row_count() const {
  uint64_t ret = 0;
  for (const auto& chunk : _chunks) {
    ret += chunk->size();
  }
  return ret;
}

bool Table::empty() const { return row_count() == 0u; }

bool Table::apply_and_check_bloom_filter(const uint16_t user_id) const {
  if (_read_too_much) {
    return true;
  }
  if (_type == TableType::Data && (user_id >= _bloom_filter.size() || _bloom_filter[user_id].empty())) {
    // no thresholds defined for the user in this data table
    return false;
  }
  ChunkID chunk_id{0};
  for (const auto& chunk : _chunks) {
    size_t prev_rows = _max_chunk_size * chunk_id;
    ColumnID column_id{0};
    for (const auto& column : chunk->columns()) {
      if (auto ref_column = std::dynamic_pointer_cast<ReferenceColumn>(column)) {
        auto table = ref_column->referenced_table();

        auto &bloom_filter = table->_bloom_filter;
        auto column_id = ref_column->referenced_column_id();

        if (user_id >= bloom_filter.size() || bloom_filter[user_id].empty()
                || bloom_filter[user_id][column_id].first == bloom_filter_size) {
          continue;
        }

        for (const auto& row_id : *ref_column->pos_list()) {
          size_t row_number = table->max_chunk_size() * row_id.chunk_id + row_id.chunk_offset;
          size_t hash = std::hash<size_t>{}(row_number) % bloom_filter_size;
          bloom_filter[user_id][column_id].second.set(hash, true);
        }

        bool above_threshold = bloom_filter[user_id][column_id].second.count() > bloom_filter[user_id][column_id].first;
        if (above_threshold) return true;

      } else { // dict or value column

        for (ChunkOffset chunk_offset{0}; chunk_offset < column->size(); ++chunk_offset) {
          size_t row_number = prev_rows + chunk_offset;
          size_t hash = std::hash<size_t>{}(row_number) % bloom_filter_size;
          _bloom_filter[user_id][column_id].second.set(hash, true);
        }

        bool above_threshold =
                _bloom_filter[user_id][column_id].second.count() > _bloom_filter[user_id][column_id].first;
        if (above_threshold) return true;
      }
      ++column_id;
    }
    ++chunk_id;
  }
  return false;
}

void Table::read_too_much() const { _read_too_much = true; }

void Table::set_bloom_filter(const uint16_t user_id, const ColumnID column_id, const BloomFilterSizeType threshold) {
  DebugAssert(_type == TableType::Data, "Can only set bloom filters on data tables");
  if (user_id >= _bloom_filter.size()) _bloom_filter.resize(user_id + 1);
  if (_bloom_filter[user_id].empty()) {
    // per default the threshold for each column is set to max bloom filter size,
    // if this is the case the filter is ignored during checking
    _bloom_filter[user_id].resize(column_count(), std::make_pair(bloom_filter_size, BloomBitset()));
  }
  _bloom_filter[user_id][column_id].first = threshold;
}

ChunkID Table::chunk_count() const { return static_cast<ChunkID>(_chunks.size()); }

const std::vector<std::shared_ptr<Chunk>>& Table::chunks() const { return _chunks; }

uint32_t Table::max_chunk_size() const { return _max_chunk_size; }

std::shared_ptr<Chunk> Table::get_chunk(ChunkID chunk_id) {
  DebugAssert(chunk_id < _chunks.size(), "ChunkID " + std::to_string(chunk_id) + " out of range");
  return _chunks[chunk_id];
}

std::shared_ptr<const Chunk> Table::get_chunk(ChunkID chunk_id) const {
  DebugAssert(chunk_id < _chunks.size(), "ChunkID " + std::to_string(chunk_id) + " out of range");
  return _chunks[chunk_id];
}

ProxyChunk Table::get_chunk_with_access_counting(ChunkID chunk_id) {
  DebugAssert(chunk_id < _chunks.size(), "ChunkID " + std::to_string(chunk_id) + " out of range");
  return ProxyChunk(_chunks[chunk_id]);
}

const ProxyChunk Table::get_chunk_with_access_counting(ChunkID chunk_id) const {
  DebugAssert(chunk_id < _chunks.size(), "ChunkID " + std::to_string(chunk_id) + " out of range");
  return ProxyChunk(_chunks[chunk_id]);
}

void Table::append_chunk(const ChunkColumns& columns, const std::optional<PolymorphicAllocator<Chunk>>& alloc,
                         const std::shared_ptr<ChunkAccessCounter>& access_counter) {
  const auto chunk_size = columns.empty() ? 0u : columns[0]->size();

#if IS_DEBUG
  for (const auto& column : columns) {
    DebugAssert(column->size() == chunk_size, "Columns don't have the same length");
    const auto is_reference_column = std::dynamic_pointer_cast<ReferenceColumn>(column) != nullptr;
    switch (_type) {
      case TableType::References:
        DebugAssert(is_reference_column, "Invalid column type");
        break;
      case TableType::Data:
        DebugAssert(!is_reference_column, "Invalid column type");
        break;
    }
  }
#endif

  std::shared_ptr<MvccColumns> mvcc_columns;

  if (_use_mvcc == UseMvcc::Yes) {
    mvcc_columns = std::make_shared<MvccColumns>(chunk_size);
  }

  _chunks.emplace_back(std::make_shared<Chunk>(columns, mvcc_columns, alloc, access_counter));
}

std::unique_lock<std::mutex> Table::acquire_append_mutex() { return std::unique_lock<std::mutex>(*_append_mutex); }

std::vector<IndexInfo> Table::get_indexes() const { return _indexes; }

size_t Table::estimate_memory_usage() const {
  auto bytes = size_t{sizeof(*this)};

  for (const auto& chunk : _chunks) {
    bytes += chunk->estimate_memory_usage();
  }

  for (const auto& column_definition : _column_definitions) {
    bytes += column_definition.name.size();
  }

  // TODO(anybody) Statistics and Indices missing from Memory Usage Estimation
  // TODO(anybody) TableLayout missing

  return bytes;
}

}  // namespace opossum
