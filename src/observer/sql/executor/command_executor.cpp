/* Copyright (c) 2021 OceanBase and/or its affiliates. All rights reserved.
miniob is licensed under Mulan PSL v2.
You can use this software according to the terms and conditions of the Mulan PSL v2.
You may obtain a copy of Mulan PSL v2 at:
         http://license.coscl.org.cn/MulanPSL2
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
See the Mulan PSL v2 for more details. */

//
// Created by Wangyunlai on 2023/4/25.
//

#include "sql/executor/command_executor.h"
#include "session/session.h"
#include "sql/stmt/stmt.h"
#include "sql/executor/create_index_executor.h"
#include "common/log/log.h"

RC CommandExecutor::execute(Session *session, Stmt *stmt)
{
  switch (stmt->type()) {
    case StmtType::CREATE_INDEX: {
      CreateIndexExecutor executor;
      return executor.execute(session, stmt);
    } break;

    default: {
      LOG_ERROR("unknown command: %d", static_cast<int>(stmt->type()));
      return RC::UNIMPLENMENT;
    }
  }

  return RC::INTERNAL;
}