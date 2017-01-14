// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "table_impl.h"
#include "tera.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fstream>
#include <sstream>

#include <boost/bind.hpp>
#include <gflags/gflags.h>

#include "common/base/string_format.h"
#include "common/file/file_path.h"
#include "common/file/recordio/record_io.h"

#include "io/coding.h"
#include "proto/kv_helper.h"
#include "proto/proto_helper.h"
#include "proto/tabletnode_client.h"
#include "sdk/cookie.h"
#include "sdk/mutate_impl.h"
#include "sdk/read_impl.h"
#include "sdk/single_row_txn.h"
#include "sdk/scan_impl.h"
#include "sdk/schema_impl.h"
#include "sdk/sdk_zk.h"
#include "utils/crypt.h"
#include "utils/string_util.h"
#include "utils/timer.h"

DECLARE_string(tera_master_meta_table_name);
DECLARE_int32(tera_sdk_delay_send_internal);
DECLARE_int32(tera_sdk_retry_times);
DECLARE_int32(tera_sdk_retry_period);
DECLARE_int32(tera_sdk_update_meta_internal);
DECLARE_bool(tera_sdk_write_sync);
DECLARE_int32(tera_sdk_batch_size);
DECLARE_int32(tera_sdk_write_send_interval);
DECLARE_int32(tera_sdk_read_send_interval);
DECLARE_int64(tera_sdk_max_mutation_pending_num);
DECLARE_int64(tera_sdk_max_reader_pending_num);
DECLARE_bool(tera_sdk_async_blocking_enabled);
DECLARE_int32(tera_sdk_timeout);
DECLARE_int32(tera_sdk_scan_buffer_limit);
DECLARE_int32(tera_sdk_update_meta_concurrency);
DECLARE_int32(tera_sdk_update_meta_buffer_limit);
DECLARE_bool(tera_sdk_cookie_enabled);
DECLARE_string(tera_sdk_cookie_path);
DECLARE_int32(tera_sdk_cookie_update_interval);
DECLARE_bool(tera_sdk_perf_counter_enabled);
DECLARE_int64(tera_sdk_perf_counter_log_interval);
DECLARE_int32(tera_rpc_timeout_period);

