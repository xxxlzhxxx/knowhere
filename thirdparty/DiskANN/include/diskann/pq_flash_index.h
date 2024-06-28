// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#pragma once
#include <cassert>
#include <condition_variable>
#include <future>
#include <mutex>
#include <optional>
#include <sstream>
#include <stack>
#include <string>
#include "common/lru_cache.h"
#include "tsl/robin_map.h"
#include "tsl/robin_set.h"

#include "knowhere/bitsetview.h"
#include "knowhere/feder/DiskANN.h"

#include "aligned_file_reader.h"
#include "concurrent_queue.h"
#include "neighbor.h"
#include "parameters.h"
#include "percentile_stats.h"
#include "pq_table.h"
#include "utils.h"
#include "diskann/distance.h"
#include "knowhere/comp/thread_pool.h"

#define MAX_GRAPH_DEGREE 512
#define SECTOR_LEN (_u64) 4096

#define FULL_PRECISION_REORDER_MULTIPLIER 3

namespace diskann {
  class ThreadSafeStateController {
   public:
    enum class Status {
      NONE,
      DOING,
      STOPPING,
      DONE,
      KILLED,
    };

    std::atomic<Status>     status;
    std::condition_variable cond;
    std::mutex              status_mtx;
  };
  template<typename T>
  struct QueryScratch {
    T *coord_scratch = nullptr;  // MUST BE AT LEAST [sizeof(T) * data_dim]

    char *sector_scratch =
        nullptr;          // MUST BE AT LEAST [MAX_N_SECTOR_READS * SECTOR_LEN]
    _u64 sector_idx = 0;  // index of next [SECTOR_LEN] scratch to use

    float *aligned_pqtable_dist_scratch =
        nullptr;  // MUST BE AT LEAST [256 * NCHUNKS]
    float *aligned_dist_scratch =
        nullptr;  // MUST BE AT LEAST diskann MAX_DEGREE
    _u8 *aligned_pq_coord_scratch =
        nullptr;  // MUST BE AT LEAST  [N_CHUNKS * MAX_DEGREE]
    T     *aligned_query_T = nullptr;
    float *aligned_query_float = nullptr;

    tsl::robin_set<_u64> *visited = nullptr;

    void reset() {
      sector_idx = 0;
      visited->clear();  // does not deallocate memory.
    }
  };

  template<typename T>
  struct ThreadData {
    QueryScratch<T> scratch;
  };

  template<typename T>
  class PQFlashIndex {
   public:
    PQFlashIndex(std::shared_ptr<AlignedFileReader> fileReader,
                 diskann::Metric metric = diskann::Metric::L2);
    ~PQFlashIndex();

    // load compressed data, and obtains the handle to the disk-resident index
    int load(uint32_t num_threads, const char *index_prefix);

    void load_cache_list(std::vector<uint32_t> &node_list);

    // asynchronously collect the access frequency of each node in the graph
    void async_generate_cache_list_from_sample_queries(std::string sample_bin,
                                                       _u64        l_search,
                                                       _u64        beamwidth,
                                                       _u64 num_nodes_to_cache);

    void cache_bfs_levels(_u64                   num_nodes_to_cache,
                          std::vector<uint32_t> &node_list);

    void cached_beam_search(
        const T *query, const _u64 k_search, const _u64 l_search, _s64 *res_ids,
        float *res_dists, const _u64 beam_width,
        const bool use_reorder_data = false, QueryStats *stats = nullptr,
        const knowhere::feder::diskann::FederResultUniq &feder = nullptr,
        knowhere::BitsetView                             bitset_view = nullptr,
        const float filter_ratio = -1.0f, const bool for_tuning = false);

    _u32 range_search(const T *query1, const double range,
                      const _u64 min_l_search, const _u64 max_l_search,
                      std::vector<_s64> &indices, std::vector<float> &distances,
                      const _u64           beam_width,
                      knowhere::BitsetView bitset_view = nullptr,
                      QueryStats          *stats = nullptr);

    void get_vector_by_ids(const int64_t *ids, const int64_t n,
                           T *const output_data);

    std::shared_ptr<AlignedFileReader> reader;

    _u64 get_num_points() const noexcept;

    _u64 get_data_dim() const noexcept;

