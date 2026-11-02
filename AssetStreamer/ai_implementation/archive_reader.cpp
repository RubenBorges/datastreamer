#include "archive_writer.hpp"

#include <chrono>
#include <iostream>

namespace {

void check(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string{"Test failed: "} +
                             std::string{message});
  }
}

template <typename Exception = std::exception, typename Function>
void check_throws(Function &&function, std::string_view message) {
  try {
    std::invoke(std::forward<Function>(function));
  } catch (const Exception &) {
    return;
  }

  throw std::runtime_error(std::string{"Expected exception was not thrown: "} +
                           std::string{message});
}

class temporary_directory {
public:
  temporary_directory() {
    const auto unique_suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    m_path = std::filesystem::temp_directory_path() /
             ("datastreamer-archive-demo-" + std::to_string(unique_suffix));
    std::filesystem::create_directories(m_path);
  }

  ~temporary_directory() {
    std::error_code error;
    std::filesystem::remove_all(m_path, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const {
    return m_path;
  }

private:
  std::filesystem::path m_path;
};

void write_text_file(const std::filesystem::path &path,
                     std::string_view contents) {
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    throw std::runtime_error("Could not create demonstration input file.");
  }

  file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  if (!file) {
    throw std::runtime_error("Could not write demonstration input file.");
  }
}

void check_bytes(std::span<const std::byte> actual,
                 std::span<const std::byte> expected,
                 std::string_view item_name) {
  check(actual.size() == expected.size(),
        std::string{item_name} + " has the expected byte count");
  check(std::ranges::equal(actual, expected),
        std::string{item_name} + " has the expected contents");
}

} // namespace

