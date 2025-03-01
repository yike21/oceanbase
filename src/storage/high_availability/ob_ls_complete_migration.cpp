/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX STORAGE
#include "ob_ls_complete_migration.h"
#include "observer/ob_server.h"
#include "share/rc/ob_tenant_base.h"
#include "logservice/ob_log_service.h"
#include "observer/ob_server_event_history_table_operator.h"
#include "storage/tablet/ob_tablet_iterator.h"


using namespace oceanbase;
using namespace common;
using namespace share;
using namespace storage;

/******************ObLSCompleteMigrationCtx*********************/
ObLSCompleteMigrationCtx::ObLSCompleteMigrationCtx()
  : ObIHADagNetCtx(),
    tenant_id_(OB_INVALID_ID),
    arg_(),
    task_id_(),
    start_ts_(0),
    finish_ts_(0)
{
}

ObLSCompleteMigrationCtx::~ObLSCompleteMigrationCtx()
{
}

bool ObLSCompleteMigrationCtx::is_valid() const
{
  return arg_.is_valid() && !task_id_.is_invalid()
      && tenant_id_ != 0 && tenant_id_ != OB_INVALID_ID;
}

void ObLSCompleteMigrationCtx::reset()
{
  tenant_id_ = OB_INVALID_ID;
  arg_.reset();
  task_id_.reset();
  start_ts_ = 0;
  finish_ts_ = 0;
  ObIHADagNetCtx::reset();
}

int ObLSCompleteMigrationCtx::fill_comment(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;

  if (!is_valid()) {
    ret = OB_NOT_INIT;
    LOG_WARN("ls complete migration ctx do not init", K(ret));
  } else if (NULL == buf || buf_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid args", K(ret), KP(buf), K(buf_len));
  } else if (OB_FAIL(databuff_printf(buf, buf_len, pos, "ls complete migration : task_id = %s, "
      "tenant_id = %s, ls_id = %s, src = %s, dest = %s",
      to_cstring(task_id_), to_cstring(tenant_id_), to_cstring(arg_.ls_id_), to_cstring(arg_.src_.get_server()),
      to_cstring(arg_.dst_.get_server())))) {
    LOG_WARN("failed to set comment", K(ret), K(buf), K(pos), K(buf_len));
  }
  return ret;
}

void ObLSCompleteMigrationCtx::reuse()
{
  ObIHADagNetCtx::reuse();
}

/******************ObLSCompleteMigrationDagNet*********************/
ObLSCompleteMigrationParam::ObLSCompleteMigrationParam()
  : arg_(),
    task_id_(),
    result_(OB_SUCCESS),
    rebuild_seq_(0)
{
}

bool ObLSCompleteMigrationParam::is_valid() const
{
  return arg_.is_valid() && !task_id_.is_invalid() && rebuild_seq_ >= 0;
}

void ObLSCompleteMigrationParam::reset()
{
  arg_.reset();
  task_id_.reset();
  result_ = OB_SUCCESS;
  rebuild_seq_ = 0;
}


ObLSCompleteMigrationDagNet::ObLSCompleteMigrationDagNet()
    : ObIDagNet(ObDagNetType::DAG_NET_TYPE_COMPLETE_MIGARTION),
      is_inited_(false),
      ctx_()

{
}

ObLSCompleteMigrationDagNet::~ObLSCompleteMigrationDagNet()
{
}

int ObLSCompleteMigrationDagNet::init_by_param(const ObIDagInitParam *param)
{
  int ret = OB_SUCCESS;
  const ObLSCompleteMigrationParam* init_param = static_cast<const ObLSCompleteMigrationParam*>(param);
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("ls complete migration dag net is init twice", K(ret));
  } else if (OB_ISNULL(param) || !param->is_valid()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("param is null or invalid", K(ret), KPC(init_param));
  } else if (OB_FAIL(this->set_dag_id(init_param->task_id_))) {
    LOG_WARN("failed to set dag id", K(ret), KPC(init_param));
  } else {
    ctx_.tenant_id_ = MTL_ID();
    ctx_.arg_ = init_param->arg_;
    ctx_.task_id_ = init_param->task_id_;
    ctx_.rebuild_seq_ = init_param->rebuild_seq_;
    if (OB_SUCCESS != init_param->result_) {
      if (OB_FAIL(ctx_.set_result(init_param->result_, false /*allow_retry*/))) {
        LOG_WARN("failed to set result", K(ret), KPC(init_param));
      }
    }

    if (OB_SUCC(ret)) {
      is_inited_ = true;
    }
  }
  return ret;
}

bool ObLSCompleteMigrationDagNet::is_valid() const
{
  return ctx_.is_valid();
}

int ObLSCompleteMigrationDagNet::start_running()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ls complete migration dag net do not init", K(ret));
  } else if (OB_FAIL(start_running_for_migration_())) {
    LOG_WARN("failed to start running for migration", K(ret));
  }
  return ret;
}

int ObLSCompleteMigrationDagNet::start_running_for_migration_()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObInitialCompleteMigrationDag *initial_dag = nullptr;
  ObTenantDagScheduler *scheduler = nullptr;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ls complete migration dag net do not init", K(ret));
  } else if (FALSE_IT(ctx_.start_ts_ = ObTimeUtil::current_time())) {
  } else if (OB_ISNULL(scheduler = MTL(ObTenantDagScheduler*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get ObTenantDagScheduler from MTL", K(ret));
  } else if (OB_FAIL(scheduler->alloc_dag(initial_dag))) {
    LOG_WARN("failed to alloc initial dag ", K(ret));
  } else if (OB_FAIL(initial_dag->init(this))) {
    LOG_WARN("failed to init initial dag", K(ret));
  } else if (OB_FAIL(add_dag_into_dag_net(*initial_dag))) {
    LOG_WARN("failed to add initial dag into dag net", K(ret));
  } else if (OB_FAIL(initial_dag->create_first_task())) {
    LOG_WARN("failed to create first task", K(ret));
  } else if (OB_FAIL(scheduler->add_dag(initial_dag))) {
    LOG_WARN("failed to add initial dag", K(ret), K(*initial_dag));
    if (OB_SIZE_OVERFLOW != ret && OB_EAGAIN != ret) {
      LOG_WARN("Fail to add task", K(ret));
      ret = OB_EAGAIN;
    }
  } else {
    initial_dag = nullptr;
  }

  if (OB_NOT_NULL(initial_dag) && OB_NOT_NULL(scheduler)) {
    initial_dag->reset_children();
    if (OB_SUCCESS != (tmp_ret = (erase_dag_from_dag_net(*initial_dag)))) {
      LOG_WARN("failed to erase dag from dag net", K(tmp_ret), KPC(initial_dag));
    }
    scheduler->free_dag(*initial_dag);
    initial_dag = nullptr;
  }
  return ret;
}

bool ObLSCompleteMigrationDagNet::operator == (const ObIDagNet &other) const
{
  bool is_same = true;
  if (this == &other) {
    // same
  } else if (this->get_type() != other.get_type()) {
    is_same = false;
  } else {
    const ObLSCompleteMigrationDagNet &other_dag_net = static_cast<const ObLSCompleteMigrationDagNet &>(other);
    if (!is_valid() || !other_dag_net.is_valid()) {
      LOG_ERROR("ls complete migration dag net is invalid", K(*this), K(other));
      is_same = false;
    } else if (ctx_.arg_.ls_id_ != other_dag_net.get_ls_id()) {
      is_same = false;
    }
  }
  return is_same;
}

int64_t ObLSCompleteMigrationDagNet::hash() const
{
  int64_t hash_value = 0;
  int tmp_ret = OB_SUCCESS;
  if (!is_inited_) {
    tmp_ret = OB_NOT_INIT;
    LOG_ERROR("ls complete migration ctx is NULL", K(tmp_ret), K(ctx_));
  } else {
    hash_value = common::murmurhash(&ctx_.arg_.ls_id_, sizeof(ctx_.arg_.ls_id_), hash_value);
  }
  return hash_value;
}

int ObLSCompleteMigrationDagNet::fill_comment(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  const int64_t MAX_TRACE_ID_LENGTH = 64;
  char task_id_str[MAX_TRACE_ID_LENGTH] = { 0 };
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ls complete migration dag net do not init ", K(ret));
  } else if (OB_FAIL(ctx_.task_id_.to_string(task_id_str, MAX_TRACE_ID_LENGTH))) {
    LOG_WARN("failed to trace task id to string", K(ret), K(ctx_));
  } else if (OB_FAIL(databuff_printf(buf, buf_len,
          "ObLSCompleteMigrationDagNet: tenant_id=%s, ls_id=%s, migration_type=%d, trace_id=%s",
          to_cstring(ctx_.tenant_id_), to_cstring(ctx_.arg_.ls_id_), ctx_.arg_.type_, task_id_str))) {
    LOG_WARN("failed to fill comment", K(ret), K(ctx_));
  }
  return ret;
}

