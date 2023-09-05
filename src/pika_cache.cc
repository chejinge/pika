#include <glog/logging.h>
#include <ctime>
#include <unordered_set>

#include "include/pika_cache.h"
#include "include/pika_cache_load_thread.h"
#include "include/pika_server.h"
#include "include/pika_slot_command.h"
#include "redis_layer/dory/include/RedisCache.h"
#include "redis_layer/dory/include/RedisDef.h"

extern PikaServer *g_pika_server;
#define EXTEND_CACHE_SIZE(N) (N * 12 / 10)

PikaCache::PikaCache(int cache_start_pos, int cache_items_per_key)
    : cache_status_(PIKA_CACHE_STATUS_NONE),
      cache_num_(0),
      cache_start_pos_(cache_start_pos),
      cache_items_per_key_(EXTEND_CACHE_SIZE(cache_items_per_key)),
      cache_load_thread_(nullptr) {
  pthread_rwlock_init(&rwlock_, nullptr);

  cache_load_thread_ = new PikaCacheLoadThread(cache_start_pos_, cache_items_per_key_);
  cache_load_thread_->StartThread();
}

PikaCache::~PikaCache() {
  delete cache_load_thread_;

  {
    std::unique_lock l(rwlock_);
    DestroyWithoutLock();
  }
  std::unique_lock l(rwlock_);
}

Status PikaCache::Init(uint32_t cache_num, dory::CacheConfig *cache_cfg) {
  std::unique_lock l(rwlock_);

  if (nullptr == cache_cfg) {
    return Status::Corruption("invalid arguments !!!");
  }
  return InitWithoutLock(cache_num, cache_cfg);
}

Status PikaCache::Reset(uint32_t cache_num, dory::CacheConfig *cache_cfg) {
  std::unique_lock l(rwlock_);

  DestroyWithoutLock();
  return InitWithoutLock(cache_num, cache_cfg);
}

void PikaCache::ResetConfig(dory::CacheConfig *cache_cfg) {
  std::unique_lock l(rwlock_);
  cache_start_pos_ = cache_cfg->cache_start_pos;
  cache_items_per_key_ = EXTEND_CACHE_SIZE(cache_cfg->cache_items_per_key);
  LOG(WARNING) << "cache_start_pos: " << cache_start_pos_ << ", cache_items_per_key: " << cache_items_per_key_;
  dory::RedisCache::SetConfig(cache_cfg);
}

void PikaCache::Destroy(void) {
  std::unique_lock l(rwlock_);
  DestroyWithoutLock();
}

void PikaCache::ProcessCronTask(void) {
  std::unique_lock l(rwlock_);
  for (uint32_t i = 0; i < caches_.size(); ++i) {
    std::unique_lock lm(cache_mutexs_);
    caches_[i]->ActiveExpireCycle();
  }
}

void PikaCache::SetCacheStatus(int status) { cache_status_ = status; }

int PikaCache::CacheStatus(void) { return cache_status_; }

/*-----------------------------------------------------------------------------
 * Normal Commands
 *----------------------------------------------------------------------------*/
void PikaCache::Info(CacheInfo &info) {
  info.clear();
  std::unique_lock l(rwlock_);
  info.status = cache_status_;
  info.cache_num = cache_num_;
  info.used_memory = dory::RedisCache::GetUsedMemory();
  info.async_load_keys_num = cache_load_thread_->AsyncLoadKeysNum();
  info.waitting_load_keys_num = cache_load_thread_->WaittingLoadKeysNum();
  dory::RedisCache::GetHitAndMissNum(&info.hits, &info.misses);
  for (uint32_t i = 0; i < caches_.size(); ++i) {
    std::unique_lock lm(cache_mutexs_);
    info.keys_num += caches_[i]->DbSize();
  }
}

bool PikaCache::Exists(std::string &key) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(cache_mutexs_);
  return caches_[cache_index]->Exists(key);
}

void PikaCache::FlushDb(void) {
  std::unique_lock l(rwlock_);
  for (uint32_t i = 0; i < caches_.size(); ++i) {
    std::unique_lock lm(cache_mutexs_);
    caches_[i]->FlushDb();
  }
}

double PikaCache::HitRatio(void) {
  std::unique_lock l(rwlock_);

  long long hits = 0;
  long long misses = 0;
  dory::RedisCache::GetHitAndMissNum(&hits, &misses);
  long long all_cmds = hits + misses;
  if (0 >= all_cmds) {
    return 0;
  }

  return hits / (all_cmds * 1.0);
}

void PikaCache::ClearHitRatio(void) {
  std::unique_lock l(rwlock_);
  dory::RedisCache::ResetHitAndMissNum();
}

Status PikaCache::Del(std::string &key) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(cache_mutexs_);
  return caches_[cache_index]->Del(key);
}

Status PikaCache::Expire(std::string &key, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Expire(key, ttl);
}

Status PikaCache::Expireat(std::string &key, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Expireat(key, ttl);
}

Status PikaCache::TTL(std::string &key, int64_t *ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->TTL(key, ttl);
}

Status PikaCache::Persist(std::string &key) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Persist(key);
}

Status PikaCache::Type(std::string &key, std::string *value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Type(key, value);
}

Status PikaCache::RandomKey(std::string *key) {
  std::unique_lock l(rwlock_);

  Status s;
  srand((unsigned)time(nullptr));
  int cache_index = rand() % caches_.size();
  for (unsigned int i = 0; i < caches_.size(); ++i) {
    cache_index = (cache_index + i) % caches_.size();

    pstd::MutexLock lm(cache_mutexs_[cache_index]);
    s = caches_[cache_index]->RandomKey(key);
    if (s.ok()) {
      break;
    }
  }
  return s;
}

