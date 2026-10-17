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

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace skel {

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

void stop_tracing(const std::string &filename) {
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

} // namespace skel