int ObLSCompleteMigrationDagNet::fill_dag_net_key(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ls complete migration dag net do not init", K(ret));
  } else if (OB_FAIL(databuff_printf(buf, buf_len,
      "ObLSCompleteMigrationDagNet: ls_id = %s, migration_type = %s",
      to_cstring(ctx_.arg_.ls_id_), ObMigrationOpType::get_str(ctx_.arg_.type_)))) {
    LOG_WARN("failed to fill comment", K(ret), K(ctx_));
  }
  return ret;
}

int ObLSCompleteMigrationDagNet::clear_dag_net_ctx()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  int32_t result = OB_SUCCESS;
  ObLSMigrationHandler *ls_migration_handler = nullptr;
  ObLSHandle ls_handle;
  LOG_INFO("start clear dag net ctx", K(ctx_));

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ls complete migration dag net do not init", K(ret));
  } else if (OB_FAIL(ObStorageHADagUtils::get_ls(ctx_.arg_.ls_id_, ls_handle))) {
    LOG_WARN("failed to get ls", K(ret), K(ctx_));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_SYS;
    LOG_ERROR("ls should not be NULL", K(ret), K(ctx_));
  } else {
    if (OB_SUCCESS != (tmp_ret = update_migration_status_(ls))) {
      LOG_WARN("failed to update migration status", K(tmp_ret), K(ret), K(ctx_));
    }

    if (OB_ISNULL(ls_migration_handler = ls->get_ls_migration_handler())) {
      tmp_ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ls migration handler should not be NULL", K(tmp_ret), K(ctx_));
    } else if (OB_FAIL(ctx_.get_result(result))) {
      LOG_WARN("failed to get ls complate migration ctx result", K(ret), K(ctx_));
    } else if (OB_FAIL(ls_migration_handler->switch_next_stage(result))) {
      LOG_WARN("failed to report result", K(ret), K(result), K(ctx_));
    }

    ctx_.finish_ts_ = ObTimeUtil::current_time();
    const int64_t cost_ts = ctx_.finish_ts_ - ctx_.start_ts_;
    FLOG_INFO("finish ls complete migration dag net", "ls id", ctx_.arg_.ls_id_, "type", ctx_.arg_.type_, K(cost_ts));
  }
  return ret;
}

int ObLSCompleteMigrationDagNet::update_migration_status_(ObLS *ls)
{
  int ret = OB_SUCCESS;
  bool is_finish = false;
  static const int64_t UPDATE_MIGRATION_STATUS_INTERVAL_MS = 100 * 1000; //100ms
  ObTenantDagScheduler *scheduler = nullptr;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("migration dag net do not init", K(ret));
  } else if (OB_ISNULL(ls)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("update migration status get invalid argument", K(ret), KP(ls));
  } else if (OB_ISNULL(scheduler = MTL(ObTenantDagScheduler*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get ObTenantDagScheduler from MTL", K(ret));
  } else {
    while (!is_finish) {
      ObMigrationStatus current_migration_status = ObMigrationStatus::OB_MIGRATION_STATUS_MAX;
      ObMigrationStatus new_migration_status = ObMigrationStatus::OB_MIGRATION_STATUS_MAX;

      if (ls->is_stopped()) {
        ret = OB_NOT_RUNNING;
        LOG_WARN("ls is not running, stop migration dag net", K(ret), K(ctx_));
        break;
      } else if (scheduler->has_set_stop()) {
        ret = OB_SERVER_IS_STOPPING;
        LOG_WARN("tenant dag scheduler has set stop, stop migration dag net", K(ret), K(ctx_));
        break;
      } else {
        // TODO: muwei should not do this before ls create finished.
        if (OB_FAIL(ls->get_migration_status(current_migration_status))) {
          LOG_WARN("failed to get migration status", K(ret), K(ctx_));
        } else if (ctx_.is_failed()) {
          if (ObMigrationOpType::REBUILD_LS_OP == ctx_.arg_.type_) {
            if (ObMigrationStatus::OB_MIGRATION_STATUS_REBUILD != current_migration_status) {
              ret = OB_ERR_UNEXPECTED;
              LOG_WARN("migration status is unexpected", K(ret), K(current_migration_status), K(ctx_));
            } else {
              new_migration_status = current_migration_status;
            }
          } else if (OB_FAIL(ObMigrationStatusHelper::trans_fail_status(current_migration_status, new_migration_status))) {
            LOG_WARN("failed to trans fail status", K(ret), K(current_migration_status), K(new_migration_status));
          }
        } else {
          if (ObMigrationOpType::REBUILD_LS_OP == ctx_.arg_.type_
              && OB_FAIL(ls->clear_saved_info())) {
            LOG_WARN("failed to clear ls saved info", K(ret), KPC(ls));
          } else {
            new_migration_status = ObMigrationStatus::OB_MIGRATION_STATUS_NONE;
          }
        }

        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(ls->set_migration_status(new_migration_status, ctx_.rebuild_seq_))) {
          LOG_WARN("failed to set migration status", K(ret), K(current_migration_status), K(new_migration_status), K(ctx_));
        } else {
          is_finish = true;
        }
      }

      if (OB_FAIL(ret)) {
        ob_usleep(UPDATE_MIGRATION_STATUS_INTERVAL_MS);
      }
    }
  }
  return ret;
}

int ObLSCompleteMigrationDagNet::deal_with_cancel()
{
  int ret = OB_SUCCESS;
  const int32_t result = OB_CANCELED;
  const bool need_retry = false;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("ls complete migration dag net do not init", K(ret));
  } else if (OB_FAIL(ctx_.set_result(result, need_retry))) {
    LOG_WARN("failed to set result", K(ret), KPC(this));
  }
  return ret;
}

/******************ObCompleteMigrationDag*********************/
ObCompleteMigrationDag::ObCompleteMigrationDag(const ObStorageHADagType sub_type)
  : ObStorageHADag(ObDagType::DAG_TYPE_MIGRATE, sub_type)
{
}

ObCompleteMigrationDag::~ObCompleteMigrationDag()
{
}

bool ObCompleteMigrationDag::operator == (const ObIDag &other) const
{
  bool is_same = true;
  if (this == &other) {
    // same
  } else if (get_type() != other.get_type()) {
    is_same = false;
  } else {
    const ObStorageHADag &ha_dag = static_cast<const ObStorageHADag&>(other);
    if (ha_dag.get_sub_type() != sub_type_) {
      is_same = false;
    } else if (OB_ISNULL(ha_dag_net_ctx_) || OB_ISNULL(ha_dag.get_ha_dag_net_ctx())) {
      is_same = false;
      LOG_ERROR("complete migration ctx should not be NULL", KP(ha_dag_net_ctx_), KP(ha_dag.get_ha_dag_net_ctx()));
    } else if (ha_dag_net_ctx_->get_dag_net_ctx_type() != ha_dag.get_ha_dag_net_ctx()->get_dag_net_ctx_type()) {
      is_same = false;
    } else {
      ObLSCompleteMigrationCtx *self_ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag_net_ctx_);
      ObLSCompleteMigrationCtx *other_ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag.get_ha_dag_net_ctx());
      if (self_ctx->arg_.ls_id_ != other_ctx->arg_.ls_id_) {
        is_same = false;
      }
    }
  }
  return is_same;
}

int64_t ObCompleteMigrationDag::hash() const
{
  int ret = OB_SUCCESS;
  int64_t hash_value = 0;
  if (OB_ISNULL(ha_dag_net_ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("complete migration ctx should not be NULL", KP(ha_dag_net_ctx_));
  } else if (ObIHADagNetCtx::LS_COMPLETE_MIGRATION != ha_dag_net_ctx_->get_dag_net_ctx_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_ERROR("ha dag net ctx type is unexpected", K(ret), KPC(ha_dag_net_ctx_));
  } else {
    ObLSCompleteMigrationCtx *self_ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag_net_ctx_);
    hash_value = common::murmurhash(
        &self_ctx->arg_.ls_id_, sizeof(self_ctx->arg_.ls_id_), hash_value);
    hash_value = common::murmurhash(
        &sub_type_, sizeof(sub_type_), hash_value);
  }
  return hash_value;
}

int ObCompleteMigrationDag::prepare_ctx(share::ObIDagNet *dag_net)
{
  int ret = OB_SUCCESS;
  ObLSCompleteMigrationDagNet *complete_dag_net = nullptr;
  ObLSCompleteMigrationCtx *self_ctx = nullptr;

  if (OB_ISNULL(dag_net)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag net should not be NULL", K(ret), KP(dag_net));
  } else if (ObDagNetType::DAG_NET_TYPE_COMPLETE_MIGARTION != dag_net->get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag net type is unexpected", K(ret), KPC(dag_net));
  } else if (FALSE_IT(complete_dag_net = static_cast<ObLSCompleteMigrationDagNet*>(dag_net))) {
  } else if (FALSE_IT(self_ctx = complete_dag_net->get_ctx())) {
  } else if (OB_ISNULL(self_ctx)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("complete migration ctx should not be NULL", K(ret), KP(self_ctx));
  } else {
    ha_dag_net_ctx_ = self_ctx;
  }
  return ret;
}