/*-----------------------------------------------------------------------------
 * String Commands
 *----------------------------------------------------------------------------*/
Status PikaCache::Set(std::string &key, std::string &value, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Set(key, value, ttl);
}

Status PikaCache::Setnx(std::string &key, std::string &value, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Setnx(key, value, ttl);
}

Status PikaCache::SetnxWithoutTTL(std::string &key, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SetnxWithoutTTL(key, value);
}

Status PikaCache::Setxx(std::string &key, std::string &value, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Setxx(key, value, ttl);
}

Status PikaCache::SetxxWithoutTTL(std::string &key, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  pstd::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SetxxWithoutTTL(key, value);
}

Status PikaCache::Get(std::string &key, std::string *value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Get(key, value);
}

Status PikaCache::Incrxx(std::string &key) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->Incr(key);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::Decrxx(std::string &key) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->Decr(key);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::IncrByxx(std::string &key, long long incr) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->IncrBy(key, incr);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::DecrByxx(std::string &key, long long incr) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->DecrBy(key, incr);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::Incrbyfloatxx(std::string &key, long double incr) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->Incrbyfloat(key, incr);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::Appendxx(std::string &key, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->Append(key, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::GetRange(std::string &key, int64_t start, int64_t end, std::string *value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->GetRange(key, start, end, value);
}

Status PikaCache::SetRangexx(std::string &key, int64_t start, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->SetRange(key, start, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::Strlen(std::string &key, int32_t *len) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->Strlen(key, len);
}

/*-----------------------------------------------------------------------------
 * Hash Commands
 *----------------------------------------------------------------------------*/
Status PikaCache::HDel(std::string &key, std::vector<std::string> &fields) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HDel(key, fields);
}

Status PikaCache::HSet(std::string &key, std::string &field, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HSet(key, field, value);
}

Status PikaCache::HSetIfKeyExist(std::string &key, std::string &field, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->HSet(key, field, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::HSetIfKeyExistAndFieldNotExist(std::string &key, std::string &field, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->HSetnx(key, field, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::HMSet(std::string &key, std::vector<storage::FieldValue> &fvs) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HMSet(key, fvs);
}

Status PikaCache::HMSetnx(std::string &key, std::vector<storage::FieldValue> &fvs, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (!caches_[cache_index]->Exists(key)) {
    caches_[cache_index]->HMSet(key, fvs);
    caches_[cache_index]->Expire(key, ttl);
    return Status::OK();
  } else {
    return Status::NotFound("key exist");
  }
}

Status PikaCache::HMSetnxWithoutTTL(std::string &key, std::vector<storage::FieldValue> &fvs) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (!caches_[cache_index]->Exists(key)) {
    caches_[cache_index]->HMSet(key, fvs);
    return Status::OK();
  } else {
    return Status::NotFound("key exist");
  }
}

Status PikaCache::HMSetxx(std::string &key, std::vector<storage::FieldValue> &fvs) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->HMSet(key, fvs);
  } else {
    return Status::NotFound("key not exist");
  }
}

Status PikaCache::HGet(std::string &key, std::string &field, std::string *value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HGet(key, field, value);
}

Status PikaCache::HMGet(std::string &key, std::vector<std::string> &fields, std::vector<storage::ValueStatus> *vss) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HMGet(key, fields, vss);
}

Status PikaCache::HGetall(std::string &key, std::vector<storage::FieldValue> *fvs) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HGetall(key, fvs);
}

Status PikaCache::HKeys(std::string &key, std::vector<std::string> *fields) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HKeys(key, fields);
}

Status PikaCache::HVals(std::string &key, std::vector<std::string> *values) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HVals(key, values);
}

Status PikaCache::HExists(std::string &key, std::string &field) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  std::lock_guard return caches_[cache_index]->HExists(key, field);
}

Status PikaCache::HIncrbyxx(std::string &key, std::string &field, int64_t value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->HIncrby(key, field, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::HIncrbyfloatxx(std::string &key, std::string &field, long double value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->HIncrbyfloat(key, field, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::HLen(std::string &key, unsigned long *len) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HLen(key, len);
}

Status PikaCache::HStrlen(std::string &key, std::string &field, unsigned long *len) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->HStrlen(key, field, len);
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/
Status PikaCache::LIndex(std::string &key, long index, std::string *element) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LIndex(key, index, element);
}

Status PikaCache::LInsert(std::string &key, storage::BeforeOrAfter &before_or_after, std::string &pivot,
                          std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LInsert(key, before_or_after, pivot, value);
}

Status PikaCache::LLen(std::string &key, unsigned long *len) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LLen(key, len);
}

Status PikaCache::LPop(std::string &key, std::string *element) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LPop(key, element);
}

Status PikaCache::LPush(std::string &key, std::vector<std::string> &values) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LPush(key, values);
}

Status PikaCache::LPushx(std::string &key, std::vector<std::string> &values) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LPushx(key, values);
}

Status PikaCache::LRange(std::string &key, long start, long stop, std::vector<std::string> *values) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LRange(key, start, stop, values);
}

Status PikaCache::LRem(std::string &key, long count, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LRem(key, count, value);
}

Status PikaCache::LSet(std::string &key, long index, std::string &value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LSet(key, index, value);
}

Status PikaCache::LTrim(std::string &key, long start, long stop) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->LTrim(key, start, stop);
}

Status PikaCache::RPop(std::string &key, std::string *element) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->RPop(key, element);
}

Status PikaCache::RPush(std::string &key, std::vector<std::string> &values) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->RPush(key, values);
}

Status PikaCache::RPushx(std::string &key, std::vector<std::string> &values) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->RPushx(key, values);
}

