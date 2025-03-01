/* Copyright (c) 2021 Xie Meiyi(xiemeiyi@hust.edu.cn) and OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Meiyi & Longda on 2021/4/13.
//
#include "storage/record/record_manager.h"
#include "rc.h"
#include "common/log/log.h"
#include "common/lang/bitmap.h"
#include "storage/common/condition_filter.h"
#include "storage/trx/trx.h"

using namespace common;

int align8(int size)
{
  return size / 8 * 8 + ((size % 8 == 0) ? 0 : 8);
}

int page_fix_size()
{
  return sizeof(PageHeader);
}

int page_record_capacity(int page_size, int record_size)
{
  // (record_capacity * record_size) + record_capacity/8 + 1 <= (page_size - fix_size)
  // ==> record_capacity = ((page_size - fix_size) - 1) / (record_size + 0.125)
  return (int)((page_size - page_fix_size() - 1) / (record_size + 0.125));
}

int page_bitmap_size(int record_capacity)
{
  return record_capacity / 8 + ((record_capacity % 8 == 0) ? 0 : 1);
}

int page_header_size(int record_capacity)
{
  const int bitmap_size = page_bitmap_size(record_capacity);
  return align8(page_fix_size() + bitmap_size);
}
////////////////////////////////////////////////////////////////////////////////
RecordPageIterator::RecordPageIterator()
{}
RecordPageIterator::~RecordPageIterator()
{}

void RecordPageIterator::init(RecordPageHandler &record_page_handler, SlotNum start_slot_num /*=0*/)
{
  record_page_handler_ = &record_page_handler;
  page_num_ = record_page_handler.get_page_num();
  bitmap_.init(record_page_handler.bitmap_, record_page_handler.page_header_->record_capacity);
  next_slot_num_ = bitmap_.next_setted_bit(start_slot_num);
}

bool RecordPageIterator::has_next()
{
  return -1 != next_slot_num_;
}

RC RecordPageIterator::next(Record &record)
{
  record.set_rid(page_num_, next_slot_num_);
  record.set_data(record_page_handler_->get_record_data(record.rid().slot_num));

  if (next_slot_num_ >= 0) {
    next_slot_num_ = bitmap_.next_setted_bit(next_slot_num_ + 1);
  }
  return record.rid().slot_num != -1 ? RC::SUCCESS : RC::RECORD_EOF;
}

////////////////////////////////////////////////////////////////////////////////

RecordPageHandler::~RecordPageHandler()
{
  cleanup();
}

RC RecordPageHandler::init(DiskBufferPool &buffer_pool, PageNum page_num, bool readonly)
{
  if (disk_buffer_pool_ != nullptr) {
    LOG_WARN("Disk buffer pool has been opened for page_num %d.", page_num);
    return RC::RECORD_OPENNED;
  }

  RC ret = RC::SUCCESS;
  if ((ret = buffer_pool.get_this_page(page_num, &frame_)) != RC::SUCCESS) {
    LOG_ERROR("Failed to get page handle from disk buffer pool. ret=%d:%s", ret, strrc(ret));
    return ret;
  }

  if (readonly) {
    frame_->read_latch();
  } else {
    frame_->write_latch();
  }
  readonly_ = readonly;
  char *data = frame_->data();

  disk_buffer_pool_ = &buffer_pool;

  page_header_ = (PageHeader *)(data);
  bitmap_ = data + page_fix_size();
  LOG_TRACE("Successfully init page_num %d.", page_num);
  return ret;
}

