/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

/*
 * Implementation using LMDB API.
 */

#include <cf3.defs.h>
#include <dbm_priv.h>
#include <logging.h>
#include <string_lib.h>
#include <misc_lib.h>
#include <file_lib.h>
#include <known_dirs.h>

#ifdef LMDB

#include <lmdb.h>

// Shared between threads.
struct DBPriv_
{
    MDB_env *env;
    MDB_dbi dbi;
    // Used to keep track of transactions.
    // We set this to the transaction address when a thread creates a
    // transaction, and back to 0x0 when it is destroyed.
    pthread_key_t txn_key;
};

// Not shared between threads.
typedef struct DBTxn_
{
    MDB_txn *txn;
    // Whether txn is a read/write (true) or read-only (false) transaction.
    bool rw_txn;
} DBTxn;

struct DBCursorPriv_
{
    DBPriv *db;
    MDB_cursor *mc;
    MDB_val delkey;
    void *curkv;
    bool pending_delete;
};

/******************************************************************************/

static int GetReadTransaction(DBPriv *db, MDB_txn **txn)
{
    DBTxn *db_txn = pthread_getspecific(db->txn_key);
    int rc = MDB_SUCCESS;

    if (!db_txn)
    {
        db_txn = xcalloc(1, sizeof(DBTxn));
        pthread_setspecific(db->txn_key, db_txn);
    }

    if (!db_txn->txn)
    {
        rc = mdb_txn_begin(db->env, NULL, MDB_RDONLY, &db_txn->txn);
        if (rc != MDB_SUCCESS)
        {
            Log(LOG_LEVEL_ERR, "Unable to open read transaction: %s", mdb_strerror(rc));
        }
    }

    *txn = db_txn->txn;

    return rc;
}

static int GetWriteTransaction(DBPriv *db, MDB_txn **txn)
{
    DBTxn *db_txn = pthread_getspecific(db->txn_key);
    int rc = MDB_SUCCESS;

    if (!db_txn)
    {
        db_txn = xcalloc(1, sizeof(DBTxn));
        pthread_setspecific(db->txn_key, db_txn);
    }

    if (db_txn->txn && !db_txn->rw_txn)
    {
        rc = mdb_txn_commit(db_txn->txn);
        if (rc != MDB_SUCCESS)
        {
            Log(LOG_LEVEL_ERR, "Unable to close read-only transaction: %s", mdb_strerror(rc));
        }
        db_txn->txn = NULL;
    }

    if (!db_txn->txn)
    {
        rc = mdb_txn_begin(db->env, NULL, 0, &db_txn->txn);
        if (rc == MDB_SUCCESS)
        {
            db_txn->rw_txn = true;
        }
        else
        {
            Log(LOG_LEVEL_ERR, "Unable to open write transaction: %s", mdb_strerror(rc));
        }
    }

    *txn = db_txn->txn;

    return rc;
}

static void AbortTransaction(DBPriv *db)
{
    DBTxn *db_txn = pthread_getspecific(db->txn_key);
    if (db_txn != NULL)
    {
        if (db_txn->txn != NULL)
        {
            mdb_txn_abort(db_txn->txn);
        }

        pthread_setspecific(db->txn_key, NULL);
        free(db_txn);
    }
}

static void DestroyTransaction(void *ptr)
{
    DBTxn *db_txn = (DBTxn *)ptr;
    UnexpectedError("Transaction object still exists when terminating thread");
    if (db_txn->txn)
    {
        UnexpectedError("Transaction still open when terminating thread!");
        mdb_txn_abort(db_txn->txn);
    }
    free(db_txn);
}

const char *DBPrivGetFileExtension(void)
{
    return "lmdb";
}

#ifndef LMDB_MAXSIZE
#define LMDB_MAXSIZE    104857600
#endif

/* Lastseen default number of maxreaders = 4x the default lmdb maxreaders */
#define DEFAULT_LASTSEEN_MAXREADERS (126*4)

