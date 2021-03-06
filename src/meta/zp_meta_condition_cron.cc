// Copyright 2017 Qihoo
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http:// www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "src/meta/zp_meta_condition_cron.h"

#include "slash/include/slash_mutex.h"
#include "slash/include/slash_string.h"
#include "pink/include/bg_thread.h"
#include "include/zp_const.h"

ZPMetaConditionCron::ZPMetaConditionCron(ZPMetaInfoStore* i_store,
    ZPMetaMigrateRegister* migrate,
    ZPMetaUpdateThread* update_thread)
  : info_store_(i_store),
  migrate_(migrate),
  update_thread_(update_thread) {
    bg_thread_ = new pink::BGThread(1024 * 1024 * 256);
    bg_thread_->set_thread_name("ZPMetaCondition");
  }

ZPMetaConditionCron::~ZPMetaConditionCron() {
  Abandon();
  delete bg_thread_;
}

void ZPMetaConditionCron::Active() {
  int ret = bg_thread_->StartThread();
  if (ret != 0) {
    LOG(FATAL) << "Failed to start meta condition cron, ret: " << ret;
  }
  LOG(INFO) << "Start condition thread succ: " << std::hex
    << bg_thread_->thread_id(); 
}

void ZPMetaConditionCron::Abandon() {
  bg_thread_->StopThread();
  bg_thread_->QueueClear();
}

void ZPMetaConditionCron::AddCronTask(const OffsetCondition& condition,
    const std::vector<UpdateTask>& update_set) {
  OffsetConditionArg* oarg = new OffsetConditionArg(this,
      condition, update_set);
  bg_thread_->DelaySchedule(kConditionCronInterval,
      &CronFunc, static_cast<void*>(oarg));
}

void ZPMetaConditionCron::CronFunc(void *p) {
  OffsetConditionArg* arg = static_cast<OffsetConditionArg*>(p);
  if (arg->cron->ChecknProcess(arg->condition, arg->update_set)) {
    delete arg;
    return;
  }

  // Try next time
  arg->cron->bg_thread_->DelaySchedule(kConditionCronInterval, &CronFunc, p);
}

// Error happend, Recover migrate or stuck partition before disard
bool ZPMetaConditionCron::RecoverWhenError(const OffsetCondition& condition) {
  Status s = Status::OK();
  UpdateTask task;
  switch (condition.error_tag) {
    case ConditionErrorTag::kRecoverMigrate:
      migrate_->PutN(1);
      // Notice: no break here
    case ConditionErrorTag::kRecoverActive:
      task.op = kOpSetActive;
      task.print_args_text = [condition]() {
        std::ostringstream out;
        out << "task: SetActive, table: " << condition.table
            << ", partition: " << condition.partition_id;
        return out.str();
      };
      task.sargs[0] = condition.table;
      task.iargs[0] = condition.partition_id;
      s = update_thread_->PendingUpdate(task);
      break;
    default:
      return true;
  }

  if (!s.ok()) {
    LOG(WARNING) << "Cron recover when error happend failed: " << s.ToString()
      << ", table: " << condition.table
      << ", partition: " << condition.partition_id
      << ", left: " << condition.left.ip() << ":" << condition.left.port();
  }
  return s.ok();
}

bool ZPMetaConditionCron::ChecknProcess(const OffsetCondition& condition,
    const std::vector<UpdateTask>& update_set) {
  // Check offset
  NodeOffset left_offset, right_offset;
  Status s = info_store_->GetNodeOffset(condition.left,
      condition.table, condition.partition_id,
      &left_offset);
  if (!s.ok()) {
    LOG(WARNING) << "ConditionCron left offset get failed: " << s.ToString()
      << ", table: " << condition.table
      << ", partition: " << condition.partition_id
      << ", left: " << condition.left.ip() << ":" << condition.left.port();
    return RecoverWhenError(condition);
  }
  s = info_store_->GetNodeOffset(condition.right,
      condition.table, condition.partition_id,
      &right_offset);
  if (!s.ok()) {
    LOG(WARNING) << "ConditionCron right offset get failed: " << s.ToString()
      << ", table: " << condition.table
      << ", partition: " << condition.partition_id
      << ", right: " << condition.right.ip() << ":" << condition.right.port();
    return RecoverWhenError(condition);
  }

  // Check condition
  switch (condition.type) {
    case ConditionType::kCloseToNotEqual:
      if (left_offset.filenum != right_offset.filenum
          || (left_offset.offset - right_offset.offset
            > g_zp_conf->stuck_offset_dist())) {
        return false;  // Not yet
      } else if (left_offset <= right_offset) {
        // We miss the condition, just abandon it
        return RecoverWhenError(condition);
      }
      break; // Met ClostToNotEqual here
    case ConditionType::kEqual:
      if (left_offset != right_offset) {
        return false;  // Not yet
      }
      break; // Met Equal here
    default:
      LOG(WARNING) << "ConditionCron unknown ConditionType: "
        << static_cast<int>(condition.type)
        << ", table: " << condition.table
        << ", partition: " << condition.partition_id
        << ", right: " << condition.right.ip() << ":" << condition.right.port();
      return RecoverWhenError(condition);
  }

  // Met the condition
  for (const auto& update : update_set) {
    s = update_thread_->PendingUpdate(update);
    if (!s.ok()) {
      // Retry next time
      LOG(WARNING) << "Pending update when met condition failed: "
        << s.ToString()
        << ", table: " << condition.table
        << ", partition: " << condition.partition_id
        << ", left node: " << condition.left.ip()
        << ":" << condition.left.port()
        << ", right node: " << condition.right.ip()
        << ":" << condition.right.port();
      return false;
    }
  }

  return true;
}