Status PikaCache::RPushnx(std::string &key, std::vector<std::string> &values, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (!caches_[cache_index]->Exists(key)) {
    caches_[cache_index]->RPush(key, values);
    caches_[cache_index]->Expire(key, ttl);
    return Status::OK();
  } else {
    return Status::NotFound("key exist");
  }
}

Status PikaCache::RPushnxWithoutTTL(std::string &key, std::vector<std::string> &values) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (!caches_[cache_index]->Exists(key)) {
    caches_[cache_index]->RPush(key, values);
    return Status::OK();
  } else {
    return Status::NotFound("key exist");
  }
}

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/
Status PikaCache::SAdd(std::string &key, std::vector<std::string> &members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SAdd(key, members);
}

Status PikaCache::SAddIfKeyExist(std::string &key, std::vector<std::string> &members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->SAdd(key, members);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::SAddnx(std::string &key, std::vector<std::string> &members, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (!caches_[cache_index]->Exists(key)) {
    caches_[cache_index]->SAdd(key, members);
    caches_[cache_index]->Expire(key, ttl);
    return Status::OK();
  } else {
    return Status::NotFound("key exist");
  }
}

Status PikaCache::SAddnxWithoutTTL(std::string &key, std::vector<std::string> &members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (!caches_[cache_index]->Exists(key)) {
    caches_[cache_index]->SAdd(key, members);
    return Status::OK();
  } else {
    return Status::NotFound("key exist");
  }
}

Status PikaCache::SCard(std::string &key, unsigned long *len) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SCard(key, len);
}

Status PikaCache::SIsmember(std::string &key, std::string &member) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SIsmember(key, member);
}

Status PikaCache::SMembers(std::string &key, std::vector<std::string> *members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SMembers(key, members);
}

Status PikaCache::SRem(std::string &key, std::vector<std::string> &members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SRem(key, members);
}

Status PikaCache::SRandmember(std::string &key, long count, std::vector<std::string> *members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SRandmember(key, count, members);
}

/*-----------------------------------------------------------------------------
 * ZSet Commands
 *----------------------------------------------------------------------------*/
Status PikaCache::ZAdd(std::string &key, std::vector<storage::ScoreMember> &score_members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->ZAdd(key, score_members);
}

void PikaCache::GetMinMaxScore(std::vector<storage::ScoreMember> &score_members, double &min, double &max) {
  if (score_members.empty()) {
    return;
  }
  min = max = score_members.front().score;
  for (auto &item : score_members) {
    if (item.score < min) {
      min = item.score;
    }
    if (item.score > max) {
      max = item.score;
    }
  }
}

bool PikaCache::GetCacheMinMaxSM(dory::RedisCache *cache_obj, std::string &key, storage::ScoreMember &min_m,
                                 storage::ScoreMember &max_m) {
  if (cache_obj) {
    std::vector<storage::ScoreMember> score_members;
    auto s = cache_obj->ZRange(key, 0, 0, &score_members);
    if (!s.ok() || score_members.empty()) {
      return false;
    }
    min_m = score_members.front();
    score_members.clear();

    s = cache_obj->ZRange(key, -1, -1, &score_members);
    if (!s.ok() || score_members.empty()) {
      return false;
    }
    max_m = score_members.front();
    return true;
  }
  return false;
}

Status PikaCache::ZAddIfKeyExist(std::string &key, std::vector<storage::ScoreMember> &score_members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock<std::mutex> ls(caches_[cache_index]);
  auto cache_obj = caches_[cache_index];
  Status s;
  if (cache_obj->Exists(key)) {
    std::unordered_set<std::string> unique;
    std::list<storage::ScoreMember> filtered_score_members;
    for (auto it = score_members.rbegin(); it != score_members.rend(); ++it) {
      if (unique.find(it->member) == unique.end()) {
        unique.insert(it->member);
        filtered_score_members.push_front(*it);
      }
    }
    std::vector<storage::ScoreMember> new_score_members;
    for (auto &item : filtered_score_members) {
      new_score_members.push_back(std::move(item));
    }

    double min_score = storage::ZSET_SCORE_MIN;
    double max_score = storage::ZSET_SCORE_MAX;
    GetMinMaxScore(new_score_members, min_score, max_score);

    storage::ScoreMember cache_min_sm;
    storage::ScoreMember cache_max_sm;
    if (!GetCacheMinMaxSM(cache_obj, key, cache_min_sm, cache_max_sm)) {
      return Status::NotFound("key not exist");
    }
    auto cache_min_score = cache_min_sm.score;
    auto cache_max_score = cache_max_sm.score;

    if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
      if (max_score < cache_max_score) {
        cache_obj->ZAdd(key, new_score_members);
      } else {
        std::vector<storage::ScoreMember> score_members_can_add;
        std::vector<std::string> members_need_remove;
        bool left_close = false;
        for (auto &item : new_score_members) {
          if (item.score == cache_max_score) {
            left_close = true;
            score_members_can_add.push_back(item);
            continue;
          }
          if (item.score < cache_max_score) {
            score_members_can_add.push_back(item);
          } else {
            members_need_remove.push_back(item.member);
          }
        }
        if (!score_members_can_add.empty()) {
          cache_obj->ZAdd(key, score_members_can_add);
          std::string cache_max_score_str = left_close ? "" : "(" + std::to_string(cache_max_score);
          std::string max_str = "+inf";
          cache_obj->ZRemrangebyscore(key, cache_max_score_str, max_str);
        }
        if (!members_need_remove.empty()) {
          cache_obj->ZRem(key, members_need_remove);
        }
      }
    } else if (cache_start_pos_ == CACHE_START_FROM_END) {
      if (min_score > cache_min_score) {
        cache_obj->ZAdd(key, new_score_members);
      } else {
        std::vector<storage::ScoreMember> score_members_can_add;
        std::vector<std::string> members_need_remove;
        bool right_close = false;
        for (auto &item : new_score_members) {
          if (item.score == cache_min_score) {
            right_close = true;
            score_members_can_add.push_back(item);
            continue;
          }
          if (item.score > cache_min_score) {
            score_members_can_add.push_back(item);
          } else {
            members_need_remove.push_back(item.member);
          }
        }
        if (!score_members_can_add.empty()) {
          cache_obj->ZAdd(key, score_members_can_add);
          std::string cache_min_score_str = right_close ? "" : "(" + std::to_string(cache_min_score);
          std::string min_str = "-inf";
          cache_obj->ZRemrangebyscore(key, min_str, cache_min_score_str);
        }
        if (!members_need_remove.empty()) {
          cache_obj->ZRem(key, members_need_remove);
        }
      }
    }

    return CleanCacheKeyIfNeeded(cache_obj, key);
  } else {
    return Status::NotFound("key not exist");
  }
}

