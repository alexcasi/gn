// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/debug/activity_analyzer.h"

#include <algorithm>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/memory_mapped_file.h"
#include "base/lazy_instance.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/stl_util.h"
#include "base/strings/string_util.h"

namespace base {
namespace debug {

namespace {
// An empty snapshot that can be returned when there otherwise is none.
LazyInstance<ActivityUserData::Snapshot>::Leaky g_empty_user_data_snapshot;
}  // namespace

ThreadActivityAnalyzer::Snapshot::Snapshot() {}
ThreadActivityAnalyzer::Snapshot::~Snapshot() {}

ThreadActivityAnalyzer::ThreadActivityAnalyzer(
    const ThreadActivityTracker& tracker)
    : activity_snapshot_valid_(tracker.CreateSnapshot(&activity_snapshot_)) {}

ThreadActivityAnalyzer::ThreadActivityAnalyzer(void* base, size_t size)
    : ThreadActivityAnalyzer(ThreadActivityTracker(base, size)) {}

ThreadActivityAnalyzer::ThreadActivityAnalyzer(
    PersistentMemoryAllocator* allocator,
    PersistentMemoryAllocator::Reference reference)
    : ThreadActivityAnalyzer(allocator->GetAsArray<char>(
                                 reference,
                                 GlobalActivityTracker::kTypeIdActivityTracker,
                                 PersistentMemoryAllocator::kSizeAny),
                             allocator->GetAllocSize(reference)) {}

ThreadActivityAnalyzer::~ThreadActivityAnalyzer() {}

void ThreadActivityAnalyzer::AddGlobalInformation(
    GlobalActivityAnalyzer* global) {
  if (!IsValid())
    return;

  // User-data is held at the global scope even though it's referenced at the
  // thread scope.
  activity_snapshot_.user_data_stack.clear();
  for (auto& activity : activity_snapshot_.activity_stack) {
    // The global GetUserDataSnapshot will return an empty snapshot if the ref
    // or id is not valid.
    activity_snapshot_.user_data_stack.push_back(global->GetUserDataSnapshot(
        activity_snapshot_.process_id, activity.user_data_ref,
        activity.user_data_id));
  }
}

GlobalActivityAnalyzer::GlobalActivityAnalyzer(
    std::unique_ptr<PersistentMemoryAllocator> allocator)
    : allocator_(std::move(allocator)), allocator_iterator_(allocator_.get()) {}

GlobalActivityAnalyzer::~GlobalActivityAnalyzer() {}

#if !defined(OS_NACL)
// static
std::unique_ptr<GlobalActivityAnalyzer> GlobalActivityAnalyzer::CreateWithFile(
    const FilePath& file_path) {
  // Map the file read-write so it can guarantee consistency between
  // the analyzer and any trackers that my still be active.
  std::unique_ptr<MemoryMappedFile> mmfile(new MemoryMappedFile());
  mmfile->Initialize(file_path, MemoryMappedFile::READ_WRITE);
  if (!mmfile->IsValid())
    return nullptr;

  if (!FilePersistentMemoryAllocator::IsFileAcceptable(*mmfile, true))
    return nullptr;

  return WrapUnique(
      new GlobalActivityAnalyzer(MakeUnique<FilePersistentMemoryAllocator>(
          std::move(mmfile), 0, 0, base::StringPiece(), true)));
}
#endif  // !defined(OS_NACL)

int64_t GlobalActivityAnalyzer::GetFirstProcess() {
  PrepareAllAnalyzers();
  return GetNextProcess();
}

int64_t GlobalActivityAnalyzer::GetNextProcess() {
  if (process_ids_.empty())
    return 0;
  int64_t pid = process_ids_.back();
  process_ids_.pop_back();
  return pid;
}

ThreadActivityAnalyzer* GlobalActivityAnalyzer::GetFirstAnalyzer(int64_t pid) {
  analyzers_iterator_ = analyzers_.begin();
  analyzers_iterator_pid_ = pid;
  if (analyzers_iterator_ == analyzers_.end())
    return nullptr;
  int64_t create_stamp;
  if (analyzers_iterator_->second->GetProcessId(&create_stamp) == pid &&
      create_stamp <= analysis_stamp_) {
    return analyzers_iterator_->second.get();
  }
  return GetNextAnalyzer();
}

ThreadActivityAnalyzer* GlobalActivityAnalyzer::GetNextAnalyzer() {
  DCHECK(analyzers_iterator_ != analyzers_.end());
  int64_t create_stamp;
  do {
    ++analyzers_iterator_;
    if (analyzers_iterator_ == analyzers_.end())
      return nullptr;
  } while (analyzers_iterator_->second->GetProcessId(&create_stamp) !=
               analyzers_iterator_pid_ ||
           create_stamp > analysis_stamp_);
  return analyzers_iterator_->second.get();
}

ThreadActivityAnalyzer* GlobalActivityAnalyzer::GetAnalyzerForThread(
    const ThreadKey& key) {
  auto found = analyzers_.find(key);
  if (found == analyzers_.end())
    return nullptr;
  return found->second.get();
}

ActivityUserData::Snapshot GlobalActivityAnalyzer::GetUserDataSnapshot(
    int64_t pid,
    uint32_t ref,
    uint32_t id) {
  ActivityUserData::Snapshot snapshot;

  void* memory = allocator_->GetAsArray<char>(
      ref, GlobalActivityTracker::kTypeIdUserDataRecord,
      PersistentMemoryAllocator::kSizeAny);
  if (memory) {
    size_t size = allocator_->GetAllocSize(ref);
    const ActivityUserData user_data(memory, size);
    user_data.CreateSnapshot(&snapshot);
    int64_t process_id;
    int64_t create_stamp;
    if (!ActivityUserData::GetOwningProcessId(memory, &process_id,
                                              &create_stamp) ||
        process_id != pid || user_data.id() != id) {
      // This allocation has been overwritten since it was created. Return an
      // empty snapshot because whatever was captured is incorrect.
      snapshot.clear();
    }
  }

  return snapshot;
}

const ActivityUserData::Snapshot&
GlobalActivityAnalyzer::GetProcessDataSnapshot(int64_t pid) {
  auto iter = process_data_.find(pid);
  if (iter == process_data_.end())
    return g_empty_user_data_snapshot.Get();
  if (iter->second.create_stamp > analysis_stamp_)
    return g_empty_user_data_snapshot.Get();
  DCHECK_EQ(pid, iter->second.process_id);
  return iter->second.data;
}

const ActivityUserData::Snapshot&
GlobalActivityAnalyzer::GetGlobalDataSnapshot() {
  global_data_snapshot_.clear();

  PersistentMemoryAllocator::Reference ref =
      PersistentMemoryAllocator::Iterator(allocator_.get())
          .GetNextOfType(GlobalActivityTracker::kTypeIdGlobalDataRecord);
  void* memory = allocator_->GetAsArray<char>(
      ref, GlobalActivityTracker::kTypeIdGlobalDataRecord,
      PersistentMemoryAllocator::kSizeAny);
  if (memory) {
    size_t size = allocator_->GetAllocSize(ref);
    const ActivityUserData global_data(memory, size);
    global_data.CreateSnapshot(&global_data_snapshot_);
  }

  return global_data_snapshot_;
}

std::vector<std::string> GlobalActivityAnalyzer::GetLogMessages() {
  std::vector<std::string> messages;
  PersistentMemoryAllocator::Reference ref;

  PersistentMemoryAllocator::Iterator iter(allocator_.get());
  while ((ref = iter.GetNextOfType(
              GlobalActivityTracker::kTypeIdGlobalLogMessage)) != 0) {
    const char* message = allocator_->GetAsArray<char>(
        ref, GlobalActivityTracker::kTypeIdGlobalLogMessage,
        PersistentMemoryAllocator::kSizeAny);
    if (message)
      messages.push_back(message);
  }

  return messages;
}

std::vector<GlobalActivityTracker::ModuleInfo>
GlobalActivityAnalyzer::GetModules() {
  std::vector<GlobalActivityTracker::ModuleInfo> modules;

  PersistentMemoryAllocator::Iterator iter(allocator_.get());
  const GlobalActivityTracker::ModuleInfoRecord* record;
  while (
      (record =
           iter.GetNextOfObject<GlobalActivityTracker::ModuleInfoRecord>()) !=
      nullptr) {
    GlobalActivityTracker::ModuleInfo info;
    if (record->DecodeTo(&info, allocator_->GetAllocSize(
                                    allocator_->GetAsReference(record)))) {
      modules.push_back(std::move(info));
    }
  }

  return modules;
}

GlobalActivityAnalyzer::ProgramLocation
GlobalActivityAnalyzer::GetProgramLocationFromAddress(uint64_t address) {
  // TODO(bcwhite): Implement this.
  return { 0, 0 };
}

GlobalActivityAnalyzer::UserDataSnapshot::UserDataSnapshot() {}
GlobalActivityAnalyzer::UserDataSnapshot::UserDataSnapshot(
    const UserDataSnapshot& rhs) = default;
GlobalActivityAnalyzer::UserDataSnapshot::UserDataSnapshot(
    UserDataSnapshot&& rhs) = default;
GlobalActivityAnalyzer::UserDataSnapshot::~UserDataSnapshot() {}

void GlobalActivityAnalyzer::PrepareAllAnalyzers() {
  // Record the time when analysis started.
  analysis_stamp_ = base::Time::Now().ToInternalValue();

  // Fetch all the records. This will retrieve only ones created since the
  // last run since the PMA iterator will continue from where it left off.
  uint32_t type;
  PersistentMemoryAllocator::Reference ref;
  while ((ref = allocator_iterator_.GetNext(&type)) != 0) {
    switch (type) {
      case GlobalActivityTracker::kTypeIdActivityTracker:
      case GlobalActivityTracker::kTypeIdActivityTrackerFree:
      case GlobalActivityTracker::kTypeIdProcessDataRecord:
      case GlobalActivityTracker::kTypeIdProcessDataRecordFree:
      case PersistentMemoryAllocator::kTypeIdTransitioning:
        // Active, free, or transitioning: add it to the list of references
        // for later analysis.
        memory_references_.insert(ref);
        break;
    }
  }

  // Clear out any old information.
  analyzers_.clear();
  process_data_.clear();
  process_ids_.clear();
  std::set<int64_t> seen_pids;

  // Go through all the known references and create objects for them with
  // snapshots of the current state.
  for (PersistentMemoryAllocator::Reference memory_ref : memory_references_) {
    // Get the actual data segment for the tracker. Any type will do since it
    // is checked below.
    void* const base = allocator_->GetAsArray<char>(
        memory_ref, PersistentMemoryAllocator::kTypeIdAny,
        PersistentMemoryAllocator::kSizeAny);
    const size_t size = allocator_->GetAllocSize(memory_ref);
    if (!base)
      continue;

    switch (allocator_->GetType(memory_ref)) {
      case GlobalActivityTracker::kTypeIdActivityTracker: {
        // Create the analyzer on the data. This will capture a snapshot of the
        // tracker state. This can fail if the tracker is somehow corrupted or
        // is in the process of shutting down.
        std::unique_ptr<ThreadActivityAnalyzer> analyzer(
            new ThreadActivityAnalyzer(base, size));
        if (!analyzer->IsValid())
          continue;
        analyzer->AddGlobalInformation(this);

        // Track PIDs.
        int64_t pid = analyzer->GetProcessId();
        if (seen_pids.find(pid) == seen_pids.end()) {
          process_ids_.push_back(pid);
          seen_pids.insert(pid);
        }

        // Add this analyzer to the map of known ones, indexed by a unique
        // thread
        // identifier.
        DCHECK(!base::ContainsKey(analyzers_, analyzer->GetThreadKey()));
        analyzer->allocator_reference_ = ref;
        analyzers_[analyzer->GetThreadKey()] = std::move(analyzer);
      } break;

      case GlobalActivityTracker::kTypeIdProcessDataRecord: {
        // Get the PID associated with this data record.
        int64_t process_id;
        int64_t create_stamp;
        ActivityUserData::GetOwningProcessId(base, &process_id, &create_stamp);
        DCHECK(!base::ContainsKey(process_data_, process_id));

        // Create a snapshot of the data. This can fail if the data is somehow
        // corrupted or the process shutdown and the memory being released.
        UserDataSnapshot& snapshot = process_data_[process_id];
        snapshot.process_id = process_id;
        snapshot.create_stamp = create_stamp;
        const ActivityUserData process_data(base, size);
        if (!process_data.CreateSnapshot(&snapshot.data))
          break;

        // Check that nothing changed. If it did, forget what was recorded.
        ActivityUserData::GetOwningProcessId(base, &process_id, &create_stamp);
        if (process_id != snapshot.process_id ||
            create_stamp != snapshot.create_stamp) {
          process_data_.erase(process_id);
          break;
        }

        // Track PIDs.
        if (seen_pids.find(process_id) == seen_pids.end()) {
          process_ids_.push_back(process_id);
          seen_pids.insert(process_id);
        }
      } break;
    }
  }

  // Reverse the list of PIDs so that they get popped in the order found.
  std::reverse(process_ids_.begin(), process_ids_.end());
}

}  // namespace debug
}  // namespace base
