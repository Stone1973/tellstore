/*
 * (C) Copyright 2015 ETH Zurich Systems Group (http://www.systems.ethz.ch/) and others.
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
 *
 * Contributors:
 *     Markus Pilman <mpilman@inf.ethz.ch>
 *     Simon Loesing <sloesing@inf.ethz.ch>
 *     Thomas Etter <etterth@gmail.com>
 *     Kevin Bocksrocker <kevin.bocksrocker@gmail.com>
 *     Lucas Braun <braunl@inf.ethz.ch>
 */

#include "ColumnMapPage.hpp"

#include "ColumnMapContext.hpp"

#include <deltamain/Record.hpp>
#include <deltamain/Table.hpp>
#include <util/CuckooHash.hpp>
#include <util/PageManager.hpp>

#include <cstring>
#include <type_traits>

namespace tell {
namespace store {
namespace deltamain {

ColumnMapHeapEntry::ColumnMapHeapEntry(uint32_t _offset, const char* data)
        : offset(_offset) {
    memcpy(prefix, data, sizeof(prefix));
}

ColumnMapHeapEntry::ColumnMapHeapEntry(uint32_t _offset, uint32_t size, const char* data)
        : offset(_offset) {
    memcpy(prefix, data, size < sizeof(prefix) ? size : sizeof(prefix));
}

ColumnMapPageModifier::ColumnMapPageModifier(const ColumnMapContext& context, PageManager& pageManager,
        Modifier& mainTableModifier, uint64_t minVersion)
        : mContext(context),
          mPageManager(pageManager),
          mMainTableModifier(mainTableModifier),
          mMinVersion(minVersion),
          mUpdatePage(new (mPageManager.alloc()) ColumnMapMainPage(mContext.fixedSizeCapacity())),
          mUpdateStartIdx(0u),
          mUpdateEndIdx(0u),
          mUpdateIdx(0u),
          mFillPage(new (mPageManager.alloc()) ColumnMapMainPage()),
          mFillHeap(mFillPage->heapData()),
          mFillEndIdx(0u),
          mFillIdx(0u),
          mFillSize(0u) {
}

bool ColumnMapPageModifier::clean(ColumnMapMainPage* page) {
    if (!needsCleaning(page)) {
        mPageList.emplace_back(page);
        return false;
    }

    auto entries = page->entryData();
    auto sizes = page->sizeData();

    uint32_t mainStartIdx = 0;
    uint32_t mainEndIdx = 0;

    typename std::remove_const<decltype(page->count)>::type i = 0;
    while (i < page->count) {
START:
        LOG_ASSERT(mFillIdx == mFillEndIdx, "Current fill index must be at the end index");
        LOG_ASSERT(mUpdateIdx == mUpdateEndIdx, "Current update index must be at the end index");

        auto baseIdx = i;
        auto newest = entries[baseIdx].newest.load();
        bool wasDelete = false;
        if (newest != 0u) {
            if (mainStartIdx != mainEndIdx) {
                LOG_ASSERT(mUpdateStartIdx == mUpdateEndIdx, "Main and update copy at the same time");
                addCleanAction(page, mainStartIdx, mainEndIdx);
                mainStartIdx = 0;
                mainEndIdx = 0;
            }

            uint64_t lowestVersion;

            // Write all updates into the update page
            if (!processUpdates(reinterpret_cast<const UpdateLogEntry*>(newest), entries[baseIdx].version,
                    lowestVersion, wasDelete)) {
                // The page is full, flush any pending clean actions and repeat
                if (mainStartIdx != mainEndIdx) {
                    LOG_ASSERT(mUpdateStartIdx == mUpdateEndIdx, "Main and update copy at the same time");
                    addCleanAction(page, mainStartIdx, mainEndIdx);
                    mainStartIdx = 0;
                    mainEndIdx = 0;
                }
                flush();
                continue;
            }

            // If all elements were overwritten by updates the main page does not need to be processed
            if (lowestVersion <= mMinVersion) {
                if (mUpdateIdx == mUpdateEndIdx) {
                    LOG_ASSERT(mFillIdx == mFillEndIdx, "No elements written but fill index advanced");

                    // Invalidate the element and remove it from the hash table - Retry from beginning if the
                    // invalidation fails
                    if (!entries[baseIdx].newest.compare_exchange_strong(newest,
                            crossbow::to_underlying(NewestPointerTag::INVALID))) {
                        LOG_ASSERT(i == baseIdx, "Changed base index without iterating");
                        continue;
                    }

                    __attribute__((unused)) auto res = mMainTableModifier.remove(entries[baseIdx].key);
                    LOG_ASSERT(res, "Removing key from hash table did not succeed");
                } else {
                    LOG_ASSERT(mFillIdx != mFillEndIdx, "Elements written without advancing the fill index");

                    // Flush the changes to the current element
                    auto fillEntry = mFillPage->entryData() + mFillEndIdx;
                    mPointerActions.emplace_back(&entries[baseIdx].newest, newest, fillEntry);

                    __attribute__((unused)) auto res = mMainTableModifier.insert(entries[baseIdx].key, fillEntry, true);
                    LOG_ASSERT(res, "Inserting key into hash table did not succeed");

                    mUpdateEndIdx = mUpdateIdx;
                    mFillEndIdx = mFillIdx;
                }

                // Skip to next key and start from the beginning
                for (++i; i < page->count && entries[i].key == entries[baseIdx].key; ++i) {
                }
                continue;
            }

            // Skip elements with version above lowest version
            for (; i < page->count && entries[i].key == entries[baseIdx].key && entries[i].version >= lowestVersion;
                    ++i) {
            }
        }

        // Process all main entries up to the first element that can be discarded
        auto copyStartIdx = i;
        auto copyEndIdx = i;
        for (; i < page->count && entries[i].key == entries[baseIdx].key; ++i) {
            auto size = mContext.entryOverhead();

            if (wasDelete) {
                LOG_ASSERT(sizes[i] != 0u, "Only data entry can follow a delete");
                if (entries[i].version < mMinVersion) {
                    --mFillIdx;
                    mFillSize -= size + mContext.fixedSize();
                    if (copyStartIdx == copyEndIdx) { // The delete must come from an update entry
                        LOG_ASSERT(mUpdateIdx > mUpdateEndIdx, "No element written before the delete");
                        --mUpdateIdx;
                    } else { // The delete must come from the previous main entry
                        --copyEndIdx;
                    }
                    wasDelete = false;
                    break;
                }
            }
            if (sizes[i] == 0u) {
                if (entries[i].version <= mMinVersion) {
                    break;
                }
                size += mContext.fixedSize();
                wasDelete = true;
            } else {
                size += sizes[i];
                wasDelete = false;
            }

            mFillSize += size;
            if (mFillSize > ColumnMapContext::MAX_DATA_SIZE) {
                if (mainStartIdx != mainEndIdx) {
                    LOG_ASSERT(mUpdateStartIdx == mUpdateEndIdx, "Main and update copy at the same time");
                    addCleanAction(page, mainStartIdx, mainEndIdx);
                    mainStartIdx = 0;
                    mainEndIdx = 0;
                }
                flush();
                i = baseIdx;
                goto START;
            }

            new (mFillPage->entryData() + mFillIdx) ColumnMapMainEntry(entries[baseIdx].key, entries[i].version);
            ++mFillIdx;
            ++copyEndIdx;

            // Check if the element is already the oldest readable element
            if (entries[i].version <= mMinVersion) {
                break;
            }
        }

        LOG_ASSERT(!wasDelete, "Last element must not be a delete");
        LOG_ASSERT(mFillIdx - mFillEndIdx == (copyEndIdx - copyStartIdx) + (mUpdateIdx - mUpdateEndIdx),
                "Fill count does not match actual number of written elements");

        // Invalidate the element if it can be removed completely otherwise enqueue modification of the newest pointer
        // Retry from beginning if the invalidation fails
        if (mFillIdx == mFillEndIdx) {
            if (!entries[baseIdx].newest.compare_exchange_strong(newest,
                    crossbow::to_underlying(NewestPointerTag::INVALID))) {
                i = baseIdx;
                continue;
            }
            __attribute__((unused)) auto res = mMainTableModifier.remove(entries[baseIdx].key);
            LOG_ASSERT(res, "Removing key from hash table did not succeed");
        } else {
            auto fillEntry = mFillPage->entryData() + mFillEndIdx;
            mPointerActions.emplace_back(&entries[baseIdx].newest, newest, fillEntry);

            __attribute__((unused)) auto res = mMainTableModifier.insert(entries[baseIdx].key, fillEntry, true);
            LOG_ASSERT(res, "Inserting key into hash table did not succeed");

            // No updates and copy region starts at end from previous - simply extend copy region from main
            if (mainEndIdx == copyStartIdx && mUpdateIdx == mUpdateStartIdx) {
                mainEndIdx = copyEndIdx;
            } else {
                // Flush any pending actions from previous main
                if (mainStartIdx != mainEndIdx) {
                    LOG_ASSERT(mUpdateStartIdx == mUpdateEndIdx, "Main and update copy at the same time");
                    addCleanAction(page, mainStartIdx, mainEndIdx);
                    mainStartIdx = 0;
                    mainEndIdx = 0;
                }
                // Enqueue updates
                mUpdateEndIdx = mUpdateIdx;

                // Enqueue main
                if (copyStartIdx != copyEndIdx) {
                    if (mUpdateStartIdx != mUpdateEndIdx) {
                        mCleanActions.emplace_back(mUpdatePage, mUpdateStartIdx, mUpdateEndIdx, 0);
                        mUpdateStartIdx = mUpdateEndIdx;
                    }
                    mainStartIdx = copyStartIdx;
                    mainEndIdx = copyEndIdx;
                }
            }
            mFillEndIdx = mFillIdx;
        }

        // Skip to next key
        for (; i < page->count && entries[i].key == entries[baseIdx].key; ++i) {
        }
    }

    LOG_ASSERT(i == page->count, "Not at end of page");

    // Append last pending clean action
    if (mainStartIdx != mainEndIdx) {
        LOG_ASSERT(mUpdateStartIdx == mUpdateIdx, "Main and update copy at the same time");
        addCleanAction(page, mainStartIdx, mainEndIdx);
    }

    return true;
}

bool ColumnMapPageModifier::append(InsertRecord& oldRecord) {
    while (true) {
        LOG_ASSERT(mFillIdx == mFillEndIdx, "Current fill index must be at the end index");
        LOG_ASSERT(mUpdateIdx == mUpdateEndIdx, "Current update index must be at the end index");
        bool wasDelete = false;
        if (oldRecord.newest() != 0u) {
            uint64_t lowestVersion;
            if (!processUpdates(reinterpret_cast<const UpdateLogEntry*>(oldRecord.newest()), oldRecord.baseVersion(),
                    lowestVersion, wasDelete)) {
                flush();
                continue;
            }

            auto size = mContext.entryOverhead();

            // Check if all elements were overwritten by the update log
            if (wasDelete && oldRecord.baseVersion() < mMinVersion) {
                mFillSize -= size + mContext.fixedSize();
                LOG_ASSERT(mFillIdx > mFillEndIdx, "No element written before the delete");
                --mFillIdx;
                LOG_ASSERT(mUpdateIdx > mUpdateEndIdx, "No element written before the delete");
                --mUpdateIdx;
            } else if (lowestVersion > std::max(mMinVersion, oldRecord.baseVersion())) {
                auto logEntry = LogEntry::entryFromData(reinterpret_cast<const char*>(oldRecord.value()));

                mFillSize += size + logEntry->size() - sizeof(InsertLogEntry);
                if (mFillSize > ColumnMapContext::MAX_DATA_SIZE) {
                    flush();
                    continue;
                }

                writeInsert(oldRecord.value());
            }

            // Invalidate the element if it can be removed completely
            // Retry from beginning if the invalidation fails
            if (mUpdateIdx == mUpdateEndIdx) {
                if (!oldRecord.tryInvalidate()) {
                    continue;
                }
                return false;
            }
        } else {
            auto logEntry = LogEntry::entryFromData(reinterpret_cast<const char*>(oldRecord.value()));
            auto size = mContext.entryOverhead() + logEntry->size() - sizeof(InsertLogEntry);

            mFillSize += size;
            if (mFillSize > ColumnMapContext::MAX_DATA_SIZE) {
                flush();
                continue;
            }

            writeInsert(oldRecord.value());
        }

        LOG_ASSERT(mFillIdx - mFillEndIdx == mUpdateIdx - mUpdateEndIdx,
                "Fill count does not match actual number of written elements");

        auto fillEntry = mFillPage->entryData() + mFillEndIdx;
        mPointerActions.emplace_back(&oldRecord.value()->newest, oldRecord.newest(), fillEntry);

        __attribute__((unused)) auto res = mMainTableModifier.insert(oldRecord.key(), fillEntry, false);
        LOG_ASSERT(res, "Inserting key into hash table did not succeed");

        mUpdateEndIdx = mUpdateIdx;
        mFillEndIdx = mFillIdx;
        return true;
    }
}

std::vector<ColumnMapMainPage*> ColumnMapPageModifier::done() {
    if (mFillEndIdx != 0u) {
        flushFillPage();
    } else {
        mPageManager.free(mFillPage);
    }
    mPageManager.free(mUpdatePage);

    return std::move(mPageList);
}

bool ColumnMapPageModifier::needsCleaning(const ColumnMapMainPage* page) {
    auto entries = page->entryData();
    typename std::remove_const<decltype(page->count)>::type i = 0;
    while (i < page->count) {
        // In case the record has pending updates it needs to be cleaned
        if (entries[i].newest.load() != 0x0u) {
            return true;
        }

        // If only one version is in the record it does not need cleaning
        if (i + 1 == page->count || entries[i].key != entries[i + 1].key) {
            return false;
        }

        // Skip to next key
        // The record needs cleaning if one but the first version can be purged
        auto key = entries[i].key;
        for (++i; i < page->count && entries[i].key == key; ++i) {
            if (entries[i].version < mMinVersion) {
                return true;
            }
        }
    }
    return false;
}

void ColumnMapPageModifier::addCleanAction(ColumnMapMainPage* page, uint32_t startIdx, uint32_t endIdx) {
    LOG_ASSERT(endIdx > startIdx, "End index must be larger than start index");

    // Do not copy and adjust heap if the table has no variable sized fields
    if (mContext.varSizeFieldCount() == 0u) {
        mCleanActions.emplace_back(page, startIdx, endIdx, 0u);
        return;
    }

    // Determine begin and end offset of the variable size heap
    auto heapEntries = reinterpret_cast<const ColumnMapHeapEntry*>(crossbow::align(page->recordData()
            + page->count * mContext.fixedSize(), 8u));
    auto beginOffset = heapEntries[endIdx - 1].offset;
    auto endOffset = (startIdx == 0 ? 0 : heapEntries[startIdx - 1].offset);
    LOG_ASSERT(beginOffset >= endOffset, "End offset larger than begin offset");

    auto length = beginOffset - endOffset;

    // Copy varsize heap into the fill page
    mFillHeap -= length;
    memcpy(mFillHeap, page->heapData() - beginOffset, length);

    // Add clean action with varsize heap offset correct
    auto offsetCorrect = static_cast<int32_t>(mFillPage->heapData() - mFillHeap) - static_cast<int32_t>(beginOffset);
    mCleanActions.emplace_back(page, startIdx, endIdx, offsetCorrect);
}

bool ColumnMapPageModifier::processUpdates(const UpdateLogEntry* newest, uint64_t baseVersion, uint64_t& lowestVersion,
        bool& wasDelete) {
    UpdateRecordIterator updateIter(newest, baseVersion);

    // Loop over update log
    for (; !updateIter.done(); updateIter.next()) {
        auto logEntry = LogEntry::entryFromData(reinterpret_cast<const char*>(updateIter.value()));
        auto size = mContext.entryOverhead();

        // If the previous update was a delete and the element is below the lowest active version then the
        // delete can be discarded. In this case the update index counter can simply be decremented by one as a
        // delete only writes the header entry in the fill page.
        if (wasDelete) {
            LOG_ASSERT(logEntry->type() == crossbow::to_underlying(RecordType::DATA),
                    "Only data entry can follow a delete");
            LOG_ASSERT(mUpdateIdx > mUpdateEndIdx, "Was delete but no element written");
            if (updateIter->version < mMinVersion) {
                --mUpdateIdx;
                --mFillIdx;
                mFillSize -= size + mContext.fixedSize();
                wasDelete = false;
                break;
            }
        }

        if (logEntry->type() == crossbow::to_underlying(RecordType::DELETE)) {
            // The entry this entry marks as deleted can not be read, skip deletion and break
            if (updateIter->version <= mMinVersion) {
                break;
            }
            size += mContext.fixedSize();
            wasDelete = true;
        } else {
            size += (logEntry->size() - sizeof(UpdateLogEntry));
            wasDelete = false;
        }

        mFillSize += size;
        if (mFillSize > ColumnMapContext::MAX_DATA_SIZE) {
            return false;
        }

        writeUpdate(updateIter.value());

        // Check if the element is already the oldest readable element
        if (updateIter->version <= mMinVersion) {
            break;
        }
    }

    lowestVersion = updateIter.lowestVersion();
    return true;
}

void ColumnMapPageModifier::writeUpdate(const UpdateLogEntry* entry) {
    // Write entries into the fill page
    new (mFillPage->entryData() + mFillIdx) ColumnMapMainEntry(entry->key, entry->version);

    auto logEntry = LogEntry::entryFromData(reinterpret_cast<const char*>(entry));

    // Everything stays zero initialized when the entry marks a deletion
    if (logEntry->type() != crossbow::to_underlying(RecordType::DELETE)) {
        // Write data into update page
        writeData(entry->data(), logEntry->size() - sizeof(UpdateLogEntry));
    } else {
        mUpdatePage->sizeData()[mUpdateIdx] = 0u;
        if (mContext.varSizeFieldCount() != 0u) {
            // Deletes do not have any data on the var size heap but the offsets must be correct nonetheless
            // Copy the current offset of the heap
            auto heapEntries = reinterpret_cast<ColumnMapHeapEntry*>(crossbow::align(mUpdatePage->recordData()
                    + mUpdatePage->count * mContext.fixedSize(), 8u));
            auto heapOffset = static_cast<uint32_t>(mFillPage->heapData() - mFillHeap);

            heapEntries += mUpdateIdx;
            for (decltype(mContext.varSizeFieldCount()) i = 0; i < mContext.varSizeFieldCount(); ++i) {
                new (heapEntries) ColumnMapHeapEntry(heapOffset, 0u, nullptr);

                // Advance pointer to offset entry of next field
                heapEntries += mUpdatePage->count;
            }
        }
    }

    ++mFillIdx;
    ++mUpdateIdx;
}

void ColumnMapPageModifier::writeInsert(const InsertLogEntry* entry) {
    // Write entries into the fill page
    new (mFillPage->entryData() + mFillIdx) ColumnMapMainEntry(entry->key, entry->version);

    // Write data into update page
    auto logEntry = LogEntry::entryFromData(reinterpret_cast<const char*>(entry));
    writeData(entry->data(), logEntry->size() - sizeof(InsertLogEntry));

    ++mFillIdx;
    ++mUpdateIdx;
}

void ColumnMapPageModifier::writeData(const char* data, uint32_t size) {
    // Write size into the update page
    LOG_ASSERT(size != 0, "Size must be larger than 0");
    mUpdatePage->sizeData()[mUpdateIdx] = size;

    // Copy all fixed size fields including the header (null bitmap) if the record has one into the update page
    auto srcData = data;
    auto destData = mUpdatePage->recordData();
    for (auto fieldLength : mContext.fieldLengths()) {
        memcpy(destData + mUpdateIdx * fieldLength, srcData, fieldLength);
        destData += mUpdatePage->count * fieldLength;
        srcData += fieldLength;
    }

    // Copy all variable sized fields into the fill page
    if (mContext.varSizeFieldCount() != 0) {
        srcData = crossbow::align(srcData, 4u);

        auto length = static_cast<size_t>((data + size) - srcData);
        mFillHeap -= length;
        memcpy(mFillHeap, srcData, length);

        auto heapEntries = reinterpret_cast<ColumnMapHeapEntry*>(crossbow::align(destData, 8u));
        heapEntries += mUpdateIdx;

        auto heapOffset = static_cast<uint32_t>(mFillPage->heapData() - mFillHeap);
        for (decltype(mContext.varSizeFieldCount()) i = 0; i < mContext.varSizeFieldCount(); ++i) {
            // Write heap entry and advance to next field
            auto varSize = *reinterpret_cast<const uint32_t*>(srcData);
            srcData += sizeof(uint32_t);
            new (heapEntries) ColumnMapHeapEntry(heapOffset, varSize, srcData);
            varSize = crossbow::align(varSize, 4u);
            srcData += varSize;

            // Advance pointer to offset entry of next field
            heapEntries += mUpdatePage->count;

            // Advance offset into the heap
            heapOffset -= sizeof(uint32_t) + varSize;
        }
    }
}

void ColumnMapPageModifier::flush() {
    flushFillPage();

    if (mUpdateIdx != 0u) {
        memset(mUpdatePage, 0, TELL_PAGE_SIZE);
        new (mUpdatePage) ColumnMapMainPage(mContext.fixedSizeCapacity());
        mUpdateStartIdx = 0u;
        mUpdateEndIdx = 0u;
        mUpdateIdx = 0u;
    }

    mFillPage = new (mPageManager.alloc()) ColumnMapMainPage();
    mFillHeap = mFillPage->heapData();
    mFillEndIdx = 0u;
    mFillIdx = 0u;
    mFillSize = 0u;
}

void ColumnMapPageModifier::flushFillPage() {
    LOG_ASSERT(mFillEndIdx > 0, "Trying to flush empty page");

    // Enqueue any pending update actions
    if (mUpdateStartIdx != mUpdateEndIdx) {
        mCleanActions.emplace_back(mUpdatePage, mUpdateStartIdx, mUpdateEndIdx, 0);
        mUpdateStartIdx = mUpdateEndIdx;
    }

    // Set correct count
    new (mFillPage) ColumnMapMainPage(mFillEndIdx);
    mPageList.emplace_back(mFillPage);

    // Copy sizes
    auto sizes = mFillPage->sizeData();
    for (const auto& action : mCleanActions) {
        auto srcData = action.page->sizeData() + action.startIdx;
        auto count = action.endIdx - action.startIdx;
        memcpy(sizes, srcData, count * sizeof(uint32_t));
        sizes += count;
    }
    LOG_ASSERT(sizes == mFillPage->sizeData() + mFillPage->count, "Did not copy all sizes");

    auto recordData = mFillPage->recordData();
    size_t startOffset = 0;

    // Copy all fixed size fields including the header (null bitmap) if the record has one into the fill page
    for (auto fieldLength : mContext.fieldLengths()) {
        for (const auto& action : mCleanActions) {
            auto srcData = action.page->recordData() + action.page->count * startOffset + action.startIdx * fieldLength;
            auto length = (action.endIdx - action.startIdx) * fieldLength;
            memcpy(recordData, srcData, length);
            recordData += length;
        }
        startOffset += fieldLength;
    }
    LOG_ASSERT(startOffset == mContext.fixedSize(), "Offset after adding all fixed size fields is not the fixed size");
    LOG_ASSERT(recordData == mFillPage->recordData() + mContext.fixedSize() * mFillPage->count,
            "Offset after adding all fixed size fields is not the fixed size");

    // Copy all variable size field heap entries
    // If the offset correction is 0 we can do a single memory copy otherwise we have to adjust the offset for every
    // element.
    recordData = crossbow::align(recordData, 8u);
    for (decltype(mContext.varSizeFieldCount()) i = 0; i < mContext.varSizeFieldCount(); ++i) {
        for (const auto& action : mCleanActions) {
            auto heapEntries = reinterpret_cast<const ColumnMapHeapEntry*>(crossbow::align(action.page->recordData()
                    + action.page->count * startOffset, 8u));
            auto srcData = heapEntries + (action.page->count * i) + action.startIdx;
            if (action.offsetCorrection == 0) {
                auto length = (action.endIdx - action.startIdx) * sizeof(ColumnMapHeapEntry);
                memcpy(recordData, srcData, length);
                recordData += length;
            } else {
                for (auto endData = srcData + (action.endIdx - action.startIdx); srcData != endData; ++srcData) {
                    auto newOffset = static_cast<int32_t>(srcData->offset) + action.offsetCorrection;
                    LOG_ASSERT(newOffset > 0, "Corrected offset must be larger than 0");
                    new (recordData) ColumnMapHeapEntry(newOffset, srcData->prefix);
                    recordData += sizeof(ColumnMapHeapEntry);
                }
            }
        }
    }
    mCleanActions.clear();

    // Adjust newest pointer
    for (auto& action : mPointerActions) {
        auto desired = reinterpret_cast<uintptr_t>(action.desired) | crossbow::to_underlying(NewestPointerTag::MAIN);
        while (!action.ptr->compare_exchange_strong(action.expected, desired)) {
            action.desired->newest.store(action.expected);
        }
    }
    mPointerActions.clear();
}

} // namespace deltamain
} // namespace store
} // namespace tell