Status PikaCache::CleanCacheKeyIfNeeded(dory::RedisCache *cache_obj, std::string &key) {
  unsigned long cache_len = 0;
  cache_obj->ZCard(key, &cache_len);
  if (cache_len > (unsigned long)cache_items_per_key_) {
    long start = 0;
    long stop = 0;
    if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
      // 淘汰尾部
      start = -cache_len + cache_items_per_key_;
      stop = -1;
    } else if (cache_start_pos_ == CACHE_START_FROM_END) {
      // 淘汰头部
      start = 0;
      stop = cache_len - cache_items_per_key_ - 1;
    }
    auto min = std::to_string(start);
    auto max = std::to_string(stop);
    cache_obj->ZRemrangebyrank(key, min, max);
  }
  return Status::OK();
}

Status PikaCache::ZAddnx(std::string &key, std::vector<storage::ScoreMember> &score_members, int64_t ttl) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (!caches_[cache_index]->Exists(key)) {
    caches_[cache_index]->ZAdd(key, score_members);
    caches_[cache_index]->Expire(key, ttl);
    return Status::OK();
  } else {
    return Status::NotFound("key exist");
  }
}

Status PikaCache::ZAddnxWithoutTTL(std::string &key, std::vector<storage::ScoreMember> &score_members) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (!caches_[cache_index]->Exists(key)) {
    caches_[cache_index]->ZAdd(key, score_members);
    return Status::OK();
  } else {
    return Status::NotFound("key exist");
  }
}

Status PikaCache::ZCard(std::string &key, unsigned long *len, const std::shared_ptr<Slot> &slot) {
  int32_t db_len = 0;
  slot->db()->ZCard(key, &db_len);
  *len = db_len;
  return Status::OK();
}

Status PikaCache::CacheZCard(std::string &key, unsigned long *len) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);

  return caches_[cache_index]->ZCard(key, len);
}

RangeStatus PikaCache::CheckCacheRangeByScore(unsigned long cache_len, double cache_min, double cache_max, double min,
                                              double max, bool left_close, bool right_close) {
  bool cache_full = (cache_len == (unsigned long)cache_items_per_key_);

  if (cache_full) {
    if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
      bool ret = (max < cache_max);
      if (ret) {
        if (max < cache_min) {
          return RangeStatus::RangeError;
        } else {
          return RangeStatus::RangeHit;
        }
      } else {
        return RangeStatus::RangeMiss;
      }
    } else if (cache_start_pos_ == CACHE_START_FROM_END) {
      bool ret = min > cache_min;
      if (ret) {
        if (min > cache_max) {
          return RangeStatus::RangeError;
        } else {
          return RangeStatus::RangeHit;
        }
      } else {
        return RangeStatus::RangeMiss;
      }
    } else {
      return RangeStatus::RangeError;
    }
  } else {
    if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
      bool ret = right_close ? max < cache_max : max <= cache_max;
      if (ret) {
        if (max < cache_min) {
          return RangeStatus::RangeError;
        } else {
          return RangeStatus::RangeHit;
        }
      } else {
        return RangeStatus::RangeMiss;
      }
    } else if (cache_start_pos_ == CACHE_START_FROM_END) {
      bool ret = left_close ? min > cache_min : min >= cache_min;
      if (ret) {
        if (min > cache_max) {
          return RangeStatus::RangeError;
        } else {
          return RangeStatus::RangeHit;
        }
      } else {
        return RangeStatus::RangeMiss;
      }
    } else {
      return RangeStatus::RangeError;
    }
  }
}

Status PikaCache::ZCount(std::string &key, std::string &min, std::string &max, unsigned long *len, ZCountCmd *cmd) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  auto cache_obj = caches_[cache_index];
  unsigned long cache_len = 0;
  cache_obj->ZCard(key, &cache_len);
  if (cache_len <= 0) {
    return Status::NotFound("key not in cache");
  } else {
    storage::ScoreMember cache_min_sm;
    storage::ScoreMember cache_max_sm;
    if (!GetCacheMinMaxSM(cache_obj, key, cache_min_sm, cache_max_sm)) {
      return Status::NotFound("key not exist");
    }
    auto cache_min_score = cache_min_sm.score;
    auto cache_max_score = cache_max_sm.score;

    if (RangeStatus::RangeHit == CheckCacheRangeByScore(cache_len, cache_min_score, cache_max_score, cmd->MinScore(),
                                                        cmd->MaxScore(), cmd->LeftClose(), cmd->RightClose())) {
      auto s = cache_obj->ZCount(key, min, max, len);
      return s;
    } else {
      return Status::NotFound("key not in cache");
    }
  }
}