RC RecordPageHandler::recover_init(DiskBufferPool &buffer_pool, PageNum page_num)
{
  if (disk_buffer_pool_ != nullptr) {
    LOG_WARN("Disk buffer pool has been opened for page_num %d.", page_num);
    return RC::RECORD_OPENNED;
  }

  RC ret = RC::SUCCESS;
  if ((ret = buffer_pool.get_this_page(page_num, &frame_)) != RC::SUCCESS) {
    LOG_ERROR("Failed to get page handle from disk buffer pool. ret=%d:%s", ret, strrc(ret));
    return ret;
  }

  char *data = frame_->data();

  disk_buffer_pool_ = &buffer_pool;

  page_header_ = (PageHeader *)(data);
  bitmap_ = data + page_fix_size();

  buffer_pool.recover_page(page_num);

  LOG_TRACE("Successfully init page_num %d.", page_num);
  return ret;
}

RC RecordPageHandler::init_empty_page(DiskBufferPool &buffer_pool, PageNum page_num, int record_size)
{
  RC ret = init(buffer_pool, page_num, false/*readonly*/);
  if (ret != RC::SUCCESS) {
    LOG_ERROR("Failed to init empty page page_num:record_size %d:%d.", page_num, record_size);
    return ret;
  }

  int page_size = BP_PAGE_DATA_SIZE;
  int record_phy_size = align8(record_size);
  page_header_->record_num = 0;
  page_header_->record_capacity = page_record_capacity(page_size, record_phy_size);
  page_header_->record_real_size = record_size;
  page_header_->record_size = record_phy_size;
  page_header_->first_record_offset = page_header_size(page_header_->record_capacity);
  bitmap_ = frame_->data() + page_fix_size();

  memset(bitmap_, 0, page_bitmap_size(page_header_->record_capacity));

  if ((ret = buffer_pool.flush_page(*frame_)) != RC::SUCCESS) {
    LOG_ERROR("Failed to flush page header %d:%d.", page_num);
    return ret;
  }

  return RC::SUCCESS;
}

RC RecordPageHandler::cleanup()
{
  if (disk_buffer_pool_ != nullptr) {
    if (readonly_) {
      frame_->read_unlatch();
    } else {
      frame_->write_unlatch();
    }
    disk_buffer_pool_->unpin_page(frame_);
    disk_buffer_pool_ = nullptr;
  }

  return RC::SUCCESS;
}

RC RecordPageHandler::insert_record(const char *data, RID *rid)
{
  ASSERT(readonly_ == false, "cannot insert record into page while the page is readonly");

  if (page_header_->record_num == page_header_->record_capacity) {
    LOG_WARN("Page is full, page_num %d:%d.", frame_->page_num());
    return RC::RECORD_NOMEM;
  }

  // 找到空闲位置
  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  int index = bitmap.next_unsetted_bit(0);
  bitmap.set_bit(index);
  page_header_->record_num++;

  // assert index < page_header_->record_capacity
  char *record_data = get_record_data(index);
  memcpy(record_data, data, page_header_->record_real_size);

  frame_->mark_dirty();

  if (rid) {
    rid->page_num = get_page_num();
    rid->slot_num = index;
  }

  // LOG_TRACE("Insert record. rid page_num=%d, slot num=%d", get_page_num(), index);
  return RC::SUCCESS;
}

RC RecordPageHandler::recover_insert_record(const char *data, RID *rid)
{
  if (page_header_->record_num == page_header_->record_capacity) {
    LOG_WARN("Page is full, page_num %d:%d.", frame_->page_num());
    return RC::RECORD_NOMEM;
  }
  if (rid->slot_num >= page_header_->record_capacity) {
    LOG_WARN("slot_num illegal, slot_num(%d) > record_capacity(%d).", rid->slot_num, page_header_->record_capacity);
    return RC::RECORD_NOMEM;
  }

  // 更新位图
  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  bitmap.set_bit(rid->slot_num);
  page_header_->record_num++;

  // 恢复数据
  char *record_data = get_record_data(rid->slot_num);
  memcpy(record_data, data, page_header_->record_real_size);

  frame_->mark_dirty();

  return RC::SUCCESS;
}

