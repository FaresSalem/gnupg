/* frontend.c - Database fronend code for keyboxd
 * Copyright (C) 2019 g10 Code GmbH
 *
 * This file is part of GnuPG.
 *
 * GnuPG is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * GnuPG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 * SPDX-License-Identifier: GPL-3.0+
 */

#include <config.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>

#include "keyboxd.h"
#include <assuan.h>
#include "../common/i18n.h"
#include "../common/userids.h"
#include "backend.h"
#include "frontend.h"


/* An object to describe a single database.  */
struct db_desc_s
{
  enum database_types db_type;
  backend_handle_t backend_handle;
};
typedef struct db_desc_s *db_desc_t;


/* The table of databases and the size of that table.  */
static db_desc_t databases;
static unsigned int no_of_databases;




/* Take a lock for reading the databases.  */
static void
take_read_lock (ctrl_t ctrl)
{
  /* FIXME */
  (void)ctrl;
}


/* Take a lock for reading and writing the databases.  */
static void
take_read_write_lock (ctrl_t ctrl)
{
  /* FIXME */
  (void)ctrl;
}


/* Release a lock.  It is valid to call this even if no lock has been
 * taken in which case this is a nop.  */
static void
release_lock (ctrl_t ctrl)
{
  /* FIXME */
  (void)ctrl;
}


/* Add a new resource to the database.  Depending on the FILENAME
 * suffix we decide which one to use.  This function must be called at
 * daemon startup because it employs no locking.  If FILENAME has no
 * directory separator, the file is expected or created below
 * "$GNUPGHOME/public-keys-v1.d/".  In READONLY mode the file must
 * exists; otherwise it is created.  */
gpg_error_t
kbxd_add_resource  (ctrl_t ctrl, const char *filename_arg, int readonly)
{
  gpg_error_t err;
  char *filename;
  enum database_types db_type = 0;
  backend_handle_t handle = NULL;
  unsigned int n, dbidx;

  /* Do tilde expansion etc. */
  if (!strcmp (filename_arg, "[cache]"))
    {
      filename = xstrdup (filename_arg);
      db_type = DB_TYPE_CACHE;
    }
  else if (strchr (filename_arg, DIRSEP_C)
#ifdef HAVE_W32_SYSTEM
      || strchr (filename_arg, '/')  /* Windows also accepts a slash.  */
#endif
      )
    filename = make_filename (filename_arg, NULL);
  else
    filename = make_filename (gnupg_homedir (), GNUPG_PUBLIC_KEYS_DIR,
                              filename_arg, NULL);

  /* If this is the first call to the function and the request is not
   * for the cache backend, add the cache backend so that it will
   * always be the first to be queried.  */
  if (!no_of_databases && !db_type)
    {
      err = be_cache_initialize ();
      /* err = kbxd_add_resource (ctrl, "[cache]", 0); */
      if (err)
        goto leave;
    }

  n = strlen (filename);
  if (db_type)
    ; /* We already know it.  */
  else if (n > 4 && !strcmp (filename + n - 4, ".kbx"))
    db_type = DB_TYPE_KBX;
  else
    {
      log_error (_("can't use file '%s': %s\n"), filename, _("unknown suffix"));
      err = gpg_error (GPG_ERR_NOT_SUPPORTED);
      goto leave;
    }

  err = gpg_error (GPG_ERR_BUG);
  switch (db_type)
    {
    case DB_TYPE_NONE: /* NOTREACHED */
      break;

    case DB_TYPE_CACHE:
      err = be_cache_add_resource (ctrl, &handle);
      break;

    case DB_TYPE_KBX:
      err = be_kbx_add_resource (ctrl, &handle, filename, readonly);
      break;
    }
  if (err)
    goto leave;

  /* All good, create an entry in the table. */
  for (dbidx = 0; dbidx < no_of_databases; dbidx++)
    if (!databases[dbidx].db_type)
      break;
  if (dbidx == no_of_databases)
    {
      /* No table yet or table is full.  */
      if (!databases)
        {
          /* Create first set of databases.  Note that the initial
           * type for all entries is DB_TYPE_NONE.  */
          dbidx = 4;
          databases = xtrycalloc (dbidx, sizeof *databases);
          if (!databases)
            {
              err = gpg_error_from_syserror ();
              goto leave;
            }
          no_of_databases = dbidx;
          dbidx = 0; /* Put into first slot.  */
        }
      else
        {
          db_desc_t newdb;

          dbidx = no_of_databases + (no_of_databases == 4? 12 : 16);
          newdb = xtrycalloc (dbidx, sizeof *databases);
          if (!databases)
            {
              err = gpg_error_from_syserror ();
              goto leave;
            }
          for (n=0; n < no_of_databases; n++)
            newdb[n] = databases[n];
          xfree (databases);
          databases = newdb;
          n = no_of_databases;
          no_of_databases = dbidx;
          dbidx = n; /* Put into first new slot.  */
        }
    }

  databases[dbidx].db_type = db_type;
  databases[dbidx].backend_handle = handle;
  handle = NULL;

 leave:
  if (err)
    {
      log_error ("error adding resource '%s': %s\n",
                 filename, gpg_strerror (err));
      be_generic_release_backend (ctrl, handle);
    }
  xfree (filename);
  return err;
}


