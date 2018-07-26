#ifndef FETCH_FAKE_STORAGE_UNIT_HPP
#define FETCH_FAKE_STORAGE_UNIT_HPP

#include "crypto/fnv.hpp"
#include "crypto/sha256.hpp"
#include "ledger/storage_unit/storage_unit_interface.hpp"

#include <mutex>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>

class FakeStorageUnit : public fetch::ledger::StorageUnitInterface {
public:
  using transaction_store_type = std::unordered_map<fetch::byte_array::ConstByteArray,fetch::chain::Transaction, fetch::crypto::CallableFNV>;
  using state_store_type = std::unordered_map<fetch::byte_array::ConstByteArray, fetch::byte_array::ConstByteArray, fetch::crypto::CallableFNV>;
  using state_archive_type = std::unordered_map<bookmark_type, state_store_type>;
  using lock_store_type = std::unordered_set<fetch::byte_array::ConstByteArray, fetch::crypto::CallableFNV>;
  using mutex_type = std::mutex;
  using lock_guard_type = std::lock_guard<mutex_type>;

  document_type GetOrCreate(fetch::byte_array::ConstByteArray const &key) override {
    lock_guard_type lock(mutex_);
    document_type doc;

    auto it = state_.find(key);
    if (it != state_.end()) {
      doc.document = it->second;
    } else {
      doc.was_created = true;
    }

    return doc;
  }

  document_type Get(fetch::byte_array::ConstByteArray const &key) override {
    lock_guard_type lock(mutex_);
    document_type doc;

    auto it = state_.find(key);
    if (it != state_.end()) {
      doc.document = it->second;
    } else {
      doc.failed = true;
    }

    return doc;
  }

  void Set(fetch::byte_array::ConstByteArray const &key, fetch::byte_array::ConstByteArray const &value) override {
    lock_guard_type lock(mutex_);
    state_[key] = value;
  }

  bool Lock(fetch::byte_array::ConstByteArray const &key) override {
    lock_guard_type lock(mutex_);
    bool success = false;

    bool const already_locked = locks_.find(key) != locks_.end();
    if (!already_locked) {
      locks_.insert(key);
      success = true;
    }

    return success;
  }

  bool Unlock(fetch::byte_array::ConstByteArray const &key) override {
    lock_guard_type lock(mutex_);
    bool success = false;

    bool const already_locked = locks_.find(key) != locks_.end();
    if (already_locked) {
      locks_.erase(key);
      success = true;
    }

    return success;
  }

  void AddTransaction(fetch::chain::Transaction const &tx) override {
    lock_guard_type lock(mutex_);
    transactions_[tx.digest()] = tx;
  }

  bool GetTransaction(fetch::byte_array::ConstByteArray const &digest, fetch::chain::Transaction &tx) override {
    lock_guard_type lock(mutex_);
    bool success = false;

    auto it = transactions_.find(digest);
    if (it != transactions_.end()) {
      tx = it->second;
      success = true;
    }

    return success;
  }

  hash_type Hash() override {
    lock_guard_type lock(mutex_);
    fetch::crypto::SHA256 hasher{};

    std::vector<fetch::byte_array::ConstByteArray> keys;
    for (auto const &elem : state_) {
      keys.emplace_back(elem.first);
    }

    std::sort(keys.begin(), keys.end());

    hasher.Reset();
    for (auto const &key : keys) {
      hasher.Update(key);
      hasher.Update(state_[key]);
    }
    hasher.Final();

    return hasher.digest();
  }

  void Commit(bookmark_type const &bookmark) override {
    lock_guard_type lock(mutex_);
    state_archive_[bookmark] = state_;
  }

  void Revert(bookmark_type const &bookmark) override {
    lock_guard_type lock(mutex_);
    auto it = state_archive_.find(bookmark);
    if (it != state_archive_.end()) {
      state_ = it->second;
    } else {
      fetch::logger.Info("Reverting to clean state: ", bookmark);

      state_.clear();
    }
  }

private:

  mutex_type mutex_;
  transaction_store_type transactions_;
  state_store_type state_;
  lock_store_type locks_;
  state_archive_type state_archive_;
};

#endif //FETCH_FAKE_STORAGE_UNIT_HPP