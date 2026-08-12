#pragma once

#include <string>

#include "db.hpp"

// ============================================================
//  建表(幂等, CREATE TABLE IF NOT EXISTS)
//  与 database/schema.sql 保持一致。
// ============================================================
bool init_db(Db& db, std::string* err = nullptr);
