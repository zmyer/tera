// Copyright (c) 2015, Baidu.com, Inc. All Rights Reserved
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "sdk/client_impl.h"
#include "version.h"

#include <iostream>

#include "gflags/gflags.h"

#include "common/file/file_path.h"
#include "common/mutex.h"
#include "proto/kv_helper.h"
#include "proto/master_client.h"
#include "proto/proto_helper.h"
#include "proto/table_meta.pb.h"
#include "proto/tabletnode_client.h"
#include "sdk/table_impl.h"
#include "sdk/sdk_utils.h"
#include "sdk/sdk_zk.h"
#include "utils/config_utils.h"
#include "utils/crypt.h"
#include "utils/schema_utils.h"
#include "utils/string_util.h"
#include "utils/utils_cmd.h"

DECLARE_string(tera_master_meta_table_name);
DECLARE_string(tera_sdk_conf_file);
DECLARE_string(tera_user_identity);
DECLARE_string(tera_user_passcode);

DECLARE_int32(tera_sdk_retry_times);
DECLARE_int32(tera_sdk_update_meta_internal);
DECLARE_int32(tera_sdk_retry_period);
DECLARE_int32(tera_sdk_thread_min_num);
DECLARE_int32(tera_sdk_thread_max_num);
DECLARE_bool(tera_sdk_rpc_limit_enabled);
DECLARE_bool(tera_sdk_table_rename_enabled);
DECLARE_int32(tera_sdk_rpc_limit_max_inflow);
DECLARE_int32(tera_sdk_rpc_limit_max_outflow);
DECLARE_int32(tera_sdk_rpc_max_pending_buffer_size);
DECLARE_int32(tera_sdk_rpc_work_thread_num);
DECLARE_int32(tera_sdk_show_max_num);
DECLARE_bool(tera_online_schema_update_enabled);

namespace tera {

static pthread_once_t sdk_client_once_control = PTHREAD_ONCE_INIT;

void LogSdkVersionInfo() {
    LOG(INFO) << "\n" << SystemVersionInfo();
}

ClientImpl::ClientImpl(const std::string& user_identity,
                       const std::string& user_passcode)
    : thread_pool_(FLAGS_tera_sdk_thread_max_num),
      user_identity_(user_identity),
      user_passcode_(user_passcode) {
    tabletnode::TabletNodeClient::SetThreadPool(&thread_pool_);
    tabletnode::TabletNodeClient::SetRpcOption(
        FLAGS_tera_sdk_rpc_limit_enabled ? FLAGS_tera_sdk_rpc_limit_max_inflow : -1,
        FLAGS_tera_sdk_rpc_limit_enabled ? FLAGS_tera_sdk_rpc_limit_max_outflow : -1,
        FLAGS_tera_sdk_rpc_max_pending_buffer_size, FLAGS_tera_sdk_rpc_work_thread_num);
    cluster_ = sdk::NewClusterFinder();
    pthread_once(&sdk_client_once_control, LogSdkVersionInfo);
}

ClientImpl::~ClientImpl() {
    {
        MutexLock l(&open_table_mutex_);
        if (!open_table_map_.empty()) {
            std::map<std::string, TableHandle>::iterator it = open_table_map_.begin() ;
            for (; it != open_table_map_.end(); ++it) {
                LOG(ERROR) << "table should be delete first: " << it->first;
            }
        }
    }
    delete cluster_;
}

bool ClientImpl::CreateTable(const TableDescriptor& desc, ErrorCode* err) {
    std::vector<string> empty_delimiter;
    return CreateTable(desc, empty_delimiter, err);
}

std::string ClientImpl::GetUserToken(const std::string& user, const std::string& password) {
    std::string token_str = user + ":" + password;
    std::string token;
    GetHashString(token_str, 0, &token);
    return token;
}

bool ClientImpl::CheckReturnValue(StatusCode status, std::string& reason, ErrorCode* err) {
    switch (status) {
        case kMasterOk:
            err->SetFailed(ErrorCode::kOK, "success");
            LOG(INFO) << "master status is OK.";
            return true;
        case kTableExist:
            reason = "table already exist.";
            err->SetFailed(ErrorCode::kBadParam, reason);
            break;
        case kTableNotExist:
            reason = "table not exist.";
            err->SetFailed(ErrorCode::kBadParam, reason);
            break;
        case kTableNotFound:
            reason = "table not found.";
            err->SetFailed(ErrorCode::kBadParam, reason);
            break;
        case kTableStatusDisable:
            reason = "table status: disable.";
            err->SetFailed(ErrorCode::kBadParam, reason);
            break;
        case kTableStatusEnable:
            reason = "table status: enable.";
            err->SetFailed(ErrorCode::kSystem, reason);
            break;
        case kInvalidArgument:
            reason = "invalid arguments.";
            err->SetFailed(ErrorCode::kBadParam, reason);
            break;
        case kNotPermission:
            reason = "permission denied.";
            err->SetFailed(ErrorCode::kNoAuth, reason);
            break;
        case kTabletReady:
            reason = "tablet is ready.";
            err->SetFailed(ErrorCode::kOK, reason);
            break;
        default:
            reason = "unknown system error, contact to cluster admin...";
            err->SetFailed(ErrorCode::kSystem, reason);
            break;
    }
    return false;
}

bool ClientImpl::CreateTable(const TableDescriptor& desc,
                             const std::vector<string>& tablet_delim,
                             ErrorCode* err) {
    if (!CheckTableDescrptor(desc, err)) {
        return false;
    }
    master::MasterClient master_client(cluster_->MasterAddr());

    CreateTableRequest request;
    CreateTableResponse response;
    request.set_sequence_id(0);
    std::string timestamp = tera::get_curtime_str_plain();
    std::string internal_table_name;
    if (FLAGS_tera_sdk_table_rename_enabled) {
        internal_table_name = desc.TableName() + "@" + timestamp;
    } else {
        internal_table_name = desc.TableName();
    }
    request.set_table_name(internal_table_name);
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));

    TableSchema* schema = request.mutable_schema();

    TableDescToSchema(desc, schema);
    schema->set_alias(desc.TableName());
    schema->set_name(internal_table_name);
    // add delimiter
    size_t delim_num = tablet_delim.size();
    for (size_t i = 0; i < delim_num; ++i) {
        const string& delim = tablet_delim[i];
        request.add_delimiters(delim);
    }
    string reason;
    if (master_client.CreateTable(&request, &response)) {
        if (CheckReturnValue(response.status(), reason, err)) {
            return true;
        }
        LOG(ERROR) << reason << "| status: " << StatusCodeToString(response.status());
    } else {
        reason = "rpc fail to create table:" + desc.TableName();
        LOG(ERROR) << reason;
        err->SetFailed(ErrorCode::kSystem, reason);
    }
    return false;
}

