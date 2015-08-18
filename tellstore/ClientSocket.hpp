#pragma once

#include <tellstore/ErrorCode.hpp>
#include <tellstore/MessageTypes.hpp>
#include <tellstore/GenericTuple.hpp>
#include <tellstore/Record.hpp>
#include <tellstore/ScanMemory.hpp>
#include <tellstore/Table.hpp>

#include <crossbow/byte_buffer.hpp>
#include <crossbow/infinio/InfinibandSocket.hpp>
#include <crossbow/infinio/RpcClient.hpp>
#include <crossbow/string.hpp>

#include <sparsehash/dense_hash_map>

#include <cstdint>
#include <memory>
#include <system_error>
#include <tuple>

namespace tell {
namespace commitmanager {
class SnapshotDescriptor;
} // namespace commitmanager

namespace store {

class ClientSocket;

/**
 * @brief Response for a Create-Table request
 */
class CreateTableResponse final
        : public crossbow::infinio::RpcResponseResult<CreateTableResponse, Table> {
    using Base = crossbow::infinio::RpcResponseResult<CreateTableResponse, Table>;

public:
    CreateTableResponse(crossbow::infinio::Fiber& fiber, const Schema& schema)
            : Base(fiber),
              mSchema(schema) {
    }

private:
    friend Base;

    static constexpr ResponseType MessageType = ResponseType::CREATE_TABLE;

    static const std::error_category& errorCategory() {
        return error::get_error_category();
    }

    void processResponse(crossbow::buffer_reader& message);

    Schema mSchema;
};

/**
 * @brief Response for a Get-Table request
 */
class GetTableResponse final : public crossbow::infinio::RpcResponseResult<GetTableResponse, Table> {
    using Base = crossbow::infinio::RpcResponseResult<GetTableResponse, Table>;

public:
    using Base::Base;

private:
    friend Base;

    static constexpr ResponseType MessageType = ResponseType::GET_TABLE;

    static const std::error_category& errorCategory() {
        return error::get_error_category();
    }

    void processResponse(crossbow::buffer_reader& message);
};

/**
 * @brief Response for a Get request
 */
class GetResponse final : public crossbow::infinio::RpcResponseResult<GetResponse, std::unique_ptr<Tuple>> {
    using Base = crossbow::infinio::RpcResponseResult<GetResponse, std::unique_ptr<Tuple>>;

public:
    using Base::Base;

private:
    friend Base;

    static constexpr ResponseType MessageType = ResponseType::GET;

    static const std::error_category& errorCategory() {
        return error::get_error_category();
    }

    void processResponse(crossbow::buffer_reader& message);
};

/**
 * @brief Response for a Modificatoin (insert, update, remove, revert) request
 */
class ModificationResponse final : public crossbow::infinio::RpcResponseResult<ModificationResponse, bool> {
    using Base = crossbow::infinio::RpcResponseResult<ModificationResponse, bool>;

public:
    using Base::Base;

private:
    friend Base;

    static constexpr ResponseType MessageType = ResponseType::MODIFICATION;

    static const std::error_category& errorCategory() {
        return error::get_error_category();
    }

    void processResponse(crossbow::buffer_reader& message);
};

/**
 * @brief Response for a Scan request
 */
class ScanResponse final : public crossbow::infinio::RpcResponseResult<ScanResponse, bool> {
    using Base = crossbow::infinio::RpcResponseResult<ScanResponse, bool>;

public:
    ScanResponse(crossbow::infinio::Fiber& fiber, ClientSocket& socket, ScanMemory memory, Record record,
            uint16_t scanId);

    /**
     * @brief Whether the scan has pending tuples to read
     *
     * Blocks until the scan is done or the next tuple is available.
     */
    bool hasNext();

    /**
     * @brief Advances the iterator to the next position and returns the tuple data
     */
    std::tuple<uint64_t, const char*, size_t> next();

private:
    friend Base;
    friend class ClientSocket;

    static constexpr ResponseType MessageType = ResponseType::SCAN;

    static const std::error_category& errorCategory() {
        return error::get_error_category();
    }

    uint16_t scanId() const {
        return mScanId;
    }

    const ScanMemory& scanMemory() const {
        return mMemory;
    }

    void processResponse(crossbow::buffer_reader& message);

    void notifyProgress(uint16_t tupleCount);

    ClientSocket& mSocket;

    ScanMemory mMemory;

    Record mRecord;

    uint16_t mScanId;

    const char* mPos;

    size_t mTuplePending;
};

/**
 * @brief Handles communication with one TellStore server
 *
 * Sends RPC requests and returns the pending response.
 */
class ClientSocket final : public crossbow::infinio::RpcClientSocket {
public:
    ClientSocket(crossbow::infinio::InfinibandSocket socket, size_t maxPendingResponses)
            : crossbow::infinio::RpcClientSocket(std::move(socket), maxPendingResponses),
              mScanId(0x0u) {
        mScans.set_empty_key(0x0u);
        mScans.set_deleted_key(std::numeric_limits<uint16_t>::max());
    }

    void connect(const crossbow::infinio::Endpoint& host, uint64_t threadNum);

    void shutdown();

    std::shared_ptr<CreateTableResponse> createTable(crossbow::infinio::Fiber& fiber, const crossbow::string& name,
            const Schema& schema);

    std::shared_ptr<GetTableResponse> getTable(crossbow::infinio::Fiber& fiber, const crossbow::string& name);

    std::shared_ptr<GetResponse> get(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const commitmanager::SnapshotDescriptor& snapshot);

    std::shared_ptr<ModificationResponse> insert(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const Record& record, const GenericTuple& tuple, const commitmanager::SnapshotDescriptor& snapshot,
            bool hasSucceeded);

    std::shared_ptr<ModificationResponse> update(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const Record& record, const GenericTuple& tuple, const commitmanager::SnapshotDescriptor& snapshot);

    std::shared_ptr<ModificationResponse> remove(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const commitmanager::SnapshotDescriptor& snapshot);

    std::shared_ptr<ModificationResponse> revert(crossbow::infinio::Fiber& fiber, uint64_t tableId, uint64_t key,
            const commitmanager::SnapshotDescriptor& snapshot);

    std::shared_ptr<ScanResponse> scan(crossbow::infinio::Fiber& fiber, uint64_t tableId, const Record& record,
            ScanMemory scanMemory, ScanQueryType queryType, uint32_t selectionLength, const char* selection,
            uint32_t queryLength, const char* query, const commitmanager::SnapshotDescriptor& snapshot);

private:
    friend class ScanResponse;

    virtual void onImmediate(uint32_t data) final override;

    void onScanComplete(uint16_t scanId);

    uint16_t mScanId;
    google::dense_hash_map<uint16_t, ScanResponse*> mScans;
};

} // namespace store
} // namespace tell