    _u64 get_max_degree() const noexcept;

    _u32 *get_medoids() const noexcept;

    size_t get_num_medoids() const noexcept;

    diskann::Metric get_metric() const noexcept;

    void getIteratorNextBatch(IteratorWorkspace* workspace, size_t res_size, const knowhere::feder::hnsw::FederResultUniq& feder_result = nullptr) const;

    virtual std::unique_ptr<IteratorWorkspace> getIteratorWorkspace(const void*, const size_t, const bool, const knowhere::BitsetView&) const;

    _u64 cal_size();

    // for async cache making task
    void destroy_cache_async_task();

   protected:
    void use_medoids_data_as_centroids();
    void setup_thread_data(_u64 nthreads);
    void destroy_thread_data();
    _u64 get_thread_data_size();

   private:
    // sector # on disk where node_id is present with in the graph part
    _u64 get_node_sector_offset(_u64 node_id) {
      return long_node ? (node_id * nsectors_per_node + 1) * SECTOR_LEN
                       : (node_id / nnodes_per_sector + 1) * SECTOR_LEN;
    }

    // obtains region of sector containing node
    char *get_offset_to_node(char *sector_buf, _u64 node_id) {
      return long_node
                 ? sector_buf
                 : sector_buf + (node_id % nnodes_per_sector) * max_node_len;
    }

    inline void copy_vec_base_data(T *des, const int64_t des_idx, void *src);

    // Init thread data and returns query norm if avaialble.
    // If there is no value, there is nothing to do with the given query
    std::optional<float> init_thread_data(ThreadData<T> &data, const T *query1);

    // Brute force search for the given query. Use beam search rather than
    // sending whole bunch of requests at once to avoid all threads sending I/O
    // requests and the time overlaps.
    // The beam width is adjusted in the function.
    void brute_force_beam_search(
        ThreadData<T> &data, const float query_norm, const _u64 k_search,
        _s64 *indices, float *distances, const _u64 beam_width_param,
        IOContext &ctx, QueryStats *stats,
        const knowhere::feder::diskann::FederResultUniq &feder,
        knowhere::BitsetView                             bitset_view);

    // Assign the index of ids to its corresponding sector and if it is in
    // cache, write to the output_data
    std::unordered_map<_u64, std::vector<_u64>>
    get_sectors_layout_and_write_data_from_cache(const int64_t *ids, int64_t n,
                                                 T *output_data);

    // index info
    // nhood of node `i` is in sector: [i / nnodes_per_sector]
    // offset in sector: [(i % nnodes_per_sector) * max_node_len]
    // nnbrs of node `i`: *(unsigned*) (buf)
    // nbrs of node `i`: ((unsigned*)buf) + 1
    _u64 max_node_len = 0, nnodes_per_sector = 0, max_degree = 0;

    // Data used for searching with re-order vectors
    _u64 ndims_reorder_vecs = 0, reorder_data_start_sector = 0,
         nvecs_per_sector = 0;

    diskann::Metric metric = diskann::Metric::L2;

    // used only for inner product search to re-scale the result value
    // (due to the pre-processing of base during index build)
    float max_base_norm = 0.0f;

    // used only for cosine search to re-scale the caculated distance.
    std::unique_ptr<float[]> base_norms = nullptr;

    // data info
    bool long_node = false;
    _u64 nsectors_per_node = 0;
    _u64 read_len_for_node = SECTOR_LEN;
    _u64 num_points = 0;
    _u64 num_frozen_points = 0;
    _u64 frozen_location = 0;
    _u64 data_dim = 0;
    _u64 disk_data_dim = 0;  // will be different from data_dim only if we use
                             // PQ for disk data (very large dimensionality)
    _u64 aligned_dim = 0;
    _u64 disk_bytes_per_point = 0;

    std::string       disk_index_file;
    std::shared_mutex node_visit_counter_mtx;
    std::vector<std::pair<_u32, std::unique_ptr<std::atomic<_u32>>>>
                      node_visit_counter;
    std::atomic<_u32> search_counter = 0;

    std::shared_ptr<ThreadSafeStateController> state_controller =
        std::make_shared<ThreadSafeStateController>();