bool ClientImpl::UpdateTableSchema(const TableDescriptor& desc, ErrorCode* err) {
    return UpdateTable(desc, err);
}

bool ClientImpl::UpdateTable(const TableDescriptor& desc, ErrorCode* err) {
    if (!IsTableExist(desc.TableName(), err)) {
        LOG(ERROR) << "table not exist: " << desc.TableName();
        return false;
    }

    master::MasterClient master_client(cluster_->MasterAddr());

    UpdateTableRequest request;
    UpdateTableResponse response;
    request.set_sequence_id(0);
    request.set_table_name(desc.TableName());
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));

    TableSchema* schema = request.mutable_schema();
    TableDescToSchema(desc, schema);

    ErrorCode err2;
    TableDescriptor* old_desc = GetTableDescriptor(desc.TableName(), &err2);
    if (old_desc == NULL) {
        return false;
    }
    TableSchema old_schema;
    TableDescToSchema(*old_desc, &old_schema);
    delete old_desc;

    // if try to update lg, need to disable table
    bool is_update_lg = IsSchemaLgDiff(*schema, old_schema);
    bool is_update_cf = IsSchemaCfDiff(*schema, old_schema);

    // compatible for old-master which no support for online-schema-update
    if (!FLAGS_tera_online_schema_update_enabled
        && IsTableEnabled(desc.TableName(), err)
        && (is_update_lg || is_update_cf)) {
        err->SetFailed(ErrorCode::kBadParam, "disable this table if you want to update (Lg | Cf) property(ies)");
        return false;
    }

    string reason;
    if (master_client.UpdateTable(&request, &response)) {
        if (CheckReturnValue(response.status(), reason, err)) {
            return true;
        }
        LOG(ERROR) << reason << "| status: " << StatusCodeToString(response.status());
    } else {
        reason = "rpc fail to update table:" + desc.TableName();
        LOG(ERROR) << reason;
        err->SetFailed(ErrorCode::kSystem, reason);
    }
    return false;
}

bool ClientImpl::UpdateCheck(const std::string& table_name, bool* done, ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());
    UpdateCheckRequest request;
    UpdateCheckResponse response;
    request.set_sequence_id(0);
    request.set_table_name(table_name);
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));

    string reason;
    if (master_client.UpdateCheck(&request, &response)) {
        if (CheckReturnValue(response.status(), reason, err)) {
            *done = response.done();
            return true;
        }
        err->SetFailed(ErrorCode::kSystem, reason);
    } else {
        reason = "rpc fail to update-check table:" + table_name;
        err->SetFailed(ErrorCode::kSystem, reason);
    }
    return false;
}

bool ClientImpl::DropTable(const std::string& name, ErrorCode* err) {
    return DeleteTable(name, err);
}

bool ClientImpl::DeleteTable(const std::string& name, ErrorCode* err) {
    std::string internal_table_name;
    if (!GetInternalTableName(name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    master::MasterClient master_client(cluster_->MasterAddr());

    DeleteTableRequest request;
    DeleteTableResponse response;
    request.set_sequence_id(0);
    request.set_table_name(internal_table_name);
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));

    string reason;
    if (master_client.DeleteTable(&request, &response)) {
        if (CheckReturnValue(response.status(), reason, err)) {
            return true;
        }
    } else {
        reason = "rpc fail to delete table: " + name;
        LOG(ERROR) << reason;
        err->SetFailed(ErrorCode::kSystem, reason);
    }
    return false;
}

bool ClientImpl::DisableTable(const std::string& name, ErrorCode* err) {
    std::string internal_table_name;
    if (!GetInternalTableName(name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    master::MasterClient master_client(cluster_->MasterAddr());

    DisableTableRequest request;
    DisableTableResponse response;
    request.set_sequence_id(0);
    request.set_table_name(internal_table_name);
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));

    string reason;
    if (master_client.DisableTable(&request, &response)) {
        if (CheckReturnValue(response.status(), reason, err)) {
            return true;
        }
        LOG(ERROR) << reason << "| status: " << StatusCodeToString(response.status());
    } else {
        reason = "rpc fail to disable table: " + name;
        LOG(ERROR) << reason;
        err->SetFailed(ErrorCode::kSystem, reason);
    }
    return false;
}