/******************ObInitialCompleteMigrationDag*********************/
ObInitialCompleteMigrationDag::ObInitialCompleteMigrationDag()
  : ObCompleteMigrationDag(ObStorageHADagType::INITIAL_COMPLETE_MIGRATION_DAG),
    is_inited_(false)
{
}

ObInitialCompleteMigrationDag::~ObInitialCompleteMigrationDag()
{
}

int ObInitialCompleteMigrationDag::fill_dag_key(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  ObLSCompleteMigrationCtx *self_ctx = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("initial prepare migration dag do not init", K(ret));
  } else if (ObIHADagNetCtx::LS_COMPLETE_MIGRATION != ha_dag_net_ctx_->get_dag_net_ctx_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ha dag net ctx type is unexpected", K(ret), KPC(ha_dag_net_ctx_));
  } else if (FALSE_IT(self_ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag_net_ctx_))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len,
         "ObInitialCompleteMigrationDag: ls_id = %s, migration_type = %s",
         to_cstring(self_ctx->arg_.ls_id_), ObMigrationOpType::get_str(self_ctx->arg_.type_)))) {
    LOG_WARN("failed to fill comment", K(ret), K(*self_ctx));
  }
  return ret;
}

int ObInitialCompleteMigrationDag::init(ObIDagNet *dag_net)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("initial complete migration dag init twice", K(ret));
  } else if (OB_ISNULL(dag_net)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("init initial complete migration dag get invalid argument", K(ret), KP(dag_net));
  } else if (OB_FAIL(ObCompleteMigrationDag::prepare_ctx(dag_net))) {
    LOG_WARN("failed to prepare ctx", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObInitialCompleteMigrationDag::create_first_task()
{
  int ret = OB_SUCCESS;
  ObInitialCompleteMigrationTask *task = NULL;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("initial complete migration dag do not init", K(ret));
  } else if (OB_FAIL(alloc_task(task))) {
    LOG_WARN("Fail to alloc task", K(ret));
  } else if (OB_FAIL(task->init())) {
    LOG_WARN("failed to init initial complete migration task", K(ret), KPC(ha_dag_net_ctx_));
  } else if (OB_FAIL(add_task(*task))) {
    LOG_WARN("Fail to add task", K(ret));
  } else {
    LOG_DEBUG("success to create first task", K(ret), KPC(this));
  }
  return ret;
}

int ObInitialCompleteMigrationDag::fill_comment(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  ObLSCompleteMigrationCtx *ctx = nullptr;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("initial complete migration dag do not init", K(ret));
  } else if (NULL == buf || buf_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid args", K(ret), KP(buf), K(buf_len));
  } else if (FALSE_IT(ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag_net_ctx_))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len,
      "ObInitialCompleteMigrationDag : dag_net_task_id = %s, tenant_id = %s, ls_id = %s, migration_type = %s, "
      "src = %s, dest = %s", to_cstring(ctx->task_id_), to_cstring(ctx->tenant_id_), to_cstring(ctx->arg_.ls_id_),
      ObMigrationOpType::get_str(ctx->arg_.type_), to_cstring(ctx->arg_.src_.get_server()),
      to_cstring(ctx->arg_.dst_.get_server())))) {
    LOG_WARN("failed to fill comment", K(ret), KPC(ctx));
  }
  return ret;
}

/******************ObInitialCompleteMigrationTask*********************/
ObInitialCompleteMigrationTask::ObInitialCompleteMigrationTask()
  : ObITask(TASK_TYPE_MIGRATE_PREPARE),
    is_inited_(false),
    ctx_(nullptr),
    dag_net_(nullptr)
{
}

ObInitialCompleteMigrationTask::~ObInitialCompleteMigrationTask()
{
}

int ObInitialCompleteMigrationTask::init()
{
  int ret = OB_SUCCESS;
  ObIDagNet *dag_net = nullptr;
  ObLSCompleteMigrationDagNet *prepare_dag_net = nullptr;

  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("initial prepare migration task init twice", K(ret));
  } else if (FALSE_IT(dag_net = this->get_dag()->get_dag_net())) {
  } else if (OB_ISNULL(dag_net)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag net should not be NULL", K(ret), KP(dag_net));
  } else if (ObDagNetType::DAG_NET_TYPE_COMPLETE_MIGARTION != dag_net->get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag net type is unexpected", K(ret), KPC(dag_net));
  } else if (FALSE_IT(prepare_dag_net = static_cast<ObLSCompleteMigrationDagNet*>(dag_net))) {
  } else {
    ctx_ = prepare_dag_net->get_ctx();
    dag_net_ = dag_net;
    is_inited_ = true;
    LOG_INFO("succeed init initial complete migration task", "ls id", ctx_->arg_.ls_id_,
        "dag_id", *ObCurTraceId::get_trace_id(), "dag_net_id", ctx_->task_id_);
  }
  return ret;
}

int ObInitialCompleteMigrationTask::process()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("initial complete migration task do not init", K(ret));
  } else if (OB_FAIL(generate_migration_dags_())) {
    LOG_WARN("failed to generate migration dags", K(ret), K(*ctx_));
  }
  if (OB_SUCCESS != (tmp_ret = record_server_event_())) {
    LOG_WARN("failed to record server event", K(tmp_ret), K(ret));
  }

  if (OB_FAIL(ret)) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = ObStorageHADagUtils::deal_with_fo(ret, this->get_dag()))) {
      LOG_WARN("failed to deal with fo", K(ret), K(tmp_ret), KPC(ctx_));
    }
  }

  return ret;
}

int ObInitialCompleteMigrationTask::record_server_event_()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx should not be null", K(ret));
  } else {
    SERVER_EVENT_ADD("storage_ha", "initial_complete_migration_task",
        "tenant_id", ctx_->tenant_id_,
        "ls_id",ctx_->arg_.ls_id_.id(),
        "src", ctx_->arg_.src_.get_server(),
        "dst", ctx_->arg_.dst_.get_server(),
        "task_id", ctx_->task_id_,
        "is_failed", ctx_->is_failed(),
        ObMigrationOpType::get_str(ctx_->arg_.type_));
  }
  return ret;
}

