// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.

#include "buffer_manager.h"

// TODO: add timer to force flush in case the buffer hasn't reached his maximum
//       capacity after N milliseconds

namespace kdb {

Status BufferManager::Get(ByteArray* key, ByteArray** value_out) {
  // TODO: need to fix the way the value is returned here: to create a new
  //       memory space and then return.
  // TODO: make sure the live buffer doesn't need to be protected by a mutex in
  //       order to be accessed -- right now I'm relying to timing, but that may
  //       be too weak to guarantee proper access

  // read the "live" buffer
  mutex_live_write_level1_.lock();
  LOG_DEBUG("LOCK", "1 lock");
  mutex_indices_level3_.lock();
  LOG_DEBUG("LOCK", "3 lock");
  auto& buffer_live = buffers_[im_live_];
  int num_items = buffer_live.size();
  mutex_indices_level3_.unlock();
  LOG_DEBUG("LOCK", "3 unlock");
  mutex_live_write_level1_.unlock();
  LOG_DEBUG("LOCK", "1 unlock");
  bool found = false;
  Order order_found;
  for (int i = 0; i < num_items; i++) {
    auto& order = buffer_live[i];
    if (order.key == key) {
      found = true;
      order_found = order;
    }
  }
  if (found) {
    LOG_DEBUG("BufferManager::Get()", "found in buffer_live");
    if (   order_found.type == OrderType::Put
        && order_found.chunk->size() == order_found.size_value) {
      *value_out = order_found.chunk;
      return Status::OK();
    } else if (order_found.type == OrderType::Remove) {
      return Status::RemoveOrder("Unable to find entry");
    } else {
      return Status::NotFound("Unable to find entry");
    }
  }

  // prepare to read the "copy" buffer
  LOG_DEBUG("LOCK", "4 lock");
  mutex_copy_write_level4_.lock();
  LOG_DEBUG("LOCK", "5 lock");
  mutex_copy_read_level5_.lock();
  num_readers_ += 1;
  mutex_copy_read_level5_.unlock();
  LOG_DEBUG("LOCK", "5 unlock");
  mutex_copy_write_level4_.unlock();
  LOG_DEBUG("LOCK", "5 unlock");

  // read from "copy" buffer
  found = false;
  LOG_DEBUG("LOCK", "3 lock");
  mutex_indices_level3_.lock();
  auto& buffer_copy = buffers_[im_copy_];
  mutex_indices_level3_.unlock();
  LOG_DEBUG("LOCK", "3 unlock");
  for (auto& order: buffer_copy) {
    if (order.key == key) {
      found = true;
      order_found = order;
    }
  }

  // exit the "copy" buffer
  LOG_DEBUG("LOCK", "5 lock");
  mutex_copy_read_level5_.lock();
  num_readers_ -= 1;
  mutex_copy_read_level5_.unlock();
  LOG_DEBUG("LOCK", "3 unlock");
  cv_read_.notify_one();
  if (found) LOG_DEBUG("BufferManager::Get()", "found in buffer_copy");
  if (   found
      && order_found.type == OrderType::Put
      && order_found.chunk->size() == order_found.size_value) {
    *value_out = order_found.chunk;
    return Status::OK();
  } else if (   found
             && order_found.type == OrderType::Remove) {
    return Status::RemoveOrder("Unable to find entry");
  } else {
    return Status::NotFound("Unable to find entry");
  }
}


Status BufferManager::Put(ByteArray* key, ByteArray* chunk) {
  //return Write(OrderType::Put, key, value);
  return Status::InvalidArgument("BufferManager::Put() is not implemented");
}


Status BufferManager::PutChunk(ByteArray* key,
                               ByteArray* chunk,
                               uint64_t offset_chunk,
                               uint64_t size_value,
                               uint64_t size_value_compressed) {
  return WriteChunk(OrderType::Put,
                    key,
                    chunk,
                    offset_chunk,
                    size_value,
                    size_value_compressed
                   );
}


Status BufferManager::Remove(ByteArray* key) {
  // TODO: The storage engine is calling data() and size() on the chunk ByteArray.
  //       The use of SimpleByteArray here is a hack to guarantee that data()
  //       and size() won't be called on a nullptr -- this needs to be cleaned up.
  auto empty_chunk = new SimpleByteArray(nullptr, 0);
  return WriteChunk(OrderType::Remove, key, empty_chunk, 0, 0, 0);
}


Status BufferManager::WriteChunk(const OrderType& op,
                                 ByteArray* key,
                                 ByteArray* chunk,
                                 uint64_t offset_chunk,
                                 uint64_t size_value,
                                 uint64_t size_value_compressed) {
  LOG_DEBUG("LOCK", "1 lock");
  std::unique_lock<std::mutex> lock_live(mutex_live_write_level1_);
  //if (key.size() + value.size() > buffer_size_) {
  //  return Status::InvalidArgument("Entry is too large.");
  //}
  LOG_TRACE("BufferManager", "Write() key:[%s] | size chunk:%d, total size value:%d offset_chunk:%llu sizeOfBuffer:%d", key->ToString().c_str(), chunk->size(), size_value, offset_chunk, buffers_[im_live_].size());

  // not sure if I should add the item then test, or test then add the item
  buffers_[im_live_].push_back(Order{op,
                                     key,
                                     chunk,
                                     offset_chunk,
                                     size_value,
                                     size_value_compressed});
  if (offset_chunk == 0) {
    sizes_[im_live_] += key->size();
  }
  sizes_[im_live_] += chunk->size();

  if (buffers_[im_live_].size()) {
    for(auto &p: buffers_[im_live_]) {   
      LOG_TRACE("BufferManager", "Write() ITEM key_ptr:[%p] key:[%s] | size chunk:%d, total size value:%d offset_chunk:%llu sizeOfBuffer:%d sizes_[im_live_]:%d", p.key, p.key->ToString().c_str(), p.chunk->size(), p.size_value, p.offset_chunk, buffers_[im_live_].size(), sizes_[im_live_]);
    }
  } else {
    LOG_TRACE("BufferManager", "Write() ITEM no buffers_[im_live_]");
  }
  /*
  */

  // NOTE: With multi-chunk entries, the last chunks may get stuck in the
  //       buffers without being flushed to secondary storage, and the storage
  //       engine will say that it doesn't has the item as the last chunk
  //       wasn't flushed yet.
  //       The use of 'force_swap_' here is a cheap bastard way of fixing the
  //       problem, by forcing the buffer to swap and flush for every last
  //       chunk encountered in a multi-chunk entry.
  //       If a Get() directly follows a Put() with a very low latency, this still
  //       won't fix the issue: needs a better solution on the long term.
  //       Builing the value by mixing data from the storage engine and the
  //       chunk in the buffers would be the best, but would add considerable
  //       complexity.
  //       => idea: return a "RETRY" command, indicating to the client that he
  //                needs to sleep for 100ms-ish and retry?
  if (   chunk->size() + offset_chunk == size_value
      && offset_chunk > 0) {
    force_swap_ = true;
  }

  // test on size for debugging remove()
  if (sizes_[im_live_] > 64) {
    force_swap_ = true;
  }

  // TODO: remove when the calls to wait() will have been replaced
  //       by calls to wait_for() -- i.e. proper timing out
  force_swap_ = true;

  if (sizes_[im_live_] > buffer_size_ || force_swap_) {
    LOG_TRACE("BufferManager", "trying to swap");
    LOG_DEBUG("LOCK", "2 lock");
    std::unique_lock<std::mutex> lock_flush(mutex_flush_level2_);
    if (can_swap_) {
      LOG_TRACE("BufferManager", "can_swap_ == true");
      LOG_DEBUG("LOCK", "3 lock");
      std::unique_lock<std::mutex> lock_swap(mutex_indices_level3_);
      LOG_TRACE("BufferManager", "Swap buffers");
      can_swap_ = false;
      force_swap_ = false;
      std::swap(im_live_, im_copy_);
      cv_flush_.notify_one();
      LOG_DEBUG("LOCK", "3 unlock");
    } else {
      LOG_TRACE("BufferManager", "can_swap_ == false");
    }
    LOG_DEBUG("LOCK", "2 unlock");
  } else {
    LOG_TRACE("BufferManager", "will not swap");
  }

  LOG_DEBUG("LOCK", "1 unlock");
  return Status::OK();
}


void BufferManager::ProcessingLoop() {
  while(true) {
    LOG_TRACE("BufferManager", "ProcessingLoop() - start");
    LOG_DEBUG("LOCK", "2 lock");
    std::unique_lock<std::mutex> lock_flush(mutex_flush_level2_);
    if (sizes_[im_copy_] == 0) {
      LOG_TRACE("BufferManager", "ProcessingLoop() - wait");
      can_swap_ = true;
      cv_flush_.wait(lock_flush);
    }
 
    // Notify the storage engine that the buffer can be flushed
    LOG_TRACE("BM", "WAIT: Get()-flush_buffer");
    EventManager::flush_buffer.StartAndBlockUntilDone(buffers_[im_copy_]);

    // Wait for the index to notify the buffer manager
    LOG_TRACE("BM", "WAIT: Get()-clear_buffer");
    EventManager::clear_buffer.Wait();
    EventManager::clear_buffer.Done();
    
    // Wait for readers
    LOG_DEBUG("LOCK", "4 lock");
    mutex_copy_write_level4_.lock();
    while(true) {
      LOG_DEBUG("LOCK", "5 lock");
      std::unique_lock<std::mutex> lock_read(mutex_copy_read_level5_);
      if (num_readers_ == 0) break;
      LOG_DEBUG("BufferManager", "ProcessingLoop() - wait for lock_read");
      cv_read_.wait(lock_read);
    }
    LOG_DEBUG("LOCK", "5 unlock");

    // Clear flush buffer
    sizes_[im_copy_] = 0;
    buffers_[im_copy_].clear();

    if (buffers_[im_copy_].size()) {
      for(auto &p: buffers_[im_copy_]) {
        LOG_TRACE("BufferManager", "ProcessingLoop() ITEM im_copy - key_ptr:[%p] key:[%s] | size chunk:%d, total size value:%d offset_chunk:%llu sizeOfBuffer:%d sizes_[im_copy_]:%d", p.key, p.key->ToString().c_str(), p.chunk->size(), p.size_value, p.offset_chunk, buffers_[im_copy_].size(), sizes_[im_copy_]);
      }
    } else {
      LOG_TRACE("BufferManager", "ProcessingLoop() ITEM no buffers_[im_copy_]");
    }

    if (buffers_[im_live_].size()) {
      for(auto &p: buffers_[im_live_]) {
        LOG_TRACE("BufferManager", "ProcessingLoop() ITEM im_live - key_ptr:[%p] key:[%s] | size chunk:%d, total size value:%d offset_chunk:%llu sizeOfBuffer:%d sizes_[im_live_]:%d", p.key, p.key->ToString().c_str(), p.chunk->size(), p.size_value, p.offset_chunk, buffers_[im_live_].size(), sizes_[im_live_]);
      }
    } else {
      LOG_TRACE("BufferManager", "ProcessingLoop() ITEM no buffers_[im_live_]");
    }

    can_swap_ = true;
    mutex_copy_write_level4_.unlock();
    LOG_DEBUG("LOCK", "4 unlock");
    LOG_DEBUG("LOCK", "2 unlock");
  }
}

};