DBPriv *DBPrivOpenDB(const char *dbpath, dbid id)
{
    DBPriv *db = xcalloc(1, sizeof(DBPriv));
    MDB_txn *txn = NULL;
    int rc;

    rc = pthread_key_create(&db->txn_key, &DestroyTransaction);
    if (rc)
    {
        Log(LOG_LEVEL_ERR, "Could not create transaction key. (pthread_key_create: '%s')",
            GetErrorStrFromCode(rc));
        free(db);
        return NULL;
    }

    rc = mdb_env_create(&db->env);
    if (rc)
    {
        Log(LOG_LEVEL_ERR, "Could not create handle for database %s: %s",
              dbpath, mdb_strerror(rc));
        goto err;
    }
    rc = mdb_env_set_mapsize(db->env, LMDB_MAXSIZE);
    if (rc)
    {
        Log(LOG_LEVEL_ERR, "Could not set mapsize for database %s: %s",
              dbpath, mdb_strerror(rc));
        goto err;
    }
    if (id == dbid_lastseen)
    {
        /* lastseen needs by default 4x more reader locks than other DBs*/
        rc = mdb_env_set_maxreaders(db->env, DEFAULT_LASTSEEN_MAXREADERS);
        if (rc)
        {
            Log(LOG_LEVEL_ERR, "Could not set maxreaders for database %s: %s",
                  dbpath, mdb_strerror(rc));
            goto err;
        }
    }
    if (id != dbid_locks)
    {
        rc = mdb_env_open(db->env, dbpath, MDB_NOSUBDIR, 0644);
    }
    else
    {
        rc = mdb_env_open(db->env, dbpath, MDB_NOSUBDIR|MDB_NOSYNC, 0644);
    }
    if (rc)
    {
        Log(LOG_LEVEL_ERR, "Could not open database %s: %s",
              dbpath, mdb_strerror(rc));
        goto err;
    }
    rc = mdb_txn_begin(db->env, NULL, MDB_RDONLY, &txn);
    if (rc)
    {
        Log(LOG_LEVEL_ERR, "Could not open database txn %s: %s",
              dbpath, mdb_strerror(rc));
        goto err;
    }
    rc = mdb_open(txn, NULL, 0, &db->dbi);
    if (rc)
    {
        Log(LOG_LEVEL_ERR, "Could not open database dbi %s: %s",
              dbpath, mdb_strerror(rc));
        mdb_txn_abort(txn);
        goto err;
    }
    rc = mdb_txn_commit(txn);
    if (rc)
    {
        Log(LOG_LEVEL_ERR, "Could not commit database dbi %s: %s",
              dbpath, mdb_strerror(rc));
        goto err;
    }

    return db;

err:
    if (db->env)
    {
        mdb_env_close(db->env);
    }
    pthread_key_delete(db->txn_key);
    free(db);
    if (rc == MDB_INVALID)
    {
        return DB_PRIV_DATABASE_BROKEN;
    }
    return NULL;
}

void DBPrivCloseDB(DBPriv *db)
{
    /* Abort LMDB transaction of the current thread. There should only be some
     * transaction open when the signal handler or atexit() hook is called. */
    AbortTransaction(db);

    if (db->env)
    {
        mdb_env_close(db->env);
    }

    pthread_key_delete(db->txn_key);
    free(db);
}

void DBPrivCommit(DBPriv *db)
{
    DBTxn *db_txn = pthread_getspecific(db->txn_key);
    if (db_txn && db_txn->txn)
    {
        int rc = mdb_txn_commit(db_txn->txn);
        if (rc != MDB_SUCCESS)
        {
            Log(LOG_LEVEL_ERR, "Could not commit database transaction: %s", mdb_strerror(rc));
        }
    }
    pthread_setspecific(db->txn_key, NULL);
    free(db_txn);
}

bool DBPrivHasKey(DBPriv *db, const void *key, int key_size)
{
    MDB_val mkey, data;
    MDB_txn *txn;
    int rc;
    // FIXME: distinguish between "entry not found" and "error occured"

    rc = GetReadTransaction(db, &txn);
    if (rc == MDB_SUCCESS)
    {
        mkey.mv_data = (void *)key;
        mkey.mv_size = key_size;
        rc = mdb_get(txn, db->dbi, &mkey, &data);
        if (rc && rc != MDB_NOTFOUND)
        {
            Log(LOG_LEVEL_ERR, "Could not read database entry: %s", mdb_strerror(rc));
            AbortTransaction(db);
        }
    }

    return rc == MDB_SUCCESS;
}