RC RecordPageHandler::delete_record(const RID *rid)
{
  ASSERT(readonly_ == false, "cannot delete record from page while the page is readonly");

  if (rid->slot_num >= page_header_->record_capacity) {
    LOG_ERROR("Invalid slot_num %d, exceed page's record capacity, page_num %d.", rid->slot_num, frame_->page_num());
    return RC::INVALID_ARGUMENT;
  }

  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  if (bitmap.get_bit(rid->slot_num)) {
    bitmap.clear_bit(rid->slot_num);
    page_header_->record_num--;
    frame_->mark_dirty();

    if (page_header_->record_num == 0) {
      // PageNum page_num = get_page_num();
      cleanup();
    }
    return RC::SUCCESS;
  } else {
    LOG_DEBUG("Invalid slot_num %d, slot is empty, page_num %d.", rid->slot_num, frame_->page_num());
    return RC::RECORD_RECORD_NOT_EXIST;
  }
}

RC RecordPageHandler::get_record(const RID *rid, Record *rec)
{
  if (rid->slot_num >= page_header_->record_capacity) {
    LOG_ERROR("Invalid slot_num:%d, exceed page's record capacity, page_num %d.", rid->slot_num, frame_->page_num());
    return RC::RECORD_INVALIDRID;
  }

  Bitmap bitmap(bitmap_, page_header_->record_capacity);
  if (!bitmap.get_bit(rid->slot_num)) {
    LOG_ERROR("Invalid slot_num:%d, slot is empty, page_num %d.", rid->slot_num, frame_->page_num());
    return RC::RECORD_RECORD_NOT_EXIST;
  }

  rec->set_rid(*rid);
  rec->set_data(get_record_data(rid->slot_num));
  return RC::SUCCESS;
}

PageNum RecordPageHandler::get_page_num() const
{
  if (nullptr == page_header_) {
    return (PageNum)(-1);
  }
  return frame_->page_num();
}

bool RecordPageHandler::is_full() const
{
  return page_header_->record_num >= page_header_->record_capacity;
}

////////////////////////////////////////////////////////////////////////////////

RecordFileHandler::~RecordFileHandler()
{
  this->close();
}

RC RecordFileHandler::init(DiskBufferPool *buffer_pool)
{
  if (disk_buffer_pool_ != nullptr) {
    LOG_ERROR("record file handler has been openned.");
    return RC::RECORD_OPENNED;
  }

  disk_buffer_pool_ = buffer_pool;

  RC rc = init_free_pages();

  LOG_INFO("open record file handle done. rc=%s", strrc(rc));
  return RC::SUCCESS;
}

void RecordFileHandler::close()
{
  if (disk_buffer_pool_ != nullptr) {
    free_pages_.clear();
    disk_buffer_pool_ = nullptr;
  }
}

RC RecordFileHandler::init_free_pages()
{
  // 遍历当前文件上所有页面，找到没有满的页面
  // 这个效率很低，会降低启动速度
  // NOTE: 由于是初始化时的动作，所以不需要加锁控制并发

  RC rc = RC::SUCCESS;
  BufferPoolIterator bp_iterator;
  bp_iterator.init(*disk_buffer_pool_);
  RecordPageHandler record_page_handler;
  PageNum current_page_num = 0;
  while (bp_iterator.has_next()) {
    current_page_num = bp_iterator.next();
    rc = record_page_handler.init(*disk_buffer_pool_, current_page_num, true/*readonly*/);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to init record page handler. page num=%d, rc=%d:%s", current_page_num, rc, strrc(rc));
      return rc;
    }

    if (!record_page_handler.is_full()) {
      free_pages_.insert(current_page_num);
    }
    record_page_handler.cleanup();
  }
  LOG_INFO("record file handler init free pages done. free page num=%d, rc=%s", free_pages_.size(), strrc(rc));
  return rc;
}

