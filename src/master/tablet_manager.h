// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TERA_MASTER_TABLET_MANAGER_H_
#define TERA_MASTER_TABLET_MANAGER_H_

#include <limits>
#include <list>
#include <map>
#include <set>
#include <stdint.h>
#include <string>
#include <vector>

#include <boost/shared_ptr.hpp>

#include "common/base/closure.h"
#include "common/mutex.h"
#include "common/thread_pool.h"

#include "proto/master_rpc.pb.h"
#include "proto/table_meta.pb.h"
#include "proto/tabletnode_rpc.pb.h"
#include "utils/counter.h"
#include "utils/fragment.h"

namespace tera {
class UpdateTableResponse;
namespace master {


// kTabletNodeOk = 40;
// kTableNotInit = 41;
// kTableReady = 42;
// kTableOnLoad = 43;
// kTableOnSplit = 44;
// kTableUnLoad = 49;
// kTableOnMerge = 50;
// kTableSplited = 51;
// kTableUnLoading = 52;
// kTableDeleted = 53;
// kTableNotCompact = 54;
// kTableOnCompact = 55;
// kTableCompacted = 56;
// kTableOffLine = 57;
// kTableWaitLoad = 58;
// kTableWaitSplit = 59;
// kTableLoadFail = 60;
// kTableSplitFail = 61;
// kTableUnLoadFail = 62;

class MasterImpl;
class Table;
typedef boost::shared_ptr<Table> TablePtr;

class Tablet {
    friend class TabletManager;
    friend class Table;
    friend std::ostream& operator << (std::ostream& o, const Tablet& tablet);

public:
    Tablet();
    explicit Tablet(const TabletMeta& meta);
    Tablet(const TabletMeta& meta, TablePtr table);
    ~Tablet();

    void ToMeta(TabletMeta* meta);
    const std::string& GetTableName();
    const std::string& GetServerAddr();
    const std::string& GetPath();
    int64_t GetDataSize();
    void GetDataSize(int64_t* size, std::vector<int64_t>* lg_size);
    int64_t GetQps();

    const std::string& GetKeyStart();
    const std::string& GetKeyEnd();
    const KeyRange& GetKeyRange();
    const TableSchema& GetSchema();
    const TabletCounter& GetCounter();
    const TabletCounter& GetAverageCounter();
    TabletStatus GetStatus();
    CompactStatus GetCompactStatus();
    std::string GetServerId();
    std::string GetExpectServerAddr();
    TablePtr GetTable();
    bool IsBusy();
    std::string DebugString();

    void UpdateSize(const TabletMeta& meta);
    void SetCounter(const TabletCounter& counter);
    void SetCompactStatus(CompactStatus compact_status);
    void SetAddr(const std::string& server_addr);
    bool SetStatus(TabletStatus new_status, TabletStatus* old_status = NULL);
    bool SetStatusIf(TabletStatus new_status, TabletStatus if_status,
                     TabletStatus* old_status = NULL);
    bool SetStatusIf(TabletStatus new_status, TabletStatus if_status,
                     TableStatus if_table_status, TabletStatus* old_status = NULL);
    bool SetAddrIf(const std::string& server_addr, TabletStatus if_status,
                   TabletStatus* old_status = NULL);
    bool SetAddrAndStatus(const std::string& server_addr,
                          TabletStatus new_status,
                          TabletStatus* old_status = NULL);
    bool SetAddrAndStatusIf(const std::string& server_addr,
                            TabletStatus new_status, TabletStatus if_status,
                            TabletStatus* old_status = NULL);
    void SetServerId(const std::string& server_id);
    void SetExpectServerAddr(const std::string& server_addr);
    TableStatus GetTableStatus();

    int32_t AddSnapshot(uint64_t snapshot);
    void ListSnapshot(std::vector<uint64_t>* snapshot);
    void DelSnapshot(int32_t id);
    int32_t AddRollback(std::string name, uint64_t snapshot_id, uint64_t rollback_point);
    void ListRollback(std::vector<Rollback>* rollbacks);

    // is belong to a table?
    bool IsBound();

    bool Verify(const std::string& table_name, const std::string& key_start,
                const std::string& key_end, const std::string& path,
                const std::string& server_addr, StatusCode* ret_status = NULL);

    void ToMetaTableKeyValue(std::string* packed_key = NULL,
                             std::string* packed_value = NULL);
    bool GetSchemaIsSyncing();

    int64_t UpdateTime();
    int64_t SetUpdateTime(int64_t timestamp);
    int64_t LoadTime();
    int64_t SetLoadTime(int64_t timestamp);

