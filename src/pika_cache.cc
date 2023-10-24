// Copyright (c) 2013-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include <thread>
#include <unordered_set>
#include "glog/logging.h"

#include "include/pika_cache.h"
#include "include/pika_server.h"
#include "include/pika_slot_command.h"
#include "cache/include/RedisCache.h"
#include "cache/include/RedisDefine.h"
#include "include/pika_cache_load_thread.h"
#include "include/pika_data_distribution.h"

extern PikaServer *g_pika_server;
#define EXTEND_CACHE_SIZE(N) (N * 12 / 10)
using Status = rocksdb::Status;

PikaCache::PikaCache(int cache_start_pos, int cache_items_per_key, std::shared_ptr<Slot> slot)
    : cache_status_(PIKA_CACHE_STATUS_NONE)
      , cache_num_(0)
      , cache_start_pos_(cache_start_pos)
      , cache_items_per_key_(EXTEND_CACHE_SIZE(cache_items_per_key))
      , cache_load_thread_(nullptr)
      , slot_(slot)
{
  std::unique_lock l(rwlock_);

  //cache_load_thread_ = new PikaCacheLoadThread(cache_start_pos_, cache_items_per_key_, std::shared_ptr<PikaCache> cache);
  cache_load_thread_->StartThread();
}

PikaCache::~PikaCache() {
  {
    std::unique_lock l(rwlock_);
    DestroyWithoutLock();
  }
}


Status PikaCache::Init(uint32_t cache_num, cache::CacheConfig *cache_cfg) {
  std::unique_lock l(rwlock_);
  return InitWithoutLock(cache_num, cache_cfg);
}

Status PikaCache::Reset(uint32_t cache_num, cache::CacheConfig *cache_cfg)
{
  std::unique_lock l(rwlock_);
  DestroyWithoutLock();
  return InitWithoutLock(cache_num, cache_cfg);
}

void PikaCache::ResetConfig(cache::CacheConfig *cache_cfg) {
  std::unique_lock l(rwlock_);
  cache_start_pos_ = cache_cfg->cache_start_pos;
  cache_items_per_key_ = EXTEND_CACHE_SIZE(cache_cfg->cache_items_per_key);
  LOG(WARNING) << "cache_start_pos: " << cache_start_pos_ << ", cache_items_per_key: " << cache_items_per_key_;
  cache::RedisCache::SetConfig(cache_cfg);
}

void PikaCache::Destroy(void) {
  std::unique_lock l(rwlock_);
  DestroyWithoutLock();
}

void PikaCache::ProcessCronTask(void)
{
  std::unique_lock l(rwlock_);
  for (uint32_t i = 0; i < caches_.size(); ++i) {
    std::unique_lock lm(*cache_mutexs_[i]);
    caches_[i]->ActiveExpireCycle();
  }
}

void PikaCache::SetCacheStatus(int status) { cache_status_ = status; }

int PikaCache::CacheStatus(void) { return cache_status_; }

/*-----------------------------------------------------------------------------
 * Normal Commands
 *----------------------------------------------------------------------------*/
void PikaCache::Info(CacheInfo &info)
{
  info.clear();
  std::unique_lock l(rwlock_);
  info.status = cache_status_;
  info.cache_num = cache_num_;
  info.used_memory = cache::RedisCache::GetUsedMemory();
  info.async_load_keys_num = cache_load_thread_->AsyncLoadKeysNum();
  info.waitting_load_keys_num = cache_load_thread_->WaittingLoadKeysNum();
  cache::RedisCache::GetHitAndMissNum(&info.hits, &info.misses);
  for (uint32_t i = 0; i < caches_.size(); ++i) {
    std::unique_lock lm(*cache_mutexs_[i]);
    info.keys_num += caches_[i]->DbSize();
  }
}


bool PikaCache::Exists(std::string &key)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Exists(key);
}

void PikaCache::FlushSlot(void)
{
  std::unique_lock l(rwlock_);
  for (uint32_t i = 0; i < caches_.size(); ++i) {
    std::unique_lock lm(*cache_mutexs_[i]);
    caches_[i]->FlushDb();
  }
}

void PikaCache::ActiveExpireCycle() {
  std::unique_lock l(rwlock_);
  for (uint32_t i = 0; i < caches_.size(); ++i) {
    std::unique_lock lm(*cache_mutexs_[i]);
    caches_[i]->ActiveExpireCycle();
  }
}


void PikaCache::ClearHitRatio(void)
{
  std::unique_lock l(rwlock_);
  cache::RedisCache::ResetHitAndMissNum();
}

Status PikaCache::Del(std::string &key)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Del(key);
}

Status PikaCache::Expire(std::string &key, int64_t ttl)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Expire(key, ttl);
}

Status PikaCache::Expireat(std::string &key, int64_t ttl)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Expireat(key, ttl);
}

Status PikaCache::Persist(std::string &key)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Persist(key);
}

Status PikaCache::Type(std::string &key, std::string *value)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Type(key, value);
}

