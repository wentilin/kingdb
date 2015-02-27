// Copyright (c) 2014, Emmanuel Goossaert. All rights reserved.
// Use of this source code is governed by the BSD 3-Clause License,
// that can be found in the LICENSE file.

#include <iostream>
#include <iomanip>
#include <thread>
#include <regex>
#include <queue>
#include <vector>
#include <string>
#include <cstdio>
#include <string.h>
#include <unistd.h>
#include <execinfo.h>
#include <chrono>
#include <sstream>
#include <csignal>
#include <random>

#include "interface/kingdb.h"
#include "kingdb/kdb.h"
#include "util/debug.h"
#include "util/status.h"
#include "util/order.h"
#include "util/byte_array.h"
#include "util/kitten.h"
#include "util/file.h"

#include "interface/snapshot.h"
#include "interface/iterator.h"

#include "unit-tests/testharness.h"


namespace kdb {

class KeyGenerator {
 public:
  virtual ~KeyGenerator() {}
  virtual std::string GetKey(uint64_t thread_id, uint64_t index, int size) = 0;
};


class SequentialKeyGenerator: public KeyGenerator {
 public:
  virtual ~SequentialKeyGenerator() {}

  virtual std::string GetKey(uint64_t thread_id, uint64_t index, int size) {
    std::stringstream ss;
    ss << std::setfill ('0') << std::setw (size);
    ss << index;
    return ss.str();
  }
};


class RandomKeyGenerator: public KeyGenerator {
 public:
  RandomKeyGenerator() {
    generator = std::mt19937(seq);
    random_dist = std::uniform_int_distribution<int>(0,255);
  }
  virtual ~RandomKeyGenerator() {}

  virtual std::string GetKey(uint64_t thread_id, uint64_t index, int size) {
    std::string str;
    str.resize(size);
    for (int i = 0; i < size; i++) {
      str[i] = static_cast<char>(random_dist(generator));
    }
    return str;
  }

 private:
  std::seed_seq seq{1, 2, 3, 4, 5, 6, 7};
  std::mt19937 generator;
  std::uniform_int_distribution<int> random_dist;
};


class DataGenerator {
 public:
  virtual ~DataGenerator() {}
  virtual void GenerateData(char *data, int size) = 0;
};

class IncompressibleDataGenerator: public DataGenerator {
 public:
  IncompressibleDataGenerator() {
    generator = std::mt19937(seq);
    random_dist = std::uniform_int_distribution<int>(0,255);
  }
  virtual ~IncompressibleDataGenerator() {}

  virtual void GenerateData(char *data, int size) {
    for (int i = 0; i < size; i++) {
      data[i] = static_cast<char>(random_dist(generator));
    }
  }

 private:
  std::seed_seq seq{1, 2, 3, 4, 5, 6, 7};
  std::mt19937 generator;
  std::uniform_int_distribution<int> random_dist;
};


class CompressibleDataGenerator: public DataGenerator {
 public:
  CompressibleDataGenerator() {
    generator = std::mt19937(seq);
    random_dist = std::uniform_int_distribution<int>(0,255);
  }
  virtual ~CompressibleDataGenerator() {}

  virtual void GenerateData(char *data, int size) {
    int i = 0;
    while (i < size) {
      char c = static_cast<char>(random_dist(generator));
      int repetition = random_dist(generator) % 30 + 1;
      if (i + repetition > size) repetition = size - i;
      memset(data + i, c, repetition);
      i += repetition;
    }
  }

 private:
  std::seed_seq seq{1, 2, 3, 4, 5, 6, 7};
  std::mt19937 generator;
  std::uniform_int_distribution<int> random_dist;
};





class DBTest {
 public:
  DBTest() {
    dbname_ = "db_test";
    db_ = nullptr;
    db_options_.compression.type = kLZ4Compression;
    index_db_options_ = 0;
    data_generator_ = nullptr;
  }

  void Open() {
    EraseDB();
    db_ = new kdb::KingDB(db_options_, dbname_);
    Status s = db_->Open();
    if (!s.IsOK()) {
      delete db_;
      log::emerg("Server", s.ToString().c_str()); 
    }
  }

  void Close() {
    db_->Close();
    delete db_;
    db_ = nullptr;
    EraseDB();
  }

