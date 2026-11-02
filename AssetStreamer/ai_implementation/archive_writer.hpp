#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <mdspan>
#include <optional>
#include <print>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#if __has_include(<inplace_vector>)
#include <inplace_vector>
#endif

// ============================================================================
// 1. DECLARATIVE STRUCTS & BLUEPRINT DEFINITIONS
// ============================================================================

enum class block_type {
  magic_bytes,
  file_version,
  index_table, // Global lookup containing chunk information
  payload_data // The section where sequential chunks are packed
};

struct layout_rule {
  block_type type;
  std::size_t fixed_size = 0;
};

template <std::size_t RuleCount> struct declarative_blueprint {
  std::array<layout_rule, RuleCount> rules;
};

// Explicit structural types for Binary Layout Parsing
struct index_entry {
  std::uint32_t chunk_id;
  std::uint64_t offset;
  std::uint64_t size;
};

struct chunk_header {
  std::uint32_t magic;
  std::uint32_t chunk_id;
  std::uint64_t data_size;
};

// A single record of data stored in a chunk payload. Can be written out as a
// log of what has been packed into the archive.
struct archive_record {
  std::string name;
  index_entry index;
};

// ============================================================================
// 2. CONCEPTS & VALIDATION ENGINE
// ============================================================================

template <typename C>
concept contiguous_byte_container = requires(C c) {
  typename C::value_type;
  requires std::same_as<typename C::value_type, std::byte>;
  { c.data() } -> std::same_as<std::byte *>;
  { c.end() };
  { c.clear() };
  { c.resize(std::size_t{}) };
  { c.resize(std::size_t{}, std::byte{}) };
  { c.push_back(std::declval<std::byte>()) };
  {
    c.insert(c.end(), static_cast<const std::byte *>(nullptr),
             static_cast<const std::byte *>(nullptr))
  };
  { c.size() } -> std::convertible_to<std::size_t>;
};

template <std::size_t N>
constexpr bool validate_blueprint(const declarative_blueprint<N> &blueprint) {
  if constexpr (N != 4) {
    return false;
  } else {
    return blueprint.rules[0].type == block_type::magic_bytes &&
           blueprint.rules[0].fixed_size == 4 &&
           blueprint.rules[1].type == block_type::file_version &&
           blueprint.rules[1].fixed_size == 2 &&
           blueprint.rules[2].type == block_type::index_table &&
           blueprint.rules[2].fixed_size == 0 &&
           blueprint.rules[3].type == block_type::payload_data &&
           blueprint.rules[3].fixed_size == 0;
  }
}

template <std::unsigned_integral T, contiguous_byte_container Container>
void append_little_endian(Container &buffer, T value) {
  std::array<std::byte, sizeof(T)> encoded{};
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    encoded[i] = static_cast<std::byte>(value & static_cast<T>(0xff));
    value >>= 8;
  }
  buffer.insert(buffer.end(), encoded.data(), encoded.data() + encoded.size());
}

template <std::unsigned_integral T>
T read_little_endian(std::span<const std::byte> bytes, std::size_t offset) {
  T value = 0;
  for (std::size_t i = 0; i < sizeof(T); ++i) {
    value |= static_cast<T>(std::to_integer<unsigned char>(bytes[offset + i]))
             << (i * 8);
  }
  return value;
}

inline constexpr std::size_t archive_prefix_size = 6;
inline constexpr std::uint16_t archive_format_version = 3;
inline constexpr std::size_t archive_index_count_size = sizeof(std::uint32_t);
inline constexpr std::size_t archive_v2_index_entry_size =
    sizeof(std::uint32_t) + 2 * sizeof(std::uint64_t);
inline constexpr std::size_t archive_index_entry_size =
    2 * sizeof(std::uint32_t) + 3 * sizeof(std::uint64_t);
inline constexpr std::size_t archive_v3_index_header_size =
    sizeof(std::uint32_t) + sizeof(std::uint64_t);
