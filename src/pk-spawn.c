/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 Richard Hughes <richard@hughsie.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <signal.h>

#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */

#include <sys/wait.h>
#include <fcntl.h>

#include <glib/gi18n.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-lowlevel.h>

#include "pk-debug.h"
#include "pk-spawn.h"
#include "pk-marshal.h"

static void     pk_spawn_class_init	(PkSpawnClass *klass);
static void     pk_spawn_init		(PkSpawn      *spawn);
static void     pk_spawn_finalize	(GObject       *object);

#define PK_SPAWN_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), PK_TYPE_SPAWN, PkSpawnPrivate))
#define PK_SPAWN_POLL_DELAY	100 /* ms */

struct PkSpawnPrivate
{
	gint			 child_pid;
	gint			 stderr_fd;
	gint			 stdout_fd;
	guint			 poll_id;
	GString			*stderr_buf;
	GString			*stdout_buf;
};

enum {
	PK_SPAWN_FINISHED,
	PK_SPAWN_STDERR,
	PK_SPAWN_STDOUT,
	PK_SPAWN_LAST_SIGNAL
};

static guint	     signals [PK_SPAWN_LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (PkSpawn, pk_spawn, G_TYPE_OBJECT)

/**
 * pk_spawn_read_fd_into_buffer:
 **/
static gboolean
pk_spawn_read_fd_into_buffer (gint fd, GString *string)
{
	gint bytes_read;
	gchar buffer[1024];

	/* read as much as we can */
	while ((bytes_read = read (fd, buffer, 1023)) > 0) {
		buffer[bytes_read] = '\0';
		g_string_append (string, buffer);
	}

	return TRUE;
}

/**
 * pk_spawn_emit_whole_lines:
 **/
static gboolean
pk_spawn_emit_whole_lines (PkSpawn *spawn, GString *string, gboolean is_stdout)
{
	guint i;
	guint size;
	gchar **lines;
	gchar *message;
	guint bytes_processed;

	if (strlen (string->str) == 0) {
		return FALSE;
	}

	/* split into lines - the las line may be incomplete */
	lines = g_strsplit (string->str, "\n", 0);
	if (lines == NULL) {
		return FALSE;
	}

	/* find size */
	for (size=0; lines[size]; size++);

	bytes_processed = 0;
	/* we only emit n-1 strings */
	for (i=0; i<(size-1); i++) {
		message = g_locale_to_utf8 (lines[i], -1, NULL, NULL, NULL);
		if (is_stdout == TRUE) {
			pk_debug ("emitting stdout %s", message);
			g_signal_emit (spawn, signals [PK_SPAWN_STDOUT], 0, message);
		} else {
			pk_debug ("emitting stderr %s", message);
			g_signal_emit (spawn, signals [PK_SPAWN_STDERR], 0, message);
		}
		g_free (message);
		bytes_processed += strlen (lines[i]) + 1;
	}

	/* remove the text we've processed */
	g_string_erase (string, 0, bytes_processed);

	g_strfreev (lines);
	return TRUE;
}

/**
 * pk_spawn_check_child:
 **/
static gboolean
pk_spawn_check_child (PkSpawn *spawn)
{
	int status;

	pk_spawn_read_fd_into_buffer (spawn->priv->stdout_fd, spawn->priv->stdout_buf);
	pk_spawn_read_fd_into_buffer (spawn->priv->stderr_fd, spawn->priv->stderr_buf);
	pk_spawn_emit_whole_lines (spawn, spawn->priv->stdout_buf, TRUE);
	pk_spawn_emit_whole_lines (spawn, spawn->priv->stderr_buf, FALSE);

	/* check if the child exited */
	if (waitpid (spawn->priv->child_pid, &status, WNOHANG) != spawn->priv->child_pid)
		return TRUE;

	/* child exited, display some information... */
	close (spawn->priv->stderr_fd);
	close (spawn->priv->stdout_fd);

	if (WEXITSTATUS (status) > 0) {
		pk_warning ("Running fork failed with return value %d", WEXITSTATUS (status));
	} else {
		pk_debug ("Running fork successful");
	}

	pk_debug ("emitting finished %i", WEXITSTATUS (status));
	g_signal_emit (spawn, signals [PK_SPAWN_FINISHED], 0, WEXITSTATUS (status));

	return FALSE;
}

/**
 * pk_spawn_kill:
 *
 * THIS IS A VERY DANGEROUS THING TO DO!
 *
 **/
gboolean
pk_spawn_kill (PkSpawn *spawn)
{
	gint retval;
	guint ret;
	pk_warning ("killing %i", spawn->priv->child_pid);
	retval = kill (spawn->priv->child_pid, SIGKILL);

	ret = TRUE;
	if (retval == EINVAL) {
		pk_warning ("The signum argument is an invalid or unsupported number");
		ret = FALSE;
	} else if (retval == EPERM) {
		pk_warning ("You do not have the privilege to send a signal to the process");
		ret = FALSE;
	}
	return ret;
}

/**
 * pk_spawn_command:
 **/
gboolean
pk_spawn_command (PkSpawn *spawn, const gchar *command)
{
	gboolean ret;
	gchar **argv;

	g_return_val_if_fail (spawn != NULL, FALSE);
	g_return_val_if_fail (PK_IS_SPAWN (spawn), FALSE);

	if (command == NULL) {
		pk_warning ("command NULL");
		return FALSE;
	}

	pk_debug ("command '%s'", command);

	/* split command line */
	argv = g_strsplit (command, " ", 0);

	/* create spawned object for tracking */
	ret = g_spawn_async_with_pipes (NULL, argv, NULL,
				 G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
				 NULL, NULL, &spawn->priv->child_pid,
				 NULL, /* stdin */
				 &spawn->priv->stdout_fd,
				 &spawn->priv->stderr_fd,
				 NULL);
	g_strfreev (argv);

	/* we failed to invoke the helper */
	if (ret == FALSE) {
		pk_warning ("failed to spawn '%s'", command);
		return FALSE;
	}

	/* install an idle handler to check if the child returnd successfully. */
	fcntl (spawn->priv->stdout_fd, F_SETFL, O_NONBLOCK);
	fcntl (spawn->priv->stderr_fd, F_SETFL, O_NONBLOCK);

	/* poll quickly */
	spawn->priv->poll_id = g_timeout_add (PK_SPAWN_POLL_DELAY, (GSourceFunc) pk_spawn_check_child, spawn);

	return TRUE;
}

/**
 * pk_spawn_class_init:
 * @klass: The PkSpawnClass
 **/
static void
pk_spawn_class_init (PkSpawnClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);

	object_class->finalize = pk_spawn_finalize;

	signals [PK_SPAWN_FINISHED] =
		g_signal_new ("finished",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE, 1, G_TYPE_INT);
	signals [PK_SPAWN_STDOUT] =
		g_signal_new ("stdout",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);
	signals [PK_SPAWN_STDERR] =
		g_signal_new ("stderr",
			      G_TYPE_FROM_CLASS (object_class), G_SIGNAL_RUN_LAST,
			      0, NULL, NULL, g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1, G_TYPE_STRING);

	g_type_class_add_private (klass, sizeof (PkSpawnPrivate));
}

