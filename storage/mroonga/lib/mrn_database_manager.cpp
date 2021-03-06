/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2014 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include <mrn_mysql.h>

#include "mrn_database_manager.hpp"
#include "mrn_encoding.hpp"
#include "mrn_lock.hpp"
#include "mrn_path_mapper.hpp"

// for debug
#define MRN_CLASS_NAME "mrn::DatabaseManager"

#ifdef WIN32
#  include <direct.h>
#  define MRN_MKDIR(pathname, mode) _mkdir((pathname))
#else
#  include <dirent.h>
#  include <unistd.h>
#  define MRN_MKDIR(pathname, mode) mkdir((pathname), (mode))
#endif

namespace mrn {
  DatabaseManager::DatabaseManager(grn_ctx *ctx)
    : ctx_(ctx),
      cache_(NULL),
      mutex_(),
      mutex_initialized_(false) {
  }

  DatabaseManager::~DatabaseManager(void) {
    if (mutex_initialized_) {
      pthread_mutex_destroy(&mutex_);
    }

    if (cache_) {
      void *db_address;
      GRN_HASH_EACH(ctx_, cache_, id, NULL, 0, &db_address, {
        grn_obj *db;
        memcpy(&db, db_address, sizeof(grn_obj *));
        grn_obj_unlink(ctx_, db);
      });
      grn_hash_close(ctx_, cache_);
    }
  }

  bool DatabaseManager::init(void) {
    MRN_DBUG_ENTER_METHOD();
    cache_ = grn_hash_create(ctx_,
                             NULL,
                             GRN_TABLE_MAX_KEY_SIZE,
                             sizeof(grn_obj *),
                             GRN_OBJ_KEY_VAR_SIZE);
    if (!cache_) {
      GRN_LOG(ctx_, GRN_LOG_ERROR,
              "failed to initialize hash table for caching opened databases");
      DBUG_RETURN(false);
    }

    if (pthread_mutex_init(&mutex_, NULL) != 0) {
      GRN_LOG(ctx_, GRN_LOG_ERROR,
              "failed to initialize mutex for opened database cache hash table");
      DBUG_RETURN(false);
    }

    mutex_initialized_ = true;
    DBUG_RETURN(true);
  }

  int DatabaseManager::open(const char *path, grn_obj **db) {
    MRN_DBUG_ENTER_METHOD();

    int error = 0;
    *db = NULL;

    mrn::PathMapper mapper(path);
    mrn::Lock lock(&mutex_);

    error = mrn::encoding::set(ctx_, system_charset_info);
    if (error) {
      DBUG_RETURN(error);
    }

    grn_id id;
    void *db_address;
    id = grn_hash_get(ctx_, cache_,
                      mapper.db_name(), strlen(mapper.db_name()),
                      &db_address);
    if (id == GRN_ID_NIL) {
      struct stat db_stat;
      if (stat(mapper.db_path(), &db_stat)) {
        GRN_LOG(ctx_, GRN_LOG_INFO,
                "database not found. creating...: <%s>", mapper.db_path());
        if (path[0] == FN_CURLIB &&
            (path[1] == FN_LIBCHAR || path[1] == FN_LIBCHAR2)) {
          ensure_database_directory();
        }
        *db = grn_db_create(ctx_, mapper.db_path(), NULL);
        if (ctx_->rc) {
          error = ER_CANT_CREATE_TABLE;
          my_message(error, ctx_->errbuf, MYF(0));
          DBUG_RETURN(error);
        }
      } else {
        *db = grn_db_open(ctx_, mapper.db_path());
        if (ctx_->rc) {
          error = ER_CANT_OPEN_FILE;
          my_message(error, ctx_->errbuf, MYF(0));
          DBUG_RETURN(error);
        }
      }
      grn_hash_add(ctx_, cache_,
                   mapper.db_name(), strlen(mapper.db_name()),
                   &db_address, NULL);
      memcpy(db_address, db, sizeof(grn_obj *));
    } else {
      memcpy(db, db_address, sizeof(grn_obj *));
      grn_ctx_use(ctx_, *db);
    }

    error = ensure_normalizers_registered(*db);

    DBUG_RETURN(error);
  }

  void DatabaseManager::close(const char *path) {
    MRN_DBUG_ENTER_METHOD();

    mrn::PathMapper mapper(path);
    mrn::Lock lock(&mutex_);

    grn_id id;
    void *db_address;
    id = grn_hash_get(ctx_, cache_,
                      mapper.db_name(), strlen(mapper.db_name()),
                      &db_address);
    if (id == GRN_ID_NIL) {
      DBUG_VOID_RETURN;
    }

    grn_obj *db = NULL;
    memcpy(&db, db_address, sizeof(grn_obj *));
    if (db) {
      grn_obj_close(ctx_, db);
    }

    grn_hash_delete_by_id(ctx_, cache_, id, NULL);

    DBUG_VOID_RETURN;
  }

  bool DatabaseManager::drop(const char *path) {
    MRN_DBUG_ENTER_METHOD();

    mrn::PathMapper mapper(path);
    mrn::Lock lock(&mutex_);

    grn_id id;
    void *db_address;
    id = grn_hash_get(ctx_, cache_,
                      mapper.db_name(), strlen(mapper.db_name()),
                      &db_address);

    grn_obj *db = NULL;
    if (id == GRN_ID_NIL) {
      struct stat dummy;
      if (stat(mapper.db_path(), &dummy) == 0) {
        db = grn_db_open(ctx_, mapper.db_path());
      }
    } else {
      memcpy(&db, db_address, sizeof(grn_obj *));
    }

    if (!db) {
      DBUG_RETURN(false);
    }

    if (grn_obj_remove(ctx_, db) == GRN_SUCCESS) {
      if (id != GRN_ID_NIL) {
        grn_hash_delete_by_id(ctx_, cache_, id, NULL);
      }
      DBUG_RETURN(true);
    } else {
      GRN_LOG(ctx_, GRN_LOG_ERROR,
              "failed to drop database: <%s>: <%s>",
              mapper.db_path(), ctx_->errbuf);
      DBUG_RETURN(false);
    }
  }