RC RecordFileHandler::insert_record(const char *data, int record_size, RID *rid)
{
  RC ret = RC::SUCCESS;
  // 找到没有填满的页面

  RecordPageHandler record_page_handler;
  bool page_found = false;
  PageNum current_page_num = 0;
  lock_.lock();
  while (!free_pages_.empty()) {
    current_page_num = *free_pages_.begin();
    ret = record_page_handler.init(*disk_buffer_pool_, current_page_num, false/*readonly*/);
    if (ret != RC::SUCCESS) {
      lock_.unlock();
      LOG_WARN("failed to init record page handler. page num=%d, rc=%d:%s", current_page_num, ret, strrc(ret));
      return ret;
    }

    if (!record_page_handler.is_full()) {
      page_found = true;
      break;
    }
    record_page_handler.cleanup();
    free_pages_.erase(free_pages_.begin());
  }
  lock_.unlock(); // 如果找到了一个有效的页面，那么此时已经拿到了页面的写锁

  // 找不到就分配一个新的页面
  if (!page_found) {
    Frame *frame = nullptr;
    if ((ret = disk_buffer_pool_->allocate_page(&frame)) != RC::SUCCESS) {
      LOG_ERROR("Failed to allocate page while inserting record. ret:%d", ret);
      return ret;
    }

    current_page_num = frame->page_num();
    ret = record_page_handler.init_empty_page(*disk_buffer_pool_, current_page_num, record_size);
    if (ret != RC::SUCCESS) {
      frame->unpin();
      LOG_ERROR("Failed to init empty page. ret:%d", ret);
      // this is for allocate_page
      return ret;
    }

    frame->unpin(); // frame 在allocate_page的时候，是有一个pin的，在init_empty_page时又会增加一个，所以这里手动释放一个

    // 这里的加锁顺序看起来与上面是相反的，但是不会出现死锁
    // 上面的逻辑是先加lock锁，然后加页面写锁，这里是先加上
    // 了页面写锁，然后加lock的锁，但是不会引起死锁。
    // 为什么？
    lock_.lock();
    free_pages_.insert(current_page_num);
    lock_.unlock();
  }

  // 找到空闲位置
  return record_page_handler.insert_record(data, rid);
}

RC RecordFileHandler::recover_insert_record(const char *data, int record_size, RID *rid)
{
  RC ret = RC::SUCCESS;
  RecordPageHandler record_page_handler;

  ret = record_page_handler.recover_init(*disk_buffer_pool_, rid->page_num);
  if (ret != RC::SUCCESS) {
    LOG_WARN("failed to init record page handler. page num=%d, rc=%d:%s", rid->page_num, ret, strrc(ret));
    return ret;
  }

  return record_page_handler.recover_insert_record(data, rid);
}

RC RecordFileHandler::delete_record(const RID *rid)
{
  RC rc = RC::SUCCESS;
  RecordPageHandler page_handler;
  if ((rc = page_handler.init(*disk_buffer_pool_, rid->page_num, false/*readonly*/)) != RC::SUCCESS) {
    LOG_ERROR("Failed to init record page handler.page number=%d. rc=%s", rid->page_num, strrc(rc));
    return rc;
  }
  rc = page_handler.delete_record(rid);
  page_handler.cleanup(); // 📢 这里注意要清理掉资源，否则会与insert_record中的加锁顺序冲突而可能出现死锁
  if (rc == RC::SUCCESS) {
    lock_.lock();
    free_pages_.insert(rid->page_num);
    lock_.unlock();
  }
  return rc;
}

RC RecordFileHandler::get_record(RecordPageHandler &page_handler, const RID *rid, bool readonly, Record *rec)
{
  RC ret = RC::SUCCESS;
  if (nullptr == rid || nullptr == rec) {
    LOG_ERROR("Invalid rid %p or rec %p, one of them is null.", rid, rec);
    return RC::INVALID_ARGUMENT;
  }

  if ((ret != page_handler.init(*disk_buffer_pool_, rid->page_num, readonly)) != RC::SUCCESS) {
    LOG_ERROR("Failed to init record page handler.page number=%d", rid->page_num);
    return ret;
  }

  return page_handler.get_record(rid, rec);
}

