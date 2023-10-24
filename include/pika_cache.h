// Copyright (c) 2013-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef PIKA_PIKA_CACHE_H
#define PIKA_PIKA_CACHE_H

#include <atomic>
#include <sstream>
#include <vector>

#include "include/pika_server.h"
#include "include/pika_define.h"
#include "include/pika_zset.h"
#include "pstd/include/pstd_mutex.h"
#include "pstd/include/pstd_status.h"
#include "cache/include/RedisCache.h"
#include "storage/storage.h"

enum RangeStatus : int {
  RangeError = 1,
  RangeHit,
  RangeMiss
};

class PikaCacheLoadThread;
class PikaCache : public pstd::noncopyable, public std::enable_shared_from_this<PikaCache> {
 public:
  struct CacheInfo {
    int status;
    uint32_t cache_num;
    long long keys_num;
    size_t used_memory;
    long long hits;
    long long misses;
    uint64_t async_load_keys_num;
    uint32_t waitting_load_keys_num;
    CacheInfo()
        : status(PIKA_CACHE_STATUS_NONE)
          , cache_num(0)
          , keys_num(0)
          , used_memory(0)
          , hits(0)
          , misses(0)
          , async_load_keys_num(0)
          , waitting_load_keys_num(0) {}
    void clear() {
      status = PIKA_CACHE_STATUS_NONE;
      cache_num = 0;
      keys_num = 0;
      used_memory = 0;
      hits = 0;
      misses = 0;
      async_load_keys_num = 0;
      waitting_load_keys_num = 0;
    }
  };

  PikaCache(int cache_start_pos_, int cache_items_per_key, std::shared_ptr<Slot> slot);
  ~PikaCache();

  rocksdb::Status Reset(uint32_t cache_num, cache::CacheConfig *cache_cfg = nullptr);
  rocksdb::Status Init(uint32_t cache_num, cache::CacheConfig *cache_cfg);
  void ResetConfig(cache::CacheConfig *cache_cfg);
  void Destroy(void);
  void ProcessCronTask(void);
  void SetCacheStatus(int status);
  int CacheStatus(void);

  // Normal Commands
  void Info(CacheInfo &info);
  long long DbSize(void);
  bool Exists(std::string &key);
  void FlushSlot(void);
  void ActiveExpireCycle();
  double HitRatio(void);
  void ClearHitRatio(void);
  rocksdb::Status Del(std::string &key);
  rocksdb::Status Expire(std::string &key, int64_t ttl);
  rocksdb::Status Expireat(std::string &key, int64_t ttl);
  rocksdb::Status TTL(std::string &key, int64_t *ttl);
  rocksdb::Status Persist(std::string &key);
  rocksdb::Status Type(std::string &key, std::string *value);
  rocksdb::Status RandomKey(std::string *key);

  // String Commands
  rocksdb::Status Set(std::string &key, std::string &value, int64_t ttl);
  rocksdb::Status SetWithoutTTL(std::string &key, std::string &value);
  rocksdb::Status Setnx(std::string &key, std::string &value, int64_t ttl);
  rocksdb::Status SetnxWithoutTTL(std::string &key, std::string &value);
  rocksdb::Status Setxx(std::string &key, std::string &value, int64_t ttl);
  rocksdb::Status SetxxWithoutTTL(std::string &key, std::string &value);
  rocksdb::Status Get(std::string &key, std::string *value);
  rocksdb::Status Incrxx(std::string &key);
  rocksdb::Status Decrxx(std::string &key);
  rocksdb::Status IncrByxx(std::string &key, long long incr);
  rocksdb::Status DecrByxx(std::string &key, long long incr);
  rocksdb::Status Incrbyfloatxx(std::string &key, long double incr);
  rocksdb::Status Appendxx(std::string &key, std::string &value);
  rocksdb::Status GetRange(std::string &key, int64_t start, int64_t end, std::string *value);
  rocksdb::Status SetRangexx(std::string &key, int64_t start, std::string &value);
  rocksdb::Status Strlen(std::string &key, int32_t *len);
  rocksdb::Status MGet(const std::vector<std::string>& keys, std::vector<storage::ValueStatus>* vss);
  rocksdb::Status MSet(const std::vector<storage::KeyValue>& kvs);


  // Cache
  rocksdb::Status WriteKvToCache(std::string &key, std::string &value, int64_t ttl);
  //  static bool CheckCacheDBScoreMembers(std::vector<storage::ScoreMember> &cache_score_members,
  //                                       std::vector<storage::ScoreMember> &db_score_members, bool print_result = true);

  std::shared_ptr<Slot> GetSlot() { return slot_; }
 private:
  rocksdb::Status InitWithoutLock(uint32_t cache_num, cache::CacheConfig *cache_cfg);
  void DestroyWithoutLock(void);
  int CacheIndex(const std::string &key);
  RangeStatus CheckCacheRange(int32_t cache_len, int32_t db_len, long start, long stop,
                              long& out_start, long& out_stop);
  RangeStatus CheckCacheRevRange(int32_t cache_len, int32_t db_len, long start, long stop,
                                 long& out_start, long& out_stop);
  RangeStatus CheckCacheRangeByScore(unsigned long cache_len, double cache_min, double cache_max, double min, double max, bool left_close, bool right_close);
  bool CacheSizeEqsDB(std::string &key);
  void GetMinMaxScore(std::vector<storage::ScoreMember> &score_members, double& min, double& max);
  bool GetCacheMinMaxSM(cache::RedisCache* cache_obj, std::string& key, storage::ScoreMember& min_m, storage::ScoreMember& max_m);
  bool ReloadCacheKeyIfNeeded(cache::RedisCache* cache_obj, std::string& key, int mem_len = -1, int db_len = -1);
  rocksdb::Status CleanCacheKeyIfNeeded(cache::RedisCache* cache_obj, std::string& key);

  PikaCache(const PikaCache&);
  PikaCache& operator=(const PikaCache&);

 private:
  std::vector<cache::RedisCache*> caches_;
  std::vector<pstd::Mutex*> cache_mutexs_;
  std::atomic<int> cache_status_;
  uint32_t cache_num_;
  std::shared_mutex rwlock_;

  // currently only take effects to zset
  int cache_start_pos_;
  int cache_items_per_key_;
  std::unique_ptr<PikaCacheLoadThread> cache_load_thread_;
  std::shared_ptr<Slot> slot_;
};
#endif  // PIKA_PIKA_CACHE_H