  int DatabaseManager::clear(void) {
    MRN_DBUG_ENTER_METHOD();

    int error = 0;

    mrn::Lock lock(&mutex_);

    grn_hash_cursor *cursor;
    cursor = grn_hash_cursor_open(ctx_, cache_,
                                  NULL, 0, NULL, 0,
                                  0, -1, 0);
    if (ctx_->rc) {
      my_message(ER_ERROR_ON_READ, ctx_->errbuf, MYF(0));
      DBUG_RETURN(ER_ERROR_ON_READ);
    }

    while (grn_hash_cursor_next(ctx_, cursor) != GRN_ID_NIL) {
      if (ctx_->rc) {
        error = ER_ERROR_ON_READ;
        my_message(error, ctx_->errbuf, MYF(0));
        break;
      }
      void *db_address;
      grn_obj *db;
      grn_hash_cursor_get_value(ctx_, cursor, &db_address);
      memcpy(&db, db_address, sizeof(grn_obj *));
      grn_rc rc = grn_hash_cursor_delete(ctx_, cursor, NULL);
      if (rc) {
        error = ER_ERROR_ON_READ;
        my_message(error, ctx_->errbuf, MYF(0));
        break;
      }
      grn_obj_close(ctx_, db);
    }
    grn_hash_cursor_close(ctx_, cursor);

    DBUG_RETURN(error);
  }

  void DatabaseManager::mkdir_p(const char *directory) {
    MRN_DBUG_ENTER_METHOD();

    int i = 0;
    char sub_directory[MRN_MAX_PATH_SIZE];
    sub_directory[0] = '\0';
    while (true) {
      if (directory[i] == FN_LIBCHAR ||
          directory[i] == FN_LIBCHAR2 ||
          directory[i] == '\0') {
        sub_directory[i] = '\0';
        struct stat directory_status;
        if (stat(sub_directory, &directory_status) != 0) {
          DBUG_PRINT("info", ("mroonga: creating directory: <%s>", sub_directory));
          GRN_LOG(ctx_, GRN_LOG_INFO, "creating directory: <%s>", sub_directory);
          if (MRN_MKDIR(sub_directory, S_IRWXU) == 0) {
            DBUG_PRINT("info",
                       ("mroonga: created directory: <%s>", sub_directory));
            GRN_LOG(ctx_, GRN_LOG_INFO, "created directory: <%s>", sub_directory);
          } else {
            DBUG_PRINT("error",
                       ("mroonga: failed to create directory: <%s>: <%s>",
                        sub_directory, strerror(errno)));
            GRN_LOG(ctx_, GRN_LOG_ERROR,
                    "failed to create directory: <%s>: <%s>",
                    sub_directory, strerror(errno));
            DBUG_VOID_RETURN;
          }
        }
      }

      if (directory[i] == '\0') {
        break;
      }

      sub_directory[i] = directory[i];
      ++i;
    }

    DBUG_VOID_RETURN;
  }

  void DatabaseManager::ensure_database_directory(void) {
    MRN_DBUG_ENTER_METHOD();

    const char *path_prefix = mrn::PathMapper::default_path_prefix;
    if (!path_prefix)
      DBUG_VOID_RETURN;

    const char *last_path_separator;
    last_path_separator = strrchr(path_prefix, FN_LIBCHAR);
    if (!last_path_separator)
      last_path_separator = strrchr(path_prefix, FN_LIBCHAR2);
    if (!last_path_separator)
      DBUG_VOID_RETURN;
    if (path_prefix == last_path_separator)
      DBUG_VOID_RETURN;

    char database_directory[MRN_MAX_PATH_SIZE];
    size_t database_directory_length = last_path_separator - path_prefix;
    strncpy(database_directory, path_prefix, database_directory_length);
    database_directory[database_directory_length] = '\0';
    mkdir_p(database_directory);

    DBUG_VOID_RETURN;
  }

  int DatabaseManager::ensure_normalizers_registered(grn_obj *db) {
    MRN_DBUG_ENTER_METHOD();

    int error = 0;
#ifdef WITH_GROONGA_NORMALIZER_MYSQL
    {
      grn_obj *mysql_normalizer;
      mysql_normalizer = grn_ctx_get(ctx_, "NormalizerMySQLGeneralCI", -1);
      if (mysql_normalizer) {
        grn_obj_unlink(ctx_, mysql_normalizer);
      } else {
#ifdef GROONGA_NORMALIZER_MYSQL_PLUGIN_IS_BUNDLED_STATIC
        char ref_path[FN_REFLEN + 1], *tmp;
        tmp = strmov(ref_path, opt_plugin_dir);
        tmp = strmov(tmp, "/ha_mroonga");
        strcpy(tmp, SO_EXT);
        grn_plugin_register_by_path(ctx_, ref_path);
#else
        grn_plugin_register(ctx_, GROONGA_NORMALIZER_MYSQL_PLUGIN_NAME);
#endif
      }
    }
#endif

    DBUG_RETURN(error);
  }
}