inline constexpr std::size_t archive_chunk_header_size =
    2 * sizeof(std::uint32_t) + sizeof(std::uint64_t);
inline constexpr std::size_t archive_payload_alignment = 16;

inline std::size_t align_archive_offset(std::size_t offset) {
  constexpr auto padding = archive_payload_alignment - 1;
  if (offset > std::numeric_limits<std::size_t>::max() - padding)
    throw std::length_error("Archive size exceeds the supported range.");
  return (offset + padding) & ~padding;
}

// ============================================================================
// 3. THE DECLARATIVE LAYOUT ENGINE
// ============================================================================

template <auto Blueprint> struct declarative_binary_layout {
  static_assert(validate_blueprint(Blueprint),
                "Unsupported declarative_blueprint: expected magic_bytes, "
                "file_version, index_table, and payload_data in that order.");

  // Helper to find the dynamic runtime or static offset of any section type
  static constexpr std::size_t
  get_section_static_offset(block_type target) noexcept {
    std::size_t offset = 0;

    for (const auto &rule : Blueprint.rules) {
      if (rule.type == target) {
        break;
      }
      offset += rule.fixed_size;
    }
    return offset;
  }

  // Appends baseline header blocks up to the dynamic entries
  template <typename Container>
  void write_prefix_headers(Container &buffer) const {
    for (const auto &rule : Blueprint.rules) {
      if (rule.type == block_type::magic_bytes) {
        append_little_endian(buffer, std::uint32_t{0x46414B50});
      } else if (rule.type == block_type::file_version) {
        append_little_endian(buffer, archive_format_version);
      }
    }
  }

  [[nodiscard]] std::optional<std::vector<archive_record>>
  extract_index_table(std::span<const std::byte> full_buffer) const {
    constexpr std::size_t count_offset = archive_prefix_size;
    constexpr std::size_t v2_table_offset =
        count_offset + archive_index_count_size;
    constexpr std::size_t v3_table_offset =
        count_offset + archive_v3_index_header_size;

    if (full_buffer.size() < v2_table_offset ||
        read_little_endian<std::uint32_t>(full_buffer, 0) != 0x46414B50) {
      return std::nullopt;
    }

    const auto version = read_little_endian<std::uint16_t>(full_buffer, 4);
    if (version != 2 && version != archive_format_version) {
      return std::nullopt;
    }

    const std::uint32_t chunk_count =
        read_little_endian<std::uint32_t>(full_buffer, count_offset);
    const std::size_t entry_size =
        version == 2 ? archive_v2_index_entry_size : archive_index_entry_size;
    const std::size_t table_offset =
        version == 2 ? v2_table_offset : v3_table_offset;
    if (full_buffer.size() < table_offset) {
      return std::nullopt;
    }

    const std::size_t available_table_bytes = full_buffer.size() - table_offset;
    if (chunk_count > available_table_bytes / entry_size) {
      return std::nullopt;
    }

    const std::size_t table_end =
        table_offset + static_cast<std::size_t>(chunk_count) * entry_size;
    const auto names_size_field =
        version == 2
            ? std::uint64_t{0}
            : read_little_endian<std::uint64_t>(full_buffer, count_offset + 4);
    if (names_size_field > std::numeric_limits<std::size_t>::max()) {
      return std::nullopt;
    }
    const std::size_t names_size = static_cast<std::size_t>(names_size_field);
    if (names_size > full_buffer.size() - table_end) {
      return std::nullopt;
    }

    const std::size_t names_end = table_end + names_size;
    const std::size_t minimum_chunk_offset = align_archive_offset(names_end);
    std::vector<archive_record> entries;
    entries.reserve(chunk_count);
    std::unordered_set<std::uint32_t> seen_ids;
    std::unordered_set<std::string> seen_names;
    std::vector<std::pair<std::size_t, std::size_t>> chunk_ranges;
    chunk_ranges.reserve(chunk_count);
    std::size_t entry_offset = table_offset;

    for (std::uint32_t i = 0; i < chunk_count; ++i) {
      index_entry entry{
          .chunk_id =
              read_little_endian<std::uint32_t>(full_buffer, entry_offset),
          .offset =
              read_little_endian<std::uint64_t>(full_buffer, entry_offset + 4),
          .size = read_little_endian<std::uint64_t>(full_buffer,
                                                    entry_offset + 12)};
      std::string name;
      if (version >= 3) {
        const auto name_offset =
            read_little_endian<std::uint64_t>(full_buffer, entry_offset + 20);
        const auto name_size =
            read_little_endian<std::uint32_t>(full_buffer, entry_offset + 28);
        if (name_offset > names_size ||
            name_size > names_size - static_cast<std::size_t>(name_offset)) {
          return std::nullopt;
        }

        const auto name_begin =
            full_buffer.begin() +
            static_cast<std::ptrdiff_t>(table_end +
                                        static_cast<std::size_t>(name_offset));
        name.assign(reinterpret_cast<const char *>(std::to_address(name_begin)),
                    name_size);
        if (name.empty() || name.find('\0') != std::string::npos ||
            !seen_names.insert(name).second) {
          return std::nullopt;
        }
      }
      entry_offset += entry_size;

      if (!seen_ids.insert(entry.chunk_id).second ||
          entry.offset > std::numeric_limits<std::size_t>::max() ||
          entry.size > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
      }

      const auto chunk_offset = static_cast<std::size_t>(entry.offset);
      const auto data_size = static_cast<std::size_t>(entry.size);
      if (chunk_offset < minimum_chunk_offset ||
          chunk_offset % archive_payload_alignment != 0 ||
          chunk_offset > full_buffer.size() ||
          archive_chunk_header_size > full_buffer.size() - chunk_offset) {
        return std::nullopt;
      }

      const std::size_t payload_offset =
          chunk_offset + archive_chunk_header_size;
      if (data_size > full_buffer.size() - payload_offset ||
          read_little_endian<std::uint32_t>(full_buffer, chunk_offset) !=
              0x43484E4B ||
          read_little_endian<std::uint32_t>(full_buffer, chunk_offset + 4) !=
              entry.chunk_id ||
          read_little_endian<std::uint64_t>(full_buffer, chunk_offset + 8) !=
              entry.size) {
        return std::nullopt;
      }

      chunk_ranges.emplace_back(chunk_offset, payload_offset + data_size);
      entries.push_back(archive_record{std::move(name), entry});
    }

    std::ranges::sort(chunk_ranges);
    for (std::size_t i = 1; i < chunk_ranges.size(); ++i) {
      if (chunk_ranges[i].first < chunk_ranges[i - 1].second) {
        return std::nullopt;
      }
    }

    return entries;
  }

  [[nodiscard]] std::optional<std::span<const std::byte>>
  extract_chunk_payload(std::span<const std::byte> full_buffer,
                        std::uint32_t target_chunk_id) const {
    auto index_table = extract_index_table(full_buffer);
    if (!index_table) {
      return {};
    }

    for (const auto &record : *index_table) {
      if (record.index.chunk_id == target_chunk_id) {
        const auto raw_data_offset =
            static_cast<std::size_t>(record.index.offset) +
            archive_chunk_header_size;
        return full_buffer.subspan(raw_data_offset,
                                   static_cast<std::size_t>(record.index.size));
      }
    }
    return std::nullopt;
  }
};