namespace tera {

TableImpl::TableImpl(const std::string& table_name,
                     common::ThreadPool* thread_pool,
                     sdk::ClusterFinder* cluster)
    : name_(table_name),
      create_time_(0),
      last_sequence_id_(0),
      timeout_(FLAGS_tera_sdk_timeout),
      commit_size_(FLAGS_tera_sdk_batch_size),
      write_commit_timeout_(FLAGS_tera_sdk_write_send_interval),
      read_commit_timeout_(FLAGS_tera_sdk_read_send_interval),
      mutation_batch_seq_(0),
      reader_batch_seq_(0),
      max_commit_pending_num_(FLAGS_tera_sdk_max_mutation_pending_num),
      max_reader_pending_num_(FLAGS_tera_sdk_max_reader_pending_num),
      meta_cond_(&meta_mutex_),
      meta_updating_count_(0),
      table_meta_cond_(&table_meta_mutex_),
      table_meta_updating_(false),
      task_pool_(thread_pool),
      master_client_(NULL),
      tabletnode_client_(NULL),
      thread_pool_(thread_pool),
      cluster_(cluster),
      cluster_private_(false),
      pending_timeout_ms_(FLAGS_tera_rpc_timeout_period) {
    if (cluster_ == NULL) {
        cluster_ = sdk::NewClusterFinder();
        cluster_private_ = true;
    }
}

TableImpl::~TableImpl() {
    ClearDelayTask();
    if (FLAGS_tera_sdk_cookie_enabled) {
        DoDumpCookie();
    }
    if (cluster_private_) {
        delete cluster_;
    }
}

RowMutation* TableImpl::NewRowMutation(const std::string& row_key) {
    RowMutationImpl* row_mu = new RowMutationImpl(this, row_key);
    return row_mu;
}

RowReader* TableImpl::NewRowReader(const std::string& row_key) {
    RowReaderImpl* row_rd = new RowReaderImpl(this, row_key);
    return row_rd;
}

void TableImpl::Put(RowMutation* row_mu) {
    ApplyMutation(row_mu);
}

void TableImpl::Put(const std::vector<RowMutation*>& row_mutations) {
    ApplyMutation(row_mutations);
}

void TableImpl::ApplyMutation(RowMutation* row_mu) {
    if (row_mu->GetError().GetType() != ErrorCode::kOK) {
        ThreadPool::Task task =
            boost::bind(&RowMutationImpl::RunCallback,
                        static_cast<RowMutationImpl*>(row_mu));
        thread_pool_->AddTask(task);
        return;
    }
    std::vector<RowMutationImpl*> mu_list;
    mu_list.push_back(static_cast<RowMutationImpl*>(row_mu));
    DistributeMutations(mu_list, true);
}

void TableImpl::ApplyMutation(const std::vector<RowMutation*>& row_mutations) {
    std::vector<RowMutationImpl*> mu_list;
    for (uint32_t i = 0; i < row_mutations.size(); i++) {
        if (row_mutations[i]->GetError().GetType() != ErrorCode::kOK) {
            ThreadPool::Task task =
                boost::bind(&RowMutationImpl::RunCallback,
                            static_cast<RowMutationImpl*>(row_mutations[i]));
            thread_pool_->AddTask(task);
            continue;
        }
        mu_list.push_back(static_cast<RowMutationImpl*>(row_mutations[i]));
    }
    DistributeMutations(mu_list, true);
}

bool TableImpl::Put(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, const int64_t value,
                    ErrorCode* err) {
    std::string value_str((char*)&value, sizeof(int64_t));
    return Put(row_key, family, qualifier, value_str, err);
}

bool TableImpl::Put(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, const std::string& value,
                    ErrorCode* err) {
    RowMutation* row_mu = NewRowMutation(row_key);
    row_mu->Put(family, qualifier, value);
    ApplyMutation(row_mu);
    *err = row_mu->GetError();
    return (err->GetType() == ErrorCode::kOK ? true : false);
}

bool TableImpl::Put(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, const std::string& value,
                    int64_t timestamp, ErrorCode* err) {
    RowMutation* row_mu = NewRowMutation(row_key);
    row_mu->Put(family, qualifier, timestamp, value);
    ApplyMutation(row_mu);
    *err = row_mu->GetError();
    return (err->GetType() == ErrorCode::kOK ? true : false);
}

bool TableImpl::Put(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, const std::string& value,
                    int32_t ttl, ErrorCode* err) {
    RowMutation* row_mu = NewRowMutation(row_key);
    row_mu->Put(family, qualifier, value, ttl);
    ApplyMutation(row_mu);
    *err = row_mu->GetError();
    return (err->GetType() == ErrorCode::kOK ? true : false);
}

bool TableImpl::Put(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, const std::string& value,
                    int64_t timestamp, int32_t ttl, ErrorCode* err) {
    RowMutation* row_mu = NewRowMutation(row_key);
    row_mu->Put(family, qualifier, timestamp, value, ttl);
    ApplyMutation(row_mu);
    *err = row_mu->GetError();
    return (err->GetType() == ErrorCode::kOK ? true : false);
}

bool TableImpl::Add(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, int64_t delta, ErrorCode* err) {
    RowMutation* row_mu = NewRowMutation(row_key);
    row_mu->Add(family, qualifier, delta);
    ApplyMutation(row_mu);
    *err = row_mu->GetError();
    return (err->GetType() == ErrorCode::kOK ? true : false);
}

bool TableImpl::AddInt64(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, int64_t delta, ErrorCode* err) {
    RowMutation* row_mu = NewRowMutation(row_key);
    row_mu->AddInt64(family, qualifier, delta);
    ApplyMutation(row_mu);
    *err = row_mu->GetError();
    return (err->GetType() == ErrorCode::kOK ? true : false);
}

bool TableImpl::PutIfAbsent(const std::string& row_key, const std::string& family,
                            const std::string& qualifier, const std::string& value,
                            ErrorCode* err) {
    RowMutation* row_mu = NewRowMutation(row_key);
    row_mu->PutIfAbsent(family, qualifier, value);
    ApplyMutation(row_mu);
    *err = row_mu->GetError();
    return (err->GetType() == ErrorCode::kOK ? true : false);
}

bool TableImpl::Append(const std::string& row_key, const std::string& family,
                       const std::string& qualifier, const std::string& value,
                       ErrorCode* err) {
    RowMutation* row_mu = NewRowMutation(row_key);
    row_mu->Append(family, qualifier, value);
    ApplyMutation(row_mu);
    *err = row_mu->GetError();
    return (err->GetType() == ErrorCode::kOK ? true : false);
}

bool TableImpl::Flush() {
    return false;
}

bool TableImpl::CheckAndApply(const std::string& rowkey, const std::string& cf_c,
                              const std::string& value, const RowMutation& row_mu,
                              ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return false;
}

int64_t TableImpl::IncrementColumnValue(const std::string& row,
                                        const std::string& family,
                                        const std::string& qualifier,
                                        int64_t amount, ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return 0L;
}

void TableImpl::SetWriteTimeout(int64_t timeout_ms) {
}

void TableImpl::Get(RowReader* row_reader) {
    std::vector<RowReaderImpl*> row_reader_list;
    row_reader_list.push_back(static_cast<RowReaderImpl*>(row_reader));
    DistributeReaders(row_reader_list, true);
}

void TableImpl::Get(const std::vector<RowReader*>& row_readers) {
    std::vector<RowReaderImpl*> row_reader_list(row_readers.size());
    for (uint32_t i = 0; i < row_readers.size(); ++i) {
        row_reader_list[i] = static_cast<RowReaderImpl*>(row_readers[i]);
    }
    DistributeReaders(row_reader_list, true);
}

bool TableImpl::Get(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, int64_t* value,
                    ErrorCode* err) {
    return Get(row_key, family, qualifier, value, 0, err);
}

bool TableImpl::Get(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, std::string* value,
                    ErrorCode* err) {
    return Get(row_key, family, qualifier, value, 0, err);
}

bool TableImpl::Get(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, int64_t* value,
                    ErrorCode* err, uint64_t snapshot_id) {
    return Get(row_key, family, qualifier, value, snapshot_id, err);
}

bool TableImpl::Get(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, int64_t* value,
                    uint64_t snapshot_id, ErrorCode* err) {
    std::string value_str;
    if (Get(row_key, family, qualifier, &value_str, err, snapshot_id)
        && value_str.size() == sizeof(int64_t)) {
        *value = *(int64_t*)value_str.c_str();
        return true;
    }
    return false;
}

bool TableImpl::Get(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, std::string* value,
                    ErrorCode* err, uint64_t snapshot_id) {
    return Get(row_key, family, qualifier, value, snapshot_id, err);
}

bool TableImpl::Get(const std::string& row_key, const std::string& family,
                    const std::string& qualifier, std::string* value,
                    uint64_t snapshot_id, ErrorCode* err) {
    RowReader* row_reader = NewRowReader(row_key);
    row_reader->AddColumn(family, qualifier);
    row_reader->SetSnapshot(snapshot_id);
    Get(row_reader);
    *err = row_reader->GetError();
    if (err->GetType() == ErrorCode::kOK) {
        *value = row_reader->Value();
        return true;
    }
    return false;
}

ResultStream* TableImpl::Scan(const ScanDescriptor& desc, ErrorCode* err) {
    ScanDescImpl * impl = desc.GetImpl();
    impl->SetTableSchema(table_schema_);
    ResultStream * results = NULL;
    if (desc.IsAsync() && (table_schema_.raw_key() != GeneralKv)) {
        VLOG(6) << "activate async-scan";
        results = new ResultStreamBatchImpl(this, impl);
    } else {
        VLOG(6) << "activate sync-scan";
        results = new ResultStreamSyncImpl(this, impl);
    }
    return results;
}

void TableImpl::ScanTabletSync(ResultStreamSyncImpl* stream) {
    ScanTabletAsync(stream);
    stream->Wait();
}

void TableImpl::ScanTabletAsync(ResultStreamImpl* stream) {
    ScanTask* scan_task = new ScanTask;
    scan_task->stream = stream;
    stream->GetRpcHandle(&scan_task->request, &scan_task->response);
    ScanTabletAsync(scan_task, true);
}

void TableImpl::ScanTabletAsync(ScanTask* scan_task, bool called_by_user) {
    if (called_by_user) {
        scan_task->SetId(next_task_id_.Inc());
        task_pool_.PutTask(scan_task);
    }

    const std::string& row_key = scan_task->stream->GetScanDesc()->GetStartRowKey();
    std::string server_addr;
    if (GetTabletAddrOrScheduleUpdateMeta(row_key, scan_task, &server_addr)) {
        CommitScan(scan_task, server_addr);
    }
}

void TableImpl::CommitScan(ScanTask* scan_task,
                           const std::string& server_addr) {
    tabletnode::TabletNodeClient tabletnode_client(server_addr);
    ResultStreamImpl* stream = scan_task->stream;
    ScanTabletRequest* request = scan_task->request;
    ScanTabletResponse* response = scan_task->response;
    response->Clear();

    ScanDescImpl* impl = stream->GetScanDesc();
    request->set_sequence_id(last_sequence_id_++);
    request->set_table_name(name_);
    request->set_start(impl->GetStartRowKey());
    request->set_end(impl->GetEndRowKey());
    request->set_snapshot_id(impl->GetSnapshot());
    request->set_timeout(impl->GetPackInterval());
    if (impl->GetStartColumnFamily() != "") {
        request->set_start_family(impl->GetStartColumnFamily());
    }
    if (impl->GetStartQualifier() != "") {
        request->set_start_qualifier(impl->GetStartQualifier());
    }
    if (impl->GetStartTimeStamp() != 0) {
        request->set_start_timestamp(impl->GetStartTimeStamp());
    }
    if (impl->GetMaxVersion() != 0) {
        request->set_max_version(impl->GetMaxVersion());
    }
    if (impl->GetBufferSize() != 0) {
        request->set_buffer_limit(impl->GetBufferSize());
    }
    if (impl->GetNumberLimit() != 0) {
        request->set_number_limit(impl->GetNumberLimit());
    }
    if (impl->GetTimerRange() != NULL) {
        TimeRange* time_range = request->mutable_timerange();
        time_range->CopyFrom(*(impl->GetTimerRange()));
    }
    if (impl->GetFilterString().size() > 0) {
        FilterList* filter_list = request->mutable_filter_list();
        filter_list->CopyFrom(impl->GetFilterList());
    }
    for (int32_t i = 0; i < impl->GetSizeofColumnFamilyList(); ++i) {
        tera::ColumnFamily* column_family = request->add_cf_list();
        column_family->CopyFrom(*(impl->GetColumnFamily(i)));
    }

    request->set_timestamp(common::timer::get_micros());
    Closure<void, ScanTabletRequest*, ScanTabletResponse*, bool, int>* done =
        NewClosure(this, &TableImpl::ScanCallBack, scan_task);
    tabletnode_client.ScanTablet(request, response, done);
}

void TableImpl::ScanCallBack(ScanTask* scan_task,
                             ScanTabletRequest* request,
                             ScanTabletResponse* response,
                             bool failed, int error_code) {
    perf_counter_.rpc_s.Add(common::timer::get_micros() - request->timestamp());
    perf_counter_.rpc_s_cnt.Inc();
    ResultStreamImpl* stream = scan_task->stream;

    if (failed) {
        if (error_code == sofa::pbrpc::RPC_ERROR_SERVER_SHUTDOWN ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNREACHABLE ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNAVAILABLE) {
            response->set_status(kServerError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_CANCELED ||
                   error_code == sofa::pbrpc::RPC_ERROR_SEND_BUFFER_FULL) {
            response->set_status(kClientError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_CONNECTION_CLOSED ||
                   error_code == sofa::pbrpc::RPC_ERROR_RESOLVE_ADDRESS) {
            response->set_status(kConnectError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_TIMEOUT) {
            response->set_status(kRPCTimeout);
        } else {
            response->set_status(kRPCError);
        }
    }

    StatusCode err = response->status();
    if (err != kTabletNodeOk && err != kSnapshotNotExist) {
        VLOG(10) << "fail to scan table: " << name_
            << " errcode: " << StatusCodeToString(err);
    }

    scan_task->SetInternalError(err);
    if (err == kTabletNodeOk
        || err == kSnapshotNotExist
        || scan_task->RetryTimes() >= static_cast<uint32_t>(FLAGS_tera_sdk_retry_times)) {
        if (err == kKeyNotInRange || err == kConnectError) {
            ScheduleUpdateMeta(stream->GetScanDesc()->GetStartRowKey(),
                               scan_task->GetMetaTimeStamp());
        }
        stream->OnFinish(request, response);
        stream->ReleaseRpcHandle(request, response);
        task_pool_.PopTask(scan_task->GetId());
        CHECK_EQ(scan_task->GetRef(), 2);
        delete scan_task;
    } else {
        scan_task->IncRetryTimes();
        ThreadPool::Task retry_task =
            boost::bind(&TableImpl::ScanTabletAsync, this, scan_task, false);
        CHECK(scan_task->RetryTimes() > 0);
        int64_t retry_interval =
            static_cast<int64_t>(pow(FLAGS_tera_sdk_delay_send_internal,
                                     scan_task->RetryTimes() - 1) * 1000);
        thread_pool_->DelayTask(retry_interval, retry_task);
    }
}

void TableImpl::SetReadTimeout(int64_t timeout_ms) {
}

bool TableImpl::LockRow(const std::string& rowkey, RowLock* lock, ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return false;
}

bool TableImpl::GetStartEndKeys(std::string* start_key, std::string* end_key,
                                ErrorCode* err) {
    err->SetFailed(ErrorCode::kNotImpl);
    return false;
}

bool TableImpl::OpenInternal(ErrorCode* err) {
    if (!UpdateTableMeta(err)) {
        LOG(ERROR) << "fail to update table meta.";
        return false;
    }
    if (FLAGS_tera_sdk_cookie_enabled) {
        if (!RestoreCookie()) {
            LOG(ERROR) << "fail to restore cookie.";
            return false;
        }
        EnableCookieUpdateTimer();
    }
    if (FLAGS_tera_sdk_perf_counter_enabled) {
        DumpPerfCounterLogDelay();
    }
    LOG(INFO) << "open table " << name_ << " at cluster " << cluster_->ClusterId();
    return true;
}

struct MuFlushPair {
    std::vector<RowMutationImpl*> mu_list;
    bool flush;
    MuFlushPair() : flush(false) {}
};

void TableImpl::DistributeMutations(const std::vector<RowMutationImpl*>& mu_list,
                                    bool called_by_user) {
    typedef std::map<std::string, MuFlushPair> TsMuMap;
    TsMuMap ts_mu_list;

    int64_t sync_min_timeout = -1;
    std::vector<RowMutationImpl*> sync_mu_list;

    // evaluate minimum timeout of sync requests
    if (called_by_user) {
        for (uint32_t i = 0; i < mu_list.size(); i++) {
            RowMutationImpl* row_mutation = (RowMutationImpl*)mu_list[i];
            if (!row_mutation->IsAsync()) {
                sync_mu_list.push_back(row_mutation);
                int64_t row_timeout = row_mutation->TimeOut() > 0 ? row_mutation->TimeOut() : timeout_;
                if (row_timeout > 0 && (sync_min_timeout <= 0 || sync_min_timeout > row_timeout)) {
                    sync_min_timeout = row_timeout;
                }
            }
        }
    }

    for (uint32_t i = 0; i < mu_list.size(); i++) {
        RowMutationImpl* row_mutation = (RowMutationImpl*)mu_list[i];
        perf_counter_.mutate_cnt.Inc();
        if (called_by_user) {
            row_mutation->SetId(next_task_id_.Inc());

            int64_t row_timeout = -1;
            if (!row_mutation->IsAsync()) {
                row_timeout = sync_min_timeout;
            } else {
                row_timeout = row_mutation->TimeOut() > 0 ? row_mutation->TimeOut() : timeout_;
            }
            SdkTask::TimeoutFunc task = boost::bind(&TableImpl::MutationTimeout, this, _1);
            task_pool_.PutTask(row_mutation, row_timeout, task);
        }

        // flow control
        if (called_by_user
            && cur_commit_pending_counter_.Add(row_mutation->MutationNum()) > max_commit_pending_num_
            && row_mutation->IsAsync()) {
            if (FLAGS_tera_sdk_async_blocking_enabled) {
                while (cur_commit_pending_counter_.Get() > max_commit_pending_num_) {
                    usleep(100000);
                }
            } else {
                cur_commit_pending_counter_.Sub(row_mutation->MutationNum());
                row_mutation->SetError(ErrorCode::kBusy, "pending too much mutations, try it later.");
                ThreadPool::Task task =
                    boost::bind(&TableImpl::BreakRequest, this, row_mutation->GetId());
                row_mutation->DecRef();
                thread_pool_->AddTask(task);
                continue;
            }
        }

        std::string server_addr;
        if (!GetTabletAddrOrScheduleUpdateMeta(row_mutation->RowKey(),
                                               row_mutation, &server_addr)) {
            continue;
        }

        MuFlushPair& mu_flush_pair = ts_mu_list[server_addr];
        std::vector<RowMutationImpl*>& ts_row_mutations = mu_flush_pair.mu_list;
        ts_row_mutations.push_back(row_mutation);

        if (!row_mutation->IsAsync()) {
            mu_flush_pair.flush = true;
        }
    }

    TsMuMap::iterator it = ts_mu_list.begin();
    for (; it != ts_mu_list.end(); ++it) {
        MuFlushPair& mu_flush_pair = it->second;
        PackMutations(it->first, mu_flush_pair.mu_list, mu_flush_pair.flush);
    }
    // 从现在开始，所有异步的row_mutation都不可以再操作了，因为随时会被用户释放

    // 不是用户调用的，立即返回
    if (!called_by_user) {
        return;
    }

    // 等待同步操作返回或超时
    for (uint32_t i = 0; i < sync_mu_list.size(); i++) {
        while (cur_commit_pending_counter_.Get() > max_commit_pending_num_) {
            usleep(100000);
        }

        RowMutationImpl* row_mutation = (RowMutationImpl*)sync_mu_list[i];
        row_mutation->Wait();
    }
}

void TableImpl::DistributeMutationsById(std::vector<int64_t>* mu_id_list) {
    std::vector<RowMutationImpl*> mu_list;
    for (uint32_t i = 0; i < mu_id_list->size(); ++i) {
        int64_t mu_id = (*mu_id_list)[i];
        SdkTask* task = task_pool_.GetTask(mu_id);
        if (task == NULL) {
            VLOG(10) << "mutation " << mu_id << " timeout when retry mutate";;
            continue;
        }
        CHECK_EQ(task->Type(), SdkTask::MUTATION);
        RowMutationImpl* row_mutation = (RowMutationImpl*)task;
        mu_list.push_back(row_mutation);
    }
    DistributeMutations(mu_list, false);
    delete mu_id_list;
}

void TableImpl::PackMutations(const std::string& server_addr,
                              std::vector<RowMutationImpl*>& mu_list,
                              bool flush) {
    MutexLock lock(&mutation_batch_mutex_);
    TaskBatch* mutation_batch = NULL;
    for (size_t i = 0; i < mu_list.size(); ++i) {
        // find existing batch or create a new batch
        if (mutation_batch == NULL) {
            std::map<std::string, TaskBatch>::iterator it = mutation_batch_map_.find(server_addr);
            if (it != mutation_batch_map_.end()) {
                mutation_batch = &it->second;
            } else {
                mutation_batch = &mutation_batch_map_[server_addr];
                mutation_batch->sequence_num = mutation_batch_seq_++;
                mutation_batch->row_id_list = new std::vector<int64_t>;
                ThreadPool::Task task = boost::bind(&TableImpl::MutationBatchTimeout, this,
                                                    server_addr, mutation_batch->sequence_num);
                int64_t timer_id = thread_pool_->DelayTask(write_commit_timeout_, task);
                mutation_batch->timer_id = timer_id;
                mutation_batch->byte_size = 0;
            }
        }

        // put mutation into the batch
        RowMutationImpl* row_mutation = mu_list[i];
        mutation_batch->row_id_list->push_back(row_mutation->GetId());
        mutation_batch->byte_size += row_mutation->Size();
        row_mutation->DecRef();

        // commit the batch if:
        // 1) batch_byte_size >= max_rpc_byte_size
        // for the *LAST* batch, commit it if:
        // 2) any mutation is sync (flush == true)
        // 3) batch_row_num >= min_batch_row_num
        if (mutation_batch->byte_size >= kMaxRpcSize ||
            (i == mu_list.size() - 1 &&
             (flush || mutation_batch->row_id_list->size() >= commit_size_))) {
            std::vector<int64_t>* mu_id_list = mutation_batch->row_id_list;
            uint64_t timer_id = mutation_batch->timer_id;
            const bool non_block_cancel = true;
            bool is_running = false;
            if (!thread_pool_->CancelTask(timer_id, non_block_cancel, &is_running)) {
                CHECK(is_running); // this delay task must be waiting for mutation_batch_mutex_
            }
            mutation_batch_map_.erase(server_addr);
            mutation_batch_mutex_.Unlock();
            CommitMutationsById(server_addr, *mu_id_list);
            delete mu_id_list;
            mutation_batch = NULL;
            mutation_batch_mutex_.Lock();
        }
    }
}

void TableImpl::MutationBatchTimeout(std::string server_addr, uint64_t batch_seq) {
    std::vector<int64_t>* mu_id_list = NULL;
    {
        MutexLock lock(&mutation_batch_mutex_);
        std::map<std::string, TaskBatch>::iterator it =
            mutation_batch_map_.find(server_addr);
        if (it == mutation_batch_map_.end()) {
            return;
        }
        TaskBatch* mutation_batch = &it->second;
        if (mutation_batch->sequence_num != batch_seq) {
            return;
        }
        mu_id_list = mutation_batch->row_id_list;
        mutation_batch_map_.erase(it);
    }
    CommitMutationsById(server_addr, *mu_id_list);
    delete mu_id_list;
}

void TableImpl::CommitMutationsById(const std::string& server_addr,
                                    std::vector<int64_t>& mu_id_list) {
    std::vector<RowMutationImpl*> mu_list;
    for (size_t i = 0; i < mu_id_list.size(); i++) {
        int64_t mu_id = mu_id_list[i];
        SdkTask* task = task_pool_.GetTask(mu_id);
        if (task == NULL) {
            VLOG(10) << "mutation " << mu_id << " timeout";
            continue;
        }
        CHECK_EQ(task->Type(), SdkTask::MUTATION);
        mu_list.push_back((RowMutationImpl*)task);
    }
    CommitMutations(server_addr, mu_list);
}

void TableImpl::CommitMutations(const std::string& server_addr,
                                std::vector<RowMutationImpl*>& mu_list) {
    tabletnode::TabletNodeClient tabletnode_client_async(server_addr);
    WriteTabletRequest* request = new WriteTabletRequest;
    WriteTabletResponse* response = new WriteTabletResponse;
    request->set_sequence_id(last_sequence_id_++);
    request->set_tablet_name(name_);
    request->set_is_sync(FLAGS_tera_sdk_write_sync);

    std::vector<int64_t>* mu_id_list = new std::vector<int64_t>;
    for (uint32_t i = 0; i < mu_list.size(); ++i) {
        RowMutationImpl* row_mutation = mu_list[i];
        RowMutationSequence* mu_seq = request->add_row_list();
        mu_seq->set_row_key(row_mutation->RowKey());
        for (uint32_t j = 0; j < row_mutation->MutationNum(); j++) {
            const RowMutation::Mutation& mu = row_mutation->GetMutation(j);
            tera::Mutation* mutation = mu_seq->add_mutation_sequence();
            SerializeMutation(mu, mutation);
        }
        SingleRowTxn* txn = (SingleRowTxn*)(row_mutation->GetTransaction());
        if (txn != NULL) {
            txn->Serialize(mu_seq);
        }
        mu_id_list->push_back(row_mutation->GetId());
        row_mutation->AddCommitTimes();
        row_mutation->DecRef();
    }

    VLOG(20) << "commit " << mu_list.size() << " mutations to " << server_addr;
    request->set_timestamp(common::timer::get_micros());
    Closure<void, WriteTabletRequest*, WriteTabletResponse*, bool, int>* done =
        NewClosure(this, &TableImpl::MutateCallBack, mu_id_list);
    tabletnode_client_async.WriteTablet(request, response, done);
}

void TableImpl::MutateCallBack(std::vector<int64_t>* mu_id_list,
                               WriteTabletRequest* request,
                               WriteTabletResponse* response,
                               bool failed, int error_code) {
    perf_counter_.rpc_w.Add(common::timer::get_micros() - request->timestamp());
    perf_counter_.rpc_w_cnt.Inc();
    if (failed) {
        if (error_code == sofa::pbrpc::RPC_ERROR_SERVER_SHUTDOWN ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNREACHABLE ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNAVAILABLE) {
            response->set_status(kServerError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_CANCELED ||
                   error_code == sofa::pbrpc::RPC_ERROR_SEND_BUFFER_FULL) {
            response->set_status(kClientError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_CONNECTION_CLOSED ||
                   error_code == sofa::pbrpc::RPC_ERROR_RESOLVE_ADDRESS) {
            response->set_status(kConnectError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_TIMEOUT) {
            response->set_status(kRPCTimeout);
        } else {
            response->set_status(kRPCError);
        }
    }

    std::map<uint32_t, std::vector<int64_t>* > retry_times_list;
    std::vector<RowMutationImpl*> not_in_range_list;
    for (uint32_t i = 0; i < mu_id_list->size(); ++i) {
        const std::string& row = request->row_list(i).row_key();
        int64_t mu_id = (*mu_id_list)[i];
        StatusCode err = response->status();
        if (err == kTabletNodeOk) {
            err = response->row_status_list(i);
        }

        if (err == kTabletNodeOk || err == kTxnFail) {
            perf_counter_.mutate_ok_cnt.Inc();
            SdkTask* task = task_pool_.PopTask(mu_id);
            if (task == NULL) {
                VLOG(10) << "mutation " << mu_id << " finish but timeout: " << DebugString(row);
                continue;
            }
            CHECK_EQ(task->Type(), SdkTask::MUTATION);
            CHECK_EQ(task->GetRef(), 1);
            RowMutationImpl* row_mutation = (RowMutationImpl*)task;
            if (err == kTabletNodeOk) {
                row_mutation->SetError(ErrorCode::kOK);
            } else {
                row_mutation->SetError(ErrorCode::kTxnFail, "transaction commit fail");
            }

            // only for flow control
            cur_commit_pending_counter_.Sub(row_mutation->MutationNum());
            int64_t perf_time = common::timer::get_micros();
            row_mutation->RunCallback();
            perf_counter_.user_callback.Add(common::timer::get_micros() - perf_time);
            perf_counter_.user_callback_cnt.Inc();
            continue;
        }
        perf_counter_.mutate_fail_cnt.Inc();

        VLOG(10) << "fail to mutate table: " << name_
            << " row: " << DebugString(row)
            << " errcode: " << StatusCodeToString(err);

        SdkTask* task = task_pool_.GetTask(mu_id);
        if (task == NULL) {
            VLOG(10) << "mutation " << mu_id << " timeout: " << DebugString(row);
            continue;
        }
        CHECK_EQ(task->Type(), SdkTask::MUTATION);
        RowMutationImpl* row_mutation = (RowMutationImpl*)task;
        row_mutation->SetInternalError(err);

        if (err == kKeyNotInRange) {
            perf_counter_.mutate_range_cnt.Inc();
            row_mutation->IncRetryTimes();
            not_in_range_list.push_back(row_mutation);
        } else {
            row_mutation->IncRetryTimes();
            std::vector<int64_t>* retry_mu_id_list = NULL;
            std::map<uint32_t, std::vector<int64_t>* >::iterator it =
                retry_times_list.find(row_mutation->RetryTimes());
            if (it != retry_times_list.end()) {
                retry_mu_id_list = it->second;
            } else {
                retry_mu_id_list = new std::vector<int64_t>;
                retry_times_list[row_mutation->RetryTimes()] = retry_mu_id_list;
            }
            retry_mu_id_list->push_back(mu_id);
            row_mutation->DecRef();
        }
    }

    if (not_in_range_list.size() > 0) {
        DistributeMutations(not_in_range_list, false);
    }
    std::map<uint32_t, std::vector<int64_t>* >::iterator it;
    for (it = retry_times_list.begin(); it != retry_times_list.end(); ++it) {
        int64_t retry_interval =
            static_cast<int64_t>(pow(FLAGS_tera_sdk_delay_send_internal, it->first) * 1000);
        ThreadPool::Task retry_task =
            boost::bind(&TableImpl::DistributeMutationsById, this, it->second);
        thread_pool_->DelayTask(retry_interval, retry_task);
    }

    delete request;
    delete response;
    delete mu_id_list;
}

void TableImpl::MutationTimeout(SdkTask* task) {
    perf_counter_.mutate_timeout_cnt.Inc();
    CHECK_NOTNULL(task);
    CHECK_EQ(task->Type(), SdkTask::MUTATION);

    RowMutationImpl* row_mutation = (RowMutationImpl*)task;
    row_mutation->ExcludeOtherRef();

    StatusCode err = row_mutation->GetInternalError();
    if (err == kKeyNotInRange || err == kConnectError) {
        ScheduleUpdateMeta(row_mutation->RowKey(),
                           row_mutation->GetMetaTimeStamp());
    }
    if (row_mutation->RetryTimes() == 0) {
        perf_counter_.mutate_queue_timeout_cnt.Inc();
        std::string err_reason = StringFormat("commit %lld times, retry 0 times, in %u ms.",
                                              row_mutation->GetCommitTimes(), timeout_);
        row_mutation->SetError(ErrorCode::kTimeout, err_reason);
    } else {
        std::string err_reason = StringFormat("commit %lld times, retry %u times, in %u ms. last error: %s",
                                              row_mutation->GetCommitTimes(), row_mutation->RetryTimes(),
                                              timeout_, StatusCodeToString(err).c_str());
        row_mutation->SetError(ErrorCode::kSystem, err_reason);
    }
    // only for flow control
    cur_commit_pending_counter_.Sub(row_mutation->MutationNum());
    int64_t perf_time = common::timer::get_micros();
    row_mutation->RunCallback();
    perf_counter_.user_callback.Add(common::timer::get_micros() - perf_time);
    perf_counter_.user_callback_cnt.Inc();
}

bool TableImpl::GetTabletLocation(std::vector<TabletInfo>* tablets,
                                  ErrorCode* err) {
    return false;
}

bool TableImpl::GetDescriptor(TableDescriptor* desc, ErrorCode* err) {
    return false;
}

void TableImpl::DistributeReaders(const std::vector<RowReaderImpl*>& row_reader_list,
                                  bool called_by_user) {
    typedef std::map<std::string, std::vector<RowReaderImpl*> > TsReaderMap;
    TsReaderMap ts_reader_list;

    int64_t sync_min_timeout = -1;
    std::vector<RowReaderImpl*> sync_reader_list;

    if (called_by_user) {
        for (uint32_t i = 0; i < row_reader_list.size(); i++) {
            RowReaderImpl* row_reader = (RowReaderImpl*)row_reader_list[i];
            if (row_reader->IsAsync()) {
                continue;
            }
            sync_reader_list.push_back(row_reader);
            int64_t row_timeout = row_reader->TimeOut() > 0 ? row_reader->TimeOut() : timeout_;
            if (row_timeout > 0 && (sync_min_timeout <= 0 || sync_min_timeout > row_timeout)) {
                sync_min_timeout = row_timeout;
            }
        }
    }

    for (uint32_t i = 0; i < row_reader_list.size(); i++) {
        perf_counter_.reader_cnt.Inc();
        RowReaderImpl* row_reader = (RowReaderImpl*)row_reader_list[i];
        if (called_by_user) {
            row_reader->SetId(next_task_id_.Inc());

            int64_t row_timeout = sync_min_timeout;
            if (row_reader->IsAsync()) {
                row_timeout = row_reader->TimeOut() > 0 ? row_reader->TimeOut() : timeout_;
            }
            SdkTask::TimeoutFunc task = boost::bind(&TableImpl::ReaderTimeout, this, _1);
            task_pool_.PutTask(row_reader, row_timeout, task);
        }

        // flow control
        if (called_by_user
            && cur_reader_pending_counter_.Inc() > max_reader_pending_num_
            && row_reader->IsAsync()) {
            if (FLAGS_tera_sdk_async_blocking_enabled) {
                while (cur_reader_pending_counter_.Get() > max_reader_pending_num_) {
                    usleep(100000);
                }
            } else {
                cur_reader_pending_counter_.Dec();
                row_reader->SetError(ErrorCode::kBusy, "pending too much readers, try it later.");
                ThreadPool::Task task =
                    boost::bind(&TableImpl::BreakRequest, this, row_reader->GetId());
                row_reader->DecRef();
                thread_pool_->AddTask(task);
                continue;
            }
        }

        std::string server_addr;
        if (!GetTabletAddrOrScheduleUpdateMeta(row_reader->RowName(), row_reader,
                                               &server_addr)) {
            continue;
        }

        std::vector<RowReaderImpl*>& ts_row_readers = ts_reader_list[server_addr];
        ts_row_readers.push_back(row_reader);
    }

    TsReaderMap::iterator it = ts_reader_list.begin();
    for (; it != ts_reader_list.end(); ++it) {
        std::vector<RowReaderImpl*>& reader_list = it->second;
        PackReaders(it->first, reader_list);
    }
    // 从现在开始，所有异步的row_reader都不可以再操作了，因为随时会被用户释放

    // 不是用户调用的，立即返回
    if (!called_by_user) {
        return;
    }

    // 等待同步操作返回或超时
    for (uint32_t i = 0; i < sync_reader_list.size(); i++) {
        while (cur_reader_pending_counter_.Get() > max_reader_pending_num_) {
            usleep(100000);
        }

        RowReaderImpl* row_reader = (RowReaderImpl*)sync_reader_list[i];
        row_reader->Wait();
    }
}

void TableImpl::PackReaders(const std::string& server_addr,
                            std::vector<RowReaderImpl*>& reader_list) {
    MutexLock lock(&reader_batch_mutex_);
    TaskBatch* reader_buffer = NULL;
    std::map<std::string, TaskBatch>::iterator it = reader_batch_map_.find(server_addr);
    if (it != reader_batch_map_.end()) {
        reader_buffer = &it->second;
    } else {
        reader_buffer = &reader_batch_map_[server_addr];
        reader_buffer->sequence_num = reader_batch_seq_++;
        reader_buffer->row_id_list = new std::vector<int64_t>;
        ThreadPool::Task task = boost::bind(&TableImpl::ReaderBatchTimeout, this,
                                            server_addr, reader_buffer->sequence_num);
        uint64_t timer_id = thread_pool_->DelayTask(read_commit_timeout_, task);
        reader_buffer->timer_id = timer_id;
    }

    for (size_t i = 0; i < reader_list.size(); ++i) {
        RowReaderImpl* reader = reader_list[i];
        reader_buffer->row_id_list->push_back(reader->GetId());
        reader->DecRef();
    }

    if (reader_buffer->row_id_list->size() >= commit_size_) {
        std::vector<int64_t>* reader_id_list = reader_buffer->row_id_list;
        uint64_t timer_id = reader_buffer->timer_id;
        const bool non_block_cancel = true;
        bool is_running = false;
        if (!thread_pool_->CancelTask(timer_id, non_block_cancel, &is_running)) {
            CHECK(is_running); // this delay task must be waiting for reader_batch_map_
        }
        reader_batch_map_.erase(server_addr);
        reader_batch_mutex_.Unlock();
        CommitReadersById(server_addr, *reader_id_list);
        delete reader_id_list;
        reader_buffer = NULL;
        reader_batch_mutex_.Lock();
    }
}

void TableImpl::ReaderBatchTimeout(std::string server_addr, uint64_t batch_seq) {
    std::vector<int64_t>* reader_id_list = NULL;
    {
        MutexLock lock(&reader_batch_mutex_);
        std::map<std::string, TaskBatch>::iterator it =
            reader_batch_map_.find(server_addr);
        if (it == reader_batch_map_.end()) {
            return;
        }
        TaskBatch* reader_buffer = &it->second;
        if (reader_buffer->sequence_num != batch_seq) {
            return;
        }
        reader_id_list = reader_buffer->row_id_list;
        reader_batch_map_.erase(it);
    }
    CommitReadersById(server_addr, *reader_id_list);
    delete reader_id_list;
}

void TableImpl::CommitReadersById(const std::string server_addr,
                                  std::vector<int64_t>& reader_id_list) {
    std::vector<RowReaderImpl*> reader_list;
    for (size_t i = 0; i < reader_id_list.size(); ++i) {
        int64_t reader_id = reader_id_list[i];
        SdkTask* task = task_pool_.GetTask(reader_id);
        if (task == NULL) {
            VLOG(10) << "reader " << reader_id << " timeout when commit read";;
            continue;
        }
        CHECK_EQ(task->Type(), SdkTask::READ);
        RowReaderImpl* reader = (RowReaderImpl*)task;
        reader_list.push_back(reader);
    }
    CommitReaders(server_addr, reader_list);
}

void TableImpl::CommitReaders(const std::string server_addr,
                              std::vector<RowReaderImpl*>& reader_list) {
    std::vector<int64_t>* reader_id_list = new std::vector<int64_t>;
    tabletnode::TabletNodeClient tabletnode_client_async(server_addr);
    ReadTabletRequest* request = new ReadTabletRequest;
    ReadTabletResponse* response = new ReadTabletResponse;
    request->set_sequence_id(last_sequence_id_++);
    request->set_tablet_name(name_);
    request->set_client_timeout_ms(pending_timeout_ms_);
    for (uint32_t i = 0; i < reader_list.size(); ++i) {
        RowReaderImpl* row_reader = reader_list[i];
        RowReaderInfo* row_reader_info = request->add_row_info_list();
        request->set_snapshot_id(row_reader->GetSnapshot());
        row_reader->ToProtoBuf(row_reader_info);
        // row_reader_info->CopyFrom(row_reader->GetRowReaderInfo());
        reader_id_list->push_back(row_reader->GetId());
        row_reader->AddCommitTimes();
        row_reader->DecRef();
    }
    request->set_timestamp(common::timer::get_micros());
    Closure<void, ReadTabletRequest*, ReadTabletResponse*, bool, int>* done =
        NewClosure(this, &TableImpl::ReaderCallBack, reader_id_list);
    tabletnode_client_async.ReadTablet(request, response, done);
}

void TableImpl::ReaderCallBack(std::vector<int64_t>* reader_id_list,
                               ReadTabletRequest* request,
                               ReadTabletResponse* response,
                               bool failed, int error_code) {
    perf_counter_.rpc_r.Add(common::timer::get_micros() - request->timestamp());
    perf_counter_.rpc_r_cnt.Inc();
    if (failed) {
        if (error_code == sofa::pbrpc::RPC_ERROR_SERVER_SHUTDOWN ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNREACHABLE ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNAVAILABLE) {
            response->set_status(kServerError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_CANCELED ||
                   error_code == sofa::pbrpc::RPC_ERROR_SEND_BUFFER_FULL) {
            response->set_status(kClientError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_CONNECTION_CLOSED ||
                   error_code == sofa::pbrpc::RPC_ERROR_RESOLVE_ADDRESS) {
            response->set_status(kConnectError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_TIMEOUT) {
            response->set_status(kRPCTimeout);
        } else {
            response->set_status(kRPCError);
        }
    }

    std::map<uint32_t, std::vector<int64_t>* > retry_times_list;
    std::vector<RowReaderImpl*> not_in_range_list;
    uint32_t row_result_index = 0;
    for (uint32_t i = 0; i < reader_id_list->size(); ++i) {
        int64_t reader_id = (*reader_id_list)[i];

        StatusCode err = response->status();
        if (err == kTabletNodeOk) {
            err = response->detail().status(i);
        }
        if (err == kTabletNodeOk || err == kKeyNotExist || err == kSnapshotNotExist) {
            perf_counter_.reader_ok_cnt.Inc();
            SdkTask* task = task_pool_.PopTask(reader_id);
            if (task == NULL) {
                VLOG(10) << "reader " << reader_id << " success but timeout";
                if (err == kTabletNodeOk) {
                    // result is timeout, discard it
                    row_result_index++;
                }
                continue;
            }
            CHECK_EQ(task->Type(), SdkTask::READ);
            CHECK_EQ(task->GetRef(), 1);

            RowReaderImpl* row_reader = (RowReaderImpl*)task;
            if (err == kTabletNodeOk) {
                row_reader->SetResult(response->detail().row_result(row_result_index++));
                row_reader->SetError(ErrorCode::kOK);
            } else if (err == kKeyNotExist) {
                row_reader->SetError(ErrorCode::kNotFound, "not found");
            } else { // err == kSnapshotNotExist
                row_reader->SetError(ErrorCode::kNotFound, "snapshot not found");
            }
            int64_t perf_time = common::timer::get_micros();
            row_reader->RunCallback();
            perf_counter_.user_callback.Add(common::timer::get_micros() - perf_time);
            perf_counter_.user_callback_cnt.Inc();
            // only for flow control
            cur_reader_pending_counter_.Dec();
            continue;
        }
        perf_counter_.reader_fail_cnt.Inc();

        VLOG(10) << "fail to read table: " << name_
            << " errcode: " << StatusCodeToString(err);

        SdkTask* task = task_pool_.GetTask(reader_id);
        if (task == NULL) {
            VLOG(10) << "reader " << reader_id << " fail but timeout";
            continue;
        }
        CHECK_EQ(task->Type(), SdkTask::READ);
        RowReaderImpl* row_reader = (RowReaderImpl*)task;
        row_reader->SetInternalError(err);

        if (err == kKeyNotInRange) {
            perf_counter_.reader_range_cnt.Inc();
            row_reader->IncRetryTimes();
            not_in_range_list.push_back(row_reader);
        } else {
            row_reader->IncRetryTimes();
            std::vector<int64_t>* retry_reader_id_list = NULL;
            std::map<uint32_t, std::vector<int64_t>* >::iterator it =
                retry_times_list.find(row_reader->RetryTimes());
            if (it != retry_times_list.end()) {
                retry_reader_id_list = it->second;
            } else {
                retry_reader_id_list = new std::vector<int64_t>;
                retry_times_list[row_reader->RetryTimes()] = retry_reader_id_list;
            }
            retry_reader_id_list->push_back(row_reader->GetId());
            row_reader->DecRef();
        }
    }

    if (not_in_range_list.size() > 0) {
        DistributeReaders(not_in_range_list, false);
    }
    std::map<uint32_t, std::vector<int64_t>* >::iterator it;
    for (it = retry_times_list.begin(); it != retry_times_list.end(); ++it) {
        int64_t retry_interval =
            static_cast<int64_t>(pow(FLAGS_tera_sdk_delay_send_internal, it->first) * 1000);
        ThreadPool::Task retry_task =
            boost::bind(&TableImpl::DistributeReadersById, this, it->second);
        thread_pool_->DelayTask(retry_interval, retry_task);
    }

    delete request;
    delete response;
    delete reader_id_list;
}

void TableImpl::DistributeReadersById(std::vector<int64_t>* reader_id_list) {
    std::vector<RowReaderImpl*> reader_list;
    for (size_t i = 0; i < reader_id_list->size(); ++i) {
        int64_t reader_id = (*reader_id_list)[i];
        SdkTask* task = task_pool_.GetTask(reader_id);
        if (task == NULL) {
            VLOG(10) << "reader " << reader_id << " timeout when retry read";
            continue;
        }
        CHECK_EQ(task->Type(), SdkTask::READ);
        reader_list.push_back((RowReaderImpl*)task);
    }
    DistributeReaders(reader_list, false);
    delete reader_id_list;
}

void TableImpl::ReaderTimeout(SdkTask* task) {
    perf_counter_.reader_timeout_cnt.Inc();
    CHECK_NOTNULL(task);
    CHECK_EQ(task->Type(), SdkTask::READ);

    RowReaderImpl* row_reader = (RowReaderImpl*)task;
    row_reader->ExcludeOtherRef();

    StatusCode err = row_reader->GetInternalError();
    if (err == kKeyNotInRange || err == kConnectError) {
        ScheduleUpdateMeta(row_reader->RowName(),
                           row_reader->GetMetaTimeStamp());
    }
    if (row_reader->RetryTimes() == 0) {
        perf_counter_.reader_queue_timeout_cnt.Inc();
        std::string err_reason = StringFormat("commit %lld times, retry 0 times, in %u ms.",
                                              row_reader->GetCommitTimes(), timeout_);
        row_reader->SetError(ErrorCode::kTimeout, err_reason);
    } else {
        std::string err_reason = StringFormat("commit %lld times, retry %u times, in %u ms. last error: %s",
                                              row_reader->GetCommitTimes(),  row_reader->RetryTimes(),
                                              timeout_, StatusCodeToString(err).c_str());
        row_reader->SetError(ErrorCode::kSystem, err_reason);
    }
    int64_t perf_time = common::timer::get_micros();
    row_reader->RunCallback();
    perf_counter_.user_callback.Add(common::timer::get_micros() - perf_time);
    perf_counter_.user_callback_cnt.Inc();
    // only for flow control
    cur_reader_pending_counter_.Dec();
}

bool TableImpl::GetTabletMetaForKey(const std::string& key, TabletMeta* meta) {
    MutexLock lock(&meta_mutex_);
    TabletMetaNode* node = GetTabletMetaNodeForKey(key);
    if (node == NULL) {
        VLOG(10) << "no meta for key: " << key;
        return false;
    }
    meta->CopyFrom(node->meta);
    return true;
}

void TableImpl::BreakScan(ScanTask* scan_task) {
    ResultStreamImpl* stream = scan_task->stream;
    stream->OnFinish(scan_task->request,
            scan_task->response);
    stream->ReleaseRpcHandle(scan_task->request,
            scan_task->response);
    delete scan_task;
}

bool TableImpl::GetTabletAddrOrScheduleUpdateMeta(const std::string& row,
                                                  SdkTask* task,
                                                  std::string* server_addr) {
    CHECK_NOTNULL(task);
    MutexLock lock(&meta_mutex_);
    TabletMetaNode* node = GetTabletMetaNodeForKey(row);
    if (node == NULL) {
        VLOG(10) << "no meta for key: " << row;
        pending_task_id_list_[row].push_back(task->GetId());
        task->DecRef();
        TabletMetaNode& new_node = tablet_meta_list_[row];
        new_node.meta.mutable_key_range()->set_key_start(row);
        new_node.meta.mutable_key_range()->set_key_end(row + '\0');
        new_node.status = WAIT_UPDATE;
        UpdateMetaAsync();
        return false;
    }
    if (node->status != NORMAL) {
        VLOG(10) << "abnormal meta for key: " << row;
        pending_task_id_list_[row].push_back(task->GetId());
        task->DecRef();
        return false;
    }
    if ((task->GetInternalError() == kKeyNotInRange || task->GetInternalError() == kConnectError)
            && task->GetMetaTimeStamp() >= node->update_time) {
        pending_task_id_list_[row].push_back(task->GetId());
        task->DecRef();
        int64_t update_interval = node->update_time
            + FLAGS_tera_sdk_update_meta_internal - get_micros() / 1000;
        if (update_interval <= 0) {
            VLOG(10) << "update meta now for key: " << row;
            node->status = WAIT_UPDATE;
            UpdateMetaAsync();
        } else {
            VLOG(10) << "update meta in " << update_interval << " (ms) for key:" << row;
            node->status = DELAY_UPDATE;
            ThreadPool::Task delay_task =
                boost::bind(&TableImpl::DelayUpdateMeta, this,
                            node->meta.key_range().key_start(),
                            node->meta.key_range().key_end());
            thread_pool_->DelayTask(update_interval, delay_task);
        }
        return false;
    }
    CHECK_EQ(node->status, NORMAL);
    task->SetMetaTimeStamp(node->update_time);
    *server_addr = node->meta.server_addr();
    return true;
}

TableImpl::TabletMetaNode* TableImpl::GetTabletMetaNodeForKey(const std::string& key) {
    meta_mutex_.AssertHeld();
    if (tablet_meta_list_.size() == 0) {
        VLOG(10) << "the meta list is empty";
        return NULL;
    }
    std::map<std::string, TabletMetaNode>::iterator it =
        tablet_meta_list_.upper_bound(key);
    if (it == tablet_meta_list_.begin()) {
        return NULL;
    } else {
        --it;
    }
    const std::string& end_key = it->second.meta.key_range().key_end();
    if (end_key != "" && end_key <= key) {
        return NULL;
    }
    return &(it->second);
}

void TableImpl::DelayUpdateMeta(std::string start_key, std::string end_key) {
    MutexLock lock(&meta_mutex_);
    std::map<std::string, TabletMetaNode>::iterator it =
            tablet_meta_list_.lower_bound(start_key);
    for (; it != tablet_meta_list_.end(); ++it) {
        TabletMetaNode& node = it->second;
        if (node.meta.key_range().key_end() > end_key) {
            break;
        }
        if (node.status != DELAY_UPDATE) {
            continue;
        }
        node.status = WAIT_UPDATE;
    }
    UpdateMetaAsync();
}

void TableImpl::UpdateMetaAsync() {
    meta_mutex_.AssertHeld();
    if (meta_updating_count_ >= static_cast<uint32_t>(FLAGS_tera_sdk_update_meta_concurrency)) {
        return;
    }
    bool need_update = false;
    std::string update_start_key;
    std::string update_end_key;
    std::string update_expand_end_key; // update more tablet than need
    std::map<std::string, TabletMetaNode>::iterator it = tablet_meta_list_.begin();
    for (; it != tablet_meta_list_.end(); ++it) {
        TabletMetaNode& node = it->second;
        if (node.status != WAIT_UPDATE && need_update) {
            update_expand_end_key = node.meta.key_range().key_start();
            break;
        } else if (node.status != WAIT_UPDATE) {
            continue;
        } else if (!need_update) {
            need_update = true;
            update_start_key = node.meta.key_range().key_start();
            update_end_key = node.meta.key_range().key_end();
        } else if (node.meta.key_range().key_start() == update_end_key) {
            update_end_key = node.meta.key_range().key_end();
        } else {
            CHECK_GT(node.meta.key_range().key_start(), update_end_key);
            update_expand_end_key = node.meta.key_range().key_start();
            break;
        }
        node.status = UPDATING;
    }
    if (!need_update) {
        return;
    }
    meta_updating_count_++;
    ScanMetaTableAsync(update_start_key, update_end_key, update_expand_end_key, false);
}

void TableImpl::ScanMetaTable(const std::string& key_start,
                              const std::string& key_end) {
    MutexLock lock(&meta_mutex_);
    meta_updating_count_++;
    ScanMetaTableAsync(key_start, key_end, key_end, false);
    while (meta_updating_count_ > 0) {
        meta_cond_.Wait();
    }
}

void TableImpl::ScanMetaTableAsyncInLock(std::string key_start, std::string key_end,
                                         std::string expand_key_end, bool zk_access) {
    MutexLock lock(&meta_mutex_);
    ScanMetaTableAsync(key_start, key_end, expand_key_end, zk_access);
}

void TableImpl::ScanMetaTableAsync(const std::string& key_start, const std::string& key_end,
                                   const std::string& expand_key_end, bool zk_access) {
    meta_mutex_.AssertHeld();
    CHECK(expand_key_end == "" || expand_key_end >= key_end);

    std::string meta_addr = cluster_->RootTableAddr(zk_access);
    if (meta_addr.empty() && !zk_access) {
        meta_addr = cluster_->RootTableAddr(true);
    }

    if (meta_addr.empty()) {
        VLOG(6) << "root is empty";

        ThreadPool::Task retry_task =
            boost::bind(&TableImpl::ScanMetaTableAsyncInLock, this, key_start, key_end,
                        expand_key_end, true);
        thread_pool_->DelayTask(FLAGS_tera_sdk_update_meta_internal, retry_task);
        return;
    }

    VLOG(6) << "root: " << meta_addr;
    tabletnode::TabletNodeClient tabletnode_client_async(meta_addr);
    ScanTabletRequest* request = new ScanTabletRequest;
    ScanTabletResponse* response = new ScanTabletResponse;
    request->set_sequence_id(last_sequence_id_++);
    request->set_table_name(FLAGS_tera_master_meta_table_name);
    MetaTableScanRange(name_, key_start, expand_key_end,
                       request->mutable_start(),
                       request->mutable_end());
    request->set_buffer_limit(FLAGS_tera_sdk_update_meta_buffer_limit);
    request->set_round_down(true);

    Closure<void, ScanTabletRequest*, ScanTabletResponse*, bool, int>* done =
        NewClosure(this, &TableImpl::ScanMetaTableCallBack, key_start, key_end, expand_key_end, ::common::timer::get_micros());
    tabletnode_client_async.ScanTablet(request, response, done);
}

void TableImpl::ScanMetaTableCallBack(std::string key_start,
                                      std::string key_end,
                                      std::string expand_key_end,
                                      int64_t start_time,
                                      ScanTabletRequest* request,
                                      ScanTabletResponse* response,
                                      bool failed, int error_code) {
    perf_counter_.get_meta.Add(::common::timer::get_micros() - start_time);
    perf_counter_.get_meta_cnt.Inc();
    if (failed) {
        if (error_code == sofa::pbrpc::RPC_ERROR_SERVER_SHUTDOWN ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNREACHABLE ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNAVAILABLE) {
            response->set_status(kServerError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_CANCELED ||
                   error_code == sofa::pbrpc::RPC_ERROR_SEND_BUFFER_FULL) {
            response->set_status(kClientError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_CONNECTION_CLOSED ||
                   error_code == sofa::pbrpc::RPC_ERROR_RESOLVE_ADDRESS) {
            response->set_status(kConnectError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_TIMEOUT) {
            response->set_status(kRPCTimeout);
        } else {
            response->set_status(kRPCError);
        }
    }

    StatusCode err = response->status();
    if (err != kTabletNodeOk) {
        VLOG(10) << "fail to scan meta table [" << request->start()
            << ", " << request->end() << "]: " << StatusCodeToString(err);
        {
            MutexLock lock(&meta_mutex_);
            GiveupUpdateTabletMeta(key_start, key_end);
        }
        ThreadPool::Task retry_task =
            boost::bind(&TableImpl::ScanMetaTableAsyncInLock, this, key_start, key_end,
                        expand_key_end, true);
        thread_pool_->DelayTask(FLAGS_tera_sdk_update_meta_internal, retry_task);
        delete request;
        delete response;
        return;
    }

    std::string return_start, return_end;
    const RowResult& scan_result = response->results();
    for (int32_t i = 0; i < scan_result.key_values_size(); i++) {
        const KeyValuePair& kv = scan_result.key_values(i);

        TabletMeta meta;
        ParseMetaTableKeyValue(kv.key(), kv.value(), &meta);

        if (i == 0) {
            return_start = meta.key_range().key_start();
        }
        if (i == scan_result.key_values_size() - 1) {
            return_end = meta.key_range().key_end();
        }

        MutexLock lock(&meta_mutex_);
        UpdateTabletMetaList(meta);
    }
    VLOG(10) << "scan meta table [" << request->start()
        << ", " << request->end() << "] success: return "
        << scan_result.key_values_size() << " records, is_complete: " << response->complete();
    bool scan_meta_error = false;
    if (scan_result.key_values_size() == 0
        || return_start > key_start
        || (response->complete() && !return_end.empty() && (key_end.empty() || return_end < key_end))) {
        LOG(ERROR) << "scan meta table [" << key_start << ", " << key_end
            << "] return [" << return_start << ", " << return_end << "]";
        // TODO(lk): process omitted tablets
        scan_meta_error = true;
    }

    MutexLock lock(&meta_mutex_);
    if (scan_meta_error) {
        ScanMetaTableAsync(key_start, key_end, expand_key_end, false);
    } else if (!return_end.empty() && (key_end.empty() || return_end < key_end)) {
        CHECK(!response->complete());
        ScanMetaTableAsync(return_end, key_end, expand_key_end, false);
    } else {
        meta_updating_count_--;
        meta_cond_.Signal();
        UpdateMetaAsync();
    }
    delete request;
    delete response;
}

void TableImpl::GiveupUpdateTabletMeta(const std::string& key_start,
                                       const std::string& key_end) {
    std::map<std::string, std::list<int64_t> >::iterator ilist =
            pending_task_id_list_.lower_bound(key_start);
    while (ilist != pending_task_id_list_.end()) {
        if (!key_end.empty() && ilist->first >= key_end) {
            break;
        }
        std::list<int64_t>& task_id_list = ilist->second;
        for (std::list<int64_t>::iterator itask = task_id_list.begin();
                itask != task_id_list.end();) {
            int64_t task_id = *itask;
            SdkTask* task = task_pool_.GetTask(task_id);
            if (task == NULL) {
                VLOG(10) << "task " << task_id << " timeout when update meta fail";
                itask = task_id_list.erase(itask);
            } else {
                task->DecRef();
            }
            ++itask;
        }
        if (task_id_list.empty()) {
            pending_task_id_list_.erase(ilist++);
        } else {
            ++ilist;
        }
    }
}

void TableImpl::UpdateTabletMetaList(const TabletMeta& new_meta) {
    meta_mutex_.AssertHeld();
    const std::string& new_start = new_meta.key_range().key_start();
    const std::string& new_end = new_meta.key_range().key_end();
    std::map<std::string, TabletMetaNode>::iterator it =
        tablet_meta_list_.upper_bound(new_start);
    if (tablet_meta_list_.size() > 0 && it != tablet_meta_list_.begin()) {
        --it;
    }
    while (it != tablet_meta_list_.end()) {
        TabletMetaNode& old_node = it->second;
        std::map<std::string, TabletMetaNode>::iterator tmp = it;
        ++it;

        const std::string& old_start = old_node.meta.key_range().key_start();
        const std::string& old_end = old_node.meta.key_range().key_end();
        // update overlaped old nodes
        if (old_start < new_start) {
            if (!old_end.empty() && old_end <= new_start) {
                //*************************************************
                //* |---old---|                                   *
                //*             |------new------|                 *
                //*************************************************
            } else if (new_end.empty() || (!old_end.empty() && old_end <= new_end)) {
                //*************************************************
                //*         |---old---|                           *
                //*             |------new------|                 *
                //*************************************************
                VLOG(10) << "meta [" << old_start << ", " << old_end << "] "
                    << "shrink to [" << old_start << ", " << new_start << "]";
                old_node.meta.mutable_key_range()->set_key_end(new_start);
            } else {
                //*************************************************
                //*         |----------old-----------|            *
                //*             |------new------|                 *
                //*************************************************
                VLOG(10) << "meta [" << old_start << ", " << old_end << "] "
                    << "split to [" << old_start << ", " << new_start << "] "
                    << "and [" << new_end << ", " << old_end << "]";
                TabletMetaNode& copy_node = tablet_meta_list_[new_end];
                copy_node = old_node;
                copy_node.meta.mutable_key_range()->set_key_start(new_end);
                old_node.meta.mutable_key_range()->set_key_end(new_start);
            }
        } else if (new_end.empty() || old_start < new_end) {
            if (new_end.empty() || (!old_end.empty() && old_end <= new_end)) {
                //*************************************************
                //*                |---old---|                    *
                //*             |------new------|                 *
                //*************************************************
                VLOG(10) << "meta [" << old_start << ", " << old_end << "] "
                    << "is covered by [" << new_start << ", " << new_end << "]";
                tablet_meta_list_.erase(tmp);
            } else {
                //*************************************************
                //*                  |-----old------|             *
                //*             |------new------|                 *
                //*************************************************
                VLOG(10) << "meta [" << old_start << ", " << old_end << "] "
                    << "shrink to [" << new_end << ", " << old_end << "]";
                TabletMetaNode& copy_node = tablet_meta_list_[new_end];
                copy_node = old_node;
                copy_node.meta.mutable_key_range()->set_key_start(new_end);
                tablet_meta_list_.erase(tmp);
            }
        } else { // !new_end.empty() && old_start >= new_end
            //*****************************************************
            //*                                   |---old---|     *
            //*                 |------new------|                 *
            //*****************************************************
            break;
        }
    }

    TabletMetaNode& new_node = tablet_meta_list_[new_start];
    new_node.meta.CopyFrom(new_meta);
    new_node.status = NORMAL;
    new_node.update_time = get_micros() / 1000;
    VLOG(10) << "add new meta [" << new_start << ", " << new_end << "]: "
        << new_meta.server_addr();
    WakeUpPendingRequest(new_node);
}

void TableImpl::WakeUpPendingRequest(const TabletMetaNode& node) {
    meta_mutex_.AssertHeld();
    const std::string& start_key = node.meta.key_range().key_start();
    const std::string& end_key = node.meta.key_range().key_end();
    const std::string& server_addr = node.meta.server_addr();
    int64_t meta_timestamp = node.update_time;

    std::vector<RowMutationImpl*> mutation_list;
    std::vector<RowReaderImpl*> reader_list;

    std::map<std::string, std::list<int64_t> >::iterator it =
        pending_task_id_list_.lower_bound(start_key);
    while (it != pending_task_id_list_.end()) {
        if (!end_key.empty() && it->first >= end_key) {
            break;
        }
        std::list<int64_t>& task_id_list = it->second;
        for (std::list<int64_t>::iterator itask = task_id_list.begin();
                itask != task_id_list.end(); ++itask) {
            int64_t task_id = *itask;
            SdkTask* task = task_pool_.GetTask(task_id);
            if (task == NULL) {
                VLOG(10) << "task " << task_id << " timeout when update meta success";
                continue;
            }
            task->SetMetaTimeStamp(meta_timestamp);

            switch (task->Type()) {
            case SdkTask::READ: {
                RowReaderImpl* reader = (RowReaderImpl*)task;
                reader_list.push_back(reader);
            } break;
            case SdkTask::MUTATION: {
                RowMutationImpl* mutation = (RowMutationImpl*)task;
                mutation_list.push_back(mutation);
            } break;
            case SdkTask::SCAN: {
                ScanTask* scan_task = (ScanTask*)task;
                CommitScan(scan_task, server_addr);
            } break;
            default:
                CHECK(false);
                break;
            }
        }
        std::map<std::string, std::list<int64_t> >::iterator tmp = it;
        ++it;
        pending_task_id_list_.erase(tmp);
    }

    if (mutation_list.size() > 0) {
        // TODO: flush ?
        PackMutations(server_addr, mutation_list, false);
    }
    if (reader_list.size() > 0) {
        PackReaders(server_addr, reader_list);
    }
}

void TableImpl::ScheduleUpdateMeta(const std::string& row,
                                   int64_t meta_timestamp) {
    MutexLock lock(&meta_mutex_);
    TabletMetaNode* node = GetTabletMetaNodeForKey(row);
    if (node == NULL) {
        TabletMetaNode& new_node = tablet_meta_list_[row];
        new_node.meta.mutable_key_range()->set_key_start(row);
        new_node.meta.mutable_key_range()->set_key_end(row + '\0');
        new_node.status = WAIT_UPDATE;
        UpdateMetaAsync();
        return;
    }
    if (node->status == NORMAL && meta_timestamp >= node->update_time) {
        int64_t update_interval = node->update_time
            + FLAGS_tera_sdk_update_meta_internal - get_micros() / 1000;
        if (update_interval <= 0) {
            node->status = WAIT_UPDATE;
            UpdateMetaAsync();
        } else {
            node->status = DELAY_UPDATE;
            ThreadPool::Task delay_task =
                boost::bind(&TableImpl::DelayUpdateMeta, this,
                            node->meta.key_range().key_start(),
                            node->meta.key_range().key_end());
            thread_pool_->DelayTask(update_interval, delay_task);
        }
    }
}

bool TableImpl::UpdateTableMeta(ErrorCode* err) {
    MutexLock lock(&table_meta_mutex_);
    table_meta_updating_ = true;

    table_meta_mutex_.Unlock();
    ReadTableMetaAsync(err, 0, false);
    table_meta_mutex_.Lock();

    while (table_meta_updating_) {
        table_meta_cond_.Wait();
    }
    if (err->GetType() != ErrorCode::kOK) {
        return false;
    }
    return true;
}

void TableImpl::ReadTableMetaAsync(ErrorCode* ret_err, int32_t retry_times,
                                   bool zk_access) {
    std::string meta_server = cluster_->RootTableAddr(zk_access);
    if (meta_server.empty() && !zk_access) {
        meta_server = cluster_->RootTableAddr(true);
    }
    if (meta_server.empty()) {
        VLOG(10) << "root is empty";

        MutexLock lock(&table_meta_mutex_);
        CHECK(table_meta_updating_);
        if (retry_times >= FLAGS_tera_sdk_retry_times) {
            ret_err->SetFailed(ErrorCode::kSystem);
            table_meta_updating_ = false;
            table_meta_cond_.Signal();
        } else {
            int64_t retry_interval =
                static_cast<int64_t>(pow(FLAGS_tera_sdk_delay_send_internal, retry_times) * 1000);
            ThreadPool::Task retry_task =
                boost::bind(&TableImpl::ReadTableMetaAsync, this, ret_err, retry_times + 1, true);
            thread_pool_->DelayTask(retry_interval, retry_task);
        }
        return;
    }

    tabletnode::TabletNodeClient tabletnode_client_async(meta_server);
    ReadTabletRequest* request = new ReadTabletRequest;
    ReadTabletResponse* response = new ReadTabletResponse;
    request->set_sequence_id(last_sequence_id_++);
    request->set_tablet_name(FLAGS_tera_master_meta_table_name);
    RowReaderInfo* row_info = request->add_row_info_list();
    MakeMetaTableKey(name_, row_info->mutable_key());

    Closure<void, ReadTabletRequest*, ReadTabletResponse*, bool, int>* done =
        NewClosure(this, &TableImpl::ReadTableMetaCallBack, ret_err, retry_times);
    tabletnode_client_async.ReadTablet(request, response, done);
}

void TableImpl::ReadTableMetaCallBack(ErrorCode* ret_err,
                                      int32_t retry_times,
                                      ReadTabletRequest* request,
                                      ReadTabletResponse* response,
                                      bool failed, int error_code) {
    if (failed) {
        if (error_code == sofa::pbrpc::RPC_ERROR_SERVER_SHUTDOWN ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNREACHABLE ||
            error_code == sofa::pbrpc::RPC_ERROR_SERVER_UNAVAILABLE) {
            response->set_status(kServerError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_CANCELED ||
                   error_code == sofa::pbrpc::RPC_ERROR_SEND_BUFFER_FULL) {
            response->set_status(kClientError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_CONNECTION_CLOSED ||
                   error_code == sofa::pbrpc::RPC_ERROR_RESOLVE_ADDRESS) {
            response->set_status(kConnectError);
        } else if (error_code == sofa::pbrpc::RPC_ERROR_REQUEST_TIMEOUT) {
            response->set_status(kRPCTimeout);
        } else {
            response->set_status(kRPCError);
        }
    }

    StatusCode err = response->status();
    if (err == kTabletNodeOk && response->detail().status_size() < 1) {
        err = kKeyNotExist;
        LOG(ERROR) << "read table meta: status size is 0";
    }
    if (err == kTabletNodeOk) {
        err = response->detail().status(0);
    }
    if (err == kTabletNodeOk && response->detail().row_result_size() < 1) {
        err = kKeyNotExist;
        LOG(ERROR) << "read table meta: row result size is 0";
    }
    if (err == kTabletNodeOk && response->detail().row_result(0).key_values_size() < 1) {
        err = kKeyNotExist;
        LOG(ERROR) << "read table meta: row result kv size is 0";
    }

    if (err != kTabletNodeOk && err != kKeyNotExist && err != kSnapshotNotExist) {
        VLOG(10) << "fail to read meta table, retry: " << retry_times
            << ", errcode: " << StatusCodeToString(err);
    }

    MutexLock lock(&table_meta_mutex_);
    CHECK(table_meta_updating_);

    if (err == kTabletNodeOk) {
        TableMeta table_meta;
        const KeyValuePair& kv = response->detail().row_result(0).key_values(0);
        ParseMetaTableKeyValue(kv.key(), kv.value(), &table_meta);
        table_schema_.CopyFrom(table_meta.schema());
        create_time_ = table_meta.create_time();
        ret_err->SetFailed(ErrorCode::kOK);
        table_meta_updating_ = false;
        table_meta_cond_.Signal();
    } else if (err == kKeyNotExist || err == kSnapshotNotExist) {
        ret_err->SetFailed(ErrorCode::kNotFound);
        table_meta_updating_ = false;
        table_meta_cond_.Signal();
    } else if (retry_times >= FLAGS_tera_sdk_retry_times) {
        ret_err->SetFailed(ErrorCode::kSystem);
        table_meta_updating_ = false;
        table_meta_cond_.Signal();
    } else {
        int64_t retry_interval =
            static_cast<int64_t>(pow(FLAGS_tera_sdk_delay_send_internal, retry_times) * 1000);
        ThreadPool::Task retry_task =
            boost::bind(&TableImpl::ReadTableMetaAsync, this, ret_err, retry_times + 1, true);
        thread_pool_->DelayTask(retry_interval, retry_task);
    }

    delete request;
    delete response;
}

bool TableImpl::RestoreCookie() {
    const std::string& cookie_dir = FLAGS_tera_sdk_cookie_path;
    if (!IsExist(cookie_dir)) {
        if (!CreateDirWithRetry(cookie_dir)) {
            LOG(INFO) << "[SDK COOKIE] fail to create cookie dir: " << cookie_dir;
            return false;
        } else {
            return true;
        }
    }
    SdkCookie cookie;
    std::string cookie_file = GetCookieFilePathName();
    if (!::tera::sdk::RestoreCookie(cookie_file, true, &cookie)) {
        return true;
    }
    if (cookie.table_name() != name_) {
        LOG(INFO) << "[SDK COOKIE] cookie name error: " << cookie.table_name()
            << ", should be: " << name_;
        return true;
    }

    MutexLock lock(&meta_mutex_);
    for (int i = 0; i < cookie.tablets_size(); ++i) {
        const TabletMeta& meta = cookie.tablets(i).meta();
        const std::string& start_key = meta.key_range().key_start();
        LOG(INFO) << "[SDK COOKIE] restore:" << meta.path()
            << " range [" << DebugString(start_key)
            << " : " << DebugString(meta.key_range().key_end()) << "]";
        TabletMetaNode& node = tablet_meta_list_[start_key];
        node.meta = meta;
        node.update_time = cookie.tablets(i).update_time();
        node.status = NORMAL;
    }
    LOG(INFO) << "[SDK COOKIE] restore finished, tablet num: " << cookie.tablets_size();
    return true;
}

std::string TableImpl::GetCookieFilePathName(void) {
    return FLAGS_tera_sdk_cookie_path + "/"
        + GetCookieFileName(name_, cluster_->ClusterId(), create_time_);
}

std::string TableImpl::GetCookieLockFilePathName(void) {
    return GetCookieFilePathName() + ".LOCK";
}

void TableImpl::DoDumpCookie() {
    std::string cookie_file = GetCookieFilePathName();
    std::string cookie_lock_file = GetCookieLockFilePathName();
    SdkCookie cookie;
    cookie.set_table_name(name_);
    {
        MutexLock lock(&meta_mutex_);
        std::map<std::string, TabletMetaNode>::iterator it = tablet_meta_list_.begin();
        for (; it != tablet_meta_list_.end(); ++it) {
            const TabletMetaNode& node = it->second;
            if (!node.meta.has_table_name() || !node.meta.has_path()) {
                continue;
            }
            SdkTabletCookie* tablet = cookie.add_tablets();
            tablet->mutable_meta()->CopyFrom(node.meta);
            tablet->set_update_time(node.update_time);
            tablet->set_status(node.status);
        }
    }
    if (!IsExist(FLAGS_tera_sdk_cookie_path) && !CreateDirWithRetry(FLAGS_tera_sdk_cookie_path)) {
        LOG(ERROR) << "[SDK COOKIE] fail to create cookie dir: " << FLAGS_tera_sdk_cookie_path;
        return;
    }
    ::tera::sdk::DumpCookie(cookie_file, cookie_lock_file, cookie);
}

void TableImpl::DumpCookie() {
    DoDumpCookie();
    ThreadPool::Task task = boost::bind(&TableImpl::DumpCookie, this);
    AddDelayTask(FLAGS_tera_sdk_cookie_update_interval * 1000LL, task);
}

void TableImpl::EnableCookieUpdateTimer() {
    ThreadPool::Task task = boost::bind(&TableImpl::DumpCookie, this);
    AddDelayTask(FLAGS_tera_sdk_cookie_update_interval * 1000LL, task);
}

std::string TableImpl::GetCookieFileName(const std::string& tablename,
                                         const std::string& cluster_id,
                                         int64_t create_time) {
    uint32_t hash = 0;
    if (GetHashNumber(cluster_id, hash, &hash) != 0) {
        LOG(FATAL) << "invalid arguments";
    }
    char hash_str[9] = {'\0'};
    sprintf(hash_str, "%08x", hash);
    std::stringstream fname;
    fname << tablename << "-" << create_time << "-" << hash_str;
    return fname.str();
}

static int64_t CalcAverage(Counter& sum, Counter& cnt, int64_t interval) {
    if (cnt.Get() == 0 || interval == 0) {
        return 0;
    } else {
        return sum.Clear() * 1000 / cnt.Clear() / interval / 1000;
    }
}

void TableImpl::DumpPerfCounterLogDelay() {
    DoDumpPerfCounterLog();
    ThreadPool::Task task =
        boost::bind(&TableImpl::DumpPerfCounterLogDelay, this);
    AddDelayTask(FLAGS_tera_sdk_perf_counter_log_interval * 1000, task);
}

void TableImpl::DoDumpPerfCounterLog() {
    LOG(INFO) << "[table " << name_ << " PerfCounter][pending]"
        << " pending_r: " << cur_reader_pending_counter_.Get()
        << " pending_w: " << cur_commit_pending_counter_.Get();
    perf_counter_.DoDumpPerfCounterLog("[table " + name_ + " PerfCounter]");
}

void TableImpl::PerfCounter::DoDumpPerfCounterLog(const std::string& log_prefix) {
    int64_t ts = common::timer::get_micros();
    int64_t interval = (ts - start_time) / 1000;
    LOG(INFO) << log_prefix << "[delay](ms)"
        << " get meta: " << CalcAverage(get_meta, get_meta_cnt, interval)
        << " callback: " << CalcAverage(user_callback, user_callback_cnt, interval)
        << " rpc_r: " << CalcAverage(rpc_r, rpc_r_cnt, interval)
        << " rpc_w: " << CalcAverage(rpc_w, rpc_w_cnt, interval)
        << " rpc_s: " << CalcAverage(rpc_s, rpc_s_cnt, interval);

    LOG(INFO) << log_prefix << "[mutation]"
        << " all: " << mutate_cnt.Clear()
        << " ok: " << mutate_ok_cnt.Clear()
        << " fail: " << mutate_fail_cnt.Clear()
        << " range: " << mutate_range_cnt.Clear()
        << " timeout: " << mutate_timeout_cnt.Clear()
        << " queue_timeout: " << mutate_queue_timeout_cnt.Clear();

    LOG(INFO) << log_prefix << "[reader]"
        << " all: " << reader_cnt.Clear()
        << " ok: " << reader_ok_cnt.Clear()
        << " fail: " << reader_fail_cnt.Clear()
        << " range: " << reader_range_cnt.Clear()
        << " timeout: " << reader_timeout_cnt.Clear()
        << " queue_timeout: " << reader_queue_timeout_cnt.Clear();
}

void TableImpl::DelayTaskWrapper(ThreadPool::Task task, int64_t task_id) {
    {
        MutexLock lock(&delay_task_id_mutex_);
        if (delay_task_ids_.erase(task_id) == 0) {
            // this task has been canceled
            return;
        }
    }
    task(task_id);
}
int64_t TableImpl::AddDelayTask(int64_t delay_time, ThreadPool::Task task) {
    MutexLock lock(&delay_task_id_mutex_);
    ThreadPool::Task t =
        boost::bind(&TableImpl::DelayTaskWrapper, this, task, _1);
    int64_t t_id = thread_pool_->DelayTask(delay_time, t);
    delay_task_ids_.insert(t_id);
    return t_id;
}
void TableImpl::ClearDelayTask() {
    MutexLock lock(&delay_task_id_mutex_);
    std::set<int64_t>::iterator it = delay_task_ids_.begin();
    for (; it != delay_task_ids_.end(); ++it) {
        thread_pool_->CancelTask(*it);
    }
    delay_task_ids_.clear();
}

void TableImpl::BreakRequest(int64_t task_id) {
    SdkTask* task = task_pool_.PopTask(task_id);
    if (task == NULL) {
        VLOG(10) << "task " << task_id << " timeout when brankrequest";
        return;
    }
    CHECK_EQ(task->GetRef(), 1);
    switch (task->Type()) {
    case SdkTask::MUTATION:
        ((RowMutationImpl*)task)->RunCallback();
        break;
    case SdkTask::READ:
        ((RowReaderImpl*)task)->RunCallback();
        break;
    default:
        CHECK(false);
        break;
    }
}

/// 创建事务
Transaction* TableImpl::StartRowTransaction(const std::string& row_key) {
    return new SingleRowTxn(this, row_key, thread_pool_);
}

/// 提交事务
void TableImpl::CommitRowTransaction(Transaction* transaction) {
    SingleRowTxn* row_txn_impl = (SingleRowTxn*)transaction;
    row_txn_impl->Commit();
}

std::string CounterCoding::EncodeCounter(int64_t counter) {
    char counter_buf[sizeof(int64_t)];
    io::EncodeBigEndian(counter_buf, counter);
    return std::string(counter_buf, sizeof(counter_buf));
}

bool CounterCoding::DecodeCounter(const std::string& buf,
                                  int64_t* counter) {
    assert(counter);
    if (buf.size() != sizeof(int64_t)) {
        *counter = 0;
        return false;
    }
    *counter = io::DecodeBigEndainSign(buf.data());
    return true;
}

} // namespace tera