Status PikaCache::ZIncrby(std::string &key, std::string &member, double increment) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::shared_ptr<std::mutex> slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->ZIncrby(key, member, increment);
}

bool PikaCache::ReloadCacheKeyIfNeeded(dory::RedisCache *cache_obj, std::string &key, int mem_len, int db_len,
                                       const std::shared_ptr<Slot> &slot) {
  if (mem_len == -1) {
    unsigned long cache_len = 0;
    cache_obj->ZCard(key, &cache_len);
    mem_len = cache_len;
  }
  if (db_len == -1) {
    db_len = 0;
    slot->db()->ZCard(key, &db_len);
    if (!db_len) {
      return false;
    }
  }
  if (db_len < cache_items_per_key_) {
    if (mem_len * 2 < db_len) {
      cache_obj->Del(key);
      PushKeyToAsyncLoadQueue(PIKA_KEY_TYPE_ZSET, key);
      return true;
    } else {
      return false;
    }
  } else {
    if (cache_items_per_key_ && mem_len * 2 < cache_items_per_key_) {
      cache_obj->Del(key);
      PushKeyToAsyncLoadQueue(PIKA_KEY_TYPE_ZSET, key);
      return true;
    } else {
      return false;
    }
  }
}

Status PikaCache::ZIncrbyIfKeyExist(std::string &key, std::string &member, double increment, ZIncrbyCmd *cmd) {
  auto eps = std::numeric_limits<double>::epsilon();
  if (-eps < increment && increment < eps) {
    return Status::NotFound("icrement is 0, nothing to be done");
  }
  if (!cmd->res().ok()) {
    return Status::NotFound("key not exist");
  }
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::shared_lock lm(cache_mutexs_);
  auto cache_obj = caches_[cache_index];
  unsigned long cache_len = 0;
  cache_obj->ZCard(key, &cache_len);

  storage::ScoreMember cache_min_sm;
  storage::ScoreMember cache_max_sm;
  if (!GetCacheMinMaxSM(cache_obj, key, cache_min_sm, cache_max_sm)) {
    return Status::NotFound("key not exist");
  }
  auto cache_min_score = cache_min_sm.score;
  auto cache_max_score = cache_max_sm.score;
  auto RemCacheRangebyscoreAndCheck = [this, cache_obj, &key, cache_len](double score) {
    auto score_rm = std::to_string(score);
    auto s = cache_obj->ZRemrangebyscore(key, score_rm, score_rm);
    ReloadCacheKeyIfNeeded(cache_obj, key, cache_len);
    return s;
  };
  auto RemCacheKeyMember = [this, cache_obj, &key, cache_len](const std::string &member, bool check = true) {
    std::vector<std::string> member_rm = {member};
    auto s = cache_obj->ZRem(key, member_rm);
    if (check) {
      ReloadCacheKeyIfNeeded(cache_obj, key, cache_len);
    }
    return s;
  };

  if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
    if (cmd->Score() > cache_max_score) {
      return RemCacheKeyMember(member);
    } else if (cmd->Score() == cache_max_score) {
      RemCacheKeyMember(member, false);
      return RemCacheRangebyscoreAndCheck(cache_max_score);
    } else {
      std::vector<storage::ScoreMember> score_member = {{cmd->Score(), member}};
      auto s = cache_obj->ZAdd(key, score_member);
      CleanCacheKeyIfNeeded(cache_obj, key);
      return s;
    }
  } else if (cache_start_pos_ == CACHE_START_FROM_END) {
    if (cmd->Score() > cache_min_score) {
      std::vector<storage::ScoreMember> score_member = {{cmd->Score(), member}};
      auto s = cache_obj->ZAdd(key, score_member);
      CleanCacheKeyIfNeeded(cache_obj, key);
      return s;
    } else if (cmd->Score() == cache_min_score) {
      RemCacheKeyMember(member, false);
      return RemCacheRangebyscoreAndCheck(cache_min_score);
    } else {
      std::vector<std::string> member_rm = {member};
      return RemCacheKeyMember(member);
    }
  }

  return Status::NotFound("key not exist");
}

RangeStatus PikaCache::CheckCacheRange(int32_t cache_len, int32_t db_len, long start, long stop, long &out_start,
                                       long &out_stop) {
  out_start = start >= 0 ? start : db_len + start;
  out_stop = stop >= 0 ? stop : db_len + stop;
  out_start = out_start <= 0 ? 0 : out_start;
  out_stop = out_stop >= db_len ? db_len - 1 : out_stop;
  if (out_start > out_stop || out_start >= db_len || out_stop < 0) {
    return RangeStatus::RangeError;
  } else {
    if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
      if (out_start < cache_len && out_stop < cache_len) {
        return RangeStatus::RangeHit;
      } else {
        return RangeStatus::RangeMiss;
      }
    } else if (cache_start_pos_ == CACHE_START_FROM_END) {
      if (out_start >= db_len - cache_len && out_stop >= db_len - cache_len) {
        out_start = out_start - (db_len - cache_len);
        out_stop = out_stop - (db_len - cache_len);
        return RangeStatus::RangeHit;
      } else {
        return RangeStatus::RangeMiss;
      }
    } else {
      return RangeStatus::RangeError;
    }
  }
}