    // PQ data
    // n_chunks = # of chunks ndims is split into
    // data: _u8 * n_chunks
    // chunk_size = chunk size of each dimension chunk
    // pq_tables = float* [[2^8 * [chunk_size]] * n_chunks]
    std::unique_ptr<_u8[]> data = nullptr;
    _u64              n_chunks;
    FixedChunkPQTable pq_table;

    // distance comparator
    DISTFUN<T>     dist_cmp;
    DISTFUN<float> dist_cmp_float;

    float dist_cmp_wrap(const T *x, const T *y, size_t d, int32_t u) {
      if (metric == Metric::COSINE) {
        return dist_cmp(x, y, d) / base_norms[u];
      } else {
        return dist_cmp(x, y, d);
      }
    }

    float dist_cmp_float_wrap(const float *x, const float *y, size_t d,
                              int32_t u) {
      if (metric == Metric::COSINE) {
        return dist_cmp_float(x, y, d) / base_norms[u];
      } else {
        return dist_cmp_float(x, y, d);
      }
    }

    // for very large datasets: we use PQ even for the disk resident index
    bool              use_disk_index_pq = false;
    _u64              disk_pq_n_chunks = 0;
    FixedChunkPQTable disk_pq_table;

    // medoid/start info

    // graph has one entry point by default,
    // we can optionally have multiple starting points
    std::unique_ptr<uint32_t[]> medoids = nullptr;
    // defaults to 1
    size_t num_medoids;
    // by default, it is empty. If there are multiple
    // centroids, we pick the medoid corresponding to the
    // closest centroid as the starting point of search
    float *centroid_data = nullptr;

    // cache
    std::shared_mutex cache_mtx;

    // nhood_cache
    std::unique_ptr<unsigned[]> nhood_cache_buf = nullptr;
    tsl::robin_map<_u32, std::pair<_u32, _u32 *>>
        nhood_cache;  // <id, <neihbors_num, neihbors>>

    // coord_cache
    T                        *coord_cache_buf = nullptr;
    tsl::robin_map<_u32, T *> coord_cache;

    // thread-specific scratch
    ConcurrentQueue<ThreadData<T>> thread_data;
    _u64                           max_nthreads;
    bool                           load_flag = false;
    std::atomic<bool>              count_visited_nodes = false;
    bool                           reorder_data_exists = false;
    _u64                           reoreder_data_offset = 0;

    mutable knowhere::lru_cache<uint64_t, uint32_t> lru_cache;
  };

  typedef std::priority_queue<Neighbor, std::vector<Neighbor>, std::greater<Neighbor>> IteratorMinHeap;

  struct IteratorWorkspace {
    IteratorWorkspace(std::unique_ptr<int8_t[]> query_data_sq, const size_t num_elements, const size_t ef,
                      const bool for_tuning, std::unique_ptr<int8_t[]> raw_query_data,
                      const knowhere::BitsetView& bitset, float accumulative_alpha)
        : query_data(query_data_sq ? (const void*)(query_data_sq.get()) : (const void*)(raw_query_data.get())),
          query_data_sq(std::move(query_data_sq)),
          visited(num_elements),
          ef(ef),
          param(std::make_unique<SearchParam>()),
          raw_query_data(std::move(raw_query_data)),
          bitset(bitset),
          accumulative_alpha(accumulative_alpha) {
        param->ef_ = 0;
        param->for_tuning = for_tuning;
    }
    const void* query_data;

    // NEVER ACCESS THIS DIRECTLY! USE query_data instead.
    std::unique_ptr<int8_t[]> query_data_sq;

    bool initial_search_done = false;
    // TODO test for memory usage of this heap and add a metric monitoring it.
    IteratorMinHeap to_visit;
    // Since iterators do not occupy a thread during the entire lifecycle of an
    // iteration request, we cannot use the visited list in the shared visited list pool,
    // thus creating a new visited list for every new iteration request.
    std::vector<bool> visited;
    std::vector<knowhere::DistId> dists;
    const size_t ef;
    std::unique_ptr<SearchParam> param;
    // though named raw_query_vector, it is normalized for cosine metric. used
    // only for refinement when quantization is enabled.
    std::unique_ptr<int8_t[]> raw_query_data;
    const knowhere::BitsetView bitset;
    float accumulative_alpha;
  };



}  // namespace diskann