int ObInitialCompleteMigrationTask::generate_migration_dags_()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObStartCompleteMigrationDag *start_complete_dag = nullptr;
  ObFinishCompleteMigrationDag *finish_complete_dag = nullptr;
  ObTenantDagScheduler *scheduler = nullptr;
  ObInitialCompleteMigrationDag *initial_complete_migration_dag = nullptr;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("initial prepare migration task do not init", K(ret));
  } else if (OB_ISNULL(scheduler = MTL(ObTenantDagScheduler*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get ObTenantDagScheduler from MTL", K(ret));
  } else if (OB_ISNULL(initial_complete_migration_dag = static_cast<ObInitialCompleteMigrationDag *>(this->get_dag()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("initial complete migration dag should not be NULL", K(ret), KP(initial_complete_migration_dag));
  } else {
    if (OB_FAIL(scheduler->alloc_dag(start_complete_dag))) {
      LOG_WARN("failed to alloc start complete migration dag ", K(ret));
    } else if (OB_FAIL(scheduler->alloc_dag(finish_complete_dag))) {
      LOG_WARN("failed to alloc finish complete migration dag", K(ret));
    } else if (OB_FAIL(start_complete_dag->init(dag_net_))) {
      LOG_WARN("failed to init start complete migration dag", K(ret));
    } else if (OB_FAIL(finish_complete_dag->init(dag_net_))) {
      LOG_WARN("failed to init finish complete migration dag", K(ret));
    } else if (OB_FAIL(this->get_dag()->add_child(*start_complete_dag))) {
      LOG_WARN("failed to add start complete dag", K(ret), KPC(start_complete_dag));
    } else if (OB_FAIL(start_complete_dag->create_first_task())) {
      LOG_WARN("failed to create first task", K(ret));
    } else if (OB_FAIL(start_complete_dag->add_child(*finish_complete_dag))) {
      LOG_WARN("failed to add finish complete migration dag as child", K(ret));
    } else if (OB_FAIL(finish_complete_dag->create_first_task())) {
      LOG_WARN("failed to create first task", K(ret));
    } else if (OB_FAIL(scheduler->add_dag(finish_complete_dag))) {
      LOG_WARN("failed to add finish complete migration dag", K(ret), K(*finish_complete_dag));
      if (OB_SIZE_OVERFLOW != ret && OB_EAGAIN != ret) {
        LOG_WARN("Fail to add task", K(ret));
        ret = OB_EAGAIN;
      }
    } else if (OB_FAIL(scheduler->add_dag(start_complete_dag))) {
      LOG_WARN("failed to add dag", K(ret), K(*start_complete_dag));
      if (OB_SIZE_OVERFLOW != ret && OB_EAGAIN != ret) {
        LOG_WARN("Fail to add task", K(ret));
        ret = OB_EAGAIN;
      }

      if (OB_SUCCESS != (tmp_ret = scheduler->cancel_dag(finish_complete_dag, initial_complete_migration_dag))) {
        LOG_WARN("failed to cancel ha dag", K(tmp_ret), KPC(initial_complete_migration_dag));
      } else {
        finish_complete_dag = nullptr;
      }
      finish_complete_dag = nullptr;
    } else {
      LOG_INFO("succeed to schedule start complete migration dag", K(*start_complete_dag));
      start_complete_dag = nullptr;
      finish_complete_dag = nullptr;
    }

    if (OB_FAIL(ret)) {
      if (OB_NOT_NULL(scheduler) && OB_NOT_NULL(start_complete_dag)) {
        scheduler->free_dag(*start_complete_dag, initial_complete_migration_dag);
        start_complete_dag = nullptr;
      }

      if (OB_NOT_NULL(scheduler) && OB_NOT_NULL(finish_complete_dag)) {
        scheduler->free_dag(*finish_complete_dag, initial_complete_migration_dag);
        finish_complete_dag = nullptr;
      }
      if (OB_SUCCESS != (tmp_ret = ctx_->set_result(ret, true /*allow_retry*/))) {
        LOG_WARN("failed to set complete migration result", K(ret), K(tmp_ret), K(*ctx_));
      }
    }
  }
  return ret;
}

/******************ObStartCompleteMigrationDag*********************/
ObStartCompleteMigrationDag::ObStartCompleteMigrationDag()
  : ObCompleteMigrationDag(ObStorageHADagType::START_COMPLETE_MIGRATION_DAG),
    is_inited_(false)
{
}

ObStartCompleteMigrationDag::~ObStartCompleteMigrationDag()
{
}

int ObStartCompleteMigrationDag::fill_dag_key(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  ObLSCompleteMigrationCtx *self_ctx = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("initial prepare migration dag do not init", K(ret));
  } else if (ObIHADagNetCtx::LS_COMPLETE_MIGRATION != ha_dag_net_ctx_->get_dag_net_ctx_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ha dag net ctx type is unexpected", K(ret), KPC(ha_dag_net_ctx_));
  } else if (FALSE_IT(self_ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag_net_ctx_))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len,
         "ObStartPrepareMigrationDag: ls_id = %s, migration_type = %s",
         to_cstring(self_ctx->arg_.ls_id_), ObMigrationOpType::get_str(self_ctx->arg_.type_)))) {
    LOG_WARN("failed to fill comment", K(ret), K(*self_ctx));
  }
  return ret;
}

int ObStartCompleteMigrationDag::init(ObIDagNet *dag_net)
{
  int ret = OB_SUCCESS;

  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("start complete migration dag init twice", K(ret));
  } else if (OB_ISNULL(dag_net)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("init start complete migration dag get invalid argument", K(ret), KP(dag_net));
  } else if (OB_FAIL(prepare_ctx(dag_net))) {
    LOG_WARN("failed to prepare ctx", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObStartCompleteMigrationDag::create_first_task()
{
  int ret = OB_SUCCESS;
  ObStartCompleteMigrationTask *task = NULL;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration dag do not init", K(ret));
  } else if (OB_FAIL(alloc_task(task))) {
    LOG_WARN("Fail to alloc task", K(ret));
  } else if (OB_FAIL(task->init())) {
    LOG_WARN("failed to init start complete migration task", K(ret), KPC(ha_dag_net_ctx_));
  } else if (OB_FAIL(add_task(*task))) {
    LOG_WARN("Fail to add task", K(ret));
  } else {
    LOG_DEBUG("success to create first task", K(ret), KPC(this));
  }
  return ret;
}

int ObStartCompleteMigrationDag::fill_comment(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  ObLSCompleteMigrationCtx *ctx = nullptr;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration dag do not init", K(ret));
  } else if (NULL == buf || buf_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid args", K(ret), KP(buf), K(buf_len));
  } else if (FALSE_IT(ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag_net_ctx_))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len,
      "ObStartCompleteMigrationDag : dag_net_task_id = %s, tenant_id = %s, ls_id = %s, migration_type = %s, "
      "src = %s, dest = %s", to_cstring(ctx->task_id_), to_cstring(ctx->tenant_id_), to_cstring(ctx->arg_.ls_id_),
      ObMigrationOpType::get_str(ctx->arg_.type_), to_cstring(ctx->arg_.src_.get_server()),
      to_cstring(ctx->arg_.dst_.get_server())))) {
    LOG_WARN("failed to fill comment", K(ret), KPC(ctx));
  }
  return ret;
}

/******************ObStartCompleteMigrationTask*********************/
ObStartCompleteMigrationTask::ObStartCompleteMigrationTask()
  : ObITask(TASK_TYPE_MIGRATE_PREPARE),
    is_inited_(false),
    ls_handle_(),
    ctx_(nullptr),
    log_sync_scn_(0),
    max_minor_end_scn_(0)
{
}

ObStartCompleteMigrationTask::~ObStartCompleteMigrationTask()
{
}

int ObStartCompleteMigrationTask::init()
{
  int ret = OB_SUCCESS;
  ObIDagNet *dag_net = nullptr;
  ObLSCompleteMigrationDagNet *complete_dag_net = nullptr;

  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("start complete migration task init twice", K(ret));
  } else if (FALSE_IT(dag_net = this->get_dag()->get_dag_net())) {
  } else if (OB_ISNULL(dag_net)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag net should not be NULL", K(ret), KP(dag_net));
  } else if (ObDagNetType::DAG_NET_TYPE_COMPLETE_MIGARTION != dag_net->get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag net type is unexpected", K(ret), KPC(dag_net));
  } else if (OB_ISNULL(complete_dag_net = static_cast<ObLSCompleteMigrationDagNet*>(dag_net))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("complete dag net should not be NULL", K(ret), KP(complete_dag_net));
  } else if (OB_FAIL(ObStorageHADagUtils::get_ls(complete_dag_net->get_ctx()->arg_.ls_id_, ls_handle_))) {
    LOG_WARN("failed to get ls", K(ret), KPC(dag_net));
  } else {
    ctx_ = complete_dag_net->get_ctx();
    is_inited_ = true;
    LOG_INFO("succeed init start complete migration task", "ls id", ctx_->arg_.ls_id_,
        "dag_id", *ObCurTraceId::get_trace_id(), "dag_net_id", ctx_->task_id_);
  }
  return ret;
}

int ObStartCompleteMigrationTask::process()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration task do not init", K(ret));
  } else if (ctx_->is_failed()) {
    //do nothing
  } else if (OB_FAIL(wait_log_sync_())) {
    LOG_WARN("failed wait log sync", K(ret), KPC(ctx_));
  } else if (OB_FAIL(wait_log_replay_sync_())) {
    LOG_WARN("failed to wait log replay sync", K(ret), KPC(ctx_));
  } else if (OB_FAIL(check_all_tablet_ready_())) {
    LOG_WARN("failed to check all tablet ready", K(ret), KPC(ctx_));
  } else if (OB_FAIL(wait_trans_tablet_explain_data_())) {
    LOG_WARN("failed to wait log replay sync", K(ret), KPC(ctx_));
  } else if (OB_FAIL(wait_log_replay_to_max_minor_end_scn_())) {
    LOG_WARN("failed to wait log replay to max minor end scn", K(ret), KPC(ctx_));
  } else if (OB_FAIL(update_ls_migration_status_hold_())) {
    LOG_WARN("failed to update ls migration status hold", K(ret), KPC(ctx_));
  } else if (OB_FAIL(change_member_list_())) {
    LOG_WARN("failed to change member list", K(ret), KPC(ctx_));
  }
  if (OB_SUCCESS != (tmp_ret = record_server_event_())) {
    LOG_WARN("failed to record server event", K(tmp_ret), K(ret));
  }

  if (OB_FAIL(ret)) {
    int tmp_ret = OB_SUCCESS;
    if (OB_SUCCESS != (tmp_ret = ObStorageHADagUtils::deal_with_fo(ret, this->get_dag()))) {
      LOG_WARN("failed to deal with fo", K(ret), K(tmp_ret), KPC(ctx_));
    }
  }
  return ret;
}

