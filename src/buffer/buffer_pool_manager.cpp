//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// buffer_pool_manager.cpp
//
// Identification: src/buffer/buffer_pool_manager.cpp
//
// Copyright (c) 2015-2019, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//
#include <list>
#include <unordered_map>

#include "buffer/buffer_pool_manager.h"
#include "common/logger.h"

namespace bustub {

BufferPoolManager::BufferPoolManager(size_t pool_size, DiskManager *disk_manager, LogManager *log_manager)
    : pool_size_(pool_size), disk_manager_(disk_manager), log_manager_(log_manager) {
  // We allocate a consecutive memory space for the buffer pool.
  pages_ = new Page[pool_size_];
  replacer_ = new LRUReplacer(pool_size);

  // Initially, every page is in the free list.
  for (size_t i = 0; i < pool_size_; ++i) {
    free_list_.emplace_back(static_cast<int>(i));
  }
}

BufferPoolManager::~BufferPoolManager() {
  delete[] pages_;
  delete replacer_;
}

Page *BufferPoolManager::FetchPageImpl(page_id_t page_id) {
  // 1.     Search the page table for the requested page (P).
  // 1.1    If P exists, pin it and return it immediately.
  // 1.2    If P does not exist, find a replacement page (R) from either the
  // free list or the replacer.
  //        Note that pages are always found from the free list first.
  // 2.     If R is dirty, write it back to the disk.
  // 3.     Delete R from the page table and insert P.
  // 4.     Update P's metadata, read in the page content from disk, and then
  // return a pointer to P.
  std::lock_guard<std::mutex> guard(mutex_);
  // LOG(DEBUG) << "Fetching #page: " << page_id;
  if (Exist(page_id)) {
    // If this page is alreay in buffer pool, simply returns it
    Page *page = &pages_[page_table_[page_id]];
    CHECK(page->GetPageId() == page_id) << page_id << " " << page->GetPageId();
    page->pin_count_++;
    // LOG(DEBUG) << "Fetching from the exising #page " << page_id << " pin_count: " << page->pin_count_;
    return page;
  } else {
    // Otherwise found a new place and read that page from disk.
    int frame_id = -1;
    if (!free_list_.empty()) {
      // If we can get a frame from free_list_
      frame_id = free_list_.front();
      free_list_.erase(free_list_.begin());
    } else if (replacer_->Size() > 0) {
      // Otherwise get a frame from the replacer, for LRU, this frame should be
      // the least recently used one
      page_id_t page_id;
      CHECK(replacer_->Victim(&page_id));
      // Remove this frame from the replacer_
      replacer_->Pin(page_id);
      frame_id = page_table_[page_id];
      // LOG(DEBUG) << "Erasing page_id: " << page_id;
      // page_table_.erase(page_id);
    } else {
      // No place to put this page.
      // throw Exception("Out of Memory.");
      return nullptr;
    }
    CHECK(page_id >= 0) << "Expected page id greater or equal to 0: " << page_id;
    // LOG(DEBUG) << "Fetching a new #page " << page_id << " to frame " << frame_id;
    Page *page = &pages_[frame_id];
    // CHECK(page->page_id_ >= 0) << page_id;
    if (page->is_dirty_) {
      // Flush the old page back to disk if dirty
      FlushPageImpl(page->page_id_);
    }
    // LOG(DEBUG) << "Erasing page_id: " << page->page_id_;
    page_table_.erase(page->page_id_);
    page_table_[page_id] = frame_id;
    page->page_id_ = page_id;
    page->pin_count_ = 1;
    page->ResetMemory();
    CHECK(page->GetPageId() == page_id) << page_id << " " << page->GetPageId();
    disk_manager_->ReadPage(page_id, page->GetData());
    return page;
  }
}

bool BufferPoolManager::UnpinPageImpl(page_id_t page_id, bool is_dirty) {
  std::lock_guard<std::mutex> guard(mutex_);
  // LOG(DEBUG) << "Unpinning #page: " << page_id;
  CHECK(Exist(page_id)) << "Expected page exists: " << page_id;
  Page *page = &pages_[page_table_[page_id]];
  CHECK(page->GetPageId() == page_id) << page_id << " " << page->GetPageId();
  CHECK(page->pin_count_ > 0) << page_id;
  page->pin_count_--;
  page->is_dirty_ = is_dirty;
  if (page->pin_count_ == 0) {
    if (page->is_dirty_) {
      FlushPageImpl(page_id);
    }
    replacer_->Unpin(page_id);
  }
  return true;
}

bool BufferPoolManager::FlushPageImpl(page_id_t page_id) {
  // std::lock_guard<std::mutex> guard(mutex_);
  // Make sure you call DiskManager::WritePage!
  if (!Exist(page_id)) {
    return false;
  } else {
    // LOG(DEBUG) << "Flusing # page: " << page_id;
    Page *page = &pages_[page_table_[page_id]];
    CHECK(page->GetPageId() == page_id) << page_id << " " << page->GetPageId();
    disk_manager_->WritePage(page_id, page->GetData());
    page->is_dirty_ = false;
    // LOG(DEBUG) << "Flushed # page: " << page_id;
    return true;
  }
}

Page *BufferPoolManager::NewPageImpl(page_id_t *page_id) {
  // 0.   Make sure you call DiskManager::AllocatePage!
  // 1.   If all the pages in the buffer pool are pinned, return nullptr.
  // 2.   Pick a victim page P from either the free list or the replacer. Always
  // pick from the free list first.
  // 3.   Update P's metadata, zero out memory and add P to the page table.
  // 4.   Set the page ID output parameter. Return a pointer to P.
  std::lock_guard<std::mutex> guard(mutex_);
  page_id_t new_page_id = page_id_++;
  CHECK(new_page_id >= 0);
  // LOG(DEBUG) << "New #page: " << new_page_id;
  int frame_id = -1;
  if (!free_list_.empty()) {
    // If we can get a frame from free_list_
    frame_id = free_list_.front();
    free_list_.erase(free_list_.begin());
  } else if (replacer_->Size() > 0) {
    page_id_t page_id;
    CHECK(replacer_->Victim(&page_id));
    // Remove this frame from the replacer_
    replacer_->Pin(page_id);
    frame_id = page_table_[page_id];
    // LOG(DEBUG) << "Erasing page_id: " << page_id;
    page_table_.erase(page_id);
  } else {
    // throw Exception("Out of Memory.");
    return nullptr;
  }
  CHECK(frame_id != -1) << "Expected find a free frame.";
  Page *page = &pages_[frame_id];
  if (page->is_dirty_) {
    FlushPageImpl(page->page_id_);
  }
  page->ResetMemory();
  page->page_id_ = new_page_id;
  page->pin_count_ = 1;
  page_table_[new_page_id] = frame_id;
  *page_id = new_page_id;
  CHECK(page->GetPageId() == new_page_id) << *page_id << " " << page->GetPageId();
  return page;
}

bool BufferPoolManager::DeletePageImpl(page_id_t page_id) {
  // 0.   Make sure you call DiskManager::DeallocatePage!
  // 1.   Search the page table for the requested page (P).
  // 1.   If P does not exist, return true.
  // 2.   If P exists, but has a non-zero pin-count, return false. Someone is
  // using the page.
  // 3.   Otherwise, P can be deleted. Remove P from the page table, reset its
  // metadata and return it to the free list.
  std::lock_guard<std::mutex> guard(mutex_);
  // LOG(DEBUG) << "Delete #page: " << page_id;
  if (!Exist(page_id)) {
    return true;
  } else {
    // disk_manager_->DeallocatePage(page_id);
    Page *page = &pages_[page_table_[page_id]];
    CHECK(page->GetPageId() == page_id) << page_id << " " << page->GetPageId();
    if (page->pin_count_ > 0) {
      return false;
    } else {
      page->ResetMemory();
      page->page_id_ = INVALID_PAGE_ID;
      page->pin_count_ = 0;
      free_list_.insert(free_list_.end(), page_table_[page_id]);
      // LOG(DEBUG) << "Erasing page_id: " << page_id;
      page_table_.erase(page_id);
      return false;
    }
  }
}

void BufferPoolManager::DebugOutput() const {
  // for (auto it : page_table_) {
  //   // LOG(DEBUG) << "#page: " << it.first << " --> " << it.second << " count: " << pages_[it.second].pin_count_;
  // }
}

void BufferPoolManager::FlushAllPagesImpl() {
  // You can do it!
  std::lock_guard<std::mutex> guard(mutex_);
  for (auto &it : page_table_) {
    FlushPageImpl(it.first);
  }
}

bool BufferPoolManager::Exist(page_id_t page_id) {
  // DebugOutput();
  return page_table_.find(page_id) != page_table_.end();
}

}  // namespace bustub
