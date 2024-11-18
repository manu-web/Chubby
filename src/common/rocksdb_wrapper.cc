#include "rocksdb_wrapper.h"
#include<rocksdb/table.h>
#include <iostream>
#include <thread> // For std::this_thread::sleep_for
#include <chrono> // For delay between retries

RocksDBWrapper::RocksDBWrapper(const std::string &db_path, const size_t cache_size)
{
    options_.create_if_missing = true;

    // Set a lock timeout (in ms) for pessimistic transactions.
    txn_options_.default_lock_timeout = 1000;

    // Set up block-based table options with a cache.
    cache_ = rocksdb::NewLRUCache(cache_size);
    table_options_.block_cache = cache_;  // Associate the cache with table options
    options_.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table_options_));

    // Open the RocksDB transactional database.
    rocksdb::Status status = rocksdb::TransactionDB::Open(options_, txn_options_, db_path, &db_);
    if (!status.ok())
    {
        std::cerr << "Error: Failed to open DB at path: " << db_path << "\n"
                  << status.ToString() << std::endl;
        db_ = nullptr;
    }
    // std::cout << "DB initialized at path: " << db_path << std::endl;
}

bool RocksDBWrapper::Get(const std::string &key, std::string &value) const
{
    if (!db_)
    {
        std::cerr << "Error: DB not initialized." << std::endl;
        return false;
    }

    // Create a read transaction using the current snapshot for isolation.
    rocksdb::ReadOptions read_options;
    const rocksdb::Snapshot *snapshot = db_->GetSnapshot();
    read_options.snapshot = snapshot;

    rocksdb::Status status = db_->Get(read_options, key, &value);

    // Release the snapshot after the read.
    db_->ReleaseSnapshot(snapshot);

    return status.ok(); // Returns true if the key was found
}

bool RocksDBWrapper::GetAllKeys(std::vector<std::string> &keys) const
{
    if (!db_)
    {
        std::cerr << "Error: DB not initialized." << std::endl;
        return false;
    }

    // Create an iterator to traverse all keys.
    rocksdb::ReadOptions read_options;
    std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(read_options));

    // Traverse the entire key space.
    for (it->SeekToFirst(); it->Valid(); it->Next())
    {
        keys.push_back(it->key().ToString());
    }

    if (!it->status().ok())
    {
        std::cerr << "Error loading keys: " << it->status().ToString() << std::endl;
        return false;
    }

    return true;
}

int RocksDBWrapper::Put(const std::string &key, const std::string &value, std::string &old_value)
{
    if (!db_)
    {
        std::cerr << "Error: DB not initialized." << std::endl;
        return -1;
    }

    // Create a pessimistic transaction.
    rocksdb::WriteOptions write_options;
    rocksdb::TransactionOptions txn_options;
    rocksdb::Transaction *txn = db_->BeginTransaction(write_options, txn_options);

    // Lock the key for pessimistic access.
    txn->SetLockTimeout(1000);

    // Read the old value within the transaction to ensure isolation.
    rocksdb::ReadOptions read_options;
    read_options.snapshot = txn->GetSnapshot(); // Use snapshot for consistent reads
    rocksdb::Status get_status = txn->Get(read_options, key, &old_value);

    bool key_found = get_status.ok();

    // Perform the Put operation inside the transaction.
    rocksdb::Status put_status = txn->Put(key, value);
    if (!put_status.ok())
    {
        std::cerr << "Error updating key: " << key << std::endl;
        delete txn;
        return -1;
    }

    // Commit the transaction.
    rocksdb::Status commit_status = txn->Commit();
    if (!commit_status.ok())
    {
        std::cerr << "Error committing transaction for key: " << key << std::endl;
        delete txn;
        return -1;
    }

    // Clean up the transaction.
    delete txn;
    return key_found ? 0 : 1;
}

rocksdb::Status RocksDBWrapper::Write(const rocksdb::WriteOptions &options, rocksdb::WriteBatch *batch) {
    return db_->Write(options, batch);  // Perform the batch write
}

const rocksdb::Snapshot *RocksDBWrapper::GetSnapshot() const
{
    return db_->GetSnapshot();
}

void RocksDBWrapper::ReleaseSnapshot(const rocksdb::Snapshot *snapshot)
{
    db_->ReleaseSnapshot(snapshot);
}

RocksDBWrapper::~RocksDBWrapper()
{
    if (db_)
    {
        delete db_;
    }
}