int ObStartCompleteMigrationTask::wait_log_sync_()
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  bool is_log_sync = false;
  bool is_need_rebuild = false;
  bool is_cancel = false;
  int64_t last_end_log_ts_ns = 0;
  int64_t current_end_log_ts_ns = 0;
  const int64_t OB_CHECK_LOG_SYNC_INTERVAL = 200 * 1000; // 200ms
  const int64_t CLOG_IN_SYNC_DELAY_TIMEOUT = 30 * 60 * 1000 * 1000; // 30 min
  bool need_wait = true;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("migration finish task do not init", K(ret));
  } else if (OB_ISNULL(ls = ls_handle_.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls should not be NULL", K(ret), KP(ls), KPC(ctx_));
  } else if (OB_FAIL(check_need_wait_(ls, need_wait))) {
    LOG_WARN("failed to check need wait log sync", K(ret), KPC(ctx_));
  } else if (!need_wait) {
    FLOG_INFO("no need wait log sync", KPC(ctx_));
  } else {
    const int64_t wait_replay_start_ts = ObTimeUtility::current_time();
    int64_t current_ts = 0;
    int64_t last_wait_replay_ts = ObTimeUtility::current_time();
    while (OB_SUCC(ret) && !is_log_sync) {
      if (ctx_->is_failed()) {
        ret = OB_CANCELED;
        STORAGE_LOG(WARN, "group task has error, cancel subtask", K(ret));
      } else if (ls->is_stopped()) {
        ret = OB_NOT_RUNNING;
        LOG_WARN("ls is not running, stop migration dag net", K(ret), K(ctx_));
      } else if (OB_FAIL(SYS_TASK_STATUS_MGR.is_task_cancel(get_dag()->get_dag_id(), is_cancel))) {
        STORAGE_LOG(ERROR, "failed to check is task canceled", K(ret), K(*this));
      } else if (is_cancel) {
        ret = OB_CANCELED;
        STORAGE_LOG(WARN, "task is cancelled", K(ret), K(*this));
      } else if (OB_FAIL(ls->is_in_sync(is_log_sync, is_need_rebuild))) {
        LOG_WARN("failed to check is in sync", K(ret), KPC(ctx_));
      }

      if (OB_FAIL(ret)) {
      } else if (is_log_sync) {
        if (OB_FAIL(ls->get_end_ts_ns(log_sync_scn_))) {
          LOG_WARN("failed to get end ts ns", K(ret), KPC(ctx_));
        } else {
          const int64_t cost_ts = ObTimeUtility::current_time() - wait_replay_start_ts;
          LOG_INFO("log is sync, stop wait_log_sync", "arg", ctx_->arg_, K(cost_ts));
        }
      } else if (is_need_rebuild) {
        const int64_t cost_ts = ObTimeUtility::current_time() - wait_replay_start_ts;
        ret = OB_LOG_NOT_SYNC;
        LOG_WARN("log is not sync", K(ret), KPC(ctx_), K(cost_ts));
      } else if (OB_FAIL(ls->get_end_ts_ns(current_end_log_ts_ns))) {
        LOG_WARN("failed to get end ts ns", K(ret), KPC(ctx_));
      } else {
        bool is_timeout = false;
        if (REACH_TENANT_TIME_INTERVAL(60 * 1000 * 1000)) {
          LOG_INFO("log is not sync, retry next loop", "arg", ctx_->arg_);
        }

        if (current_end_log_ts_ns == last_end_log_ts_ns) {
          const int64_t current_ts = ObTimeUtility::current_time();
          if ((current_ts - last_wait_replay_ts) > CLOG_IN_SYNC_DELAY_TIMEOUT) {
            is_timeout = true;
          }

          if (is_timeout) {
            if (OB_FAIL(ctx_->set_result(OB_LOG_NOT_SYNC, true /*allow_retry*/))) {
                LOG_WARN("failed to set result", K(ret), KPC(ctx_));
            } else {
              ret = OB_LOG_NOT_SYNC;
              STORAGE_LOG(WARN, "failed to check log replay sync. timeout, stop migration task",
                  K(ret), K(*ctx_), K(CLOG_IN_SYNC_DELAY_TIMEOUT), K(wait_replay_start_ts),
                  K(current_ts), K(current_end_log_ts_ns));
            }
          }
        } else if (last_end_log_ts_ns > current_end_log_ts_ns) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("last end log ts should not smaller than current end log ts", K(ret), K(last_end_log_ts_ns), K(current_end_log_ts_ns));
        } else {
          last_end_log_ts_ns = current_end_log_ts_ns;
          last_wait_replay_ts = ObTimeUtility::current_time();
        }

        if (OB_SUCC(ret)) {
          ob_usleep(OB_CHECK_LOG_SYNC_INTERVAL);
        }
      }
    }
    if (OB_SUCC(ret) && !is_log_sync) {
      const int64_t cost_ts = ObTimeUtility::current_time() - wait_replay_start_ts;
      ret = OB_LOG_NOT_SYNC;
      LOG_WARN("log is not sync", K(ret), KPC(ctx_), K(cost_ts));
    }
  }
  return ret;
}

int ObStartCompleteMigrationTask::wait_log_replay_sync_()
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  logservice::ObLogService *log_service = nullptr;
  bool wait_log_replay_success = false;
  bool is_cancel = false;
  int64_t current_replay_log_ts_ns = 0;
  int64_t last_replay_log_ts_ns = 0;
  const int64_t OB_CHECK_LOG_REPLAY_INTERVAL = 200 * 1000; // 200ms
  const int64_t CLOG_IN_REPLAY_DELAY_TIMEOUT = 30 * 60 * 1000 * 1000L; // 30 min
  bool need_wait = false;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration task do not init", K(ret));
  } else if (OB_ISNULL(ls = ls_handle_.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls should not be NULL", K(ret), KP(ls), KPC(ctx_));
  } else if (OB_FAIL(check_need_wait_(ls, need_wait))) {
    LOG_WARN("failed to check need wait log replay", K(ret), KPC(ctx_));
  } else if (!need_wait) {
    FLOG_INFO("no need wait replay log sync", KPC(ctx_));
  } else {
    const int64_t wait_replay_start_ts = ObTimeUtility::current_time();
    int64_t last_replay_ts = 0;
    int64_t current_ts = 0;
    while (OB_SUCC(ret) && !wait_log_replay_success) {
      if (ctx_->is_failed()) {
        ret = OB_CANCELED;
        STORAGE_LOG(WARN, "group task has error, cancel subtask", K(ret));
      } else if (ls->is_stopped()) {
        ret = OB_NOT_RUNNING;
        LOG_WARN("ls is not running, stop migration dag net", K(ret), K(ctx_));
      } else if (OB_FAIL(SYS_TASK_STATUS_MGR.is_task_cancel(get_dag()->get_dag_id(), is_cancel))) {
        STORAGE_LOG(ERROR, "failed to check is task canceled", K(ret), K(*this));
      } else if (is_cancel) {
        ret = OB_CANCELED;
        STORAGE_LOG(WARN, "task is cancelled", K(ret), K(*this));
      } else if (OB_FAIL(ls->get_max_decided_log_ts_ns(current_replay_log_ts_ns))) {
        LOG_WARN("failed to get current replay log ts", K(ret), KPC(ctx_));
      } else if (current_replay_log_ts_ns + IS_REPLAY_DONE_THRESHOLD_NS >= log_sync_scn_) {
        wait_log_replay_success = true;
        const int64_t cost_ts = ObTimeUtility::current_time() - wait_replay_start_ts;
        LOG_INFO("wait replay log ts ns success, stop wait", "arg", ctx_->arg_, K(cost_ts));
      } else {
        current_ts = ObTimeUtility::current_time();
        bool is_timeout = false;
        if (REACH_TENANT_TIME_INTERVAL(60 * 1000 * 1000)) {
          LOG_INFO("replay log is not sync, retry next loop", "arg", ctx_->arg_,
              "current_replay_log_ts_ns", current_replay_log_ts_ns,
              "log_sync_scn", log_sync_scn_);
        }

        if (current_replay_log_ts_ns == last_replay_log_ts_ns) {
          if (current_ts - last_replay_ts > CLOG_IN_REPLAY_DELAY_TIMEOUT) {
            is_timeout = true;
          }
          if (is_timeout) {
            if (OB_FAIL(ctx_->set_result(OB_WAIT_REPLAY_TIMEOUT, true /*allow_retry*/))) {
                LOG_WARN("failed to set result", K(ret), KPC(ctx_));
            } else {
              ret = OB_WAIT_REPLAY_TIMEOUT;
              STORAGE_LOG(WARN, "failed to check log replay sync. timeout, stop migration task",
                  K(ret), K(*ctx_), K(CLOG_IN_REPLAY_DELAY_TIMEOUT), K(wait_replay_start_ts),
                  K(current_ts), K(current_replay_log_ts_ns));
            }
          }
        } else if (last_replay_log_ts_ns > current_replay_log_ts_ns) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("last end log ts should not smaller than current end log ts", K(ret),
              K(last_replay_log_ts_ns), K(current_replay_log_ts_ns));
        } else {
          last_replay_log_ts_ns = current_replay_log_ts_ns;
          last_replay_ts = current_ts;
        }

        if (OB_SUCC(ret)) {
          ob_usleep(OB_CHECK_LOG_REPLAY_INTERVAL);
        }
      }
    }

    if (OB_SUCC(ret) && !wait_log_replay_success) {
      const int64_t cost_ts = ObTimeUtility::current_time() - wait_replay_start_ts;
      ret = OB_LOG_NOT_SYNC;
      LOG_WARN("log is not sync", K(ret), KPC(ctx_), K(cost_ts));
    }
  }
  return ret;
}