RangeStatus PikaCache::CheckCacheRevRange(int32_t cache_len, int32_t db_len, long start, long stop, long &out_start,
                                          long &out_stop) {
  // db 正向的index
  long start_index = stop >= 0 ? db_len - stop - 1 : -stop - 1;
  long stop_index = start >= 0 ? db_len - start - 1 : -start - 1;
  start_index = start_index <= 0 ? 0 : start_index;
  stop_index = stop_index >= db_len ? db_len - 1 : stop_index;
  if (start_index > stop_index || start_index >= db_len || stop_index < 0) {
    return RangeStatus::RangeError;
  } else {
    if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
      if (start_index < cache_len && stop_index < cache_len) {
        // cache 逆向的index
        out_start = cache_len - stop_index - 1;
        out_stop = cache_len - start_index - 1;

        return RangeStatus::RangeHit;
      } else {
        return RangeStatus::RangeMiss;
      }
    } else if (cache_start_pos_ == CACHE_START_FROM_END) {
      if (start_index >= db_len - cache_len && stop_index >= db_len - cache_len) {
        // cache 正向的index
        int cache_start = start_index - (db_len - cache_len);
        int cache_stop = stop_index - (db_len - cache_len);
        // cache 逆向的index
        out_start = cache_len - cache_stop - 1;
        out_stop = cache_len - cache_start - 1;
        return RangeStatus::RangeHit;
      } else {
        return RangeStatus::RangeMiss;
      }
    } else {
      return RangeStatus::RangeError;
    }
  }
}

Status PikaCache::ZRange(std::string &key, long start, long stop, std::vector<storage::ScoreMember> *score_members,
                         const std::shared_ptr<Slot> &slot) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);

  auto cache_obj = caches_[cache_index];
  auto db_obj = slot->db();
  Status s;
  if (cache_obj->Exists(key)) {
    unsigned long cache_len = 0;
    cache_obj->ZCard(key, &cache_len);
    int32_t db_len = 0;
    db_obj->ZCard(key, &db_len);
    long out_start;
    long out_stop;
    RangeStatus rs = CheckCacheRange(cache_len, db_len, start, stop, out_start, out_stop);
    if (rs == RangeStatus::RangeHit) {
      return cache_obj->ZRange(key, out_start, out_stop, score_members);
    } else if (rs == RangeStatus::RangeMiss) {
      ReloadCacheKeyIfNeeded(cache_obj, key, cache_len, db_len);
      return Status::NotFound("key not in cache");
    } else if (rs == RangeStatus::RangeError) {
      return Status::NotFound("error range");
    } else {
      return Status::Corruption("unknown error");
    }
  } else {
    return Status::NotFound("key not in cache");
  }
}

Status PikaCache::ZRangebyscore(std::string &key, std::string &min, std::string &max,
                                std::vector<storage::ScoreMember> *score_members, ZRangebyscoreCmd *cmd) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);

  auto cache_obj = caches_[cache_index];
  unsigned long cache_len = 0;
  cache_obj->ZCard(key, &cache_len);
  if (cache_len <= 0) {
    return Status::NotFound("key not in cache");
  } else {
    storage::ScoreMember cache_min_sm;
    storage::ScoreMember cache_max_sm;
    if (!GetCacheMinMaxSM(cache_obj, key, cache_min_sm, cache_max_sm)) {
      return Status::NotFound("key not exist");
    }

    if (RangeStatus::RangeHit == CheckCacheRangeByScore(cache_len, cache_min_sm.score, cache_max_sm.score,
                                                        cmd->MinScore(), cmd->MaxScore(), cmd->LeftClose(),
                                                        cmd->RightClose())) {
      return cache_obj->ZRangebyscore(key, min, max, score_members, cmd->Offset(), cmd->Count());
    } else {
      return Status::NotFound("key not in cache");
    }
  }
}

Status PikaCache::ZRank(std::string &key, std::string &member, long *rank, const std::shared_ptr<Slot> &slot) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);

  auto cache_obj = caches_[cache_index];
  unsigned long cache_len = 0;
  cache_obj->ZCard(key, &cache_len);
  if (cache_len <= 0) {
    return Status::NotFound("key not in cache");
  } else {
    auto s = cache_obj->ZRank(key, member, rank);
    if (s.ok()) {
      if (cache_start_pos_ == CACHE_START_FROM_END) {
        int32_t db_len = 0;
        slot->db()->ZCard(key, &db_len);
        *rank = db_len - cache_len + *rank;
      }
      return s;
    } else {
      return Status::NotFound("key not in cache");
    }
  }
}

Status PikaCache::ZRem(std::string &key, std::vector<std::string> &members, std::shared_ptr<Slot> slot) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);

  auto s = caches_[cache_index]->ZRem(key, members);
  ReloadCacheKeyIfNeeded(caches_[cache_index], key);
  return s;
}

Status PikaCache::ZRemrangebyrank(std::string &key, std::string &min, std::string &max, int32_t ele_deleted,
                                  const std::shared_ptr<Slot> &slot) {
  std::unique_lock l(rwlock_);
  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  auto cache_obj = caches_[cache_index];
  unsigned long cache_len = 0;
  cache_obj->ZCard(key, &cache_len);
  if (cache_len <= 0) {
    return Status::NotFound("key not in cache");
  } else {
    auto db_obj = slot->db();
    int32_t db_len = 0;
    db_obj->ZCard(key, &db_len);
    db_len += ele_deleted;
    auto start = std::stol(min);
    auto stop = std::stol(max);

    int32_t start_index = start >= 0 ? start : db_len + start;
    int32_t stop_index = stop >= 0 ? stop : db_len + stop;
    start_index = start_index <= 0 ? 0 : start_index;
    stop_index = stop_index >= db_len ? db_len - 1 : stop_index;
    if (start_index > stop_index) {
      return Status::NotFound("error range");
    }

    if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
      if ((uint32_t)start_index <= cache_len) {
        auto cache_min_str = std::to_string(start_index);
        auto cache_max_str = std::to_string(stop_index);
        auto s = cache_obj->ZRemrangebyrank(key, cache_min_str, cache_max_str);
        ReloadCacheKeyIfNeeded(cache_obj, key, cache_len, db_len - ele_deleted);
        return s;
      } else {
        return Status::NotFound("error range");
      }
    } else if (cache_start_pos_ == CACHE_START_FROM_END) {
      if ((uint32_t)stop_index >= db_len - cache_len) {
        int32_t cache_min = start_index - (db_len - cache_len);
        int32_t cache_max = stop_index - (db_len - cache_len);
        cache_min = cache_min <= 0 ? 0 : cache_min;
        cache_max = cache_max >= (int32_t)cache_len ? cache_len - 1 : cache_max;

        auto cache_min_str = std::to_string(cache_min);
        auto cache_max_str = std::to_string(cache_max);
        auto s = cache_obj->ZRemrangebyrank(key, cache_min_str, cache_max_str);

        ReloadCacheKeyIfNeeded(cache_obj, key, cache_len, db_len - ele_deleted);
        return s;
      } else {
        return Status::NotFound("error range");
      }
    } else {
      return Status::NotFound("error range");
    }
  }
}