    void* GetMergeParam();
    void SetMergeParam(void* merge_param);

private:
    Tablet(const Tablet&) {}
    Tablet& operator=(const Tablet&) {return *this;}

    static bool CheckStatusSwitch(TabletStatus old_status,
                                  TabletStatus new_status);

    mutable Mutex mutex_;
    TabletMeta meta_;
    TablePtr table_;
    int64_t update_time_;
    int64_t load_time_;
    std::string server_id_;
    std::string expect_server_addr_;
    std::list<TabletCounter> counter_list_;
    TabletCounter average_counter_;
    struct TabletAccumulateCounter {
        uint64_t low_read_cell;
        uint64_t scan_rows;
        uint64_t scan_kvs;
        uint64_t scan_size;
        uint64_t read_rows;
        uint64_t read_kvs;
        uint64_t read_size;
        uint64_t write_rows;
        uint64_t write_kvs;
        uint64_t write_size;

        TabletAccumulateCounter() {
            memset(this, 0, sizeof(TabletAccumulateCounter));
        }
    } accumu_counter_;
    void* merge_param_;
};

typedef class boost::shared_ptr<Tablet> TabletPtr;
std::ostream& operator << (std::ostream& o, const TabletPtr& tablet);
std::ostream& operator << (std::ostream& o, const TablePtr& table);

class Table {
    friend class Tablet;
    friend class TabletManager;
    friend std::ostream& operator << (std::ostream& o, const Table& tablet);

public:
    Table(const std::string& table_name);
    bool FindTablet(const std::string& key_start, TabletPtr* tablet);
    void FindTablet(const std::string& server_addr,
                   std::vector<TabletPtr>* tablet_meta_list);
    void GetTablet(std::vector<TabletPtr>* tablet_meta_list);
    const std::string& GetTableName();
    TableStatus GetStatus();
    bool SetStatus(TableStatus new_status, TableStatus* old_status = NULL);
    bool CheckStatusSwitch(TableStatus old_status, TableStatus new_status);
    const TableSchema& GetSchema();
    void SetSchema(const TableSchema& schema);
    const TableCounter& GetCounter();
    int32_t AddSnapshot(uint64_t snapshot);
    int32_t DelSnapshot(uint64_t snapshot);
    void ListSnapshot(std::vector<uint64_t>* snapshots);
    int32_t AddRollback(std::string rollback_name);
    void ListRollback(std::vector<std::string>* rollback_names);
    void AddDeleteTabletCount();
    bool NeedDelete();
    void ToMetaTableKeyValue(std::string* packed_key = NULL,
                             std::string* packed_value = NULL);
    void ToMeta(TableMeta* meta);
    uint64_t GetNextTabletNo();
    bool GetTabletsForGc(std::set<uint64_t>* live_tablets,
                         std::set<uint64_t>* dead_tablets);
    void RefreshCounter();
    int64_t GetTabletsCount();
    bool GetSchemaIsSyncing();
    void SetSchemaIsSyncing(bool flag);
    bool GetSchemaSyncLockOrFailed();
    void ResetRangeFragment();
    bool AddToRange(const std::string& start, const std::string& end);
    bool IsCompleteRange() const;
    RangeFragment* GetRangeFragment();
    void UpdateRpcDone();
    void StoreUpdateRpc(UpdateTableResponse* response, google::protobuf::Closure* done);
    bool IsSchemaSyncedAtRange(const std::string& start, const std::string& end);
    void SetOldSchema(TableSchema* schema);
    bool GetOldSchema(TableSchema* schema);
    void ClearOldSchema();
    bool PrepareUpdate(const TableSchema& schema);
    void AbortUpdate();
    void CommitUpdate();

private:
    Table(const Table&) {}
    Table& operator=(const Table&) {return *this;}
    typedef std::map<std::string, TabletPtr> TabletList;
    TabletList tablets_list_;
    mutable Mutex mutex_;
    std::string name_;
    TableSchema schema_;
    std::vector<uint64_t> snapshot_list_;
    std::vector<std::string> rollback_names_;
    TableStatus status_;
    uint32_t deleted_tablet_num_;
    uint64_t max_tablet_no_;
    int64_t create_time_;
    TableCounter counter_;
    bool schema_is_syncing_; // is schema syncing to all ts(all tablets)
    RangeFragment* rangefragment_;
    UpdateTableResponse* update_rpc_response_;
    google::protobuf::Closure* update_rpc_done_;
    TableSchema* old_schema_;
};

class TabletManager {
public:
    typedef Closure<bool, const std::string&, StatusCode*> FindCondCallback;