Status PikaCache::RandomKey(std::string *key)
{
  std::unique_lock l(rwlock_);

  Status s;
  srand((unsigned)time(NULL));
  int cache_index = rand() % caches_.size();
  for (unsigned int i = 0; i < caches_.size(); ++i) {
    cache_index = (cache_index + i) % caches_.size();

    std::unique_lock lm(*cache_mutexs_[cache_index]);
    s = caches_[cache_index]->RandomKey(key);
    if (s.ok()) {
      break;
    }
  }
  return s;
}

Status PikaCache::TTL(std::string &key, int64_t *ttl)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->TTL(key, ttl);
}

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/
Status PikaCache::Set(std::string &key, std::string &value, int64_t ttl)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Set(key, value, ttl);
}

Status PikaCache::SetWithoutTTL(std::string &key, std::string &value) {
  std::unique_lock l(rwlock_);
  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->SetWithoutTTL(key, value);
}


Status PikaCache::Setnx(std::string &key, std::string &value, int64_t ttl)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Setnx(key, value, ttl);
}

Status PikaCache::SetnxWithoutTTL(std::string &key, std::string &value)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->SetnxWithoutTTL(key, value);
}

Status PikaCache::Setxx(std::string &key, std::string &value, int64_t ttl)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Setxx(key, value, ttl);
}

Status PikaCache::SetxxWithoutTTL(std::string &key, std::string &value)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->SetxxWithoutTTL(key, value);
}

Status PikaCache::Get(std::string &key, std::string *value)
{
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Get(key, value);
}

Status PikaCache::MGet(const std::vector<std::string> &keys, std::vector<storage::ValueStatus> *vss) {
  std::shared_lock l(rwlock_);
  vss->resize(keys.size());
  auto ret = Status::OK();
  for (int i = 0; i < keys.size(); ++i) {
    int cache_index = CacheIndex(keys[i]);
    std::unique_lock lm(*cache_mutexs_[cache_index]);
    auto s = caches_[cache_index]->Get(keys[i], &(*vss)[i].value);
    (*vss)[i].status = s;
    if (!s.ok()) {
      ret = s;
    }
  }
  return ret;
}

Status PikaCache::Incrxx(std::string &key)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->Incr(key);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::Decrxx(std::string &key)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->Decr(key);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::IncrByxx(std::string &key, long long incr)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->IncrBy(key, incr);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::DecrByxx(std::string &key, long long incr)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->DecrBy(key, incr);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::Incrbyfloatxx(std::string &key, long double incr)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->Incrbyfloat(key, incr);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::Appendxx(std::string &key, std::string &value)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->Append(key, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::GetRange(std::string &key, int64_t start, int64_t end, std::string *value)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->GetRange(key, start, end, value);
}

Status PikaCache::SetRangexx(std::string &key, int64_t start, std::string &value)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->SetRange(key, start, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::Strlen(std::string &key, int32_t *len)
{
  std::shared_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(*cache_mutexs_[cache_index]);
  return caches_[cache_index]->Strlen(key, len);
}

int PikaCache::CacheIndex(const std::string &key)
{
  uint32_t crc = CRC32Update(0, key.data(), (int)key.size());
  return (int)(crc % caches_.size());
}

Status PikaCache::WriteKvToCache(std::string &key, std::string &value, int64_t ttl)
{
  if (0 >= ttl) {
    if (PIKA_TTL_NONE == ttl) {
      return SetnxWithoutTTL(key, value);
    } else {
      return Del(key);
    }
  } else {
    return Setnx(key, value, ttl);
  }
  return Status::OK();
}

Status PikaCache::InitWithoutLock(uint32_t cache_num, cache::CacheConfig *cache_cfg)
{
  cache_status_ = PIKA_CACHE_STATUS_INIT;

  cache_num_ = cache_num;
  if (NULL != cache_cfg) {
    cache::RedisCache::SetConfig(cache_cfg);
  }

  for (uint32_t i = 0; i < cache_num; ++i) {
    cache::RedisCache *cache = new cache::RedisCache();
    Status s = cache->Open();
    if (!s.ok()) {
      LOG(ERROR) << "PikaCache::InitWithoutLock Open cache failed";
      DestroyWithoutLock();
      cache_status_ = PIKA_CACHE_STATUS_NONE;
      return Status::Corruption("create redis cache failed");
    }
    caches_.push_back(cache);
    cache_mutexs_.push_back(new pstd::Mutex());
  }
  cache_status_ = PIKA_CACHE_STATUS_OK;

  return Status::OK();
}

void PikaCache::DestroyWithoutLock(void)
{
  cache_status_ = PIKA_CACHE_STATUS_DESTROY;

  for (auto iter = caches_.begin(); iter != caches_.end(); ++iter) {
    delete *iter;
  }
  caches_.clear();

  for (auto iter = cache_mutexs_.begin(); iter != cache_mutexs_.end(); ++iter) {
    delete *iter;
  }
  cache_mutexs_.clear();
}