int DBPrivGetValueSize(DBPriv *db, const void *key, int key_size)
{
    MDB_val mkey, data;
    MDB_txn *txn;
    int rc;

    data.mv_size = 0;

    rc = GetReadTransaction(db, &txn);
    if (rc == MDB_SUCCESS)
    {
        mkey.mv_data = (void *)key;
        mkey.mv_size = key_size;
        rc = mdb_get(txn, db->dbi, &mkey, &data);
        if (rc && rc != MDB_NOTFOUND)
        {
            Log(LOG_LEVEL_ERR, "Could not read database entry: %s", mdb_strerror(rc));
            AbortTransaction(db);
        }
    }

    return data.mv_size;
}

bool DBPrivRead(DBPriv *db, const void *key, int key_size, void *dest, int dest_size)
{
    MDB_val mkey, data;
    MDB_txn *txn;
    int rc;
    bool ret = false;

    rc = GetReadTransaction(db, &txn);
    if (rc == MDB_SUCCESS)
    {
        mkey.mv_data = (void *)key;
        mkey.mv_size = key_size;
        rc = mdb_get(txn, db->dbi, &mkey, &data);
        if (rc == MDB_SUCCESS)
        {
            if (dest_size > data.mv_size)
            {
                dest_size = data.mv_size;
            }
            memcpy(dest, data.mv_data, dest_size);
            ret = true;
        }
        else if (rc != MDB_NOTFOUND)
        {
            Log(LOG_LEVEL_ERR, "Could not read database entry: %s", mdb_strerror(rc));
            AbortTransaction(db);
        }
    }
    return ret;
}

bool DBPrivWrite(DBPriv *db, const void *key, int key_size, const void *value, int value_size)
{
    MDB_val mkey, data;
    MDB_txn *txn;
    int rc = GetWriteTransaction(db, &txn);
    if (rc == MDB_SUCCESS)
    {
        mkey.mv_data = (void *)key;
        mkey.mv_size = key_size;
        data.mv_data = (void *)value;
        data.mv_size = value_size;
        rc = mdb_put(txn, db->dbi, &mkey, &data, 0);
        if (rc != MDB_SUCCESS)
        {
            Log(LOG_LEVEL_ERR, "Could not write database entry: %s", mdb_strerror(rc));
            AbortTransaction(db);
        }
    }
    return rc == MDB_SUCCESS;
}

bool DBPrivDelete(DBPriv *db, const void *key, int key_size)
{
    MDB_val mkey;
    MDB_txn *txn;
    int rc = GetWriteTransaction(db, &txn);
    if (rc == MDB_SUCCESS)
    {
        mkey.mv_data = (void *)key;
        mkey.mv_size = key_size;
        rc = mdb_del(txn, db->dbi, &mkey, NULL);
        if (rc == MDB_NOTFOUND)
        {
            Log(LOG_LEVEL_DEBUG, "Entry not found: %s", mdb_strerror(rc));
        }
        else if (rc != MDB_SUCCESS)
        {
            Log(LOG_LEVEL_ERR, "Could not delete: %s", mdb_strerror(rc));
            AbortTransaction(db);
        }
    }
    return rc == MDB_SUCCESS;
}

DBCursorPriv *DBPrivOpenCursor(DBPriv *db)
{
    DBCursorPriv *cursor = NULL;
    MDB_txn *txn;
    int rc;
    MDB_cursor *mc;

    rc = GetWriteTransaction(db, &txn);
    if (rc == MDB_SUCCESS)
    {
        rc = mdb_cursor_open(txn, db->dbi, &mc);
        if (rc == MDB_SUCCESS)
        {
            cursor = xcalloc(1, sizeof(DBCursorPriv));
            cursor->db = db;
            cursor->mc = mc;
        }
        else
        {
            Log(LOG_LEVEL_ERR, "Could not open cursor: %s", mdb_strerror(rc));
            AbortTransaction(db);
        }
        /* txn remains with cursor */
    }

    return cursor;
}