int ObStartCompleteMigrationTask::wait_trans_tablet_explain_data_()
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  logservice::ObLogService *log_service = nullptr;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start prepare migration task do not init", K(ret));
  } else if (OB_ISNULL(ls = ls_handle_.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls should not be NULL", K(ret), KPC(ctx_));
  } else {
    //TODO(muwei.ym) wait log replay to max tablet minor sstable log ts
  }
  return ret;
}

int ObStartCompleteMigrationTask::change_member_list_()
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  const int64_t start_ts = ObTimeUtility::current_time();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration task do not init", K(ret));
  } else if (OB_ISNULL(ls = ls_handle_.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to change member list", K(ret), KP(ls));
  } else if (ObMigrationOpType::ADD_LS_OP != ctx_->arg_.type_
      && ObMigrationOpType::MIGRATE_LS_OP != ctx_->arg_.type_) {
    //do nothing
  } else {
    if (ObMigrationOpType::ADD_LS_OP == ctx_->arg_.type_) {
      const int64_t change_member_list_timeout_us = GCONF.sys_bkgd_migration_change_member_list_timeout;
      if (OB_FAIL(ls->add_member(ctx_->arg_.dst_, ctx_->arg_.paxos_replica_number_, change_member_list_timeout_us))) {
        LOG_WARN("failed to add member", K(ret), KPC(ctx_));
      }
    } else if (ObMigrationOpType::MIGRATE_LS_OP == ctx_->arg_.type_) {
      const int64_t change_member_list_timeout_us = GCONF.sys_bkgd_migration_change_member_list_timeout;
      if (OB_FAIL(ls->replace_member(ctx_->arg_.dst_, ctx_->arg_.src_, change_member_list_timeout_us))) {
        LOG_WARN("failed to repalce member", K(ret), KPC(ctx_));
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("change member list get invalid type", K(ret), KPC(ctx_));
    }

    if (OB_SUCC(ret)) {
      const int64_t cost_ts = ObTimeUtility::current_time() - start_ts;
      LOG_INFO("succeed change member list", "cost", cost_ts, "tenant_id", ctx_->tenant_id_, "ls_id", ctx_->arg_.ls_id_);
    }
  }
  return ret;
}

int ObStartCompleteMigrationTask::check_need_wait_(
    ObLS *ls,
    bool &need_wait)
{
  int ret = OB_SUCCESS;
  need_wait = true;
  ObLSRestoreStatus ls_restore_status;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration task do not init", K(ret));
  } else if (OB_ISNULL(ls)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("check need wait log sync get invalid argument", K(ret), KP(ls));
  } else if (OB_FAIL(ls->get_restore_status(ls_restore_status))) {
    LOG_WARN("failed to get restore status", K(ret), KPC(ctx_));
  } else if (ls_restore_status.is_in_restore()) {
    need_wait = false;
  } else if (ObMigrationOpType::REBUILD_LS_OP == ctx_->arg_.type_) {
    need_wait = false;
  } else if (ObMigrationOpType::ADD_LS_OP == ctx_->arg_.type_
      || ObMigrationOpType::MIGRATE_LS_OP == ctx_->arg_.type_) {
    need_wait = true;
  } else if (ObMigrationOpType::CHANGE_LS_OP == ctx_->arg_.type_) {
    if (!ObReplicaTypeCheck::is_replica_with_ssstore(ls->get_replica_type())
        && ObReplicaTypeCheck::is_full_replica(ctx_->arg_.dst_.get_replica_type())) {
      need_wait = true;
    }
  }
  return ret;
}

int ObStartCompleteMigrationTask::update_ls_migration_status_hold_()
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  const ObMigrationStatus hold_status = ObMigrationStatus::OB_MIGRATION_STATUS_HOLD;
  int64_t rebuild_seq = 0;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration task do not init", K(ret));
  } else if (ObMigrationOpType::REBUILD_LS_OP == ctx_->arg_.type_) {
    //do nothing
  } else if (OB_ISNULL(ls = ls_handle_.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to change member list", K(ret), KP(ls));
  } else if (OB_FAIL(ls->set_migration_status(hold_status, rebuild_seq))) {
    LOG_WARN("failed to set migration status", K(ret), KPC(ls));
  } else {
#ifdef ERRSIM
    SERVER_EVENT_SYNC_ADD("storage_ha", "update_ls_migration_status_hold",
        "tenant_id", ctx_->tenant_id_,
        "ls_id", ctx_->arg_.ls_id_.id(),
        "src", ctx_->arg_.src_.get_server(),
        "dst", ctx_->arg_.dst_.get_server(),
        "task_id", ctx_->task_id_);
#endif
    DEBUG_SYNC(AFTER_CHANGE_MIGRATION_STATUS_HOLD);
  }
  return ret;
}

int ObStartCompleteMigrationTask::check_all_tablet_ready_()
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  const int64_t check_all_tablet_start_ts = ObTimeUtility::current_time();
  const bool need_initial_state = false;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration task do not init", K(ret));
  } else if (OB_ISNULL(ls = ls_handle_.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to change member list", K(ret), KP(ls));
  } else {
    ObHALSTabletIDIterator iter(ls->get_ls_id(), need_initial_state);
    ObTabletID tablet_id;
    if (OB_FAIL(ls->get_tablet_svr()->build_tablet_iter(iter))) {
      LOG_WARN("failed to build tablet iter", K(ret), KPC(ctx_));
    } else {
      while (OB_SUCC(ret)) {
        if (OB_FAIL(iter.get_next_tablet_id(tablet_id))) {
          if (OB_ITER_END == ret) {
            ret = OB_SUCCESS;
            break;
          } else {
            LOG_WARN("failed to get tablet id", K(ret));
          }
        } else if (OB_FAIL(check_tablet_ready_(tablet_id, ls))) {
          LOG_WARN("failed to check tablet ready", K(ret), K(tablet_id), KPC(ls));
        }
      }
      LOG_INFO("check all tablet ready finish", K(ret), "ls_id", ctx_->arg_.ls_id_,
          "cost ts", ObTimeUtility::current_time() - check_all_tablet_start_ts);
    }
  }
  return ret;
}