RC RecordFileHandler::visit_record(const RID &rid, bool readonly, std::function<void(Record &)> visitor)
{
  RC rc = RC::SUCCESS;

  RecordPageHandler page_handler;
  if ((rc != page_handler.init(*disk_buffer_pool_, rid.page_num, readonly)) != RC::SUCCESS) {
    LOG_ERROR("Failed to init record page handler.page number=%d", rid.page_num);
    return rc;
  }

  Record record;
  rc = page_handler.get_record(&rid, &record);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to get record from record page handle. rid=%s, rc=%s", rid.to_string().c_str(), strrc(rc));
    return rc;
  }

  visitor(record);
  return rc;
}

////////////////////////////////////////////////////////////////////////////////

RecordFileScanner::~RecordFileScanner()
{
  close_scan();
}

RC RecordFileScanner::open_scan(Table *table, 
                                DiskBufferPool &buffer_pool,
                                Trx *trx,
                                bool readonly,
                                ConditionFilter *condition_filter)
{
  close_scan();

  table_ = table;
  disk_buffer_pool_ = &buffer_pool;
  trx_ = trx;
  readonly_ = readonly;

  RC rc = bp_iterator_.init(buffer_pool);
  if (rc != RC::SUCCESS) {
    LOG_WARN("failed to init bp iterator. rc=%d:%s", rc, strrc(rc));
    return rc;
  }
  condition_filter_ = condition_filter;

  rc = fetch_next_record();
  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  return rc;
}

RC RecordFileScanner::fetch_next_record()
{
  RC rc = RC::SUCCESS;
  if (record_page_iterator_.is_valid()) {
    rc = fetch_next_record_in_page();
    if (rc == RC::SUCCESS || rc != RC::RECORD_EOF) {
      return rc;
    }
  }

  while (bp_iterator_.has_next()) {
    PageNum page_num = bp_iterator_.next();
    record_page_handler_.cleanup();
    rc = record_page_handler_.init(*disk_buffer_pool_, page_num, readonly_);
    if (rc != RC::SUCCESS) {
      LOG_WARN("failed to init record page handler. rc=%d:%s", rc, strrc(rc));
      return rc;
    }

    record_page_iterator_.init(record_page_handler_);
    rc = fetch_next_record_in_page();
    if (rc == RC::SUCCESS || rc != RC::RECORD_EOF) {
      return rc;
    }
  }
  next_record_.rid().slot_num = -1;
  return RC::RECORD_EOF;
}

RC RecordFileScanner::fetch_next_record_in_page()
{
  RC rc = RC::SUCCESS;
  while (record_page_iterator_.has_next()) {
    rc = record_page_iterator_.next(next_record_);
    if (rc != RC::SUCCESS) {
      return rc;
    }

    if (condition_filter_ != nullptr && !condition_filter_->filter(next_record_)) {
      continue;
    }

    if (trx_ == nullptr) {
      return rc;
    }

    rc = trx_->visit_record(table_, next_record_, readonly_);
    if (rc == RC::RECORD_INVISIBLE) {
      continue;
    }
    return rc;
  }

  next_record_.rid().slot_num = -1;
  return RC::RECORD_EOF;
}

RC RecordFileScanner::close_scan()
{
  if (disk_buffer_pool_ != nullptr) {
    disk_buffer_pool_ = nullptr;
  }

  if (condition_filter_ != nullptr) {
    condition_filter_ = nullptr;
  }

  record_page_handler_.cleanup();

  return RC::SUCCESS;
}

bool RecordFileScanner::has_next()
{
  return next_record_.rid().slot_num != -1;
}

RC RecordFileScanner::next(Record &record)
{
  record = next_record_;

  RC rc = fetch_next_record();
  if (rc == RC::RECORD_EOF) {
    rc = RC::SUCCESS;
  }
  return rc;
}