/* Release all per session objects.  */
void
kbxd_release_session_info (ctrl_t ctrl)
{
  if (!ctrl)
    return;
  be_release_request (ctrl->opgp_req);
  ctrl->opgp_req = NULL;
  be_release_request (ctrl->x509_req);
  ctrl->x509_req = NULL;
}



/* Search for the keys described by (DESC,NDESC) and return them to
 * the caller.  If RESET is set, the search state is first reset. */
gpg_error_t
kbxd_search (ctrl_t ctrl, KEYDB_SEARCH_DESC *desc, unsigned int ndesc,
             int reset)
{
  gpg_error_t err;
  int i;
  unsigned int dbidx;
  db_desc_t db;
  db_request_t request;
  int start_at_ubid = 0;

  if (DBG_CLOCK)
    log_clock ("%s: enter", __func__);

  if (DBG_LOOKUP)
    {
      log_debug ("%s: %u search descriptions:\n", __func__, ndesc);
      for (i = 0; i < ndesc; i ++)
        {
          /* char *t = keydb_search_desc_dump (&desc[i]); */
          /* log_debug ("%s   %d: %s\n", __func__, i, t); */
          /* xfree (t); */
        }
    }

  take_read_lock (ctrl);

  /* Allocate a handle object if none exists for this context.  */
  if (!ctrl->opgp_req)
    {
      ctrl->opgp_req = xtrycalloc (1, sizeof *ctrl->opgp_req);
      if (!ctrl->opgp_req)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }
    }
  request = ctrl->opgp_req;

  /* If requested do a reset.  Using the reset flag is faster than
   * letting the caller do a separate call for an intial reset.  */
  if (!desc || reset)
    {
      for (dbidx=0; dbidx < no_of_databases; dbidx++)
        {
          db = databases + dbidx;
          if (!db->db_type)
            continue;  /* Empty slot.  */

          switch (db->db_type)
            {
            case DB_TYPE_NONE: /* NOTREACHED */
              break;

            case DB_TYPE_CACHE:
              err = 0; /* Nothing to do.  */
              break;

            case DB_TYPE_KBX:
              err = be_kbx_search (ctrl, db->backend_handle, request, NULL, 0);
              break;
            }
          if (err)
            {
              log_error ("error during the %ssearch reset: %s\n",
                         reset? "initial ":"", gpg_strerror (err));
              goto leave;
            }
        }
      request->any_search = 0;
      request->any_found = 0;
      request->next_dbidx = 0;
      if (!desc) /* Reset only mode */
        {
          err = 0;
          goto leave;
        }
    }


  /* Move to the next non-empty slot.  */
 next_db:
  for (dbidx=request->next_dbidx; (dbidx < no_of_databases
                                   && !databases[dbidx].db_type); dbidx++)
    ;
  request->next_dbidx = dbidx;
  if (!(dbidx < no_of_databases))
    {
      /* All databases have been searched.  Put the non-found mark
       * into the cache for all descriptors.
       * FIXME: We need to see which pubkey type we need to insert.  */
      be_cache_not_found (ctrl, PUBKEY_TYPE_UNKNOWN, desc, ndesc);
      err = gpg_error (GPG_ERR_NOT_FOUND);
      goto leave;
    }
  db = databases + dbidx;

  /* Divert to the backend for the actual search.  */
  switch (db->db_type)
    {
    case DB_TYPE_NONE:
      /* NOTREACHED */
      err = gpg_error (GPG_ERR_INTERNAL);
      break;

    case DB_TYPE_CACHE:
      err = be_cache_search (ctrl, db->backend_handle, request,
                             desc, ndesc);
      /* Expected error codes from the cache lookup are:
       *  0 - found and returned via the cache
       *  GPG_ERR_NOT_FOUND - marked in the cache as not available
       *  GPG_ERR_EOF - cache miss. */
      break;

    case DB_TYPE_KBX:
      if (start_at_ubid)
        {
          /* We need to set the startpoint for the search.  */
          err = be_kbx_seek (ctrl, db->backend_handle, request,
                             request->last_cached_ubid);
          if (err)
            {
              log_debug ("%s: seeking %s to an UBID failed: %s\n",
                         __func__, strdbtype (db->db_type), gpg_strerror (err));
              break;
            }
        }
      err = be_kbx_search (ctrl, db->backend_handle, request,
                           desc, ndesc);
      if (start_at_ubid && gpg_err_code (err) == GPG_ERR_EOF)
        be_cache_mark_final (ctrl, request);
      break;
    }

  if (DBG_LOOKUP)
    log_debug ("%s: searched %s (db %u of %u) => %s\n",
               __func__, strdbtype (db->db_type), dbidx, no_of_databases,
               gpg_strerror (err));
  request->any_search = 1;
  start_at_ubid = 0;
  if (!err)
    {
      request->any_found = 1;
    }
  else if (gpg_err_code (err) == GPG_ERR_EOF)
    {
      if (db->db_type == DB_TYPE_CACHE && request->last_cached_valid)
        {
          if (request->last_cached_final)
            goto leave;
          start_at_ubid = 1;
        }
      request->next_dbidx++;
      goto next_db;
    }


 leave:
  release_lock (ctrl);
  if (DBG_CLOCK)
    log_clock ("%s: leave (%s)", __func__, err? "not found" : "found");
  return err;
}



