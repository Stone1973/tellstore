#pragma once

#include "ClientSocket.hpp"
#include "Table.hpp"

#include <util/CommitManager.hpp>
#include <util/GenericTuple.hpp>
#include <util/sparsehash/dense_hash_set>

#include <crossbow/infinio/InfinibandService.hpp>
#include <crossbow/infinio/Fiber.hpp>
#include <crossbow/non_copyable.hpp>
#include <crossbow/string.hpp>

#include <boost/functional/hash.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <system_error>
#include <tuple>
#include <vector>

namespace tell {
namespace store {

struct ClientConfig;
class ClientProcessor;
class Record;

/**
 * @brief Class representing a TellStore transaction
 */
class ClientTransaction : crossbow::non_copyable {
public:
    ClientTransaction(ClientProcessor& processor, crossbow::infinio::Fiber& fiber, SnapshotDescriptor snapshot);

    ~ClientTransaction();

    ClientTransaction(ClientTransaction&& other);

    uint64_t version() const {
        return mSnapshot.version();
    }

    std::shared_ptr<GetResponse> get(const Table& table, uint64_t key);

    std::shared_ptr<ModificationResponse> insert(const Table& table, uint64_t key, const GenericTuple& tuple,
            bool hasSucceeded = true);

    std::shared_ptr<ModificationResponse> update(const Table& table, uint64_t key, const GenericTuple& tuple);

    std::shared_ptr<ModificationResponse> remove(const Table& table, uint64_t key);

    std::shared_ptr<ScanResponse> scan(const Table& table, uint32_t queryLength, const char* query);

    void commit();

    void abort();

private:
    struct ModifiedHasher {
        size_t operator()(const std::tuple<uint64_t, uint64_t>& value) const {
            auto hash = std::hash<uint64_t>()(std::get<0>(value));
            boost::hash_combine(hash, std::hash<uint64_t>()(std::get<1>(value)));
            return hash;
        }
    };

    void rollbackModified();

    template <typename Response, typename Fun>
    std::shared_ptr<Response> executeInTransaction(const Table& table, Fun fun);

    ClientProcessor& mProcessor;
    crossbow::infinio::Fiber& mFiber;

    SnapshotDescriptor mSnapshot;

    google::dense_hash_set<std::tuple<uint64_t, uint64_t>, ModifiedHasher> mModified;

    bool mCommitted;
};

/**
 * @brief Class to interact with the TellStore from within a fiber
 */
class ClientHandle : crossbow::non_copyable, crossbow::non_movable {
public:
    ClientHandle(ClientProcessor& processor, crossbow::infinio::Fiber& fiber);

    std::shared_ptr<CreateTableResponse> createTable(const crossbow::string& name, const Schema& schema);

    std::shared_ptr<GetTableResponse> getTable(const crossbow::string& name);

    ClientTransaction startTransaction();

    std::shared_ptr<GetResponse> get(const Table& table, uint64_t key);

    std::shared_ptr<ModificationResponse> insert(const Table& table, uint64_t key, uint64_t version,
            const GenericTuple& tuple, bool hasSucceeded = true);

    std::shared_ptr<ModificationResponse> update(const Table& table, uint64_t key, uint64_t version,
            const GenericTuple& tuple);

    std::shared_ptr<ModificationResponse> remove(const Table& table, uint64_t key, uint64_t version);

    std::shared_ptr<ScanResponse> scan(const Table& table, uint32_t queryLength, const char* query);

private:
    ClientProcessor& mProcessor;
    crossbow::infinio::Fiber& mFiber;
};

/**
 * @brief Class managing all running TellStore fibers
 */
class ClientProcessor : crossbow::non_copyable, crossbow::non_movable {
public:
    ClientProcessor(CommitManager& commitManager, crossbow::infinio::InfinibandService& service,
            crossbow::infinio::LocalMemoryRegion& scanRegion, const ClientConfig& config, uint64_t processorNum);

    uint64_t transactionCount() const {
        return mTransactionCount.load();
    }

    void execute(const std::function<void(ClientHandle&)>& fun);

private:
    friend class ClientHandle;
    friend class ClientTransaction;

    ClientTransaction start(crossbow::infinio::Fiber& fiber) {
        auto snapshot = mCommitManager.startTx();
        return {*this, fiber, std::move(snapshot)};
    }

    std::shared_ptr<CreateTableResponse> createTable(crossbow::infinio::Fiber& fiber, const crossbow::string& name,
            const Schema& schema) {
        return mTellStoreSocket.createTable(fiber, name, schema);
    }

    std::shared_ptr<GetTableResponse> getTable(crossbow::infinio::Fiber& fiber, const crossbow::string& name) {
        return mTellStoreSocket.getTable(fiber, name);
    }

    std::shared_ptr<GetResponse> get(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const SnapshotDescriptor& snapshot) {
        return mTellStoreSocket.get(fiber, tableId, key, snapshot);
    }

    std::shared_ptr<ModificationResponse> insert(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const Record& record, const GenericTuple& tuple, const SnapshotDescriptor& snapshot, bool hasSucceeded) {
        return mTellStoreSocket.insert(fiber, tableId, key, record, tuple, snapshot, hasSucceeded);
    }

    std::shared_ptr<ModificationResponse> update(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const Record& record, const GenericTuple& tuple, const SnapshotDescriptor& snapshot) {
        return mTellStoreSocket.update(fiber, tableId, key, record, tuple, snapshot);
    }

    std::shared_ptr<ModificationResponse> remove(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const SnapshotDescriptor& snapshot) {
        return mTellStoreSocket.remove(fiber, tableId, key, snapshot);
    }

    std::shared_ptr<ModificationResponse> revert(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const SnapshotDescriptor& snapshot) {
        return mTellStoreSocket.revert(fiber, tableId, key, snapshot);
    }

    std::shared_ptr<ScanResponse> scan(crossbow::infinio::Fiber& fiber, uint64_t tableId, const Record& record,
            uint32_t queryLength, const char* query, const SnapshotDescriptor& snapshot) {
        return mTellStoreSocket.scan(fiber, tableId, record, queryLength, query, mScanRegion, snapshot);
    }

    void commit(const SnapshotDescriptor& snapshot) {
        mCommitManager.commitTx(snapshot);
    }

    CommitManager& mCommitManager;
    crossbow::infinio::LocalMemoryRegion& mScanRegion;

    std::unique_ptr<crossbow::infinio::InfinibandProcessor> mProcessor;

    ClientSocket mTellStoreSocket;

    uint64_t mProcessorNum;
    std::atomic<uint64_t> mTransactionCount;
};

/**
 * @brief Class managing all TellStore client processors
 *
 * Dispatches new client functions to the processor with the least amout of load.
 */
class ClientManager : crossbow::non_copyable, crossbow::non_movable {
public:
    ClientManager(crossbow::infinio::InfinibandService& service, const ClientConfig& config);

    void execute(std::function<void(ClientHandle&)> fun);

private:
    friend class ClientProcessor;

    crossbow::infinio::LocalMemoryRegion& scanRegion() {
        return mScanRegion;
    }

    crossbow::infinio::LocalMemoryRegion mScanRegion;

    CommitManager mCommitManager;

    std::vector<std::unique_ptr<ClientProcessor>> mProcessor;
};

} // namespace store
} // namespace tell