bool ClientImpl::EnableTable(const std::string& name, ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());
    std::string internal_table_name;
    if (!GetInternalTableName(name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    EnableTableRequest request;
    EnableTableResponse response;
    request.set_sequence_id(0);
    request.set_table_name(internal_table_name);
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));

    std::string reason;
    if (master_client.EnableTable(&request, &response)) {
        if (CheckReturnValue(response.status(), reason, err)) {
            return true;
        }
        LOG(ERROR) << reason << "| status: " << StatusCodeToString(response.status());
    } else {
        reason = "rpc fail to enable table: " + name;
        LOG(ERROR) << reason;
        err->SetFailed(ErrorCode::kSystem, reason);
    }
    return false;
}

void ClientImpl::DoShowUser(OperateUserResponse& response,
                            std::vector<std::string>& user_groups) {
    if (!response.has_user_info()) {
        return;
    }
    UserInfo user_info = response.user_info();
    user_groups.push_back(user_info.user_name());
    for (int i = 0; i < user_info.group_name_size(); ++i) {
        user_groups.push_back(user_info.group_name(i));
    }
}

bool ClientImpl::OperateUser(UserInfo& operated_user, UserOperateType type,
                             std::vector<std::string>& user_groups, ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());
    OperateUserRequest request;
    OperateUserResponse response;
    request.set_sequence_id(0);
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));
    request.set_op_type(type);
    UserInfo* user_info = request.mutable_user_info();
    user_info->CopyFrom(operated_user);
    std::string reason;
    if (master_client.OperateUser(&request, &response)) {
        if (CheckReturnValue(response.status(), reason, err)) {
            if (type == kShowUser) {
                DoShowUser(response, user_groups);
            }
            return true;
        }
        LOG(ERROR) << reason << "| status: " << StatusCodeToString(response.status());
    } else {
        reason = "rpc fail to operate user: " + operated_user.user_name();
        LOG(ERROR) << reason;
        err->SetFailed(ErrorCode::kSystem, reason);
    }
    return false;
}

bool ClientImpl::CreateUser(const std::string& user,
                            const std::string& password, ErrorCode* err) {
    UserInfo created_user;
    created_user.set_user_name(user);
    created_user.set_token(GetUserToken(user, password));
    std::vector<std::string> null;
    return OperateUser(created_user, kCreateUser, null, err);
}

bool ClientImpl::DeleteUser(const std::string& user, ErrorCode* err) {
    UserInfo deleted_user;
    deleted_user.set_user_name(user);
    std::vector<std::string> null;
    return OperateUser(deleted_user, kDeleteUser, null, err);
}

bool ClientImpl::ChangePwd(const std::string& user,
                           const std::string& password, ErrorCode* err) {
    UserInfo updated_user;
    updated_user.set_user_name(user);
    updated_user.set_token(GetUserToken(user, password));
    std::vector<std::string> null;
    return OperateUser(updated_user, kChangePwd, null, err);
}

bool ClientImpl::ShowUser(const std::string& user, std::vector<std::string>& user_groups,
                          ErrorCode* err) {
    UserInfo user_info;
    user_info.set_user_name(user);
    user_info.set_token(GetUserToken(user_identity_, user_passcode_));
    return OperateUser(user_info, kShowUser, user_groups, err);
}

bool ClientImpl::AddUserToGroup(const std::string& user_name,
                                const std::string& group_name, ErrorCode* err) {
    UserInfo user;
    user.set_user_name(user_name);
    user.add_group_name(group_name);
    std::vector<std::string> null;
    return OperateUser(user, kAddToGroup, null, err);
}

bool ClientImpl::DeleteUserFromGroup(const std::string& user_name,
                                     const std::string& group_name, ErrorCode* err) {
    UserInfo user;
    user.set_user_name(user_name);
    user.add_group_name(group_name);
    std::vector<std::string> null;
    return OperateUser(user, kDeleteFromGroup, null, err);
}

bool ClientImpl::GetInternalTableName(const std::string& table_name, ErrorCode* err,
                                      std::string* internal_table_name) {
    *internal_table_name = table_name;
    tabletnode::TabletNodeClient meta_client(cluster_->RootTableAddr(true));
    ScanTabletRequest request;
    ScanTabletResponse response;
    request.set_sequence_id(0);
    request.set_table_name(FLAGS_tera_master_meta_table_name);
    request.set_start("");
    request.set_end("@~");
    if (!meta_client.ScanTablet(&request, &response)
          || response.status() != kTabletNodeOk) {
        LOG(ERROR) << "fail to scan meta: " << StatusCodeToString(response.status());
        if (err != NULL) {
            err->SetFailed(ErrorCode::kSystem, "system error");
        }
        return false;
    }
    if (err != NULL) {
        err->SetFailed(ErrorCode::kOK);
    }
    int32_t table_size = response.results().key_values_size();
    for (int32_t i = 0; i < table_size; i++) {
        const KeyValuePair& record = response.results().key_values(i);
        const string& key = record.key();
        const string& value = record.value();
        if (key[0] == '@') {
            TableMeta meta;
            ParseMetaTableKeyValue(key, value, &meta);
            if (meta.schema().alias() == table_name) {
                *internal_table_name =  meta.table_name();
                break;
            }
        } else if (key[0] > '@') {
            break;
        } else {
            continue;
        }
    }
    return true;
}