  bool IterateOverOptions() {
    db_options_ = DatabaseOptions();
    read_options_ = ReadOptions();
    write_options_ = WriteOptions();

    if (index_db_options_ == 0) {
      data_generator_ = new IncompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
    } else if (index_db_options_ == 1) {
      data_generator_ = new CompressibleDataGenerator();
      db_options_.compression.type = kNoCompression;
    } else if (index_db_options_ == 2) {
      data_generator_ = new IncompressibleDataGenerator();
      db_options_.compression.type = kNoCompression;
    } else if (index_db_options_ == 3) {
      data_generator_ = new CompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
      db_options_.storage__hashing_algorithm = kMurmurHash3_64;
    } else if (index_db_options_ == 4) {
      data_generator_ = new IncompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
      read_options_.verify_checksums = true;
    } else if (index_db_options_ == 5) {
      data_generator_ = new CompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
      read_options_.verify_checksums = true;
    } else if (index_db_options_ == 6) {
      data_generator_ = new IncompressibleDataGenerator();
      db_options_.compression.type = kNoCompression;
      read_options_.verify_checksums = true;
    } else if (index_db_options_ == 7) {
      data_generator_ = new IncompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
      write_options_.sync = true;
    } else if (index_db_options_ == 8) {
      data_generator_ = new IncompressibleDataGenerator();
      db_options_.compression.type = kNoCompression;
      db_options_.write_buffer__mode = kWriteBufferModeBlocking;
      db_options_.write_buffer__size = 1024 * 256;
      db_options_.storage__maximum_chunk_size = 1024 * 8;
      db_options_.storage__hstable_size = 1024 * 200;
    } else if (index_db_options_ == 9) {
      data_generator_ = new IncompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
      db_options_.write_buffer__mode = kWriteBufferModeBlocking;
      db_options_.write_buffer__size = 1024 * 256;
      db_options_.storage__maximum_chunk_size = 1024 * 8;
      db_options_.storage__hstable_size = 1024 * 200;
    } else if (index_db_options_ == 10) {
      data_generator_ = new CompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
      db_options_.write_buffer__mode = kWriteBufferModeBlocking;
      db_options_.write_buffer__size = 1024 * 256;
      db_options_.storage__maximum_chunk_size = 1024 * 8;
      db_options_.storage__hstable_size = 1024 * 200;
    } else if (index_db_options_ == 11) {
      data_generator_ = new IncompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
      db_options_.write_buffer__mode = kWriteBufferModeBlocking;
    } else if (index_db_options_ == 12) {
      data_generator_ = new CompressibleDataGenerator();
      db_options_.compression.type = kLZ4Compression;
      db_options_.write_buffer__mode = kWriteBufferModeBlocking;
    } else {
      return false;
    }

    fprintf(stdout, "Database Options: Stage %d\n", index_db_options_);
    index_db_options_ += 1;
    return true;
  }

  void EraseDB() {
    struct dirent *entry;
    DIR *dir;
    char filepath[FileUtil::maximum_path_size()];

    struct stat info;
    if (stat(dbname_.c_str(), &info) != 0) return;

    dir = opendir(dbname_.c_str());
    while ((entry = readdir(dir)) != nullptr) {
      sprintf(filepath, "%s/%s", dbname_.c_str(), entry->d_name);
      std::remove(filepath);
    }
    rmdir(dbname_.c_str());
  }

  kdb::Status Get(ReadOptions& read_options_, const std::string& key, std::string *value_out) {
    return Status::OK();
  }

  kdb::Status Put(const std::string& key, const std::string& value) {
    return Status::OK();
  }


  kdb::KingDB* db_;
  DataGenerator *data_generator_;
  std::string dbname_;
  DatabaseOptions db_options_;
  ReadOptions read_options_;
  WriteOptions write_options_;
  int index_db_options_;
};


TEST(DBTest, KeysWithNullBytes) {
  Open();
  kdb::Status s;
  kdb::Logger::set_current_level("emerg");

  int num_count_valid = 0;

  //kdb::Kitten kitten = kdb::Kitten::NewDeepCopyKitten("blahblah", 8);
  //fprintf(stderr, "kitten: %s\n", kitten.ToString().c_str());

  std::string key1("000000000000key1");
  std::string key2("000000000000key2");

  key1[5] = '\0';
  key2[5] = '\0';

  std::string value1("value1");
  std::string value2("value2");

  s = db_->Put(write_options_, key1, value1);
  s = db_->Put(write_options_, key2, value2);

  std::string out_str;
  s = db_->Get(read_options_, key1, &out_str);
  if (s.IsOK() && out_str == "value1") num_count_valid += 1;
  //fprintf(stderr, "num_count_valid:%d\n", num_count_valid);

  s = db_->Get(read_options_, key2, &out_str);
  if (s.IsOK() && out_str == "value2") num_count_valid += 1;
  //fprintf(stderr, "num_count_valid:%d\n", num_count_valid);

  // Sleeping to let the buffer store the entries on secondary storage
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));

  s = db_->Get(read_options_, key1, &out_str);
  if (s.IsOK() && out_str == "value1") num_count_valid += 1;
  //fprintf(stderr, "num_count_valid:%d\n", num_count_valid);

  s = db_->Get(read_options_, key2, &out_str);
  if (s.IsOK() && out_str == "value2") num_count_valid += 1;
  //fprintf(stderr, "num_count_valid:%d\n", num_count_valid);

  ASSERT_EQ(num_count_valid, 4);
  Close();
}



