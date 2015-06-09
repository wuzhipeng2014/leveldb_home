// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "../db/table_cache.h"

#include "../db/filename.h"
#include "../include/leveldb/env.h"
#include "../include/leveldb/table.h"
#include "../util/coding.h"

namespace leveldb {

struct TableAndFile {
  RandomAccessFile* file;
  Table* table;
};

static void DeleteEntry(const Slice& key, void* value) {
  TableAndFile* tf = reinterpret_cast<TableAndFile*>(value);
  delete tf->table;
  delete tf->file;
  delete tf;
}

static void UnrefEntry(void* arg1, void* arg2) {
  Cache* cache = reinterpret_cast<Cache*>(arg1);
  Cache::Handle* h = reinterpret_cast<Cache::Handle*>(arg2);
  cache->Release(h);
}

TableCache::TableCache(const std::string& dbname,
                       const Options* options,
                       int entries)
    : env_(options->env),
      dbname_(dbname),
      options_(options),
      cache_(NewLRUCache(entries)) {
}

TableCache::~TableCache() {
  delete cache_;
}

//从table cache中根据file_number查找指定的table，将找到的结果存储在handle指向的指针中
Status TableCache::FindTable(uint64_t file_number, uint64_t file_size,
                             Cache::Handle** handle) {
  Status s;
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  Slice key(buf, sizeof(buf));
  *handle = cache_->Lookup(key);
  if (*handle == NULL) { //如果在cache中没有找到table，则根据file_number和dbnmae_打开一个RadomAccessFile
   //fname的格式为：<db name>.<filenumber(%6u)>.sst
	  std::string fname = TableFileName(dbname_, file_number);
    RandomAccessFile* file = NULL;
    Table* table = NULL;
    s = env_->NewRandomAccessFile(fname, &file);
    if (!s.ok()) { 
      std::string old_fname = SSTTableFileName(dbname_, file_number);
      if (env_->NewRandomAccessFile(old_fname, &file).ok()) {
        s = Status::OK();
      }
    }
    if (s.ok()) {
      s = Table::Open(*options_, file, file_size, &table);
    }

    if (!s.ok()) {
      assert(table == NULL);
      delete file;
      // We do not cache error results so that if the error is transient,
      // or somebody repairs the file, we recover automatically.
    } else {
      TableAndFile* tf = new TableAndFile;
      tf->file = file;
      tf->table = table;
	  //将找到的table插入到cache中
      *handle = cache_->Insert(key, tf, 1, &DeleteEntry);
    }
  }
  return s;
}

Iterator* TableCache::NewIterator(const ReadOptions& options,
                                  uint64_t file_number,
                                  uint64_t file_size,
                                  Table** tableptr) {
  if (tableptr != NULL) {
    *tableptr = NULL;
  }

  Cache::Handle* handle = NULL;
  //从cache对象中取出table对象指针
  Status s = FindTable(file_number, file_size, &handle);
  if (!s.ok()) {
    return NewErrorIterator(s);
  }
  Table* table = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
  //调用table的newiterator函数返回iterator对象
  Iterator* result = table->NewIterator(options);
  //为iterator注册一个clearup函数
  result->RegisterCleanup(&UnrefEntry, cache_, handle);
  if (tableptr != NULL) {
    *tableptr = table;
  }
  return result;
}

//这是一个查找函数，如果在指定文集爱你中seek到internal key （k），就调用saver指定的函数
Status TableCache::Get(const ReadOptions& options,
                       uint64_t file_number,
                       uint64_t file_size,
                       const Slice& k,
                       void* arg,
                       void (*saver)(void*, const Slice&, const Slice&)) {
  Cache::Handle* handle = NULL;
  Status s = FindTable(file_number, file_size, &handle);
  if (s.ok()) {
    Table* t = reinterpret_cast<TableAndFile*>(cache_->Value(handle))->table;
    s = t->InternalGet(options, k, arg, saver);
    cache_->Release(handle);
  }
  return s;
}

//该函数用以清除指定文件所有cache的entry，就是根据file number清除chache对象
void TableCache::Evict(uint64_t file_number) {
  char buf[sizeof(file_number)];
  EncodeFixed64(buf, file_number);
  cache_->Erase(Slice(buf, sizeof(buf)));
}

}  // namespace leveldb