Table* ClientImpl::OpenTable(const std::string& table_name,
                             ErrorCode* err) {
    open_table_mutex_.Lock();
    TableHandle& th = open_table_map_[table_name];
    th.ref++;

    if (th.mu == NULL) {
        // open a new table
        CHECK(th.handle == NULL);
        th.mu = new Mutex();
        th.mu->Lock();
        open_table_mutex_.Unlock();
        VLOG(10) << "open a new table: " << table_name;
        th.handle = OpenTableInternal(table_name, &th.err);
        th.mu->Unlock();
    } else {
        open_table_mutex_.Unlock();
    }

    th.mu->Lock();
    if (err) {
        *err = th.err;
    }
    if (th.handle == NULL) {
        VLOG(10) << "open null table: " << table_name;
        th.mu->Unlock();
        MutexLock l(&open_table_mutex_);
        if (--th.ref == 0) {
            delete th.mu;
            open_table_map_.erase(table_name);
        }
        return NULL;
    }

    Table* table_impl = th.handle;
    th.mu->Unlock();
    return new TableWrapper(table_impl, this);
}

TableImpl* ClientImpl::OpenTableInternal(const std::string& table_name,
                                         ErrorCode* err) {
    std::string internal_table_name;
    if (!GetInternalTableName(table_name, err, &internal_table_name)) {
        std::string reason = "fail to scan meta schema";
        if (err != NULL) {
            err->SetFailed(ErrorCode::kBadParam, reason);
        }
        LOG(ERROR) << reason;
        return NULL;
    }
    err->SetFailed(ErrorCode::kOK);
    TableImpl* table = new TableImpl(internal_table_name, &thread_pool_, cluster_);
    if (table == NULL) {
        std::string reason = "fail to new TableImpl";
        if (err != NULL) {
            err->SetFailed(ErrorCode::kBadParam, reason);
        }
        LOG(ERROR) << reason;
        return NULL;
    }
    if (!table->OpenInternal(err)) {
        delete table;
        return NULL;
    }
    return table;
}

void ClientImpl::CloseTable(const std::string& table_name) {
    MutexLock l(&open_table_mutex_);
    std::map<std::string, TableHandle>::iterator it;
    it = open_table_map_.find(table_name);
    assert(it != open_table_map_.end());
    TableHandle& h = it->second;
    h.ref--;
    if (h.ref == 0) {
        VLOG(10) << "close table: " << table_name;
        delete h.handle;
        delete h.mu;
        open_table_map_.erase(it);
    }
}

bool ClientImpl::GetTabletLocation(const string& table_name,
                                   std::vector<TabletInfo>* tablets,
                                   ErrorCode* err) {
    TableMeta table_meta;
    TabletMetaList tablet_list;

    if (!ShowTablesInfo(table_name, &table_meta, &tablet_list, err)) {
        LOG(ERROR) << "table not exist: " << table_name;
        return false;
    }

    for (int i = 0; i < tablet_list.meta_size(); ++i) {
        ParseTabletEntry(tablet_list.meta(i), tablets);
    }
    return true;
}