/**
 * pk_spawn_init:
 * @spawn: This class instance
 **/
static void
pk_spawn_init (PkSpawn *spawn)
{
	spawn->priv = PK_SPAWN_GET_PRIVATE (spawn);

	spawn->priv->child_pid = -1;
	spawn->priv->stderr_fd = -1;
	spawn->priv->stdout_fd = -1;
	spawn->priv->poll_id = 0;

	spawn->priv->stderr_buf = g_string_new ("");
	spawn->priv->stdout_buf = g_string_new ("");
}

/**
 * pk_spawn_finalize:
 * @object: The object to finalize
 **/
static void
pk_spawn_finalize (GObject *object)
{
	PkSpawn *spawn;

	g_return_if_fail (object != NULL);
	g_return_if_fail (PK_IS_SPAWN (object));

	spawn = PK_SPAWN (object);

	g_return_if_fail (spawn->priv != NULL);

	/* disconnect the poll in case we were cancelled before completion */
	g_source_remove (spawn->priv->poll_id);

	/* free the buffers */
	g_string_free (spawn->priv->stderr_buf, TRUE);
	g_string_free (spawn->priv->stdout_buf, TRUE);

	G_OBJECT_CLASS (pk_spawn_parent_class)->finalize (object);
}

/**
 * pk_spawn_new:
 *
 * Return value: a new PkSpawn object.
 **/
PkSpawn *
pk_spawn_new (void)
{
	PkSpawn *spawn;
	spawn = g_object_new (PK_TYPE_SPAWN, NULL);
	return PK_SPAWN (spawn);
}

/***************************************************************************
 ***                          MAKE CHECK TESTS                           ***
 ***************************************************************************/
#ifdef PK_BUILD_TESTS
#include <libselftest.h>

static GMainLoop *loop;

/**
 * pk_test_finished_cb:
 **/
static void
pk_test_finished_cb (PkSpawn *spawn, gint exitcode, LibSelfTest *test)
{
	pk_debug ("spawn exitcode=%i", exitcode);
	g_main_loop_quit (loop);
}

/**
 * pk_test_stdout_cb:
 **/
static void
pk_test_stdout_cb (PkSpawn *spawn, const gchar *line, LibSelfTest *test)
{
	pk_debug ("stdout '%s'", line);
}

/**
 * pk_test_stderr_cb:
 **/
static void
pk_test_stderr_cb (PkSpawn *spawn, const gchar *line, LibSelfTest *test)
{
	pk_debug ("stderr '%s'", line);
}

void
libst_spawn (LibSelfTest *test)
{
	PkSpawn *spawn;
	gboolean ret;

	if (libst_start (test, "PkSpawn", CLASS_AUTO) == FALSE) {
		return;
	}

	spawn = pk_spawn_new ();
	g_signal_connect (spawn, "finished",
			  G_CALLBACK (pk_test_finished_cb), test);
	g_signal_connect (spawn, "stdout",
			  G_CALLBACK (pk_test_stdout_cb), test);
	g_signal_connect (spawn, "stderr",
			  G_CALLBACK (pk_test_stderr_cb), test);

	/************************************************************/
	libst_title (test, "make sure return error for missing file");
	ret = pk_spawn_command (spawn, "./pk-spawn-test-xxx.sh");
	if (ret == FALSE) {
		libst_success (test, "failed to run invalid file");
	} else {
		libst_failed (test, "ran incorrect file");
	}

#if 0
	/************************************************************/
	libst_title (test, "make sure run correct helper");
	ret = pk_spawn_command (spawn, "./pk-spawn-test.sh");
	if (ret == TRUE) {
		libst_success (test, "ran correct file");
	} else {
		libst_failed (test, "did not run helper");
	}
#endif

#if 0
	/* spin for a bit */
	loop = g_main_loop_new (NULL, FALSE);
	g_main_loop_run (loop);
#endif

	g_object_unref (spawn);

	libst_end (test);
}
#endif

