/*
 * Copyright 2023 Intel Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//! [Includes]
// SVS Dependencies
#include "svs/core/recall.h"          // Convenient k-recall@n computation.
#include "svs/lib/threads.h"          // Thread pool-related dependencies.
#include "svs/orchestrators/vamana.h" // bulk of the dependencies required.

// Alternative main definition
#include "svsmain.h"

// stl
#include <future>
#include <map>
#include <queue>
#include <string>
#include <string_view>
#include <vector>
//! [Includes]

namespace {

//! [Helper Utilities]
template <typename T> struct MoC {
    MoC(T&& rhs)
        : obj(std::move(rhs)) {}
    MoC(const MoC& other)
        : obj(std::move(other.obj)) {}
    T& get() { return obj; }
    mutable T obj;
};
//! [Helper Utilities]

//! [Custom thread pool implementation]
class CustomThreadPool {
  public:
    explicit CustomThreadPool(size_t num_threads) {
        threads_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            threads_.emplace_back([this]() {
                while (!stop_) {
                    std::function<void()> task;
                    {
                        std::unique_lock lock(mtx_);
                        while (queue_.empty() && !stop_) {
                            cv_.wait(lock);
                        }
                        if (!queue_.empty()) {
                            task = queue_.front();
                            queue_.pop();
                        }
                    }

                    if (task) {
                        task();
                    }
                }
            });
        }
    }

    CustomThreadPool(CustomThreadPool&&) = delete;
    CustomThreadPool(const CustomThreadPool&) = delete;
    CustomThreadPool& operator=(CustomThreadPool&&) = delete;
    CustomThreadPool& operator=(const CustomThreadPool&) = delete;

    ~CustomThreadPool() {
        shutdown();
        for (auto& t : threads_) {
            t.join();
        }
    }

    template <typename C> std::future<void> insert(C&& task) {
        std::promise<void> prom;
        std::future<void> fu = prom.get_future();
        {
            std::scoped_lock lock(mtx_);
            queue_.push([moc = MoC{std::move(prom)},
                         task = std::forward<C>(task)]() mutable {
                task();
                moc.obj.set_value();
            });
        }
        cv_.notify_one();
        return fu;
    }

    size_t size() const { return threads_.size(); }

    void shutdown() {
        std::scoped_lock lock(mtx_);
        stop_ = true;
        cv_.notify_all();
    }

  private:
    std::vector<std::thread> threads_;
    std::mutex mtx_;
    std::condition_variable cv_;

    bool stop_{false};
    std::queue<std::function<void()>> queue_;
};

/////
///// The wrapper for CustomThreadPool to work on SVS
/////
class CustomThreadPoolWrapper {
  public:
    CustomThreadPoolWrapper(size_t num_threads)
        : threadpool_{std::make_unique<CustomThreadPool>(num_threads)} {}

    void parallel_for(std::function<void(size_t)> f, size_t n) {
        std::vector<std::future<void>> futures;
        futures.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            futures.emplace_back(threadpool_->insert([&f, i]() { f(i); }));
        }

        // wait until all tasks are finished
        for (auto& fu : futures) {
            fu.get();
        }
    }

    size_t size() const { return threadpool_->size(); }

  private:
    std::unique_ptr<CustomThreadPool> threadpool_;
};
static_assert(svs::threads::ThreadPool<CustomThreadPoolWrapper>);
//! [Custom thread pool implementation]
} // namespace

//! [Helper Utilities]
double run_recall(
    svs::Vamana& index,
    const svs::data::SimpleData<float>& queries,
    const svs::data::SimpleData<uint32_t>& groundtruth,
    size_t search_window_size,
    size_t num_neighbors,
    std::string_view message = ""
) {
    index.set_search_window_size(search_window_size);
    auto results = index.search(queries, num_neighbors);
    double recall = svs::k_recall_at_n(groundtruth, results, num_neighbors, num_neighbors);
    if (!message.empty()) {
        fmt::print("[{}] ", message);
    }
    fmt::print("Windowsize = {}, Recall = {}\n", search_window_size, recall);
    return recall;
}

const bool DEBUG = false;
void check(double expected, double got, double eps = 0.001) {
    double diff = std::abs(expected - got);
    if constexpr (DEBUG) {
        fmt::print("Expected {}. Got {}\n", expected, got);
    } else {
        if (diff > eps) {
            throw ANNEXCEPTION("Expected ", expected, ". Got ", got, '!');
        }
    }
}
//! [Helper Utilities]

// Alternative main definition
int svs_main(std::vector<std::string> args) {
    //! [Argument Extraction]
    const size_t nargs = args.size();
    if (nargs != 4) {
        throw ANNEXCEPTION("Expected 3 arguments. Instead, got ", nargs, '!');
    }
    const std::string& data_vecs = args.at(1);
    const std::string& query_vecs = args.at(2);
    const std::string& groundtruth_vecs = args.at(3);
    //! [Argument Extraction]

    // Building the index

    //! [Build Parameters]
    auto parameters = svs::index::vamana::VamanaBuildParameters{
        1.2,  // alpha
        64,   // graph max degree
        128,  // search window size
        1024, // max candidate pool size
        60,   // prune to degree
        true, // full search history
    };
    //! [Build Parameters]

    //! [Index Build]
    size_t num_threads = 4;
    svs::Vamana index = svs::Vamana::build<float>(
        parameters,
        svs::VectorDataLoader<float>(data_vecs),
        svs::DistanceL2(),
        CustomThreadPoolWrapper(num_threads)
    );
    //! [Index Build]

    // Searching the index

    //! [Load Aux]
    // Load the queries and ground truth.
    auto queries = svs::load_data<float>(query_vecs);
    auto groundtruth = svs::load_data<uint32_t>(groundtruth_vecs);
    //! [Load Aux]

    //! [Perform Queries]
    index.set_search_window_size(30);
    svs::QueryResult<size_t> results = index.search(queries, 10);
    double recall = svs::k_recall_at_n(groundtruth, results);
    check(0.8215, recall);
    //! [Perform Queries]

    //! [Search Window Size]
    auto expected_recall =
        std::map<size_t, double>({{10, 0.5509}, {20, 0.7281}, {30, 0.8215}, {40, 0.8788}});
    for (auto windowsize : {10, 20, 30, 40}) {
        recall = run_recall(index, queries, groundtruth, windowsize, 10, "Sweep");
        check(expected_recall.at(windowsize), recall);
    }
    //! [Search Window Size]

    // Saving the index

    //! [Saving]
    index.save("example_config", "example_graph", "example_data");
    //! [Saving]

    // Reloading a saved index

    //! [Loading]
    // We can reload an index from a previously saved set of files.
    index = svs::Vamana::assemble<float>(
        "example_config",
        svs::GraphLoader("example_graph"),
        svs::VectorDataLoader<float>("example_data"),
        svs::DistanceType::L2,
        4 // num_threads
    );

    recall = run_recall(index, queries, groundtruth, 30, 10, "Reload");
    check(0.8215, recall);
    //! [Loading]

    //! [Only Loading]
    // We can reload an index from a previously saved set of files.
    index = svs::Vamana::assemble<float>(
        "example_config",
        svs::GraphLoader("example_graph"),
        svs::VectorDataLoader<float>("example_data"),
        svs::DistanceType::L2,
        4 // num_threads
    );
    //! [Only Loading]

    //! [Set a new thread pool]
    index.set_threadpool(CustomThreadPoolWrapper(4));
    //! [Set a new thread pool]

    return 0;
}

// Special main providing some helpful utilities.
SVS_DEFINE_MAIN();
//! [Example All]