bool DBPrivAdvanceCursor(DBCursorPriv *cursor, void **key, int *key_size,
                     void **value, int *value_size)
{
    MDB_val mkey, data;
    int rc;
    bool retval = false;

    if (cursor->curkv)
    {
        free(cursor->curkv);
        cursor->curkv = NULL;
    }
    if ((rc = mdb_cursor_get(cursor->mc, &mkey, &data, MDB_NEXT)) == MDB_SUCCESS)
    {
        // Align second buffer to 64-bit boundary, to avoid alignment errors on
        // certain platforms.
        size_t keybuf_size = mkey.mv_size;
        if (keybuf_size & 0x7)
        {
            keybuf_size += 8 - (keybuf_size % 8);
        }
        cursor->curkv = xmalloc(keybuf_size + data.mv_size);
        memcpy(cursor->curkv, mkey.mv_data, mkey.mv_size);
        *key = cursor->curkv;
        *key_size = mkey.mv_size;
        *value_size = data.mv_size;
        memcpy((char *)cursor->curkv+keybuf_size, data.mv_data, data.mv_size);
        *value = (char *)cursor->curkv + keybuf_size;
        retval = true;
    }
    else if (rc != MDB_NOTFOUND)
    {
        Log(LOG_LEVEL_ERR, "Could not advance cursor: %s", mdb_strerror(rc));
    }
    if (cursor->pending_delete)
    {
        int r2;
        /* Position on key to delete */
        r2 = mdb_cursor_get(cursor->mc, &cursor->delkey, NULL, MDB_SET);
        if (r2 == MDB_SUCCESS)
        {
            r2 = mdb_cursor_del(cursor->mc, 0);
        }
        /* Reposition the cursor if it was valid before */
        if (rc == MDB_SUCCESS)
        {
            mkey.mv_data = *key;
            rc = mdb_cursor_get(cursor->mc, &mkey, NULL, MDB_SET);
        }
        cursor->pending_delete = false;
    }
    return retval;
}

bool DBPrivDeleteCursorEntry(DBCursorPriv *cursor)
{
    int rc = mdb_cursor_get(cursor->mc, &cursor->delkey, NULL, MDB_GET_CURRENT);
    if (rc == MDB_SUCCESS)
    {
        cursor->pending_delete = true;
    }
    return rc == MDB_SUCCESS;
}

bool DBPrivWriteCursorEntry(DBCursorPriv *cursor, const void *value, int value_size)
{
    MDB_val data;
    int rc;

    cursor->pending_delete = false;
    data.mv_data = (void *)value;
    data.mv_size = value_size;

    if (cursor->curkv)
    {
        MDB_val curkey;
        curkey.mv_data = cursor->curkv;
        curkey.mv_size = sizeof(cursor->curkv);

        if ((rc = mdb_cursor_put(cursor->mc, &curkey, &data, MDB_CURRENT)) != MDB_SUCCESS)
        {
            Log(LOG_LEVEL_ERR, "Could not write cursor entry: %s", mdb_strerror(rc));
        }
    }
    else
    {
        Log(LOG_LEVEL_ERR, "Could not write cursor entry: cannot find current key");
        rc = MDB_INVALID;
    }
    return rc == MDB_SUCCESS;
}

void DBPrivCloseCursor(DBCursorPriv *cursor)
{
    if (cursor->curkv)
    {
        free(cursor->curkv);
    }

    if (cursor->pending_delete)
    {
        mdb_cursor_del(cursor->mc, 0);
    }

    mdb_cursor_close(cursor->mc);
    free(cursor);
}

char *DBPrivDiagnose(const char *dbpath)
{
    return StringFormat("Unable to diagnose LMDB file (not implemented) for '%s'", dbpath);
}

int UpdateLastSeenMaxReaders(int maxreaders)
{
    int rc = 0;
    /* We assume that every cf_lastseen DB has already a minimum of 504 maxreaders */
    if (maxreaders > DEFAULT_LASTSEEN_MAXREADERS)
    {
        char workbuf[CF_BUFSIZE];
        MDB_env *env = NULL;
        rc = mdb_env_create(&env);
        if (rc)
        {
            Log(LOG_LEVEL_ERR, "Could not create lastseen database env : %s",
                mdb_strerror(rc));
            goto err;
        }

        rc = mdb_env_set_maxreaders(env, maxreaders);
        if (rc)
        {
            Log(LOG_LEVEL_ERR, "Could not change lastseen maxreaders to %d : %s",
                maxreaders, mdb_strerror(rc));
            goto err;
        }

        snprintf(workbuf, CF_BUFSIZE, "%s%ccf_lastseen.lmdb", GetWorkDir(), FILE_SEPARATOR);
        rc = mdb_env_open(env, workbuf, MDB_NOSUBDIR, 0644);
        if (rc)
        {
            Log(LOG_LEVEL_ERR, "Could not open lastseen database env : %s",
                mdb_strerror(rc));
        }
err:
        if (env)
        {
            mdb_env_close(env);
        }
    }
    return rc;
}
#endif