TEST(DBTest, SingleThreadSmallEntries) {
  kdb::Logger::set_current_level("emerg");
  while (IterateOverOptions()) {
    Open();

    int size = 100;
    char *buffer_large = new char[size+1];
    for (auto i = 0; i < size; i++) {
      buffer_large[i] = 'a';
    }
    buffer_large[size+1] = '\0';

    int num_items = 1000;
    KeyGenerator* kg = new RandomKeyGenerator();
    std::map<std::string, std::string> saved_data;

    for (auto i = 0; i < num_items; i++) {
      std::string key_str = kg->GetKey(0, i, 16);
      kdb::Kitten key = kdb::Kitten::NewDeepCopyKitten(key_str.c_str(), key_str.size());

      data_generator_->GenerateData(buffer_large, 100);
      kdb::Kitten value = kdb::Kitten::NewDeepCopyKitten(buffer_large, 100);

      kdb::Status s = db_->Put(write_options_, key, value);
      if (!s.IsOK()) {
        fprintf(stderr, "ClientEmbedded - Error: %s\n", s.ToString().c_str());
      }
      std::string value_str(buffer_large, 100);
      saved_data[key_str] = value_str;
    }

    int count_items_end = 0;
    kdb::Iterator iterator = db_->NewIterator(read_options_);
    kdb::Status s = iterator.GetStatus();
    if (!s.IsOK()) {
      fprintf(stderr, "Error: %s\n", s.ToString().c_str());
    }

    for (iterator.Begin(); iterator.IsValid(); iterator.Next()) {
      kdb::MultipartReader mp_reader = iterator.GetMultipartValue();

      for (mp_reader.Begin(); mp_reader.IsValid(); mp_reader.Next()) {
        kdb::Kitten part;
        kdb::Status s = mp_reader.GetPart(&part);
      }

      kdb::Status s = mp_reader.GetStatus();
      if (s.IsOK()) {
        count_items_end += 1;
      } else {
        fprintf(stderr, "ClientEmbedded - Error: %s\n", s.ToString().c_str());
      }
    }
    
    delete[] buffer_large;
    ASSERT_EQ(count_items_end, num_items);
    iterator.Close();
    Close();
  }
}



TEST(DBTest, SingleThreadSnapshot) {
  kdb::Logger::set_current_level("emerg");
  while (IterateOverOptions()) {
    Open();

    int size = 100;
    char *buffer_large = new char[size+1];
    for (auto i = 0; i < size; i++) {
      buffer_large[i] = 'a';
    }
    buffer_large[size+1] = '\0';

    int num_items = 1000;
    KeyGenerator* kg = new RandomKeyGenerator();
    std::map<std::string, std::string> saved_data;

    for (auto i = 0; i < num_items; i++) {
      std::string key_str = kg->GetKey(0, i, 16);
      kdb::Kitten key = kdb::Kitten::NewDeepCopyKitten(key_str.c_str(), key_str.size());

      data_generator_->GenerateData(buffer_large, 100);
      kdb::Kitten value = kdb::Kitten::NewDeepCopyKitten(buffer_large, 100);

      kdb::Status s = db_->Put(write_options_, key, value);
      if (!s.IsOK()) {
        fprintf(stderr, "ClientEmbedded - Error: %s\n", s.ToString().c_str());
      }
      std::string value_str(buffer_large, 100);
      saved_data[key_str] = value_str;
    }

    int count_items_end = 0;
    kdb::Snapshot snapshot = db_->NewSnapshot();
    kdb::Iterator iterator = snapshot.NewIterator(read_options_);
    kdb::Status s = iterator.GetStatus();
    if (!s.IsOK()) {
      fprintf(stderr, "Error: %s\n", s.ToString().c_str());
    }

    for (iterator.Begin(); iterator.IsValid(); iterator.Next()) {
      kdb::MultipartReader mp_reader = iterator.GetMultipartValue();

      for (mp_reader.Begin(); mp_reader.IsValid(); mp_reader.Next()) {
        kdb::Kitten part;
        kdb::Status s = mp_reader.GetPart(&part);
      }

      kdb::Status s = mp_reader.GetStatus();
      if (s.IsOK()) {
        count_items_end += 1;
      } else {
        fprintf(stderr, "ClientEmbedded - Error: %s\n", s.ToString().c_str());
      }
    }
    
    delete[] buffer_large;
    ASSERT_EQ(count_items_end, num_items);
    iterator.Close();
    snapshot.Close();
    Close();
  }
}