int ObStartCompleteMigrationTask::check_tablet_ready_(
    const common::ObTabletID &tablet_id,
    ObLS *ls)
{
  int ret = OB_SUCCESS;
  const int64_t timeout_us = ObTabletCommon::NO_CHECK_GET_TABLET_TIMEOUT_US;
  const int64_t OB_CHECK_TABLET_READY_INTERVAL = 200 * 1000; // 200ms
  const int64_t OB_CHECK_TABLET_READY_TIMEOUT = 30 * 60 * 1000 * 1000L; // 30 min

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration task do not init", K(ret));
  } else if (!tablet_id.is_valid() || OB_ISNULL(ls)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("check tablet ready get invalid argument", K(ret), K(tablet_id), KP(ls));
  } else {
    const int64_t wait_tablet_start_ts = ObTimeUtility::current_time();
    bool is_cancel = false;

    while (OB_SUCC(ret)) {
      ObTabletHandle tablet_handle;
      ObTablet *tablet = nullptr;
      if (ls->is_stopped()) {
        ret = OB_NOT_RUNNING;
        LOG_WARN("ls is not running, stop migration dag net", K(ret), K(ctx_));
      } else if (OB_FAIL(SYS_TASK_STATUS_MGR.is_task_cancel(get_dag()->get_dag_id(), is_cancel))) {
        STORAGE_LOG(ERROR, "failed to check is task canceled", K(ret), K(*this));
      } else if (is_cancel) {
        ret = OB_CANCELED;
        STORAGE_LOG(WARN, "task is cancelled", K(ret), K(*this));
      } else if (OB_FAIL(ls->get_tablet(tablet_id, tablet_handle, timeout_us))) {
        if (OB_TABLET_NOT_EXIST == ret) {
          ret = OB_SUCCESS;
          break;
        } else {
          LOG_WARN("failed to get tablet", K(ret), K(tablet_id));
        }
      } else if (OB_ISNULL(tablet = tablet_handle.get_obj())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("tablet should not be NULL", K(ret), KP(tablet), K(tablet_handle), K(tablet_id));
      } else if (tablet->get_tablet_meta().ha_status_.is_data_status_complete()
          || !tablet->get_tablet_meta().ha_status_.is_restore_status_full()) {
        ObSSTableArray &minor_sstables = tablet->get_table_store().get_minor_sstables();
        if (minor_sstables.empty()) {
          max_minor_end_scn_ = MAX(max_minor_end_scn_, tablet->get_tablet_meta().clog_checkpoint_ts_);
        } else {
          max_minor_end_scn_ = MAX(max_minor_end_scn_, minor_sstables.array_[minor_sstables.count() - 1]->get_end_log_ts());
        }
        break;
      } else {
        const int64_t current_ts = ObTimeUtility::current_time();
        if (REACH_TENANT_TIME_INTERVAL(60 * 1000 * 1000)) {
          LOG_INFO("tablet not ready, retry next loop", "arg", ctx_->arg_, "tablet_id", tablet_id,
              "wait_tablet_start_ts", wait_tablet_start_ts,
              "current_ts", current_ts);
        }

        if (current_ts - wait_tablet_start_ts < OB_CHECK_TABLET_READY_TIMEOUT) {
        } else {
          if (OB_FAIL(ctx_->set_result(OB_WAIT_TABLET_READY_TIMEOUT, true /*allow_retry*/))) {
            LOG_WARN("failed to set result", K(ret), KPC(ctx_));
          } else {
            ret = OB_WAIT_TABLET_READY_TIMEOUT;
            STORAGE_LOG(WARN, "failed to check tablet ready, timeout, stop migration task",
                K(ret), K(*ctx_), KPC(tablet), K(current_ts),
                K(wait_tablet_start_ts));
          }
        }

        if (OB_SUCC(ret)) {
          ob_usleep(OB_CHECK_TABLET_READY_INTERVAL);
        }
      }
    }
  }
  return ret;
}

int ObStartCompleteMigrationTask::wait_log_replay_to_max_minor_end_scn_()
{
  int ret = OB_SUCCESS;
  ObLSHandle ls_handle;
  ObLS *ls = nullptr;
  bool is_cancel = false;
  bool need_wait = true;
  int64_t current_replay_log_ts_ns = 0;
  const int64_t OB_WAIT_LOG_REPLAY_INTERVAL = 200 * 1000; // 200ms
  const int64_t OB_WAIT_LOG_REPLAY_TIMEOUT = 30 * 60 * 1000 * 1000L; // 30 min

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("start complete migration task do not init", K(ret));
  } else if (OB_FAIL(ObStorageHADagUtils::get_ls(ctx_->arg_.ls_id_, ls_handle))) {
    LOG_WARN("failed to get ls", K(ret), KPC(ctx_));
  } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ls should not be NULL", K(ret), KP(ls), KPC(ctx_));
  } else if (OB_FAIL(check_need_wait_(ls, need_wait))) {
    LOG_WARN("failed to check need replay to max minor end scn", K(ret), KPC(ls), KPC(ctx_));
  } else if (!need_wait) {
    LOG_INFO("no need to wait ls checkpoint ts push", K(ret), KPC(ctx_));
  } else {
    const int64_t wait_replay_start_ts = ObTimeUtility::current_time();
    while (OB_SUCC(ret)) {
      if (ctx_->is_failed()) {
        ret = OB_CANCELED;
        STORAGE_LOG(WARN, "ls migration task is failed, cancel wait ls check point ts push", K(ret));
      } else if (ls->is_stopped()) {
        ret = OB_NOT_RUNNING;
        LOG_WARN("ls is not running, stop migration dag net", K(ret), K(ctx_));
      } else if (OB_FAIL(SYS_TASK_STATUS_MGR.is_task_cancel(get_dag()->get_dag_id(), is_cancel))) {
        STORAGE_LOG(ERROR, "failed to check is task canceled", K(ret), K(*this));
      } else if (is_cancel) {
        ret = OB_CANCELED;
        STORAGE_LOG(WARN, "task is cancelled", K(ret), K(*this));
      } else if (OB_FAIL(ls->get_max_decided_log_ts_ns(current_replay_log_ts_ns))) {
        LOG_WARN("failed to get current replay log ts", K(ret), KPC(ctx_));
      } else if (current_replay_log_ts_ns >= max_minor_end_scn_) {
        const int64_t cost_ts = ObTimeUtility::current_time() - wait_replay_start_ts;
        LOG_INFO("wait replay log ts push to max minor end scn success, stop wait", "arg", ctx_->arg_,
            K(cost_ts), K(max_minor_end_scn_), K(current_replay_log_ts_ns));
        break;
      } else {
        const int64_t current_ts = ObTimeUtility::current_time();
        if (REACH_TENANT_TIME_INTERVAL(60 * 1000 * 1000)) {
          LOG_INFO("ls wait replay to max minor sstable end log ts, retry next loop", "arg", ctx_->arg_,
              "wait_replay_start_ts", wait_replay_start_ts,
              "current_ts", current_ts);
        }

        if (current_ts - wait_replay_start_ts < OB_WAIT_LOG_REPLAY_TIMEOUT) {
        } else {
          if (OB_FAIL(ctx_->set_result(OB_WAIT_REPLAY_TIMEOUT, true /*allow_retry*/))) {
            LOG_WARN("failed to set result", K(ret), KPC(ctx_));
          } else {
            ret = OB_WAIT_REPLAY_TIMEOUT;
            STORAGE_LOG(WARN, "failed to wait replay to max minor end scn, timeout, stop migration task",
                K(ret), K(*ctx_), K(current_ts),
                K(wait_replay_start_ts));
          }
        }

        if (OB_SUCC(ret)) {
          ob_usleep(OB_WAIT_LOG_REPLAY_INTERVAL);
        }
      }
    }
  }
  return ret;
}

int ObStartCompleteMigrationTask::record_server_event_()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx should not be null", K(ret));
  } else {
    SERVER_EVENT_ADD("storage_ha", "start_complete_migration_task",
        "tenant_id", ctx_->tenant_id_,
        "ls_id", ctx_->arg_.ls_id_.id(),
        "src", ctx_->arg_.src_.get_server(),
        "dst", ctx_->arg_.dst_.get_server(),
        "task_id", ctx_->task_id_,
        "is_failed", ctx_->is_failed(),
        ObMigrationOpType::get_str(ctx_->arg_.type_));
  }
  return ret;
}

/******************ObFinishCompleteMigrationDag*********************/
ObFinishCompleteMigrationDag::ObFinishCompleteMigrationDag()
  : ObCompleteMigrationDag(ObStorageHADagType::FINISH_COMPLETE_MIGRATION_DAG),
    is_inited_(false)
{
}

ObFinishCompleteMigrationDag::~ObFinishCompleteMigrationDag()
{
}

int ObFinishCompleteMigrationDag::fill_dag_key(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  ObLSCompleteMigrationCtx *self_ctx = nullptr;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("initial prepare migration dag do not init", K(ret));
  } else if (ObIHADagNetCtx::LS_COMPLETE_MIGRATION != ha_dag_net_ctx_->get_dag_net_ctx_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ha dag net ctx type is unexpected", K(ret), KPC(ha_dag_net_ctx_));
  } else if (FALSE_IT(self_ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag_net_ctx_))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len,
         "ObFinishCompleteMigrationDag: ls_id = %s, migration_type = %s",
         to_cstring(self_ctx->arg_.ls_id_), ObMigrationOpType::get_str(self_ctx->arg_.type_)))) {
    LOG_WARN("failed to fill comment", K(ret), K(*self_ctx));
  }
  return ret;
}

int ObFinishCompleteMigrationDag::init(ObIDagNet *dag_net)
{
  int ret = OB_SUCCESS;

  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("finish complete migration dag init twice", K(ret));
  } else if (OB_ISNULL(dag_net)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("init finish complete migration dag get invalid argument", K(ret), KP(dag_net));
  } else if (OB_FAIL(prepare_ctx(dag_net))) {
    LOG_WARN("failed to prepare ctx", K(ret));
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObFinishCompleteMigrationDag::create_first_task()
{
  int ret = OB_SUCCESS;
  ObFinishCompleteMigrationTask *task = NULL;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("finish complete migration dag do not init", K(ret));
  } else if (OB_FAIL(alloc_task(task))) {
    LOG_WARN("Fail to alloc task", K(ret));
  } else if (OB_FAIL(task->init())) {
    LOG_WARN("failed to init finish complete migration task", K(ret), KPC(ha_dag_net_ctx_));
  } else if (OB_FAIL(add_task(*task))) {
    LOG_WARN("Fail to add task", K(ret));
  } else {
    LOG_DEBUG("success to create first task", K(ret), KPC(this));
  }
  return ret;
}