// ============================================================================
// 4. THE WRITER AND READER CORE CLASSES
// ============================================================================

template <auto Blueprint,
          contiguous_byte_container Container = std::vector<std::byte>>
class archive_writer {
public:
  constexpr archive_writer() = default;
  constexpr explicit archive_writer(Container &&container)
      : m_buffer(std::move(container)) {}

  void add_chunk(std::uint32_t chunk_id, std::span<const std::byte> data) {
    add_staged_chunk(chunk_id, "chunk-" + std::to_string(chunk_id),
                     std::vector<std::byte>(data.begin(), data.end()));
  }

  std::uint32_t add_data(std::string name, std::span<const std::byte> data) {
    return add_named_chunk(std::move(name),
                           std::vector<std::byte>(data.begin(), data.end()));
  }

  // The callback fills a buffer and returns bytes read; zero ends this item.
  // Socket adapters should define an item boundary (for example, one message).
  template <typename Reader>
    requires std::invocable<Reader &, std::span<std::byte>> &&
             std::convertible_to<
                 std::invoke_result_t<Reader &, std::span<std::byte>>,
                 std::size_t>
  std::uint32_t add_stream(std::string name, Reader &&reader) {
    validate_new_name(name);

    std::vector<std::byte> data;
    std::vector<std::byte> buffer(64 * 1024);
    auto read = std::forward<Reader>(reader);

    while (true) {
      const auto bytes_read =
          static_cast<std::size_t>(std::invoke(read, std::span{buffer}));
      if (bytes_read > buffer.size()) {
        throw std::runtime_error(
            "Stream reader returned a byte count larger than its buffer.");
      }
      if (bytes_read == 0) {
        break;
      }
      if (bytes_read > data.max_size() - data.size()) {
        throw std::length_error("Stream item exceeds the supported size.");
      }
      data.insert(data.end(), buffer.begin(),
                  buffer.begin() + static_cast<std::ptrdiff_t>(bytes_read));
    }

    return add_named_chunk(std::move(name), std::move(data));
  }