Status PikaCache::ZRemrangebyscore(std::string &key, std::string &min, std::string &max,
                                   const std::shared_ptr<Slot> &slot) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  std::unique_lock lm(cache_mutexs_[cache_index]);
  auto s = caches_[cache_index]->ZRemrangebyscore(key, min, max);
  ReloadCacheKeyIfNeeded(caches_[cache_index], key, slot);
  return s;
}

Status PikaCache::ZRevrange(std::string &key, long start, long stop, std::vector<storage::ScoreMember> *score_members,
                            const std::shared_ptr<Slot> &slot) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);

  auto cache_obj = caches_[cache_index];
  auto db_obj = slot->db();
  Status s;
  if (cache_obj->Exists(key)) {
    unsigned long cache_len = 0;
    cache_obj->ZCard(key, &cache_len);
    int32_t db_len = 0;
    db_obj->ZCard(key, &db_len);
    long out_start;
    long out_stop;
    RangeStatus rs = CheckCacheRevRange(cache_len, db_len, start, stop, out_start, out_stop);
    if (rs == RangeStatus::RangeHit) {
      return cache_obj->ZRevrange(key, out_start, out_stop, score_members);
    } else if (rs == RangeStatus::RangeMiss) {
      ReloadCacheKeyIfNeeded(cache_obj, key, cache_len, db_len);
      return Status::NotFound("key not in cache");
    } else if (rs == RangeStatus::RangeError) {
      return Status::NotFound("error revrange");
    } else {
      return Status::Corruption("unknown error");
    }
  } else {
    return Status::NotFound("key not in cache");
  }
}

Status PikaCache::ZRevrangebyscore(std::string &key, std::string &min, std::string &max,
                                   std::vector<storage::ScoreMember> *score_members, ZRevrangebyscoreCmd *cmd) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);

  auto cache_obj = caches_[cache_index];
  unsigned long cache_len = 0;
  cache_obj->ZCard(key, &cache_len);
  if (cache_len <= 0) {
    return Status::NotFound("key not in cache");
  } else {
    storage::ScoreMember cache_min_sm;
    storage::ScoreMember cache_max_sm;
    if (!GetCacheMinMaxSM(cache_obj, key, cache_min_sm, cache_max_sm)) {
      return Status::NotFound("key not exist");
    }
    auto cache_min_score = cache_min_sm.score;
    auto cache_max_score = cache_max_sm.score;

    auto rs = CheckCacheRangeByScore(cache_len, cache_min_score, cache_max_score, cmd->MinScore(), cmd->MaxScore(),
                                     cmd->LeftClose(), cmd->RightClose());
    if (RangeStatus::RangeHit == rs) {
      return cache_obj->ZRevrangebyscore(key, min, max, score_members, cmd->Offset(), cmd->Count());
    } else if (RangeStatus::RangeMiss == rs) {
      ReloadCacheKeyIfNeeded(cache_obj, key, cache_len);
      return Status::NotFound("score range miss");
    } else {
      return Status::NotFound("score range error");
    }
  }
}

bool PikaCache::CacheSizeEqsDB(std::string &key, const std::shared_ptr<Slot> &slot) {
  int32_t db_len = 0;
  slot->ZCard(key, &db_len);

  std::unique_lock l(rwlock_);
  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  unsigned long cache_len = 0;
  caches_[cache_index]->ZCard(key, &cache_len);

  return db_len == (int32_t)cache_len;
}

Status PikaCache::ZRevrangebylex(std::string &key, std::string &min, std::string &max,
                                 std::vector<std::string> *members, const std::shared_ptr<Slot> &slot) {
  if (CacheSizeEqsDB(key, slot)) {
    std::unique_lock l(rwlock_);

    int cache_index = CacheIndex(key);
    slash::MutexLock lm(cache_mutexs_[cache_index]);

    return caches_[cache_index]->ZRevrangebylex(key, min, max, members);
  } else {
    return Status::NotFound("key not in cache");
  }
}

Status PikaCache::ZRevrank(std::string &key, std::string &member, long *rank, const std::shared_ptr<Slot> &slot) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);

  auto cache_obj = caches_[cache_index];
  unsigned long cache_len = 0;
  cache_obj->ZCard(key, &cache_len);
  if (cache_len <= 0) {
    return Status::NotFound("key not in cache");
  } else {
    auto s = cache_obj->ZRevrank(key, member, rank, slot);
    if (s.ok()) {
      if (cache_start_pos_ == CACHE_START_FROM_BEGIN) {
        int32_t db_len = 0;
        slot->db()->ZCard(key, &db_len);
        *rank = db_len - cache_len + *rank;
      }
      return s;
    } else {
      return Status::NotFound("member not in cache");
    }
  }
}