    TabletManager(Counter* sequence_id, MasterImpl* master_impl, ThreadPool* thread_pool);
    ~TabletManager();

    void Init();
    void Stop();

    bool DumpMetaTable(const std::string& addr, StatusCode* ret_status = NULL);
    bool ClearMetaTable(const std::string& addr, StatusCode* ret_status = NULL);

    bool DumpMetaTableToFile(const std::string& filename,
                             StatusCode* ret_status = NULL);

    bool AddTable(const std::string& table_name, const TableMeta& meta, TablePtr* table,
                  StatusCode* ret_status);

    bool AddTablet(const TabletMeta& meta, const TableSchema& schema,
                   TabletPtr* tablet, StatusCode* ret_status = NULL);

    bool AddTablet(const std::string& table_name, const std::string& key_start,
                   const std::string& key_end, const std::string& path,
                   const std::string& server_addr, const TableSchema& schema,
                   const TabletStatus& table_status, int64_t data_size,
                   TabletPtr* tablet, StatusCode* ret_status = NULL);

    bool DeleteTable(const std::string& table_name,
                     StatusCode* ret_status = NULL);

    bool DeleteTablet(const std::string& table_name,
                      const std::string& key_start,
                      StatusCode* ret_status = NULL);

    bool FindTablet(const std::string& table_name,
                    const std::string& key_start, TabletPtr* tablet,
                    StatusCode* ret_status = NULL);

    void FindTablet(const std::string& server_addr,
                    std::vector<TabletPtr>* tablet_meta_list,
                    bool need_disabled_tables);

    bool FindTable(const std::string& table_name,
                   std::vector<TabletPtr>* tablet_meta_list,
                   StatusCode* ret_status = NULL);

    bool FindTable(const std::string& table_name, TablePtr* tablet);

    int64_t SearchTable(std::vector<TabletPtr>* tablet_meta_list,
                        const std::string& prefix_table_name,
                        const std::string& start_table_name = "",
                        const std::string& start_tablet_key = "",
                        uint32_t max_found = std::numeric_limits<unsigned int>::max(),
                        StatusCode* ret_status = NULL);

    bool ShowTable(std::vector<TablePtr>* table_meta_list,
                      std::vector<TabletPtr>* tablet_meta_list,
                      const std::string& start_table_name = "",
                      const std::string& start_tablet_key = "",
                      uint32_t max_table_found = std::numeric_limits<unsigned int>::max(),
                      uint32_t max_tablet_found = std::numeric_limits<unsigned int>::max(),
                      bool* is_more = NULL,
                      StatusCode* ret_status = NULL);

    bool GetMetaTabletAddr(std::string* addr);

    void ClearTableList();

    double OfflineTabletRatio();

    bool PickMergeTablet(TabletPtr& tablet, TabletPtr* tablet2);

    void LoadTableMeta(const std::string& key, const std::string& value);
    void LoadTabletMeta(const std::string& key, const std::string& value);

    int64_t GetAllTabletsCount();
private:
    void PackTabletMeta(TabletMeta* meta, const std::string& table_name,
                        const std::string& key_start = "",
                        const std::string& key_end = "",
                        const std::string& path = "",
                        const std::string& server_addr = "",
                        const TabletStatus& table_status = kTableNotInit,
                        int64_t data_size = 0);

    bool CheckStatusSwitch(TabletStatus old_status, TabletStatus new_status);

    bool WriteMetaTabletRecord(const TabletMeta& meta,
                               StatusCode* ret_status = NULL);
    bool DeleteMetaTabletRecord(const TabletMeta& meta,
                                StatusCode* ret_status = NULL);

    bool RpcChannelHealth(int32_t err_code);
    void TryMajorCompact(Tablet* tablet);
    void MajorCompactCallback(Tablet* tb, int32_t retry,
                              CompactTabletRequest* request,
                              CompactTabletResponse* response,
                              bool failed, int error_code);

    void WriteToStream(std::ofstream& ofs, const std::string& key,
                       const std::string& value);

private:
    typedef std::map<std::string, TablePtr> TableList;
    TableList all_tables_;
    mutable Mutex mutex_;
    Counter* this_sequence_id_;
    MasterImpl* master_impl_;
};

int64_t CounterWeightedSum(int64_t a1, int64_t a2);

} // namespace master
} // namespace tera

#endif // TERA_MASTER_TABLET_MANAGER_H_