/* Store; that is insert or update the key (BLOB,BLOBLEN).  MODE
 * controls whether only updates or only inserts are allowed.  */
gpg_error_t
kbxd_store (ctrl_t ctrl, const void *blob, size_t bloblen,
            enum kbxd_store_modes mode)
{
  gpg_error_t err;
  db_request_t request;
  unsigned int dbidx;
  db_desc_t db;
  char ubid[UBID_LEN];
  enum pubkey_types pktype;
  int insert = 0;

  if (DBG_CLOCK)
    log_clock ("%s: enter", __func__);

  take_read_write_lock (ctrl);

  /* Allocate a handle object if none exists for this context.  */
  if (!ctrl->opgp_req)
    {
      ctrl->opgp_req = xtrycalloc (1, sizeof *ctrl->opgp_req);
      if (!ctrl->opgp_req)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }
    }
  request = ctrl->opgp_req;

  /* Check whether to insert or update.  */
  err = be_ubid_from_blob (blob, bloblen, &pktype, ubid);
  if (err)
    goto leave;

  /* FIXME: We force the use of the KBX backend.  */
  for (dbidx=0; dbidx < no_of_databases; dbidx++)
    if (databases[dbidx].db_type == DB_TYPE_KBX)
      break;
  if (!(dbidx < no_of_databases))
    {
      err = gpg_error (GPG_ERR_NOT_INITIALIZED);
      goto leave;
    }
  db = databases + dbidx;

  err = be_kbx_seek (ctrl, db->backend_handle, request, ubid);
  if (!err)
    ; /* Found - need to update.  */
  else if (gpg_err_code (err) == GPG_ERR_EOF)
    insert = 1; /* Not found - need to insert.  */
  else
    {
      log_debug ("%s: searching fingerprint failed: %s\n",
                 __func__, gpg_strerror (err));
      goto leave;
    }

  if (insert)
    {
      if (mode == KBXD_STORE_UPDATE)
        err = gpg_error (GPG_ERR_CONFLICT);
      else
        err = be_kbx_insert (ctrl, db->backend_handle, request,
                             pktype, blob, bloblen);
    }
  else /* Update.  */
    {
      if (mode == KBXD_STORE_INSERT)
        err = gpg_error (GPG_ERR_CONFLICT);
      else
        err = be_kbx_update (ctrl, db->backend_handle, request,
                             pktype, blob, bloblen);
    }

 leave:
  release_lock (ctrl);
  if (DBG_CLOCK)
    log_clock ("%s: leave", __func__);
  return err;
}




/* Delete; remove the blob identified by UBID.  */
gpg_error_t
kbxd_delete (ctrl_t ctrl, const unsigned char *ubid)
{
  gpg_error_t err;
  db_request_t request;
  unsigned int dbidx;
  db_desc_t db;

  if (DBG_CLOCK)
    log_clock ("%s: enter", __func__);

  take_read_write_lock (ctrl);

  /* Allocate a handle object if none exists for this context.  */
  if (!ctrl->opgp_req)
    {
      ctrl->opgp_req = xtrycalloc (1, sizeof *ctrl->opgp_req);
      if (!ctrl->opgp_req)
        {
          err = gpg_error_from_syserror ();
          goto leave;
        }
    }
  request = ctrl->opgp_req;

  /* FIXME: We force the use of the KBX backend.  */
  for (dbidx=0; dbidx < no_of_databases; dbidx++)
    if (databases[dbidx].db_type == DB_TYPE_KBX)
      break;
  if (!(dbidx < no_of_databases))
    {
      err = gpg_error (GPG_ERR_NOT_INITIALIZED);
      goto leave;
    }
  db = databases + dbidx;

  err = be_kbx_seek (ctrl, db->backend_handle, request, ubid);
  if (!err)
    ; /* Found - we can delete.  */
  else if (gpg_err_code (err) == GPG_ERR_EOF)
    {
      err = gpg_error (GPG_ERR_NOT_FOUND);
      goto leave;
    }
  else
    {
      log_debug ("%s: searching primary fingerprint failed: %s\n",
                 __func__, gpg_strerror (err));
      goto leave;
    }

  err = be_kbx_delete (ctrl, db->backend_handle, request);

 leave:
  release_lock (ctrl);
  if (DBG_CLOCK)
    log_clock ("%s: leave", __func__);
  return err;
}
