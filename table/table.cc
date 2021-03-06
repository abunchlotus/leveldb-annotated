// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "leveldb/table.h"

#include "leveldb/cache.h"
#include "leveldb/comparator.h"
#include "leveldb/env.h"
#include "leveldb/filter_policy.h"
#include "leveldb/options.h"
#include "table/block.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/two_level_iterator.h"
#include "util/coding.h"

namespace leveldb {

// Table类主要是完成读取工作
// 这里面是真正存放东西的地方
// 只会处理下面两样
// - meta data 实际上就是filter
// - data block index

struct Table::Rep {
  ~Rep() {
    // 释放filter读者即FilterBlockReader
    delete filter;
    // 释放filter的数据部分
    delete [] filter_data;
    // 释放data block index
    delete index_block;
  }
  // 传进来的选项
  Options options;
  // 状态
  Status status;
  // 随机访问的文件
  RandomAccessFile* file;
  // 缓存中的id?
  // 主要是用在LRUCache里
  uint64_t cache_id;
  // filter block的读者(或者叫读取器更合适)
  // 按照filter block的格式将filter读出来
  FilterBlockReader* filter;
  // filter需要使用到的数据
  // 也就是filter在内存中的二进制表示
  const char* filter_data;

  // meta block index handle
  // meta block index index实际上就是Footer里面的那个
  // meta_block_index_handle
  BlockHandle metaindex_handle;  // Handle to metaindex_block: saved from footer
  // data block index
  Block* index_block;
};

// 读取sst文件
// 工厂类，这里将table传进来，然后将结果放到table里面传回去。
// 总结一下：open的时候，只会把data block index和meta block读出来。
// 这是因为data block index里面存有key的信息，可以直接用来进行检索
// 但是并不是所有的key都在里面。
// meta block index里面只有一个filter.name和offset/size
// 存放meta block index是没有什么意义的。
// 所以为了简便起见，也是为了压缩内存
// 这里只存放了
// - data block index
// - meta block
Status Table::Open(const Options& options,
                   RandomAccessFile* file,
                   uint64_t size,
                   Table** table) {
  // 一开始置空
  // 如果需要读取的size小于Footer的长度
  // 那么往上报错
  *table = nullptr;
  if (size < Footer::kEncodedLength) {
    return Status::Corruption("file is too short to be an sstable");
  }

  // 如果size足够，那么开始把footer读出来
  char footer_space[Footer::kEncodedLength];
  Slice footer_input;
  // 1. 读取footer，消费一个IOPS
  Status s = file->Read(size - Footer::kEncodedLength, Footer::kEncodedLength,
                        &footer_input, footer_space);
  if (!s.ok()) return s;

  // 读出来footer之后
  Footer footer;
  s = footer.DecodeFrom(&footer_input);
  if (!s.ok()) return s;

  // 这里把data block index读出来
  // 注意，偏移由footer.index_handle来指定
  // Read the index block
  BlockContents index_block_contents;
  if (s.ok()) {
    ReadOptions opt;
    if (options.paranoid_checks) {
      opt.verify_checksums = true;
    }
    // 这里只是把内容从文件中移到
    // index_block_contents
    // 这段内存里面
    // 消费第二个IOPS
    // 并且从代码看来，这个文件在打开的时候，应该是使用了cache的。
    // 也就是说，没有使用O_DIRECT的方式来打开。
    // footer.index_handle()指向的是data block index的offset & size.
    s = ReadBlock(file, opt, footer.index_handle(), &index_block_contents);
  }

  if (s.ok()) {
    // We've successfully read the footer and the index block: we're
    // ready to serve requests.
    // 读取成功之后，那么这里利用内存index_block_contents
    // 生成block对象
    // 所以除去footer之外，这里data block index是一个生成的块
    Block* index_block = new Block(index_block_contents);
    // 生成好之后，这里开始设置rep
    // 相当于工厂类，需要在这里进行生成和处理
    Rep* rep = new Table::Rep;
    rep->options = options;
    rep->file = file;
    // 取出meta block index index
    rep->metaindex_handle = footer.metaindex_handle();
    // 设置data block index
    rep->index_block = index_block;
    // 设置cache id
    // 后面可以看一下如何利用这个id在cache中搜索的？
    rep->cache_id = (options.block_cache ? options.block_cache->NewId() : 0);
    // 开始取出meta block
    rep->filter_data = nullptr;
    rep->filter = nullptr;
    *table = new Table(rep);
    (*table)->ReadMeta(footer);
  }

  return s;
}

// 这个函数的主要作用有两个：
// 1. 通过footer读出meta block index index
// 2. 取出meta block index
// 3. 读出meta block的真正内容
void Table::ReadMeta(const Footer& footer) {
  // 如果没有filter，那么也就不用读取了
  if (rep_->options.filter_policy == nullptr) {
    return;  // Do not need any metadata
  }

  // TODO(sanjay): Skip this if footer.metaindex_handle() size indicates
  // it is an empty block.
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  // 从meta block index index位置读出 meta block index
  // 并且把内容放到contents里面
  BlockContents contents;
  if (!ReadBlock(rep_->file, opt, footer.metaindex_handle(), &contents).ok()) {
    // Do not propagate errors since meta info is not needed for operation
    return;
  }
  // 利用contents生成meta block index
  // meta block index的格式是
  // | filter.name | BlockHandle | 
  // | compresstype 1 byte       | 
  // | crc32 4 byte              |
  Block* meta = new Block(contents);

  // 这里直接就指定了compare?
  // 这里可能是先随意设置一个，在ReadFilter函数里面会把这个给改掉
  Iterator* iter = meta->NewIterator(BytewiseComparator());

  // 这里生成filter的名字
  std::string key = "filter.";
  key.append(rep_->options.filter_policy->Name());
  // 这里的key就是filter.xxName
  // value就是BlockHandle
  iter->Seek(key);
  // 这里必须是iter->key() == key
  if (iter->Valid() && iter->key() == Slice(key)) {
    // 得到BlockHandle之后，去读出filter block
    // filter block也就是meta block
    ReadFilter(iter->value());
  }
  // meta block index
  // meta block index的iter也被放弃不要
  delete iter;
  delete meta;
}

// 当得到filter block的offset/size之后，把
// filter block 即 meta block读出来
void Table::ReadFilter(const Slice& filter_handle_value) {
  // filter_handle_value记录了meta block 的offset/size
  Slice v = filter_handle_value;
  BlockHandle filter_handle;
  // 由于传进来的handle是压缩表示，所以这里需要解码
  if (!filter_handle.DecodeFrom(&v).ok()) {
    return;
  }

  // We might want to unify with ReadBlock() if we start
  // requiring checksum verification in Table::Open.
  // 看一上是否需要进行crc校验
  ReadOptions opt;
  if (rep_->options.paranoid_checks) {
    opt.verify_checksums = true;
  }
  // 从文件的偏移处把内容，从文件中读到内存里面
  BlockContents block;
  if (!ReadBlock(rep_->file, opt, filter_handle, &block).ok()) {
    return;
  }
  // 如果ReadBlock里面申请了这段内存
  // 那么后面是需要释放这段内存的
  if (block.heap_allocated) {
    // 指向原始的数据
    // block.data是一个slice
    // 所以filter_data可以把里面的数据部分偷走
    rep_->filter_data = block.data.data();     // Will need to delete later
  }

  // 生成相应的filter
  // 虽然名字是叫Reader
  // 但是这个BlockReader本质上就是一个filter
  // 可以用来过滤key
  rep_->filter = new FilterBlockReader(rep_->options.filter_policy, block.data);
}

Table::~Table() {
  delete rep_;
}

// 删除一个block
static void DeleteBlock(void* arg, void* ignored) {
  delete reinterpret_cast<Block*>(arg);
}

// 删除相应的cache
static void DeleteCachedBlock(const Slice& key, void* value) {
  Block* block = reinterpret_cast<Block*>(value);
  delete block;
}

// 释放一次cache的引用
static void ReleaseBlock(void* arg, void* h) {
  Cache* cache = reinterpret_cast<Cache*>(arg);
  Cache::Handle* handle = reinterpret_cast<Cache::Handle*>(h);
  cache->Release(handle);
}

// 这里是把一个编码过的BlockHandle指向的data block里面的data(key/value)部分
// 读出来
// arg就是一个Table对象
// index_value就是一个编码过的offset & size
// 真正的读取操作由ReadBlock底层来完成。
// 而这个函数主要是操作与Table & Block Cache相关的事情。
// Convert an index iterator value (i.e., an encoded BlockHandle)
// into an iterator over the contents of the corresponding block.
Iterator* Table::BlockReader(void* arg,
                             const ReadOptions& options,
                             const Slice& index_value) {
  // arg就是table，那么为什么不直接命名为Table *arg
  // 这里是为了添加到cache的时候方便
  Table* table = reinterpret_cast<Table*>(arg);
  // block_cache指的就是对sst文件中的data block部分的cache
  Cache* block_cache = table->rep_->options.block_cache;
  Block* block = nullptr;
  Cache::Handle* cache_handle = nullptr;

  // 取出offset/size
  BlockHandle handle;
  Slice input = index_value; // 虽然在内存里面，但是扔然是编码过的
  Status s = handle.DecodeFrom(&input); // 解码成能用的格式
  // We intentionally allow extra stuff in index_value so that we
  // can add more features in the future.

  if (s.ok()) {
    // 这里生成一段内存，存放block contents
    BlockContents contents;
    // 如果block cache不为空
    if (block_cache != nullptr) {
      // 把cache_id/offset编码之后，放到cache_key_buffer
      // 这里是把cache_key_buffer当成一个cache的index
      // 如果是把cache当成一个map<cache_key, value>
      // 那么cache_key_buffer就是前面的key
      // value就是相应的block指针

      // 注意block cache里面索引的生成方式
      char cache_key_buffer[16];
      // 当前table是有一个id的。
      EncodeFixed64(cache_key_buffer, table->rep_->cache_id);
      EncodeFixed64(cache_key_buffer+8, handle.offset());
      
      // 下面这段逻辑代码虽然看起来很长，逻辑如下：
      // 1. 从cache中看一下是否可以找到相应的block
      //   a. 找到，那么直接返回
      //   b. 没有找到，那么就从文件中读取相应的block到contents里面。
      Slice key(cache_key_buffer, sizeof(cache_key_buffer));

      // 看一下是否已经在block cache里面了。
      cache_handle = block_cache->Lookup(key);
      if (cache_handle != nullptr) {
        // 如果能够找到
        // return reinterpret_cast<LRUHandle*>(handle)->value;
        // 其实这里value部分就是一个Block*
        // cache.cc:695   virtual void* Value(Handle* handle) {
        //           return reinterpret_cast<LRUHandle*>(handle)->value;
        //       }
        block = reinterpret_cast<Block*>(block_cache->Value(cache_handle));
      } else {
        // 如果没有在table cache中找到
        // 那么读到contents里面
        // contents就是一段内存
        s = ReadBlock(table->rep_->file, options, handle, &contents);
        if (s.ok()) {
          // 利用contents这段内存来生成block.
          // 注意：内存仍然是共享的
          block = new Block(contents);
          // 如果是需要加到cache中。
          if (contents.cachable && options.fill_cache) {
            cache_handle = block_cache->Insert(
                key, block, block->size(), &DeleteCachedBlock);
          }
        }
      }
    } else {
      // 如果根本就没有开cache，那么也就不用想了
      // 直接从文件中把相应的内容读到contents里面
      s = ReadBlock(table->rep_->file, options, handle, &contents);
      if (s.ok()) {
        block = new Block(contents);
      }
    }
  }

  Iterator* iter;
  if (block != nullptr) {
    // 这里生成相应的Iterator，并且会注册删除时的回调函数
    iter = block->NewIterator(table->rep_->options.comparator);
    // cache_handle是cache中的句柄
    if (cache_handle == nullptr) {
      // 所有的Iterator都可以注册这个回调函数，在退出的时候，自动执行清理工作。
      iter->RegisterCleanup(&DeleteBlock, block, nullptr);
    } else {
      iter->RegisterCleanup(&ReleaseBlock, block_cache, cache_handle);
    }
  } else {
    iter = NewErrorIterator(s);
  }
  return iter;
}

// 这里生成一个二级的Iterator
// 内部是一个block iterator
Iterator* Table::NewIterator(const ReadOptions& options) const {
  return NewTwoLevelIterator(
      rep_->index_block->NewIterator(rep_->options.comparator),
      &Table::BlockReader, const_cast<Table*>(this), options);
}

// 这个函数只有一个地方用到了，那就是
// ./db/table_cache.cc:119:    s = t->InternalGet(options, k, arg, saver);
// 这个函数的作用就是：找到相应的key/value之后，然后用saver来进行一次函数的回调
Status Table::InternalGet(const ReadOptions& options, const Slice& k,
                          void* arg,
                          void (*saver)(void*, const Slice&, const Slice&)) {
  Status s;
  // 生成data block index的Iterator
  Iterator* iiter = rep_->index_block->NewIterator(rep_->options.comparator);
  // 索引区移动到k
  iiter->Seek(k);
  // 如果这个iter是有效的
  if (iiter->Valid()) {
    // 这里需要注意一下data block index的格式
    // | split_key | blockHandle|
    // 所以这里取出来的是handle，也就是拿到了offset,size
    Slice handle_value = iiter->value();
    // 这里拿到filter
    FilterBlockReader* filter = rep_->filter;
    BlockHandle handle;
    // 如果有filter，那么看一下相应的key是否存在
    if (filter != nullptr &&
        handle.DecodeFrom(&handle_value).ok() &&
        !filter->KeyMayMatch(handle.offset(), k)) {
      // filter的策略是：如果回答不存在，那是肯定没有
      // 如果回答有，但是有可能找不到
      // 这里是发现不存在，那么肯定是不存在了。
      // Not found
    } else {
      // 如果没有filter，那么就需要去block里面找了
      // 当给定offset/size之后，就可以把一个完整的block构建出来了
      Iterator* block_iter = BlockReader(this, options, iiter->value());
      // 然后利用这个block的iterator移动到k这里。
      block_iter->Seek(k);
      if (block_iter->Valid()) {
        // 如果iter是有效的
        // 那么执行传进来的回调函数
        (*saver)(arg, block_iter->key(), block_iter->value());
      }
      // 返回iter的状态
      s = block_iter->status();
      delete block_iter;
    }
  }
  // 如果状态是ok的，那么看iiter的状态
  if (s.ok()) {
    s = iiter->status();
  }
  // 清除data block index的iterator
  delete iiter;
  return s;
}

// 这个函数是查找key最接近的offset
uint64_t Table::ApproximateOffsetOf(const Slice& key) const {
  // 取得data block index里面的iterator
  Iterator* index_iter =
      rep_->index_block->NewIterator(rep_->options.comparator);
  // seek到key边上
  index_iter->Seek(key);
  uint64_t result;
  // 这个index_iter是否还有效?
  if (index_iter->Valid()) {
    BlockHandle handle;
    // 如果有效，取offset/size
    Slice input = index_iter->value();
    // 编码
    Status s = handle.DecodeFrom(&input);
    // 取出offset
    if (s.ok()) {
      result = handle.offset();
    } else {
      // Strange: we can't decode the block handle in the index block.
      // We'll just return the offset of the metaindex block, which is
      // close to the whole file size for this case.
      // 这里有意思的是，如果找不到，那么返回的是
      // meta block index index
      result = rep_->metaindex_handle.offset();
    }
  } else {
    // key is past the last key in the file.  Approximate the offset
    // by returning the offset of the metaindex block (which is
    // right near the end of the file).
    // 同理，返回meta block index index
    result = rep_->metaindex_handle.offset();
  }
  delete index_iter;
  return result;
}

}  // namespace leveldb