  std::uint32_t add_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
      throw std::runtime_error("Failed to open source file: " + path.string());
    }

    auto read = [&file, &path](std::span<std::byte> buffer) -> std::size_t {
      file.read(reinterpret_cast<char *>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
      const auto bytes_read = file.gcount();
      if (file.bad() || (file.fail() && !file.eof())) {
        throw std::runtime_error("Failed while reading source file: " +
                                 path.string());
      }
      return static_cast<std::size_t>(bytes_read);
    };

    return add_stream(path.filename().string(), read);
  }

  std::uint32_t add_file(const std::string &path) {
    return add_file(std::filesystem::path{path});
  }

  void add_files(std::span<const std::filesystem::path> paths) {
    const auto original_size = m_staged_chunks.size();
    const auto original_next_id = m_next_chunk_id;
    try {
      for (const auto &path : paths) {
        add_file(path);
      }
    } catch (...) {
      m_staged_chunks.resize(original_size);
      m_next_chunk_id = original_next_id;
      throw;
    }
  }

  void add_files(const std::vector<std::filesystem::path> &paths) {
    add_files(std::span<const std::filesystem::path>{paths});
  }

  void add_files(const std::vector<std::string> &paths) {
    std::vector<std::filesystem::path> filesystem_paths;
    filesystem_paths.reserve(paths.size());
    for (const auto &path : paths) {
      filesystem_paths.emplace_back(path);
    }
    add_files(filesystem_paths);
  }

  void finalize_and_write(const std::filesystem::path &path) {
    if (m_staged_chunks.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("Archive contains too many chunks.");
    }
    if (m_staged_chunks.size() >
        (std::numeric_limits<std::size_t>::max() - archive_prefix_size -
         archive_v3_index_header_size) /
            archive_index_entry_size) {
      throw std::length_error("Archive index exceeds the supported size.");
    }

    std::size_t names_size = 0;
    for (const auto &chunk : m_staged_chunks) {
      if (chunk.name.size() > std::numeric_limits<std::uint32_t>::max() ||
          chunk.name.size() >
              std::numeric_limits<std::size_t>::max() - names_size) {
        throw std::length_error(
            "Archive item name exceeds the supported size.");
      }
      names_size += chunk.name.size();
    }

    const std::size_t entries_size =
        m_staged_chunks.size() * archive_index_entry_size;
    const std::size_t names_offset =
        archive_prefix_size + archive_v3_index_header_size + entries_size;
    if (names_size > std::numeric_limits<std::size_t>::max() - names_offset) {
      throw std::length_error("Archive name table exceeds the supported size.");
    }

    m_buffer.clear();
    m_layout.write_prefix_headers(m_buffer);

    const auto chunk_count = static_cast<std::uint32_t>(m_staged_chunks.size());
    append_little_endian(m_buffer, chunk_count);
    append_little_endian(m_buffer, static_cast<std::uint64_t>(names_size));

    std::size_t current_chunk_offset =
        align_archive_offset(names_offset + names_size);
    std::size_t name_offset = 0;
    std::vector<archive_record> generated_entries;
    generated_entries.reserve(m_staged_chunks.size());

    for (const auto &chunk : m_staged_chunks) {
      if (chunk.data.size() > std::numeric_limits<std::uint64_t>::max()) {
        throw std::length_error("Chunk exceeds the supported size.");
      }

      if (current_chunk_offset > std::numeric_limits<std::size_t>::max() -
                                     archive_chunk_header_size ||
          chunk.data.size() > std::numeric_limits<std::size_t>::max() -
                                  current_chunk_offset -
                                  archive_chunk_header_size) {
        throw std::length_error("Archive exceeds the supported size.");
      }

      generated_entries.push_back(archive_record{
          .name = chunk.name,
          .index = index_entry{
              .chunk_id = chunk.id,
              .offset = current_chunk_offset,
              .size = static_cast<std::uint64_t>(chunk.data.size())}});
      append_little_endian(m_buffer, chunk.id);
      append_little_endian(m_buffer,
                           static_cast<std::uint64_t>(current_chunk_offset));
      append_little_endian(m_buffer,
                           static_cast<std::uint64_t>(chunk.data.size()));
      append_little_endian(m_buffer, static_cast<std::uint64_t>(name_offset));
      append_little_endian(m_buffer,
                           static_cast<std::uint32_t>(chunk.name.size()));
      name_offset += chunk.name.size();
      current_chunk_offset = align_archive_offset(
          current_chunk_offset + archive_chunk_header_size + chunk.data.size());
    }

    for (const auto &chunk : m_staged_chunks) {
      const auto *name_bytes =
          reinterpret_cast<const std::byte *>(chunk.name.data());
      m_buffer.insert(m_buffer.end(), name_bytes,
                      name_bytes + chunk.name.size());
    }

    const std::size_t first_chunk_offset =
        align_archive_offset(m_buffer.size());
    m_buffer.resize(first_chunk_offset, std::byte{});

    for (std::size_t i = 0; i < m_staged_chunks.size(); ++i) {
      if (m_buffer.size() != generated_entries[i].index.offset) {
        throw std::logic_error("Archive chunk offset calculation failed.");
      }

      const auto &chunk = m_staged_chunks[i];
      append_little_endian(m_buffer, std::uint32_t{0x43484E4B});
      append_little_endian(m_buffer, chunk.id);
      append_little_endian(m_buffer,
                           static_cast<std::uint64_t>(chunk.data.size()));
      m_buffer.insert(m_buffer.end(), chunk.data.begin(), chunk.data.end());

      if (i + 1 < m_staged_chunks.size()) {
        m_buffer.resize(align_archive_offset(m_buffer.size()), std::byte{});
      }
    }

    if (static_cast<std::uintmax_t>(m_buffer.size()) >
        static_cast<std::uintmax_t>(
            std::numeric_limits<std::streamsize>::max())) {
      throw std::length_error("Archive exceeds the supported file size.");
    }

    std::ofstream file(path, std::ios::binary);
    if (!file)
      throw std::runtime_error(
          "Failed to open archive path for execution writing.");
    file.write(reinterpret_cast<const char *>(m_buffer.data()),
               static_cast<std::streamsize>(m_buffer.size()));
    if (!file)
      throw std::runtime_error("Failed while writing archive file.");
    file.close();
    if (!file)
      throw std::runtime_error("Failed while closing archive file.");
  }

