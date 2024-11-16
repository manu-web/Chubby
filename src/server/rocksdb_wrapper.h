#ifndef ROCKSDB_WRAPPER_H
#define ROCKSDB_WRAPPER_H

#include <string>
#include <rocksdb/db.h>
#include <rocksdb/utilities/transaction.h>
#include <rocksdb/utilities/transaction_db.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

class RocksDBWrapper
{
public:
    // Constructor initializes the RocksDB database at the given path.
    RocksDBWrapper(const std::string &db_path, size_t cache_size);

    // Destructor to close the RocksDB instance.
    ~RocksDBWrapper();

    bool Get(const std::string &key, std::string &value) const;

    bool GetAllKeys(std::vector<std::string> &keys) const;

    int Put(const std::string &key, const std::string &value, std::string &old_value);

    rocksdb::Status Write(const rocksdb::WriteOptions &options, rocksdb::WriteBatch *batch);

    // Create a snapshot for isolation.
    const rocksdb::Snapshot *GetSnapshot() const;

    // Release the snapshot.
    void ReleaseSnapshot(const rocksdb::Snapshot *snapshot);

private:
    rocksdb::TransactionDB *db_;                    // Pointer to the RocksDB transactional instance.
    rocksdb::Options options_;                      // Options for RocksDB configuration.
    rocksdb::TransactionDBOptions txn_options_;     // Transaction-specific options.
    rocksdb::BlockBasedTableOptions table_options_; // Table options for caching.
    std::shared_ptr<rocksdb::Cache> cache_;         // Cache for RocksDB.
};

#endif