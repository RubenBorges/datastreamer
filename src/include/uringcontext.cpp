// g++ -std=c++20 uringcontext.cpp -o my_program -luring
#include <liburing.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h>

struct FileContents {
    std::string path;
    std::vector<std::vector<char>> contents;
    std::error_code error;
};

class URingContext {
    struct FileReadRequest {
        explicit FileReadRequest(std::string file_path)
            : result{std::move(file_path), {}, {}}, buffer(4096) {
            fd = open(result.path.c_str(), O_RDONLY);
            if (fd < 0) {
                throw std::runtime_error(
                    "Could not open " + result.path + ": " +
                    std::strerror(errno));
            }
        }

        ~FileReadRequest() {
            if (fd >= 0) {
                close(fd);
            }
        }

        FileReadRequest(const FileReadRequest&) = delete;
        FileReadRequest& operator=(const FileReadRequest&) = delete;

        FileContents result;
        std::vector<char> buffer;
        int fd = -1;
        off_t offset = 0;
    };

    io_uring ring_{};
    std::unordered_map<FileReadRequest*, std::unique_ptr<FileReadRequest>>
        requests_;
    std::vector<FileContents> completed_;

    void submit_read(FileReadRequest& request) {
        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            throw std::runtime_error("io_uring submission queue is full");
        }

        io_uring_prep_read(sqe, request.fd, request.buffer.data(),
                           request.buffer.size(), request.offset);
        io_uring_sqe_set_data(sqe, &request);

        const int ret = io_uring_submit(&ring_);
        if (ret < 0) {
            throw std::runtime_error(
                std::string("io_uring_submit: ") + std::strerror(-ret));
        }
        if (ret == 0) {
            throw std::runtime_error("io_uring_submit submitted no requests");
        }
    }

public:
    explicit URingContext(unsigned int queue_depth) {
        const int ret = io_uring_queue_init(queue_depth, &ring_, 0);
        if (ret < 0) {
            throw std::runtime_error(
                std::string("io_uring_queue_init: ") + std::strerror(-ret));
        }
    }

    URingContext(const URingContext&) = delete;
    URingContext& operator=(const URingContext&) = delete;
    URingContext(URingContext&&) = delete;
    URingContext& operator=(URingContext&&) = delete;

    ~URingContext() {
        io_uring_queue_exit(&ring_);
    }

    void read_file(std::string path) {
        auto request = std::make_unique<FileReadRequest>(std::move(path));
        FileReadRequest* const request_ptr = request.get();
        requests_.emplace(request_ptr, std::move(request));

        try {
            submit_read(*request_ptr);
        } catch (...) {
            requests_.erase(request_ptr);
            throw;
        }
    }

    [[nodiscard]] bool has_pending_reads() const noexcept {
        return !requests_.empty();
    }

    void event_loop_once() {
        io_uring_cqe* cqe = nullptr;
        const int ret = io_uring_wait_cqe(&ring_, &cqe);
        if (ret < 0) {
            throw std::runtime_error(
                std::string("io_uring_wait_cqe: ") + std::strerror(-ret));
        }

        auto* request =
            static_cast<FileReadRequest*>(io_uring_cqe_get_data(cqe));
        const int result = cqe->res;
        io_uring_cqe_seen(&ring_, cqe);

        if (!request || !requests_.contains(request)) {
            throw std::runtime_error("Received completion for unknown request");
        }

        if (result < 0) {
            request->result.error =
                std::error_code(-result, std::generic_category());
            completed_.push_back(std::move(request->result));
            requests_.erase(request);
            return;
        }

        if (result == 0) {
            completed_.push_back(std::move(request->result));
            requests_.erase(request);
            return;
        }

        request->result.contents.emplace_back(
            request->buffer.begin(), request->buffer.begin() + result);
        request->offset += result;
        submit_read(*request);
    }

    [[nodiscard]] std::vector<FileContents> take_completed_files() {
        return std::exchange(completed_, {});
    }
};

int main() {
    try {
        URingContext ring(32);

        const std::vector<std::string> files{
            "test1.txt",
            "test2.txt",
            "test3.txt",
        };

        for (const auto& path : files) {
            try {
                ring.read_file(path);
            } catch (const std::exception& e) {
                std::cerr << e.what() << '\n';
            }
        }

        while (ring.has_pending_reads()) {
            ring.event_loop_once();
        }

        auto completed_files = ring.take_completed_files();
        for (const auto& file : completed_files) {
            if (file.error) {
                std::cerr << file.path << ": " << file.error.message() << '\n';
                continue;
            }

            std::cout << "\n--- " << file.path << " ---\n";
            for (const auto& chunk : file.contents) {
                std::cout.write(chunk.data(),
                                static_cast<std::streamsize>(chunk.size()));
            }
            std::cout << '\n';
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