int ObFinishCompleteMigrationDag::fill_comment(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  ObLSCompleteMigrationCtx *ctx = nullptr;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("finish complete migration dag do not init", K(ret));
  } else if (NULL == buf || buf_len <= 0) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid args", K(ret), KP(buf), K(buf_len));
  } else if (FALSE_IT(ctx = static_cast<ObLSCompleteMigrationCtx *>(ha_dag_net_ctx_))) {
  } else if (OB_FAIL(databuff_printf(buf, buf_len,
      "ObFinishCompleteMigrationDag : dag_net_task_id = %s, tenant_id = %s, ls_id = %s, migration_type = %s, "
      "src = %s, dest = %s", to_cstring(ctx->task_id_), to_cstring(ctx->tenant_id_), to_cstring(ctx->arg_.ls_id_),
      ObMigrationOpType::get_str(ctx->arg_.type_), to_cstring(ctx->arg_.src_.get_server()),
      to_cstring(ctx->arg_.dst_.get_server())))) {
    LOG_WARN("failed to fill comment", K(ret), KPC(ctx));
  }
  return ret;
}

/******************ObFinishCompleteMigrationTask*********************/
ObFinishCompleteMigrationTask::ObFinishCompleteMigrationTask()
  : ObITask(TASK_TYPE_MIGRATE_PREPARE),
    is_inited_(false),
    ctx_(nullptr),
    dag_net_(nullptr)
{
}

ObFinishCompleteMigrationTask::~ObFinishCompleteMigrationTask()
{
}

int ObFinishCompleteMigrationTask::init()
{
  int ret = OB_SUCCESS;
  ObIDagNet *dag_net = nullptr;
  ObLSCompleteMigrationDagNet *complete_dag_net = nullptr;

  if (is_inited_) {
    ret = OB_INIT_TWICE;
    LOG_WARN("finish complete migration task init twice", K(ret));
  } else if (FALSE_IT(dag_net = this->get_dag()->get_dag_net())) {
  } else if (OB_ISNULL(dag_net)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag net should not be NULL", K(ret), KP(dag_net));
  } else if (ObDagNetType::DAG_NET_TYPE_COMPLETE_MIGARTION != dag_net->get_type()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("dag net type is unexpected", K(ret), KPC(dag_net));
  } else if (FALSE_IT(complete_dag_net = static_cast<ObLSCompleteMigrationDagNet*>(dag_net))) {
  } else {
    ctx_ = complete_dag_net->get_ctx();
    dag_net_ = dag_net;
    is_inited_ = true;
    LOG_INFO("succeed init finish complete migration task", "ls id", ctx_->arg_.ls_id_,
        "dag_id", *ObCurTraceId::get_trace_id(), "dag_net_id", ctx_->task_id_);
  }
  return ret;
}

int ObFinishCompleteMigrationTask::process()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  FLOG_INFO("start do finish complete migration task", KPC(ctx_));

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("finish complete migration task do not init", K(ret));
  } else {
    if (ctx_->is_failed()) {
      bool allow_retry = false;
      if (OB_FAIL(ctx_->check_allow_retry(allow_retry))) {
        LOG_ERROR("failed to check need retry", K(ret), K(*ctx_));
      } else if (allow_retry) {
        ctx_->reuse();
        if (OB_FAIL(generate_prepare_initial_dag_())) {
          LOG_WARN("failed to generate prepare initial dag", K(ret), KPC(ctx_));
        }
      }
    } else if (OB_FAIL(try_enable_vote_())) {
      LOG_WARN("failed to try enable vote", K(ret), KPC(ctx_));
    }

    if (OB_FAIL(ret)) {
      const bool need_retry = false;
      if (OB_SUCCESS != (tmp_ret = ctx_->set_result(ret, need_retry))) {
        LOG_WARN("failed to set result", K(ret), K(ret), K(tmp_ret), KPC(ctx_));
      }
    }
  }

  if (OB_SUCCESS != (tmp_ret = record_server_event_())) {
    LOG_WARN("failed to record server event", K(tmp_ret), K(ret));
  }
  return ret;
}

int ObFinishCompleteMigrationTask::generate_prepare_initial_dag_()
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  ObInitialCompleteMigrationDag *initial_complete_dag = nullptr;
  ObTenantDagScheduler *scheduler = nullptr;
  ObFinishCompleteMigrationDag *finish_complete_migration_dag = nullptr;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("finish complete migration task do not init", K(ret));
  } else if (OB_ISNULL(finish_complete_migration_dag = static_cast<ObFinishCompleteMigrationDag *>(this->get_dag()))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("finish complete migration dag should not be NULL", K(ret), KP(finish_complete_migration_dag));
  } else if (OB_ISNULL(scheduler = MTL(ObTenantDagScheduler*))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("failed to get ObTenantDagScheduler from MTL", K(ret));
  } else {
    if (OB_FAIL(scheduler->alloc_dag(initial_complete_dag))) {
      LOG_WARN("failed to alloc initial complete migration dag ", K(ret));
    } else if (OB_FAIL(initial_complete_dag->init(dag_net_))) {
      LOG_WARN("failed to init initial complete migration dag", K(ret));
    } else if (OB_FAIL(this->get_dag()->add_child(*initial_complete_dag))) {
      LOG_WARN("failed to add initial complete dag as child", K(ret), KPC(initial_complete_dag));
    } else if (OB_FAIL(initial_complete_dag->create_first_task())) {
      LOG_WARN("failed to create first task", K(ret));
    } else if (OB_FAIL(scheduler->add_dag(initial_complete_dag))) {
      LOG_WARN("failed to add initial complete migration dag", K(ret), K(*initial_complete_dag));
      if (OB_SIZE_OVERFLOW != ret && OB_EAGAIN != ret) {
        LOG_WARN("Fail to add task", K(ret));
        ret = OB_EAGAIN;
      }
    } else {
      LOG_INFO("start create initial complete migration dag", K(ret), K(*ctx_));
      initial_complete_dag = nullptr;
    }

    if (OB_NOT_NULL(initial_complete_dag) && OB_NOT_NULL(scheduler)) {
      scheduler->free_dag(*initial_complete_dag, finish_complete_migration_dag);
      initial_complete_dag = nullptr;
    }
  }
  return ret;
}

int ObFinishCompleteMigrationTask::try_enable_vote_()
{
  int ret = OB_SUCCESS;
  ObLS *ls = nullptr;
  ObLSHandle ls_handle;

  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("finish complete migration task do not init", K(ret));
  } else if (ObMigrationOpType::REBUILD_LS_OP != ctx_->arg_.type_) {
    //do nothing
  } else {
    if (OB_FAIL(ObStorageHADagUtils::get_ls(ctx_->arg_.ls_id_, ls_handle))) {
      LOG_WARN("failed to get ls", K(ret), K(ctx_));
    } else if (OB_ISNULL(ls = ls_handle.get_ls())) {
      ret = OB_ERR_SYS;
      LOG_ERROR("ls should not be NULL", K(ret), KPC(ctx_));
    }

  #ifdef ERRSIM
    if (OB_SUCC(ret)) {
      ret = E(EventTable::EN_MIGRATION_ENABLE_VOTE_FAILED) OB_SUCCESS;
      if (OB_FAIL(ret)) {
        STORAGE_LOG(ERROR, "fake EN_MIGRATION_ENABLE_VOTE_FAILED", K(ret));
      }
    }
  #endif

    if (OB_FAIL(ret)) {
    } else if (OB_ISNULL(ls)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ls should not be null", K(ret));
    } else if (OB_FAIL(ls->enable_vote())) {
      LOG_WARN("failed to enable vote", K(ret), KPC(ctx_));
    } else {
      LOG_INFO("succeed enable vote", KPC(ctx_));
    #ifdef ERRSIM
      if (OB_SUCC(ret)) {
        ret = E(EventTable::EN_MIGRATION_ENABLE_VOTE_RETRY) OB_SUCCESS;
        if (OB_FAIL(ret)) {
          STORAGE_LOG(ERROR, "fake EN_MIGRATION_ENABLE_VOTE_RETRY", K(ret));
        }
      }
    #endif
    }
  }
  return ret;
}

int ObFinishCompleteMigrationTask::record_server_event_()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ctx_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ctx should not be null", K(ret));
  } else {
    SERVER_EVENT_ADD("storage_ha", "finish_complete_migration_task",
        "tenant_id", ctx_->tenant_id_,
        "ls_id", ctx_->arg_.ls_id_.id(),
        "src", ctx_->arg_.src_.get_server(),
        "dst", ctx_->arg_.dst_.get_server(),
        "task_id", ctx_->task_id_,
        "is_failed", ctx_->is_failed(),
        ObMigrationOpType::get_str(ctx_->arg_.type_));
  }
  return ret;
}

