/*
 * Copyright (C) 2010 Robert Ancell.
 * Author: Robert Ancell <robert.ancell@canonical.com>
 * 
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version. See http://www.gnu.org/copyleft/gpl.html the full text of the
 * license.
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <pwd.h>
#include <errno.h>
#include <string.h>

#include "session.h"

enum {
    PROP_0,
    PROP_CONFIG,
    PROP_USERNAME,
    PROP_COMMAND
};

enum {
    EXITED,
    LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

struct SessionPrivate
{
    GKeyFile *config;

    /* User running this session */
    gchar *username;

    /* Environment variables */
    GHashTable *env;

    /* Command to run for this session */
    gchar *command;

    /* Session process */
    GPid pid;
};

G_DEFINE_TYPE (Session, session, G_TYPE_OBJECT);

Session *
session_new (GKeyFile *config, const char *username, const char *command)
{
    return g_object_new (SESSION_TYPE, "config", config, "username", username, "command", command, NULL);
}

const gchar *
session_get_username (Session *session)
{
    return session->priv->username;
}

const gchar *
session_get_command (Session *session)
{
    return session->priv->command;
}

void
session_set_env (Session *session, const gchar *name, const gchar *value)
{
    g_hash_table_insert (session->priv->env, g_strdup (name), g_strdup (value));
}

static void
session_watch_cb (GPid pid, gint status, gpointer data)
{
    Session *session = data;

    if (WIFEXITED (status))
        g_debug ("Session exited with return value %d", WEXITSTATUS (status));
    else if (WIFSIGNALED (status))
        g_debug ("Session terminated with signal %d", WTERMSIG (status));

    session->priv->pid = 0;

    g_signal_emit (session, signals[EXITED], 0);
}

static void
session_fork_cb (gpointer data)
{
    struct passwd *user_info = data;
  
    if (setgid (user_info->pw_gid) != 0)
    {
        g_warning ("Failed to set group ID: %s", strerror (errno));
        _exit(1);
    }
    // FIXME: Is there a risk of connecting to the process for a user in the given group and accessing memory?
    if (setuid (user_info->pw_uid) != 0)
    {
        g_warning ("Failed to set user ID: %s", strerror (errno));
        _exit(1);
    }
    if (chdir (user_info->pw_dir) != 0)
        g_warning ("Failed to change directory: %s", strerror (errno));
}

static gchar **session_get_env (Session *session)
{
    gchar **env;
    gpointer key, value;
    GHashTableIter iter;
    gint i = 0;

    env = g_malloc (sizeof (gchar *) * (g_hash_table_size (session->priv->env) + 1));
    g_hash_table_iter_init (&iter, session->priv->env);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        // FIXME: Do these need to be freed?
        env[i] = g_strdup_printf("%s=%s", (gchar *)key, (gchar *)value);
        i++;
    }
    env[i] = NULL;

    return env;
}

gboolean
session_start (Session *session)
{
    struct passwd *user_info;
    //gint session_stdin, session_stdout, session_stderr;
    gboolean result;
    gint argc;
    gchar **argv, **env;
    gchar *env_string;
    GError *error = NULL;

    g_return_val_if_fail (session->priv->pid == 0, FALSE);

    errno = 0;
    user_info = getpwnam (session->priv->username);
    if (!user_info)
    {
        if (errno == 0)
            g_warning ("Unable to get information on user %s: User does not exist", session->priv->username);
        else
            g_warning ("Unable to get information on user %s: %s", session->priv->username, strerror (errno));
        return FALSE;
    }
  
    session_set_env (session, "USER", user_info->pw_name);
    session_set_env (session, "HOME", user_info->pw_dir);
    session_set_env (session, "SHELL", user_info->pw_shell);

    env = session_get_env (session);

    result = g_shell_parse_argv (session->priv->command, &argc, &argv, &error);
    if (!result)
        g_error ("Failed to parse session command line: %s", error->message);
    g_clear_error (&error);
    if (!result)
        return FALSE;

    env_string = g_strjoinv (" ", env);
    g_debug ("Launching greeter: %s %s", env_string, session->priv->command);
    g_free (env_string);

    result = g_spawn_async/*_with_pipes*/ (user_info->pw_dir,
                                       argv,
                                       env,
                                       G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH,
                                       session_fork_cb, user_info,
                                       &session->priv->pid,
                                       //&session_stdin, &session_stdout, &session_stderr,
                                       &error);

    if (!result)
        g_warning ("Failed to spawn session: %s", error->message);
    else
        g_child_watch_add (session->priv->pid, session_watch_cb, session);
    g_clear_error (&error);

    return session->priv->pid != 0;
}

static void
session_init (Session *session)
{
    session->priv = G_TYPE_INSTANCE_GET_PRIVATE (session, SESSION_TYPE, SessionPrivate);
    session->priv->env = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

static void
session_set_property(GObject      *object,
                     guint         prop_id,
                     const GValue *value,
                     GParamSpec   *pspec)
{
    Session *self;

    self = SESSION (object);

    switch (prop_id) {
    case PROP_CONFIG:
        self->priv->config = g_value_get_pointer (value);
        break;
    case PROP_USERNAME:
        self->priv->username = g_strdup (g_value_get_string (value));
        break;
    case PROP_COMMAND:
        self->priv->command = g_strdup (g_value_get_string (value));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}


static void
session_get_property(GObject    *object,
                     guint       prop_id,
                     GValue     *value,
                     GParamSpec *pspec)
{
    Session *self;

    self = SESSION (object);

    switch (prop_id) {
    case PROP_CONFIG:
        g_value_set_pointer (value, self->priv->config);
        break;
    case PROP_USERNAME:
        g_value_set_string (value, self->priv->username);
        break;
    case PROP_COMMAND:
        g_value_set_string (value, self->priv->command);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
        break;
    }
}

static void
session_class_init (SessionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->set_property = session_set_property;
    object_class->get_property = session_get_property;

    g_type_class_add_private (klass, sizeof (SessionPrivate));

    g_object_class_install_property (object_class,
                                     PROP_CONFIG,
                                     g_param_spec_pointer ("config",
                                                           "config",
                                                           "Configuration",
                                                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
    g_object_class_install_property (object_class,
                                     PROP_USERNAME,
                                     g_param_spec_string ("username",
                                                          "username",
                                                          "Session user",
                                                          NULL,
                                                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));
    g_object_class_install_property (object_class,
                                     PROP_COMMAND,
                                     g_param_spec_string ("command",
                                                          "command",
                                                          "Session executable command",
                                                          NULL,
                                                          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

    signals[EXITED] =
        g_signal_new ("exited",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      G_STRUCT_OFFSET (SessionClass, exited),
                      NULL, NULL,
                      g_cclosure_marshal_VOID__VOID,
                      G_TYPE_NONE, 0);
}
