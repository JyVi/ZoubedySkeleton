#include "skeletonize/perfetto_tracing.hpp"
#include <fstream>
#include <iostream>
#include <perfetto.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <chrono>
#include <atomic>
#include <future>
#include <vector>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace skel {
    class TraceWriter {
    public:
        explicit TraceWriter(const std::string& filename,
                             size_t max_queued_bytes = 64 * 1024 * 1024)
            : max_queued_bytes_(max_queued_bytes) {
            fd_ = ::open(filename.c_str(),
                         O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                         0644);

            if (fd_ < 0)
                throw std::runtime_error(
                    std::string("open failed: ") + std::strerror(errno));

            worker_ = std::thread(&TraceWriter::run, this);
        }

        ~TraceWriter() {
            finish();
            if (fd_ >= 0)
                ::close(fd_);
        }

        // Copies data because Perfetto owns the callback memory.
        void enqueue(const char* data, size_t size) {
            if (size == 0)
                return;

            std::vector<char> chunk(data, data + size);

            std::unique_lock<std::mutex> lock(mutex_);

            not_full_.wait(lock, [&] {
                return error_ || finished_ ||
                       queued_bytes_ + size <= max_queued_bytes_;
            });

            if (error_)
                throw std::runtime_error(error_message_);
            if (finished_)
                throw std::runtime_error("Writer already finished");

            queued_bytes_ += size;
            queue_.push_back(std::move(chunk));

            lock.unlock();
            not_empty_.notify_one();
        }

        void finish() {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                finished_ = true;
            }

            not_empty_.notify_all();
            not_full_.notify_all();

            if (worker_.joinable())
                worker_.join();

            if (error_)
                throw std::runtime_error(error_message_);
        }

    private:
        void run() {
            constexpr size_t BATCH_SIZE = 64 * 1024 * 1024; // 16 MiB

            std::vector<char> batch;
            batch.reserve(BATCH_SIZE);

            auto flush_batch = [&]() -> bool {
                size_t offset = 0;

                while (offset < batch.size()) {
                    ssize_t n = ::write(
                        fd_,
                        batch.data() + offset,
                        batch.size() - offset
                    );

                    if (n < 0 && errno == EINTR)
                        continue;

                    if (n <= 0) {
                        std::lock_guard<std::mutex> lock(mutex_);
                        error_ = true;
                        error_message_ =
                            std::string("write failed: ") + std::strerror(errno);
                        finished_ = true;
                        not_full_.notify_all();
                        not_empty_.notify_all();
                        return false;
                    }

                    offset += static_cast<size_t>(n);
                    bytes_written_ += static_cast<size_t>(n);
                }

                batch.clear(); // Retains allocated capacity.
                return true;
            };

            for (;;) {
                std::vector<char> chunk;

                {
                    std::unique_lock<std::mutex> lock(mutex_);

                    not_empty_.wait(lock, [&] {
                        return !queue_.empty() || finished_;
                    });

                    if (queue_.empty() && finished_)
                        break;

                    chunk = std::move(queue_.front());
                    queue_.pop_front();
                    queued_bytes_ -= chunk.size();
                }

                not_full_.notify_one();

                size_t offset = 0;

                while (offset < chunk.size()) {
                    const size_t space = BATCH_SIZE - batch.size();
                    const size_t count =
                        std::min(space, chunk.size() - offset);

                    batch.insert(
                        batch.end(),
                        chunk.data() + offset,
                        chunk.data() + offset + count
                    );

                    offset += count;

                    if (batch.size() == BATCH_SIZE && !flush_batch())
                        return;
                }
            }

            // Write the final partial batch.
            if (!batch.empty())
                flush_batch();
        }

    public:
        uint64_t bytes_written() const {
            return bytes_written_;
        }

    private:
        int fd_ = -1;
        const size_t max_queued_bytes_;

        std::mutex mutex_;
        std::condition_variable not_empty_, not_full_;
        std::deque<std::vector<char>> queue_;
        size_t queued_bytes_ = 0;
        bool finished_ = false;
        bool error_ = false;
        std::string error_message_;
        std::thread worker_;
        std::atomic<uint64_t> bytes_written_{0};
    };

// Global tracing session
static std::unique_ptr<perfetto::TracingSession> g_tracing_session;

void init_tracing() {
  // Prevent double initialization
  static bool initialized = false;
  if (initialized)
    return;

  perfetto::TracingInitArgs args;
  args.backends |= perfetto::kInProcessBackend;
  perfetto::Tracing::Initialize(args);
  perfetto::TrackEvent::Register();
  initialized = true;
}