private:
  struct staged_chunk {
    std::uint32_t id;
    std::string name;
    std::vector<std::byte> data;
  };

  std::uint32_t add_named_chunk(std::string name, std::vector<std::byte> data) {
    validate_new_name(name);
    const auto id = allocate_chunk_id();
    add_staged_chunk(id, std::move(name), std::move(data));
    return id;
  }

  void validate_new_name(const std::string &name) const {
    if (name.empty() || name.find('\0') != std::string::npos) {
      throw std::invalid_argument(
          "Archive item names must be non-empty and contain no null bytes.");
    }
    if (std::ranges::any_of(m_staged_chunks, [&name](const auto &chunk) {
          return chunk.name == name;
        })) {
      throw std::invalid_argument(
          "Archive item names must be unique within an archive.");
    }
  }

  std::uint32_t allocate_chunk_id() {
    while (m_next_chunk_id <= std::numeric_limits<std::uint32_t>::max()) {
      const auto candidate = static_cast<std::uint32_t>(m_next_chunk_id++);
      const bool used =
          std::ranges::any_of(m_staged_chunks, [candidate](const auto &chunk) {
            return chunk.id == candidate;
          });
      if (!used) {
        return candidate;
      }
    }
    throw std::length_error("No chunk IDs remain in the supported range.");
  }

  void add_staged_chunk(std::uint32_t id, std::string name,
                        std::vector<std::byte> data) {
    if (std::ranges::any_of(m_staged_chunks, [id](const auto &chunk) {
          return chunk.id == id;
        })) {
      throw std::invalid_argument(
          "Chunk IDs must be unique within an archive.");
    }
    validate_new_name(name);
    m_staged_chunks.push_back(
        staged_chunk{id, std::move(name), std::move(data)});
  }

  declarative_binary_layout<Blueprint> m_layout;
  Container m_buffer;
  std::vector<staged_chunk> m_staged_chunks;
  std::uint64_t m_next_chunk_id = 0;
};