TEST(DBTest, SingleThreadSingleLargeEntry) {
  while (IterateOverOptions()) {
    Open();
    kdb::Logger::set_current_level("emerg");

    uint64_t total_size = (uint64_t)1 << 30;
    //total_size *= 5;
    total_size = 1024*1024 * 2;
    int buffersize = 1024 * 64;
    char buffer[buffersize];
    for (int i = 0; i < buffersize; ++i) {
      buffer[i] = 'a';
    }
    std::string key_str = "myentry";
    kdb::Status s;

    std::seed_seq seq{1, 2, 3, 4, 5, 6, 7};
    std::mt19937 generator(seq);
    std::uniform_int_distribution<int> random_dist(0,255);

    //char buffer_full[total_size];

    //usleep(10 * 1000000);
    kdb::Kitten key = kdb::Kitten::NewDeepCopyKitten(key_str.c_str(), key_str.size());
    kdb::MultipartWriter mp_writer = db_->NewMultipartWriter(write_options_, key, total_size);

    int fd = open("/tmp/kingdb-input", O_WRONLY|O_CREAT|O_TRUNC, 0644);

    for (uint64_t i = 0; i < total_size; i += buffersize) {

      for (int j = 0; j < buffersize; ++j) {
        char random_char = static_cast<char>(random_dist(generator));
        buffer[j] = random_char;
      }

      int size_current = buffersize;
      if (i + size_current > total_size) {
        size_current = total_size - i;
      }

      kdb::Kitten value = kdb::Kitten::NewDeepCopyKitten(buffer, size_current);
      s = mp_writer.PutPart(value);

      write(fd, buffer, size_current);
      //memcpy(buffer_full + i, buffer, size_current);
      if (!s.IsOK()) {
        fprintf(stderr, "PutChunk(): %s\n", s.ToString().c_str());
      }
    }

    close(fd);

    usleep(4 * 1000000);

    kdb::MultipartReader mp_reader = db_->NewMultipartReader(read_options_, key);
    Kitten value_out;

    uint64_t bytes_read = 0;
    int fd_output = open("/tmp/kingdb-output", O_WRONLY|O_CREAT|O_TRUNC, 0644);

    for (mp_reader.Begin(); mp_reader.IsValid(); mp_reader.Next()) {
      Kitten part;
      Status s = mp_reader.GetPart(&part);
      if (write(fd_output, part.data(), part.size()) < 0) {
        fprintf(stderr, "ClientEmbedded - Couldn't write to output file: [%s]\n", strerror(errno));
      }

      bytes_read += part.size();
    }

    s = mp_reader.GetStatus();
    if (!s.IsOK()) {
      fprintf(stderr, "ClientEmbedded - Error: %s\n", s.ToString().c_str());
    }
    close(fd_output);

    ASSERT_EQ(s.IsOK(), true);
    Close();
  }
}


TEST(DBTest, FileUtil) {
  int fd = open("/tmp/allocate", O_WRONLY|O_CREAT, 0644);
  auto start = std::chrono::high_resolution_clock::now();
  size_t mysize = 1024*1024 * (int64_t)256;
  fprintf(stderr, "mysize: %zu\n", mysize);
  Status s = FileUtil::fallocate(fd, mysize);
  std::cout << s.ToString() << std::endl;
  close(fd);
  auto end = std::chrono::high_resolution_clock::now();
  std::chrono::duration<float> duration = end - start;
  std::chrono::milliseconds d = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
  std::cout << d.count() << " ms" << std::endl;

  fprintf(stderr, "Free size: %" PRIu64 " GB\n", FileUtil::fs_free_space("/tmp/") / (1024*1024*256));
}


} // end namespace kdb

void handler(int sig) {
  int depth_max = 20;
  void *array[depth_max];
  size_t depth;

  depth = backtrace(array, depth_max);
  fprintf(stderr, "Error: signal %d:\n", sig);
  backtrace_symbols_fd(array, depth, STDERR_FILENO);
  exit(1);
}


int main() {

  signal(SIGSEGV, handler);
  signal(SIGABRT, handler);

  return kdb::test::RunAllTests();
}
