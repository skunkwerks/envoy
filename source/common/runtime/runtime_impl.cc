#include "runtime_impl.h"

#include "envoy/event/dispatcher.h"
#include "envoy/stats/stats.h"
#include "envoy/thread_local/thread_local.h"

#include "common/common/utility.h"
#include "common/filesystem/filesystem_impl.h"

#include <fcntl.h>
#include <unistd.h>

#if defined(LINUX)
#include <uuid/uuid.h>
#elif defined(__FreeBSD__)
#include <sys/types.h>
#include <uuid.h>
#endif

namespace Runtime {

const size_t RandomGeneratorImpl::UUID_LENGTH = 36;

std::string RandomGeneratorImpl::uuid() {
  char *generated_uuid = nullptr;
  uint32_t status = 0;
  uuid_t uuid;

#if defined(LINUX)
  generated_uuid = malloc(sizeof(char) * UUID_LENGTH);
  uuid_generate_random(uuid);
  uuid_unparse(uuid, generated_uuid);
  free(generated_uuid);
#elif defined(__FreeBSD__)
  uuid_create(&uuid, &status);
  uuid_to_string(&uuid, &generated_uuid, &status);
  free(generated_uuid);
#endif

  std::string string_uuid = std::string(generated_uuid);
  if (0 != status) {
    throw EnvoyException(fmt::format("unable to generate uuid, status: {}", status));
  }

  return string_uuid;
}

SnapshotImpl::SnapshotImpl(const std::string& root_path, const std::string& override_path,
                           RuntimeStats& stats, RandomGenerator& generator)
    : generator_(generator) {
  try {
    walkDirectory(root_path, "");
    if (Filesystem::directoryExists(override_path)) {
      walkDirectory(override_path, "");
      stats.override_dir_exists_.inc();
    } else {
      stats.override_dir_not_exists_.inc();
    }

    stats.load_success_.inc();
  } catch (EnvoyException& e) {
    stats.load_error_.inc();
    log_debug("error creating runtime snapshot: {}", e.what());
  }

  stats.num_keys_.set(values_.size());
}

const std::string& SnapshotImpl::get(const std::string& key) const {
  auto entry = values_.find(key);
  if (entry == values_.end()) {
    return EMPTY_STRING;
  } else {
    return entry->second.string_value_;
  }
}

uint64_t SnapshotImpl::getInteger(const std::string& key, uint64_t default_value) const {
  auto entry = values_.find(key);
  if (entry == values_.end() || !entry->second.uint_value_.valid()) {
    return default_value;
  } else {
    return entry->second.uint_value_.value();
  }
}

void SnapshotImpl::walkDirectory(const std::string& path, const std::string& prefix) {
  log_debug("walking directory: {}", path);
  Directory current_dir(path);
  while (true) {
    errno = 0;
    dirent* entry = readdir(current_dir.dir_);
    if (entry == nullptr && errno != 0) {
      throw EnvoyException(fmt::format("unable to iterate directory: {}", path));
    }

    if (entry == nullptr) {
      break;
    }

    std::string full_path = path + "/" + entry->d_name;
    std::string full_prefix;
    if (prefix.empty()) {
      full_prefix = entry->d_name;
    } else {
      full_prefix = prefix + "." + entry->d_name;
    }

    if (entry->d_type == DT_DIR && std::string(entry->d_name) != "." &&
        std::string(entry->d_name) != "..") {
      walkDirectory(full_path, full_prefix);
    } else if (entry->d_type == DT_REG) {
      // Suck the file into a string. This is not very efficient but it should be good enough
      // for small files. Also, as noted elsewhere, none of this is non-blocking which could
      // theoretically lead to issues.
      log_debug("reading file: {}", full_path);
      Entry entry;
      entry.string_value_ = Filesystem::fileReadToEnd(full_path);
      StringUtil::rtrim(entry.string_value_);

      // As a perf optimization, attempt to convert the string into an integer. If we don't
      // succeed that's fine.
      uint64_t converted;
      if (StringUtil::atoul(entry.string_value_.c_str(), converted)) {
        entry.uint_value_.value(converted);
      }

      values_[full_prefix] = entry;
    }
  }
}

LoaderImpl::LoaderImpl(Event::Dispatcher& dispatcher, ThreadLocal::Instance& tls,
                       const std::string& root_symlink_path, const std::string& subdir,
                       const std::string& override_dir, Stats::Store& store,
                       RandomGenerator& generator)
    : watcher_(dispatcher.createFilesystemWatcher()), tls_(tls), tls_slot_(tls.allocateSlot()),
      generator_(generator), root_path_(root_symlink_path + "/" + subdir),
      override_path_(root_symlink_path + "/" + override_dir), stats_(generateStats(store)) {
  watcher_->addWatch(root_symlink_path, Filesystem::Watcher::Events::MovedTo,
                     [this](uint32_t) -> void { onSymlinkSwap(); });

  onSymlinkSwap();
}

RuntimeStats LoaderImpl::generateStats(Stats::Store& store) {
  std::string prefix = "runtime.";
  RuntimeStats stats{
      ALL_RUNTIME_STATS(POOL_COUNTER_PREFIX(store, prefix), POOL_GAUGE_PREFIX(store, prefix))};
  return stats;
}

void LoaderImpl::onSymlinkSwap() {
  current_snapshot_.reset(new SnapshotImpl(root_path_, override_path_, stats_, generator_));
  ThreadLocal::ThreadLocalObjectPtr ptr_copy = current_snapshot_;
  tls_.set(tls_slot_, [ptr_copy](Event::Dispatcher&)
                          -> ThreadLocal::ThreadLocalObjectPtr { return ptr_copy; });
}

Snapshot& LoaderImpl::snapshot() { return tls_.getTyped<Snapshot>(tls_slot_); }

} // Runtime