template <auto Blueprint,
          contiguous_byte_container Container = std::vector<std::byte>>
class archive_reader {
public:
  constexpr archive_reader() = default;
  constexpr explicit archive_reader(Container &&container)
      : m_buffer(std::move(container)) {}

  void load_from_file(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file)
      throw std::runtime_error("Failed to target path for extraction.");

    const auto end_position = file.tellg();
    if (end_position < std::streampos(0))
      throw std::runtime_error("Failed to determine archive file size.");

    const auto file_size_offset = end_position - std::streampos(0);
    if (static_cast<std::uintmax_t>(file_size_offset) >
            std::numeric_limits<std::size_t>::max() ||
        file_size_offset > std::numeric_limits<std::streamsize>::max()) {
      throw std::length_error("Archive exceeds the supported file size.");
    }

    const auto file_size = static_cast<std::size_t>(file_size_offset);

    file.seekg(0, std::ios::beg);
    if (!file)
      throw std::runtime_error("Failed to seek to the start of the archive.");

    m_buffer.resize(file_size);

    if (file_size != 0) {
      file.read(reinterpret_cast<char *>(m_buffer.data()),
                static_cast<std::streamsize>(file_size));
      if (!file || file.gcount() != static_cast<std::streamsize>(file_size))
        throw std::runtime_error("Failed while reading archive file.");
    }
  }

  [[nodiscard]] std::vector<archive_record> list_records() const {
    const std::span<const std::byte> full_view{m_buffer.data(),
                                               m_buffer.size()};
    auto records = m_layout.extract_index_table(full_view);
    if (!records) {
      throw std::runtime_error("Archive index is malformed or unsupported.");
    }
    return std::move(*records);
  }

  [[nodiscard]] archive_record get_record(std::string_view name) const {
    for (const auto &record : list_records()) {
      if (record.name == name) {
        return record;
      }
    }
    throw std::runtime_error("Archive item name was not found.");
  }

  [[nodiscard]] std::span<const std::byte>
  get_chunk(std::uint32_t chunk_id) const {
    const std::span<const std::byte> full_view{m_buffer.data(),
                                               m_buffer.size()};
    const auto chunk_bytes =
        m_layout.extract_chunk_payload(full_view, chunk_id);
    if (!chunk_bytes) {
      throw std::runtime_error(
          "Target chunk ID was not found or archive is malformed.");
    }
    return *chunk_bytes;
  }

  [[nodiscard]] std::span<const std::byte>
  get_chunk(std::string_view name) const {
    const auto record = get_record(name);
    return get_chunk(record.index.chunk_id);
  }

  template <typename ElementType, std::size_t... Extents,
            typename... DynamicExtents>
  [[nodiscard]] auto get_mdspan_view(std::uint32_t chunk_id,
                                     DynamicExtents... dynamic_extents) const {
    // Payload bytes are interpreted using the caller's native ElementType.
    static_assert(std::is_trivially_copyable_v<ElementType>,
                  "Archive elements must be trivially copyable.");
    using extents_type = std::extents<std::size_t, Extents...>;
    static_assert(sizeof...(DynamicExtents) == extents_type::rank_dynamic(),
                  "Provide one extent value for each dynamic dimension.");
    static_assert((std::integral<DynamicExtents> && ...),
                  "Dynamic extents must be integer values.");
    static_assert(
        (!std::same_as<std::remove_cv_t<DynamicExtents>, bool> && ...),
        "Dynamic extents cannot be bool values.");

    const auto validate_dynamic_extent = [](auto extent) {
      if (!std::in_range<std::size_t>(extent))
        throw std::invalid_argument("Dynamic extents must be non-negative.");
    };
    if constexpr (sizeof...(DynamicExtents) > 0) {
      (validate_dynamic_extent(dynamic_extents), ...);
    }

    const auto chunk_bytes = get_chunk(chunk_id);

    const extents_type extents(static_cast<std::size_t>(dynamic_extents)...);
    std::size_t element_count = 1;
    for (std::size_t dimension = 0; dimension < extents_type::rank();
         ++dimension) {
      const auto extent = extents.extent(dimension);
      if (extent != 0 &&
          element_count > std::numeric_limits<std::size_t>::max() / extent) {
        throw std::length_error("Requested mdspan shape is too large.");
      }
      element_count *= extent;
    }
    if (element_count >
        std::numeric_limits<std::size_t>::max() / sizeof(ElementType)) {
      throw std::length_error("Requested mdspan shape is too large.");
    }

    const std::size_t expected_size = element_count * sizeof(ElementType);
    if (expected_size != chunk_bytes.size()) {
      throw std::runtime_error(
          "Requested mdspan shape does not match the chunk size.");
    }

    const auto *typed_ptr =
        reinterpret_cast<const ElementType *>(chunk_bytes.data());
    if (reinterpret_cast<std::uintptr_t>(typed_ptr) % alignof(ElementType) !=
        0) {
      throw std::runtime_error(
          "Archive chunk is not aligned for the requested element type.");
    }

    return std::mdspan<const ElementType, extents_type>(typed_ptr, extents);
  }

  template <typename ElementType, std::size_t... Extents,
            typename... DynamicExtents>
  [[nodiscard]] auto get_mdspan_view(std::string_view name,
                                     DynamicExtents... dynamic_extents) const {
    return get_mdspan_view<ElementType, Extents...>(
        get_record(name).index.chunk_id, dynamic_extents...);
  }

private:
  declarative_binary_layout<Blueprint> m_layout;
  Container m_buffer;
};

// ============================================================================
// 5. DEFINED COMPILATION ENTRY POINT
// ============================================================================
inline constexpr declarative_blueprint<4> global_archive_format{
    .rules = {{{.type = block_type::magic_bytes, .fixed_size = 4},
               {.type = block_type::file_version, .fixed_size = 2},
               {.type = block_type::index_table}, // runtime sizing calculation
               {.type = block_type::payload_data}}}};