int main() {
  try {
    temporary_directory temp;
    const auto &directory = temp.path();
    const auto archive_path = directory / "demo-assets.pak";

    const auto first_file = directory / "first.txt";
    const auto second_file = directory / "second.txt";
    const auto third_file = directory / "third.txt";
    const auto fourth_file = directory / "fourth.txt";
    const auto fifth_file = directory / "fifth.txt";

    write_text_file(first_file, "file added from a path list");
    write_text_file(second_file, "file added from a filesystem path vector");
    write_text_file(third_file, "file added from a string path vector");
    write_text_file(fourth_file, "file added using add_file(path)");
    write_text_file(fifth_file, "file added using add_file(string)");

    std::array<std::filesystem::path, 1> path_span{first_file};
    std::vector<std::filesystem::path> filesystem_paths{second_file};
    std::vector<std::string> string_paths{third_file.string()};

    std::array<float, 4> matrix_data{1.1f, 2.2f, 3.3f, 4.4f};
    std::array<std::byte, 0> empty_data{};

    std::vector<std::byte> stream_data(70 * 1024);
    for (std::size_t i = 0; i < stream_data.size(); ++i) {
      stream_data[i] = static_cast<std::byte>(i % 251);
    }

    archive_writer<global_archive_format> writer{std::vector<std::byte>{}};

    writer.add_files(std::span<const std::filesystem::path>{path_span});
    writer.add_files(filesystem_paths);
    writer.add_files(string_paths);
    writer.add_file(fourth_file);
    writer.add_file(fifth_file.string());

    const auto matrix_id =
        writer.add_data("matrix.f32", std::as_bytes(std::span{matrix_data}));
    const auto empty_id =
        writer.add_data("empty.bin", std::span<const std::byte>{empty_data});

    std::size_t stream_offset = 0;
    std::size_t stream_read_calls = 0;
    const auto stream_id =
        writer.add_stream("stream.bin", [&](std::span<std::byte> destination) {
          ++stream_read_calls;
          const auto remaining = stream_data.size() - stream_offset;
          const auto bytes_to_copy = std::min(destination.size(), remaining);
          std::ranges::copy(std::span<const std::byte>{stream_data}.subspan(
                                stream_offset, bytes_to_copy),
                            destination.begin());
          stream_offset += bytes_to_copy;
          return bytes_to_copy;
        });

    check(stream_read_calls > 2,
          "stream input is read repeatedly in bounded-size buffers");

    const std::array<float, 2> legacy_values{8.0f, 9.0f};
    constexpr std::uint32_t legacy_id = 500;
    writer.add_chunk(legacy_id, std::as_bytes(std::span{legacy_values}));

    check_throws<std::invalid_argument>(
        [&] { writer.add_chunk(legacy_id, std::span<const std::byte>{}); },
        "duplicate numeric chunk IDs are rejected");
    check_throws<std::invalid_argument>(
        [&] { writer.add_data("matrix.f32", std::span<const std::byte>{}); },
        "duplicate item names are rejected");

    writer.finalize_and_write(archive_path);

    archive_reader<global_archive_format> reader;
    reader.load_from_file(archive_path);

    const auto records = reader.list_records();
    check(records.size() == 9, "all nine sources are listed in the archive");

    const std::array<std::string_view, 9> expected_names{
        "first.txt",  "second.txt", "third.txt",  "fourth.txt", "fifth.txt",
        "matrix.f32", "empty.bin",  "stream.bin", "chunk-500"};
    for (std::size_t i = 0; i < records.size(); ++i) {
      check(records[i].name == expected_names[i],
            "record names match their source names");
    }

    check(records[0].index.chunk_id == 0 &&
              records[5].index.chunk_id == matrix_id &&
              records[6].index.chunk_id == empty_id &&
              records[7].index.chunk_id == stream_id &&
              records[8].index.chunk_id == legacy_id,
          "generated and caller-supplied IDs are preserved");

    const auto matrix_record = reader.get_record("matrix.f32");
    check(matrix_record.index.chunk_id == matrix_id,
          "record lookup by name returns its generated ID");
    check_throws<std::runtime_error>(
        [&] { (void)reader.get_record("missing.bin"); },
        "missing record names are reported");

    const std::string first_file_contents = "file added from a path list";
    check_bytes(reader.get_chunk("first.txt"),
                std::as_bytes(std::span{first_file_contents}),
                "path-list item");

    const std::string second_file_contents =
        "file added from a filesystem path vector";
    check_bytes(reader.get_chunk(records[1].index.chunk_id),
                std::as_bytes(std::span{second_file_contents}),
                "filesystem-path item");

    const std::string third_file_contents =
        "file added from a string path vector";
    check_bytes(reader.get_chunk("third.txt"),
                std::as_bytes(std::span{third_file_contents}),
                "string-path item");

    const std::string fourth_file_contents = "file added using add_file(path)";
    check_bytes(reader.get_chunk("fourth.txt"),
                std::as_bytes(std::span{fourth_file_contents}),
                "single filesystem-path item");

    const std::string fifth_file_contents = "file added using add_file(string)";
    check_bytes(reader.get_chunk("fifth.txt"),
                std::as_bytes(std::span{fifth_file_contents}),
                "single string-path item");

    check_bytes(reader.get_chunk(stream_id), stream_data, "stream item");
    check(reader.get_chunk(empty_id).empty(),
          "an empty item remains distinguishable from a missing item");

    const auto matrix_by_id = reader.get_mdspan_view<float, 2, 2>(matrix_id);
    const auto matrix_by_name =
        reader.get_mdspan_view<float, 2, 2>("matrix.f32");
    const auto dynamic_view =
        reader.get_mdspan_view<float, std::dynamic_extent>(matrix_id, 4);
    check(matrix_by_id[0, 1] == 2.2f && matrix_by_name[1, 0] == 3.3f &&
              dynamic_view[3] == 4.4f,
          "fixed, named, and dynamic mdspan views return the expected values");

    check_throws<std::runtime_error>(
        [&] { (void)reader.get_mdspan_view<float, 3>(matrix_id); },
        "a shape that does not match the payload is rejected");
    check_throws<std::runtime_error>(
        [&] { (void)reader.get_chunk(std::uint32_t{9999}); },
        "missing numeric chunk IDs are reported");

    std::ifstream archive_file(archive_path, std::ios::binary | std::ios::ate);
    if (!archive_file) {
      throw std::runtime_error("Could not reopen the generated archive.");
    }
    const auto archive_size = static_cast<std::size_t>(archive_file.tellg());
    std::vector<std::byte> archive_bytes(archive_size);
    archive_file.seekg(0, std::ios::beg);
    archive_file.read(reinterpret_cast<char *>(archive_bytes.data()),
                      static_cast<std::streamsize>(archive_bytes.size()));
    if (!archive_file) {
      throw std::runtime_error("Could not read the generated archive.");
    }

    archive_reader<global_archive_format> reader_from_memory{
        std::move(archive_bytes)};
    check(reader_from_memory.list_records().size() == records.size(),
          "reader can also be constructed from an existing byte container");
    check_bytes(reader_from_memory.get_chunk("stream.bin"), stream_data,
                "memory-loaded stream item");

    archive_writer<global_archive_format> rollback_writer;
    const std::vector<std::filesystem::path> failing_batch{
        first_file, directory / "does-not-exist.txt"};
    check_throws<std::runtime_error>(
        [&] { rollback_writer.add_files(failing_batch); },
        "a failed file batch reports the source-file error");
    const auto after_rollback_id = rollback_writer.add_data(
        "after-rollback.bin", std::span<const std::byte>{});
    rollback_writer.finalize_and_write(directory / "rollback.pak");

    archive_reader<global_archive_format> rollback_reader;
    rollback_reader.load_from_file(directory / "rollback.pak");
    const auto rollback_records = rollback_reader.list_records();
    check(rollback_records.size() == 1 &&
              rollback_records.front().index.chunk_id == after_rollback_id,
          "failed file batches roll back their staged records and IDs");

    check_throws<std::runtime_error>(
        [&] { writer.add_file(directory / "missing-source.txt"); },
        "a missing source file reports an error");
    check_throws<std::runtime_error>(
        [&] {
          archive_writer<global_archive_format> bad_writer;
          bad_writer.finalize_and_write(directory / "missing" / "out.pak");
        },
        "an invalid archive output path reports an error");

    std::vector<std::byte> malformed_archive(10);
    malformed_archive[0] = std::byte{'P'};
    malformed_archive[1] = std::byte{'K'};
    malformed_archive[2] = std::byte{'A'};
    malformed_archive[3] = std::byte{'F'};
    malformed_archive[4] =
        static_cast<std::byte>(archive_format_version & 0xff);
    malformed_archive[5] = static_cast<std::byte>(archive_format_version >> 8);
    malformed_archive[6] = std::byte{0xff};
    malformed_archive[7] = std::byte{0xff};
    malformed_archive[8] = std::byte{0xff};
    malformed_archive[9] = std::byte{0xff};

    archive_reader<global_archive_format> malformed_reader{
        std::move(malformed_archive)};
    check_throws<std::runtime_error>(
        [&] { (void)malformed_reader.list_records(); },
        "malformed archive indexes are rejected");

    std::cout << "Archive demonstration and checks passed.\n"
              << "Added five files, an in-memory matrix, an empty item, a "
                 "stream, and a legacy numeric-ID chunk.\n"
              << "Listed and read all records, checked names and IDs, "
                 "validated mdspan access, and exercised error handling.\n"
              << "Example matrix value [0, 1]: " << matrix_by_id[0, 1] << '\n';
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