Status PikaCache::ZScore(std::string &key, std::string &member, double *score) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  auto s = caches_[cache_index]->ZScore(key, member, score);
  if (!s.ok()) {
    return Status::NotFound("key or member not in cache");
  }
  return s;
}

Status PikaCache::ZRangebylex(std::string &key, std::string &min, std::string &max, std::vector<std::string> *members,
                              const std::shared_ptr<Slot> &slot) {
  if (CacheSizeEqsDB(key, slot)) {
    std::unique_lock l(rwlock_);

    int cache_index = CacheIndex(key);
    slash::MutexLock lm(cache_mutexs_[cache_index]);

    return caches_[cache_index]->ZRangebylex(key, min, max, members);
  } else {
    return Status::NotFound("key not in cache");
  }
}

Status PikaCache::ZLexcount(std::string &key, std::string &min, std::string &max, unsigned long *len,
                            const std::shared_ptr<Slot> &slot) {
  if (CacheSizeEqsDB(key, slot)) {
    std::unique_lock l(rwlock_);

    int cache_index = CacheIndex(key);
    slash::MutexLock lm(cache_mutexs_[cache_index]);

    return caches_[cache_index]->ZLexcount(key, min, max, len);
  } else {
    return Status::NotFound("key not in cache");
  }
}

Status PikaCache::ZRemrangebylex(std::string &key, std::string &min, std::string &max,
                                 const std::shared_ptr<Slot> &slot) {
  if (CacheSizeEqsDB(key, slot)) {
    std::unique_lock l(rwlock_);

    int cache_index = CacheIndex(key);
    slash::MutexLock lm(cache_mutexs_[cache_index]);

    return caches_[cache_index]->ZRemrangebylex(key, min, max);
  } else {
    return Status::NotFound("key not in cache");
  }
}

/*-----------------------------------------------------------------------------
 * Bit Commands
 *----------------------------------------------------------------------------*/
Status PikaCache::SetBit(std::string &key, size_t offset, long value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->SetBit(key, offset, value);
}

Status PikaCache::SetBitIfKeyExist(std::string &key, size_t offset, long value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  if (caches_[cache_index]->Exists(key)) {
    return caches_[cache_index]->SetBit(key, offset, value);
  }
  return Status::NotFound("key not exist");
}

Status PikaCache::GetBit(std::string &key, size_t offset, long *value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->GetBit(key, offset, value);
}

Status PikaCache::BitCount(std::string &key, long start, long end, long *value, bool have_offset) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->BitCount(key, start, end, value, have_offset);
}

Status PikaCache::BitPos(std::string &key, long bit, long *value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->BitPos(key, bit, value);
}

Status PikaCache::BitPos(std::string &key, long bit, long start, long *value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->BitPos(key, bit, start, value);
}

Status PikaCache::BitPos(std::string &key, long bit, long start, long end, long *value) {
  std::unique_lock l(rwlock_);

  int cache_index = CacheIndex(key);
  slash::MutexLock lm(cache_mutexs_[cache_index]);
  return caches_[cache_index]->BitPos(key, bit, start, end, value);
}

Status PikaCache::InitWithoutLock(uint32_t cache_num, dory::CacheConfig *cache_cfg) {
  cache_status_ = PIKA_CACHE_STATUS_INIT;

  cache_num_ = cache_num;
  if (nullptr != cache_cfg) {
    dory::RedisCache::SetConfig(cache_cfg);
  }

  for (uint32_t i = 0; i < cache_num; ++i) {
    dory::RedisCache *cache = new dory::RedisCache();
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

void PikaCache::DestroyWithoutLock(void) {
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

int PikaCache::CacheIndex(const std::string &key) {
  uint32_t crc = CRC32Update(0, key.data(), (int)key.size());
  return (int)(crc % caches_.size());
}

Status PikaCache::WriteKvToCache(std::string &key, std::string &value, int64_t ttl) {
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

Status PikaCache::WriteHashToCache(std::string &key, std::vector<storage::FieldValue> &fvs, int64_t ttl) {
  if (0 >= ttl) {
    if (PIKA_TTL_NONE == ttl) {
      return HMSetnxWithoutTTL(key, fvs);
    } else {
      return Del(key);
    }
  } else {
    return HMSetnx(key, fvs, ttl);
  }
  return Status::OK();
}

Status PikaCache::WriteListToCache(std::string &key, std::vector<std::string> &values, int64_t ttl) {
  if (0 >= ttl) {
    if (PIKA_TTL_NONE == ttl) {
      return RPushnxWithoutTTL(key, values);
    } else {
      return Del(key);
    }
  } else {
    return RPushnx(key, values, ttl);
  }
  return Status::OK();
}

Status PikaCache::WriteSetToCache(std::string &key, std::vector<std::string> &members, int64_t ttl) {
  if (0 >= ttl) {
    if (PIKA_TTL_NONE == ttl) {
      return SAddnxWithoutTTL(key, members);
    } else {
      return Del(key);
    }
  } else {
    return SAddnx(key, members, ttl);
  }
  return Status::OK();
}

Status PikaCache::WriteZSetToCache(std::string &key, std::vector<storage::ScoreMember> &score_members, int64_t ttl) {
  if (0 >= ttl) {
    if (PIKA_TTL_NONE == ttl) {
      return ZAddnxWithoutTTL(key, score_members);
    } else {
      return Del(key);
    }
  } else {
    return ZAddnx(key, score_members, ttl);
  }
  return Status::OK();
}

void PikaCache::PushKeyToAsyncLoadQueue(const char key_type, std::string &key) {
  cache_load_thread_->Push(key_type, key);
}