void start_tracing() {
  if (g_tracing_session) {
    std::cerr << "[Perfetto] Tracing is already active!\n";
    return;
  }

  perfetto::TraceConfig cfg;

  auto* buf_cfg = cfg.add_buffers();

  // 2. Set the size (2 GB)
  //buf_cfg->set_size_kb(1024 * 2048);
  buf_cfg->set_size_kb(1024 * 1);

  // 3. Set the fill policy ON THE BUFFER CONFIG, NOT on 'cfg'
  buf_cfg->set_fill_policy(perfetto::protos::gen::TraceConfig_BufferConfig_FillPolicy::TraceConfig_BufferConfig_FillPolicy_RING_BUFFER);

  auto *ds_cfg = cfg.add_data_sources()->mutable_config();
  ds_cfg->set_name("track_event");

  g_tracing_session = perfetto::Tracing::NewTrace();
  g_tracing_session->Setup(cfg);
  g_tracing_session->StartBlocking();

  std::cout << "[Perfetto] Tracing started.\n";
}

void stop_tracing_old(const std::string &filename) {
  if (!g_tracing_session) {
    std::cerr << "[Perfetto] No active tracing session to stop.\n";
    return;
  }

  std::cout << "[Perfetto] Stopping tracing and flushing to disk...\n";
  g_tracing_session->StopBlocking();

  std::vector<char> trace_data = g_tracing_session->ReadTraceBlocking();

  std::ofstream file(filename, std::ios::binary);
  if (file.is_open()) {
    file.write(trace_data.data(), trace_data.size());
    std::cout << "[Perfetto] Successfully wrote " << trace_data.size()
              << " bytes to " << filename << "\n";
  } else {
    std::cerr << "[Perfetto] Failed to open file for writing!\n";
  }

  g_tracing_session.reset();
}

void stop_tracing_slow(const std::string &filename) {
    auto start = std::chrono::steady_clock::now();

    if (!g_tracing_session) {
        std::cerr << "[Perfetto] No active tracing session to stop.\n";
        return;
    }

    std::cout << "[Perfetto] Stopping tracing...\n";
    // StopBlocking automatically flushes all in-flight data to the buffers
    g_tracing_session->StopBlocking();

    std::cout << "[Perfetto] Streaming trace to disk asynchronously...\n";
    std::ofstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "[Perfetto] Failed to open file for writing: " << filename << "\n";
        g_tracing_session.reset();
        return;
    }

    // 32MB internal buffer for the ofstream to minimize disk syscalls
    std::vector<char> file_buffer(32 * 1024 * 1024);
    file.rdbuf()->pubsetbuf(file_buffer.data(), file_buffer.size());

    std::atomic<size_t> total_bytes{0};

    // Use promise/future for efficient, zero-CPU waiting
    std::promise<void> read_done_promise;
    std::future<void> read_done_future = read_done_promise.get_future();

    // A SINGLE call to ReadTrace is all you need.
    // Perfetto will automatically invoke this callback multiple times
    // until args.has_more is false.
    g_tracing_session->ReadTrace([&](perfetto::TracingSession::ReadTraceCallbackArgs args) {
        if (args.size > 0) {
            // Write the chunk directly to disk. No intermediate std::vector allocation!
            file.write(args.data, args.size);
            total_bytes += args.size;
        }

        // The service guarantees all data is sent when has_more is false.
        if (!args.has_more) {
            read_done_promise.set_value(); // Signal completion to the main thread
        }
    });

    // Wait efficiently for the async read to fully complete (0% CPU usage)
    read_done_future.wait();

    file.flush();
    std::cout << "[Perfetto] Successfully streamed " << total_bytes.load()
              << " bytes to " << filename << "\n";

    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "Execution time: " << duration_ms.count() << " ms\n";

    // Safe to reset now, as we know the callback has completely finished
    g_tracing_session.reset();
}

void stop_tracing(const std::string& filename) {
    if (!g_tracing_session) {
        std::cerr << "No active tracing session\n";
        return;
    }

    auto start = std::chrono::steady_clock::now();

    g_tracing_session->StopBlocking();

    try {
        TraceWriter writer(filename, 64 * 1024 * 1024);

        std::promise<void> done;
        auto future = done.get_future();

        g_tracing_session->ReadTrace(
            [&](perfetto::TracingSession::ReadTraceCallbackArgs args) {
                try {
                    if (args.size)
                        writer.enqueue(args.data, args.size);

                    if (!args.has_more)
                        done.set_value();
                } catch (...) {
                    // Preserve the exception and signal the waiting thread.
                    try {
                        done.set_exception(std::current_exception());
                    } catch (...) {}
                }
            });

        future.get();  // Wait for Perfetto to finish extraction.
        writer.finish(); // Drain all queued data and join the writer.

        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - start).count();

        std::cout << "Wrote " << writer.bytes_written()
                  << " bytes in " << elapsed << " s\n";

    } catch (const std::exception& e) {
        std::cerr << "Trace export failed: " << e.what() << '\n';
    }

    g_tracing_session.reset();
}
} // namespace skel
