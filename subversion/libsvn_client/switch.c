/*
 * switch.c:  implement 'switch' feature via wc & ra interfaces.
 *
 * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

/* ==================================================================== */



/*** Includes. ***/

#include <assert.h>

#include "svn_wc.h"
#include "svn_client.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "client.h"



/*** Code. ***/

/* This feature is essentially identical to 'svn update' (see
   ./update.c), but with two differences:

     - the reporter->finish_report() routine needs to make the server
       run delta_dirs() on two *different* paths, rather than on two
       identical paths.

     - after the update runs, we need to more than just
       ensure_uniform_revision;  we need to rewrite all the entries'
       URL attributes.
*/


svn_error_t *
svn_client_switch (const svn_delta_edit_fns_t *before_editor,
                   void *before_edit_baton,
                   const svn_delta_edit_fns_t *after_editor,
                   void *after_edit_baton,
                   svn_client_auth_baton_t *auth_baton,
                   svn_stringbuf_t *path,
                   svn_stringbuf_t *switch_url,
                   const svn_client_revision_t *revision,
                   svn_boolean_t recurse,
                   svn_wc_notify_func_t notify_func,
                   void *notify_baton,
                   apr_pool_t *pool)
{
  const svn_delta_edit_fns_t *switch_editor;
  void *switch_edit_baton;
  const svn_ra_reporter_t *reporter;
  void *report_baton;
  svn_wc_entry_t *entry, *session_entry;
  svn_stringbuf_t *URL, *anchor, *target;
  void *ra_baton, *session;
  svn_ra_plugin_t *ra_lib;
  svn_revnum_t revnum;
  svn_error_t *err = NULL;

  /* Sanity check.  Without these, the switch is meaningless. */
  assert (path != NULL);
  assert (path->len > 0);
  assert (switch_url != NULL);
  assert (switch_url->len > 0);

  SVN_ERR (svn_wc_entry (&entry, path, pool));
  if (! entry)
    return svn_error_createf
      (SVN_ERR_WC_PATH_NOT_FOUND, 0, NULL, pool,
       "svn_client_update: %s is not under revision control", path->data);

  if (entry->kind == svn_node_file)
    {
      SVN_ERR (svn_wc_get_actual_target (path, &anchor, &target, pool));
      
      /* get the parent entry */
      SVN_ERR (svn_wc_entry (&session_entry, anchor, pool));
      if (! entry)
        return svn_error_createf
          (SVN_ERR_WC_PATH_NOT_FOUND, 0, NULL, pool,
           "svn_client_update: %s is not under revision control", path->data);
    }
  else if (entry->kind == svn_node_dir)
    {
      /* Unlike 'svn up', we do *not* split the local path into an
         anchor/target pair.  We do this because we know that the
         target isn't going to be deleted, because we're doing a
         switch.  This means the update editor gets anchored on PATH
         itself, and thus PATH's name will never change, which is
         exactly what we want. */
      anchor = path;
      target = NULL;
      session_entry = entry;
    }

  if (! session_entry->url)
    return svn_error_createf
      (SVN_ERR_ENTRY_MISSING_URL, 0, NULL, pool,
       "svn_client_switch: entry '%s' has no URL", path->data);
  URL = svn_stringbuf_dup (session_entry->url, pool);

  /* Get revnum set to something meaningful, so we can fetch the
     switch editor. */
  if (revision->kind == svn_client_revision_number)
    revnum = revision->value.number; /* do the trivial conversion manually */
  else
    revnum = SVN_INVALID_REVNUM; /* no matter, do real conversion later */

  /* Get the RA vtable that matches working copy's current URL. */
  SVN_ERR (svn_ra_init_ra_libs (&ra_baton, pool));
  SVN_ERR (svn_ra_get_ra_library (&ra_lib, ra_baton, URL->data, pool));
    
  if (entry->kind == svn_node_dir)
    {
      /* Open an RA session to 'source' URL */
      SVN_ERR (svn_client__open_ra_session (&session, ra_lib, URL, path,
                                            TRUE, TRUE, auth_baton, pool));
      SVN_ERR (svn_client__get_revision_number
               (&revnum, ra_lib, session, revision, path->data, pool));

      /* Fetch the switch (update) editor.  If REVISION is invalid, that's
         okay; the RA driver will call editor->set_target_revision() later
         on. */
      SVN_ERR (svn_wc_get_switch_editor (anchor, target,
                                         revnum, switch_url, recurse,
                                         &switch_editor, &switch_edit_baton,
                                         pool));
      
      /* Wrap it up with outside editors. */
      svn_delta_wrap_editor (&switch_editor, &switch_edit_baton,
                             before_editor, before_edit_baton,
                             switch_editor, switch_edit_baton,
                             after_editor, after_edit_baton, pool);

      /* Tell RA to do a update of URL+TARGET to REVISION; if we pass an
         invalid revnum, that means RA will use the latest revision. */
      SVN_ERR (ra_lib->do_switch (session,
                                  &reporter, &report_baton,
                                  revnum,
                                  target,
                                  recurse,
                                  switch_url,
                                  switch_editor, switch_edit_baton));
      
      /* Drive the reporter structure, describing the revisions within
         PATH.  When we call reporter->finish_report, the
         update_editor will be driven by svn_repos_dir_delta. */ 
      err = svn_wc_crawl_revisions (path, reporter, report_baton,
                                    TRUE, recurse,
                                    notify_func, notify_baton,
                                    pool);
    }
  
  else if (entry->kind == svn_node_file)
    {
      /* If switching a single file, just fetch the file directly and
         "install" it into the working copy just like the
         update-editor does.  */

      apr_array_header_t *proparray;
      apr_hash_t *prophash;
      apr_hash_index_t *hi;
      apr_file_t *fp;
      svn_stringbuf_t *new_text_path;
      svn_stream_t *file_stream;
      svn_revnum_t fetched_rev = 1; /* this will be set by get_file() */

      /* Create a unique file */
      SVN_ERR (svn_io_open_unique_file (&fp, &new_text_path,
                                        path, ".new-text-base",
                                        FALSE, /* don't delete on close */
                                        pool));

      /* Create a generic stream that operates on this file.  */
      file_stream = svn_stream_from_aprfile (fp, pool);

      /* Open an RA session to 'target' file URL. */      
      SVN_ERR (svn_client__open_ra_session (&session, ra_lib, switch_url, path,
                                            TRUE, TRUE, auth_baton, pool));
      SVN_ERR (svn_client__get_revision_number
               (&revnum, ra_lib, session, revision, path->data, pool));

      /* Passing "" as a relative path, since we opened the file URL.
         This pushes the text of the file into our file_stream, which
         means it ends up in our unique tmpfile.  We also get the full
         proplist. */
      SVN_ERR (ra_lib->get_file (session, "", revnum, file_stream,
                                 &fetched_rev, &prophash));
      SVN_ERR (svn_stream_close (file_stream));

      /* Convert the prophash into an array, which is what
         svn_wc_install_file (and its helpers) want.  */
      proparray = apr_array_make (pool, 1, sizeof(svn_prop_t));
      for (hi = apr_hash_first (pool, prophash); hi; hi = apr_hash_next (hi))
        {
          const void *key;
          apr_ssize_t klen;
          void *val;
          svn_prop_t *prop;
          
          apr_hash_this (hi, &key, &klen, &val);

          prop = apr_array_push (proparray);
          prop->name = (const char *) key;
          prop->value = svn_string_create_from_buf ((svn_stringbuf_t *) val,
                                                    pool);
        }

      /* This the same code as the update-editor's close_file(). */
      SVN_ERR (svn_wc_install_file (path->data, revnum,
                                    new_text_path->data,
                                    proparray, TRUE, /* is full proplist */
                                    switch_url->data, /* new url */
                                    pool));
      
      /* ### shouldn't the user see a 'U' somehow?  we have no trace
         editor here!  Think about this... */
    }  
  
  /* Sleep for one second to ensure timestamp integrity. */
  apr_sleep (APR_USEC_PER_SEC * 1);

  if (err)
    return err;

  /* Close the RA session. */
  SVN_ERR (ra_lib->close (session));

  return SVN_NO_ERROR;
}



/* 
 * local variables:
 * eval: (load-file "../../tools/dev/svn-dev.el")
 * end: */