TableDescriptor* ClientImpl::GetTableDescriptor(const string& table_name,
                                                ErrorCode* err) {
    std::string internal_table_name;
    if (!GetInternalTableName(table_name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return NULL;
    }
    std::vector<TableInfo> table_list;
    ListInternal(&table_list, NULL, internal_table_name, "", 1, 0, err);
    if (table_list.size() > 0
        && table_list[0].table_desc->TableName() == internal_table_name) {
        return table_list[0].table_desc;
    }
    return NULL;
}

//bool ClientImpl::List(std::vector<TableInfo>* table_list,
//                      std::vector<TabletInfo>* tablet_list,
//                      ErrorCode* err) {
//      tabletnode::TabletNodeClient meta_client;
//      meta_client.ResetTabletNodeClient(cluster_->RootTableAddr());
//
//      ScanTabletRequest request;
//      ScanTabletResponse response;
//      request.set_sequence_id(0);
//      request.set_table_name(FLAGS_tera_master_meta_table_name);
//      //MetaTableListScanRange(request.mutable_start(), request.mutable_end());
//      request.mutable_key_range()->set_key_start("");
//      request.mutable_key_range()->set_key_end("");
//
//      if (!meta_client.ScanTablet(&request, &response)
//          || response.status() != kTabletNodeOk) {
//          err->SetFailed(ErrorCode::kSystem, "system error");
//          return false;
//      }
//
//      err->SetFailed(ErrorCode::kOK);
//
//      int32_t table_size = response.kv_list_size();
//      for (int32_t i = 0; i < table_size; i++) {
//          const KeyValuePair& record = response.kv_list(i);
//          const string& key = record.key();
//          const string& value = record.value();
//
//          if (key[0] == '@') {
//              TableMeta meta;
//              ParseMetaTableKeyValue(key, value, &meta);
//              ParseTableEntry(meta, table_list);
//          } else if (key[0] > '@') {
//              TabletMeta meta;
//              ParseMetaTableKeyValue(key, value, &meta);
//              ParseTabletEntry(meta, tablet_list);
//          } else {
//              continue;
//          }
//      }
//      return true;
//}

bool ClientImpl::List(std::vector<TableInfo>* table_list, ErrorCode* err) {
    std::vector<TabletInfo> tablet_list;
    return ListInternal(table_list, &tablet_list, "", "",
                        FLAGS_tera_sdk_show_max_num,
                        0, err);
}

bool ClientImpl::ShowTableSchema(const string& name, TableSchema* schema,
                                 ErrorCode* err) {
    tabletnode::TabletNodeClient meta_client(cluster_->RootTableAddr(true));
    ScanTabletRequest request;
    ScanTabletResponse response;
    request.set_sequence_id(0);
    request.set_table_name(FLAGS_tera_master_meta_table_name);
    request.set_start("");
    request.set_end("@~");
    if (!meta_client.ScanTablet(&request, &response)
        || response.status() != kTabletNodeOk) {
        LOG(ERROR) << "fail to scan meta: " << StatusCodeToString(response.status());
        err->SetFailed(ErrorCode::kSystem, "system error");
        return false;
    }
    int32_t table_size = response.results().key_values_size();
    for (int32_t i = 0; i < table_size; i++) {
        const KeyValuePair& record = response.results().key_values(i);
        const string& key = record.key();
        const string& value = record.value();
        if (key[0] == '@') {
            TableMeta meta;
            ParseMetaTableKeyValue(key, value, &meta);
            if (meta.schema().name() == name
                || meta.schema().alias() == name) {
                *schema = meta.schema();
                return true;
            }
        } else if (key[0] > '@') {
            break;
        } else {
            continue;
        }
    }
    return false;
}

// show exactly one table
bool ClientImpl::ShowTablesInfo(const string& name,
                                TableMeta* meta,
                                TabletMetaList* tablet_list,
                                ErrorCode* err) {
    TableMetaList table_list;
    std::string internal_table_name;
    if (!GetInternalTableName(name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    bool result = DoShowTablesInfo(&table_list, tablet_list, internal_table_name,
                                   false, err);
    if ((table_list.meta_size() == 0)
        || (table_list.meta(0).table_name() != internal_table_name)) {
        return false;
    }
    if (result) {
        meta->CopyFrom(table_list.meta(0));
    }
    return result;
}

bool ClientImpl::ShowTablesInfo(TableMetaList* table_list,
                                TabletMetaList* tablet_list,
                                bool is_brief,
                                ErrorCode* err) {
    return DoShowTablesInfo(table_list, tablet_list, "", is_brief, err);
}

bool ClientImpl::DoShowTablesInfo(TableMetaList* table_list,
                                  TabletMetaList* tablet_list,
                                  const string& table_name,
                                  bool is_brief,
                                  ErrorCode* err) {
    if (table_list == NULL || tablet_list == NULL) {
        return false;
    }
    table_list->Clear();
    tablet_list->Clear();

    master::MasterClient master_client(cluster_->MasterAddr());
    std::string start_tablet_key;
    std::string start_table_name = table_name; // maybe a empty string
    bool has_more = true;
    bool has_error = false;
    bool table_meta_copied = false;
    std::string err_msg;
    while(has_more && !has_error) {
        ShowTablesRequest request;
        ShowTablesResponse response;
        if (!table_name.empty()) {
            request.set_max_table_num(1);
        }
        request.set_start_table_name(start_table_name);
        request.set_start_tablet_key(start_tablet_key);
        request.set_max_tablet_num(FLAGS_tera_sdk_show_max_num); //tablets be fetched at most in one RPC
        request.set_sequence_id(0);
        request.set_user_token(GetUserToken(user_identity_, user_passcode_));
        request.set_all_brief(is_brief);

        if (master_client.ShowTables(&request, &response) &&
            response.status() == kMasterOk) {
            if (response.all_brief()) {
                // show all table brief
                table_list->CopyFrom(response.table_meta_list());
                return true;
            }

            if (response.table_meta_list().meta_size() == 0) {
                has_error = true;
                err_msg = StatusCodeToString(response.status());
                break;
            }
            if (!table_meta_copied) {
                table_list->CopyFrom(response.table_meta_list());
                table_meta_copied = true;
            }
            if (response.tablet_meta_list().meta_size() == 0) {
                has_more = false;
            }
            for(int i = 0; i < response.tablet_meta_list().meta_size(); i++){
                const std::string& table_name = response.tablet_meta_list().meta(i).table_name();
                const std::string& tablet_key = response.tablet_meta_list().meta(i).key_range().key_start();
                // compatible to old master
                if (table_name > start_table_name
                    || (table_name == start_table_name && tablet_key >= start_tablet_key)) {
                    tablet_list->add_meta()->CopyFrom(response.tablet_meta_list().meta(i));
                    tablet_list->add_counter()->CopyFrom(response.tablet_meta_list().counter(i));
                    // old tera master will not return timestamp #963
                    if (response.tablet_meta_list().timestamp_size() > 0) {
                        tablet_list->add_timestamp(response.tablet_meta_list().timestamp(i));
                    }
                }
                if (i == response.tablet_meta_list().meta_size() - 1 ) {
                    std::string prev_table_name = start_table_name;
                    start_table_name = response.tablet_meta_list().meta(i).table_name();
                    std::string last_key = response.tablet_meta_list().meta(i).key_range().key_start();
                    if (prev_table_name > start_table_name
                        || (prev_table_name == start_table_name && last_key <= start_tablet_key)) {
                        LOG(WARNING) << "the master has older version";
                        has_more = false;
                        break;
                    }
                    start_tablet_key = last_key;
                }
            }
            start_tablet_key.append(1,'\0'); // fetch next tablet
        } else {
            if (response.status() != kMasterOk &&
                response.status() != kTableNotFound) {
                has_error = true;
                err_msg = StatusCodeToString(response.status());
            }
            has_more = false;
        }
        VLOG(16) << "fetch meta table name: " << start_table_name
                 << " tablet size: " << response.tablet_meta_list().meta_size()
                 << " next start: " << DebugString(start_tablet_key);
    };

    if (has_error) {
        LOG(ERROR) << "fail to show table info.";
        if (err != NULL) {
            err->SetFailed(ErrorCode::kSystem, err_msg);
        }
        return false;
    }
    return true;
}

bool ClientImpl::ShowTabletNodesInfo(const string& addr,
                                     TabletNodeInfo* info,
                                     TabletMetaList* tablet_list,
                                     ErrorCode* err) {
    if (info == NULL || tablet_list == NULL) {
        return false;
    }
    info->Clear();
    tablet_list->Clear();

    master::MasterClient master_client(cluster_->MasterAddr());

    ShowTabletNodesRequest request;
    ShowTabletNodesResponse response;
    request.set_sequence_id(0);
    request.set_addr(addr);
    request.set_is_showall(false);
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));

    if (master_client.ShowTabletNodes(&request, &response) &&
        response.status() == kMasterOk) {
        if (response.tabletnode_info_size() == 0) {
            return false;
        }
        info->CopyFrom(response.tabletnode_info(0));
        tablet_list->CopyFrom(response.tabletmeta_list());
        return true;
    }
    LOG(ERROR) << "fail to show tabletnode info: " << addr;
    err->SetFailed(ErrorCode::kSystem, StatusCodeToString(response.status()));
    return false;
}

bool ClientImpl::ShowTabletNodesInfo(std::vector<TabletNodeInfo>* infos,
                                     ErrorCode* err) {
    if (infos == NULL) {
        return false;
    }
    infos->clear();

    master::MasterClient master_client(cluster_->MasterAddr());

    ShowTabletNodesRequest request;
    ShowTabletNodesResponse response;
    request.set_sequence_id(0);
    request.set_is_showall(true);

    if (master_client.ShowTabletNodes(&request, &response) &&
        response.status() == kMasterOk) {
        if (response.tabletnode_info_size() == 0) {
            return true;
        }
        for (int i = 0; i < response.tabletnode_info_size(); ++i) {
            infos->push_back(response.tabletnode_info(i));
        }
        return true;
    }
    LOG(ERROR) << "fail to show tabletnode info";
    err->SetFailed(ErrorCode::kSystem, StatusCodeToString(response.status()));
    return false;
}

bool ClientImpl::List(const string& table_name, TableInfo* table_info,
                      std::vector<TabletInfo>* tablet_list, ErrorCode* err) {
    std::vector<TableInfo> table_list;
    std::string internal_table_name;
    if (!GetInternalTableName(table_name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    bool ret = ListInternal(&table_list, tablet_list, internal_table_name, "", 1,
                            FLAGS_tera_sdk_show_max_num, err);
    if (table_list.size() > 0
        && table_list[0].table_desc->TableName() == internal_table_name) {
        *table_info = table_list[0];
    }
    return ret;
}

bool ClientImpl::IsTableExist(const string& table_name, ErrorCode* err) {
    std::vector<TableInfo> table_list;
    std::string internal_table_name;
    if (!GetInternalTableName(table_name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    ListInternal(&table_list, NULL, internal_table_name, "", 1, 0, err);
    if (table_list.size() > 0
        && table_list[0].table_desc->TableName() == internal_table_name) {
        return true;
    }
    return false;
}

bool ClientImpl::IsTableEnabled(const string& table_name, ErrorCode* err) {
    std::vector<TableInfo> table_list;
    std::string internal_table_name;
    if (!GetInternalTableName(table_name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    ListInternal(&table_list, NULL, internal_table_name, "", 1, 0, err);
    if (table_list.size() > 0
        && table_list[0].table_desc->TableName() == internal_table_name) {
        if (table_list[0].status == "kTableEnable") {
            return true;
        } else {
            return false;
        }
    } else {
        LOG(ERROR) << "table not exist: " << table_name;
    }
    return false;
}

bool ClientImpl::IsTableEmpty(const string& table_name, ErrorCode* err) {
    std::vector<TableInfo> table_list;
    std::vector<TabletInfo> tablet_list;
    std::string internal_table_name;
    if (!GetInternalTableName(table_name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    ListInternal(&table_list, &tablet_list, internal_table_name, "", 1,
                 FLAGS_tera_sdk_show_max_num, err);
    if (table_list.size() > 0
        && table_list[0].table_desc->TableName() == internal_table_name) {
        if (tablet_list.size() == 0
            || (tablet_list.size() == 1 && tablet_list[0].data_size <= 0)) {
            return true;
        }
        return false;
    }
    LOG(ERROR) << "table not exist: " << table_name;
    return true;
}

bool ClientImpl::GetSnapshot(const string& name, uint64_t* snapshot, ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());

    std::string internal_table_name;
    if (!GetInternalTableName(name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    GetSnapshotRequest request;
    GetSnapshotResponse response;
    request.set_sequence_id(0);
    request.set_table_name(internal_table_name);

    if (master_client.GetSnapshot(&request, &response)) {
        if (response.status() == kMasterOk) {
            std::cout << name << " get snapshot successfully" << std::endl;
            *snapshot = response.snapshot_id();
            return true;
        }
    }
    err->SetFailed(ErrorCode::kSystem, StatusCodeToString(response.status()));
    std::cout << name << " get snapshot failed";
    return false;
}

bool ClientImpl::DelSnapshot(const string& name, uint64_t snapshot, ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());

    std::string internal_table_name;
    if (!GetInternalTableName(name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    DelSnapshotRequest request;
    DelSnapshotResponse response;
    request.set_sequence_id(0);
    request.set_table_name(internal_table_name);
    request.set_snapshot_id(snapshot);

    if (master_client.DelSnapshot(&request, &response)) {
        if (response.status() == kMasterOk) {
            std::cout << name << " del snapshot successfully" << std::endl;
            return true;
        }
    }
    err->SetFailed(ErrorCode::kSystem, StatusCodeToString(response.status()));
    std::cout << name << " del snapshot failed";
    return false;
}

bool ClientImpl::Rollback(const string& name, uint64_t snapshot,
                          const std::string& rollback_name, ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());

    std::string internal_table_name;
    if (!GetInternalTableName(name, err, &internal_table_name)) {
        LOG(ERROR) << "faild to scan meta schema";
        return false;
    }
    RollbackRequest request;
    RollbackResponse response;
    request.set_sequence_id(0);
    request.set_table_name(internal_table_name);
    request.set_snapshot_id(snapshot);
    request.set_rollback_name(rollback_name);
    std::cout << name << " " << rollback_name << std::endl;

    if (master_client.GetRollback(&request, &response)) {
        if (response.status() == kMasterOk) {
            std::cout << name << " rollback to snapshot sucessfully" << std::endl;
            return true;
        }
    }
    err->SetFailed(ErrorCode::kSystem, StatusCodeToString(response.status()));
    std::cout << name << " rollback to snapshot failed";
    return false;
}

bool ClientImpl::CmdCtrl(const string& command,
                         const std::vector<string>& arg_list,
                         bool* bool_result,
                         string* str_result,
                         ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());

    CmdCtrlRequest request;
    CmdCtrlResponse response;
    request.set_sequence_id(0);
    request.set_command(command);
    std::vector<string>::const_iterator it = arg_list.begin();
    for (; it != arg_list.end(); ++it) {
        request.add_arg_list(*it);
    }

    if (!master_client.CmdCtrl(&request, &response)
        || response.status() != kMasterOk) {
        LOG(ERROR) << "fail to run cmd: " << command;
        err->SetFailed(ErrorCode::kBadParam);
        return false;
    }
    if (bool_result != NULL && response.has_bool_result()) {
        *bool_result = response.bool_result();
    }
    if (str_result != NULL && response.has_str_result()) {
        *str_result = response.str_result();
    }
    return true;
}

bool ClientImpl::Rename(const std::string& old_table_name,
                        const std::string& new_table_name,
                        ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());
    RenameTableRequest request;
    RenameTableResponse response;
    uint64_t sequence_id = 0;
    request.set_sequence_id(sequence_id);
    request.set_old_table_name(old_table_name);
    request.set_new_table_name(new_table_name);
    bool ok = master_client.RenameTable(&request, &response);
    if (!ok || response.status() != kMasterOk) {
        err->SetFailed(ErrorCode::kSystem, "failed to rename table");
        return false;
    }
    LOG(INFO) << "rename table OK. " << old_table_name
              << " -> " << new_table_name;
    return true;
}

bool ClientImpl::ListInternal(std::vector<TableInfo>* table_list,
                              std::vector<TabletInfo>* tablet_list,
                              const string& start_table_name,
                              const string& start_tablet_key,
                              uint32_t max_table_found,
                              uint32_t max_tablet_found,
                              ErrorCode* err) {
    master::MasterClient master_client(cluster_->MasterAddr());

    uint64_t sequence_id = 0;
    ShowTablesRequest request;
    ShowTablesResponse response;
    request.set_sequence_id(sequence_id);
    request.set_max_table_num(max_table_found);
    request.set_max_tablet_num(max_tablet_found);
    request.set_start_table_name(start_table_name);
    request.set_start_tablet_key(start_tablet_key);
    request.set_user_token(GetUserToken(user_identity_, user_passcode_));

    bool is_more = true;
    while (is_more) {
        if (!master_client.ShowTables(&request, &response)
            || response.status() != kMasterOk) {
            LOG(ERROR) << "fail to show tables from table: "
                << request.start_table_name() << ", key: "
                << request.start_tablet_key() << ", status: "
                << StatusCodeToString(response.status());
            err->SetFailed(ErrorCode::kSystem);
            return false;
        }

        const tera::TableMetaList& table_meta_list = response.table_meta_list();
        const tera::TabletMetaList& tablet_meta_list = response.tablet_meta_list();
        for (int32_t i = 0; i < table_meta_list.meta_size(); ++i) {
            const TableMeta& meta = table_meta_list.meta(i);
            ParseTableEntry(meta, table_list);
        }
        for (int32_t i = 0; i < tablet_meta_list.meta_size(); ++i) {
            const TabletMeta& meta = tablet_meta_list.meta(i);
            ParseTabletEntry(meta, tablet_list);
        }
        if (!response.has_is_more() || !response.is_more()) {
            is_more = false;
        } else {
            if (tablet_meta_list.meta_size() == 0) { //argument @max_tablet_found maybe zero
                break;
            }
            const tera::TabletMeta& meta = tablet_meta_list.meta(tablet_meta_list.meta_size()-1);
            const string& last_key = meta.key_range().key_start();
            request.set_start_table_name(meta.table_name());
            request.set_start_tablet_key(tera::NextKey(last_key));
            request.set_sequence_id(sequence_id++);
        }
    }

    return true;
}

bool ClientImpl::ParseTableEntry(const TableMeta meta, std::vector<TableInfo>* table_list) {
    if (table_list == NULL) {
        return true;
    }
    TableInfo table_info;
    const TableSchema& schema = meta.schema();
    table_info.table_desc = new TableDescriptor(schema.name());

    TableSchemaToDesc(schema, table_info.table_desc);

    for (int i = 0; i < meta.snapshot_list_size(); ++i) {
        table_info.table_desc->AddSnapshot(meta.snapshot_list(i));
    }
    table_info.status = StatusCodeToString(meta.status());
    table_list->push_back(table_info);
    return true;
}

bool ClientImpl::ParseTabletEntry(const TabletMeta& meta, std::vector<TabletInfo>* tablet_list) {
    if (tablet_list == NULL) {
        return true;
    }
    TabletInfo tablet;
    tablet.table_name = meta.table_name();
    tablet.path = meta.path();
    tablet.start_key = meta.key_range().key_start();
    tablet.end_key = meta.key_range().key_end();
    tablet.server_addr = meta.server_addr();
    tablet.data_size = meta.size();
    tablet.status = StatusCodeToString(meta.status());

    tablet_list->push_back(tablet);
    return true;
}

static Mutex g_mutex;
static bool g_is_glog_init = false;

static int SpecifiedFlagfileCount(const std::string& confpath) {
    int count = 0;
    if (!confpath.empty()) {
        count++;
    }
    if (!FLAGS_tera_sdk_conf_file.empty()) {
        count++;
    }
    return count;
}

static int InitFlags(const std::string& confpath, const std::string& log_prefix) {
    // search conf file, priority:
    //   user-specified > ./tera.flag > ../conf/tera.flag
    std::string flagfile;
    if (SpecifiedFlagfileCount(confpath) > 1) {
        LOG(ERROR) << "should specify no more than one config file";
        return -1;
    }

    if (!confpath.empty() && IsExist(confpath)){
        flagfile = confpath;
    } else if(!confpath.empty() && !IsExist(confpath)){
        LOG(ERROR) << "specified config file(function argument) not found: "
            << confpath;
        return -1;
    } else if (!FLAGS_tera_sdk_conf_file.empty() && IsExist(confpath)) {
        flagfile = FLAGS_tera_sdk_conf_file;
    } else if (!FLAGS_tera_sdk_conf_file.empty() && !IsExist(confpath)) {
        LOG(ERROR) << "specified config file(FLAGS_tera_sdk_conf_file) not found";
        return -1;
    } else if (IsExist("./tera.flag")) {
        flagfile = "./tera.flag";
    } else if (IsExist("../conf/tera.flag")) {
        flagfile = "../conf/tera.flag";
    } else if (IsExist(utils::GetValueFromEnv("TERA_CONF"))) {
        flagfile = utils::GetValueFromEnv("TERA_CONF");
    } else {
        LOG(ERROR) << "hasn't specify the flagfile, but default config file not found";
        return -1;
    }

    utils::LoadFlagFile(flagfile);

    if (!g_is_glog_init) {
        ::google::InitGoogleLogging(log_prefix.c_str());
        utils::SetupLog(log_prefix);
        g_is_glog_init = true;
    }

    LOG(INFO) << "USER = " << FLAGS_tera_user_identity;
    LOG(INFO) << "Load config file: " << flagfile;
    return 0;
}

Client* Client::NewClient(const string& confpath, const string& log_prefix, ErrorCode* err) {
    // Protect the section from [load flagfile] to [new a client instance],
    // because the client constructor will use flagfile options to initial its private options
    MutexLock locker(&g_mutex);
    if (InitFlags(confpath, log_prefix) != 0) {
        if (err != NULL) {
            std::string reason = "init tera flag failed";
            err->SetFailed(ErrorCode::kBadParam, reason);
        }
        return NULL;
    }
    return new ClientImpl(FLAGS_tera_user_identity,
                          FLAGS_tera_user_passcode);
}

Client* Client::NewClient(const string& confpath, ErrorCode* err) {
    return NewClient(confpath, "teracli", err);
}

Client* Client::NewClient() {
    return NewClient("", "teracli", NULL);
}

void Client::SetGlogIsInitialized() {
    MutexLock locker(&g_mutex);
    g_is_glog_init = true;
}

} // namespace tera
