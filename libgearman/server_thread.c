/* Gearman server and library
 * Copyright (C) 2008 Brian Aker, Eric Day
 * All rights reserved.
 *
 * Use and distribution licensed under the BSD license.  See
 * the COPYING file in the parent directory for full text.
 */

/**
 * @file
 * @brief Server Thread Definitions
 */

#include "common.h"

/*
 * Private declarations
 */

/**
 * @addtogroup gearman_server_private Private Server Functions
 * @ingroup gearman_server
 * @{
 */

/**
 * Try reading packets for a connection.
 */
gearman_return_t _thread_packet_read(gearman_server_con_st *server_con);

/**
 * Flush outgoing packets for a connection.
 */
static gearman_return_t _thread_packet_flush(gearman_server_con_st *server_con);

/**
 * Start processing thread for the server.
 */
static gearman_return_t _proc_thread_start(gearman_server_st *server);

/**
 * Kill processing thread for the server.
 */
static void _proc_thread_kill(gearman_server_st *server);

/**
 * Processing thread.
 */
static void *_proc(void *data);

/**
 * Wrapper for log handling.
 */
static void _log(gearman_st *gearman __attribute__ ((unused)), uint8_t verbose,
                 const char *line, void *fn_arg);

/** @} */

/*
 * Public definitions
 */

gearman_server_thread_st * 
gearman_server_thread_create(gearman_server_st *server,
                             gearman_server_thread_st *thread)
{
  if (server->thread_count == 1)
  {
    /* The server is going to be multi-threaded, start processing thread. */
    if (_proc_thread_start(server) != GEARMAN_SUCCESS)
      return NULL;
  }

  if (thread == NULL)
  {
    thread= malloc(sizeof(gearman_server_thread_st));
    if (thread == NULL)
    {
      _proc_thread_kill(server);
      return NULL;
    }

    memset(thread, 0, sizeof(gearman_server_thread_st));
    thread->options|= GEARMAN_SERVER_THREAD_ALLOCATED;
  }
  else
    memset(thread, 0, sizeof(gearman_server_thread_st));

  thread->server= server;

  if (pthread_mutex_init(&(thread->lock), NULL) != 0)
  {
    if (thread->options & GEARMAN_SERVER_THREAD_ALLOCATED)
      free(thread);
    return NULL;
  }

  GEARMAN_LIST_ADD(server->thread, thread,)

  thread->gearman= gearman_create(&(thread->gearman_static));
  if (thread->gearman == NULL)
  {
    gearman_server_thread_free(thread);
    return NULL;
  }

  gearman_set_options(thread->gearman, GEARMAN_NON_BLOCKING, 1);
  gearman_set_options(thread->gearman, GEARMAN_DONT_TRACK_PACKETS, 1);

  return thread;
}

void gearman_server_thread_free(gearman_server_thread_st *thread)
{
  gearman_server_con_st *con;
  gearman_server_packet_st *packet;

  if (thread->server->thread_count > 1)
    _proc_thread_kill(thread->server);

  while (thread->con_list != NULL)
    gearman_server_con_free(thread->con_list);

  while (thread->free_con_list != NULL)
  {
    con= thread->free_con_list;
    thread->free_con_list= con->next;
    free(con);
  }

  while (thread->free_packet_list != NULL)
  {
    packet= thread->free_packet_list;
    thread->free_packet_list= packet->next;
    free(packet);
  }

  if (thread->gearman != NULL)
    gearman_free(thread->gearman);

  pthread_mutex_destroy(&(thread->lock));

  GEARMAN_LIST_DEL(thread->server->thread, thread,)

  if (thread->options & GEARMAN_SERVER_THREAD_ALLOCATED)
    free(thread);
}

const char *gearman_server_thread_error(gearman_server_thread_st *thread)
{
  return gearman_error(thread->gearman);
}

int gearman_server_thread_errno(gearman_server_thread_st *thread)
{
  return gearman_errno(thread->gearman);
}

void gearman_server_thread_set_event_watch(gearman_server_thread_st *thread,
                                           gearman_event_watch_fn *event_watch,
                                           void *event_watch_arg)
{
  gearman_set_event_watch(thread->gearman, event_watch, event_watch_arg);
}

void gearman_server_thread_set_run(gearman_server_thread_st *thread,
                                   gearman_server_thread_run_fn *run_fn, 
                                   void *run_arg)
{
  thread->run_fn= run_fn;
  thread->run_arg= run_arg;
}

void gearman_server_thread_set_log(gearman_server_thread_st *thread,
                                   gearman_server_thread_log_fn *log_fn, 
                                   void *log_fn_arg)
{
  thread->log_fn= log_fn;
  thread->log_fn_arg= log_fn_arg;
  gearman_set_log(thread->gearman, _log, thread);
}

gearman_server_con_st *
gearman_server_thread_run(gearman_server_thread_st *thread,
                          gearman_return_t *ret_ptr)
{
  gearman_con_st *con;
  gearman_server_con_st *server_con;

  /* If we are multi-threaded, we may have packets to flush or connections that
     should start reading again. */
  if (thread->server->thread_count > 1)
  {
    while ((server_con= gearman_server_con_io_next(thread)) != NULL)
    {
      if (server_con->options & GEARMAN_SERVER_CON_FREE)
      {
        gearman_server_con_free(server_con);
        continue;
      }

      if (server_con->ret != GEARMAN_SUCCESS)
      {
        *ret_ptr= server_con->ret;
        return server_con;
      }

      /* See if any outgoing packets were queued. */
      *ret_ptr= _thread_packet_flush(server_con);
      if (*ret_ptr != GEARMAN_SUCCESS && *ret_ptr != GEARMAN_IO_WAIT)
        return server_con;
    }
  }

  /* Check for new activity on connections. */
  while ((con= gearman_con_ready(thread->gearman)) != NULL)
  {
    /* Inherited classes anyone? Some people would call this a hack, I call
       it clean (avoids extra ptrs). Brian, I'll give you your C99 0-byte
       arrays at the ends of structs for this. :) */
    server_con= (gearman_server_con_st *)con;

    /* Try to read new packets. */
    if (con->revents & POLLIN)
    {
      *ret_ptr= _thread_packet_read(server_con);
      if (*ret_ptr != GEARMAN_SUCCESS && *ret_ptr != GEARMAN_IO_WAIT)
        return server_con;
    }

    /* Flush existing outgoing packets. */
    if (con->revents & POLLOUT)
    {
      *ret_ptr= _thread_packet_flush(server_con);
      if (*ret_ptr != GEARMAN_SUCCESS && *ret_ptr != GEARMAN_IO_WAIT)
        return server_con;
    }
  }

  /* Start flushing new outgoing packets if we are single threaded. */
  if (thread->server->thread_count == 1)
  {
    while ((server_con= gearman_server_con_io_next(thread)) != NULL)
    {
      *ret_ptr= _thread_packet_flush(server_con);
      if (*ret_ptr != GEARMAN_SUCCESS && *ret_ptr != GEARMAN_IO_WAIT)
        return server_con;
    }
  }

  /* Check for the two shutdown modes. */
  if (thread->server->shutdown)
    *ret_ptr= GEARMAN_SHUTDOWN;
  else if (thread->server->shutdown_graceful)
  {
    if (thread->server->job_count == 0)
      *ret_ptr= GEARMAN_SHUTDOWN;
    else
      *ret_ptr= GEARMAN_SHUTDOWN_GRACEFUL;
  }
  else
    *ret_ptr= GEARMAN_SUCCESS;

  return NULL;
}

/*
 * Private definitions
 */

gearman_return_t _thread_packet_read(gearman_server_con_st *server_con)
{
  gearman_return_t ret;

  while (1)
  {
    if (server_con->packet == NULL)
    {
      server_con->packet= gearman_server_packet_create(server_con->thread,
                                                       true);
      if (server_con->packet == NULL)
        return GEARMAN_MEMORY_ALLOCATION_FAILURE;
    }

    (void)gearman_con_recv(&(server_con->con), &(server_con->packet->packet),
                           &ret, true);
    if (ret != GEARMAN_SUCCESS)
    {
      if (ret == GEARMAN_IO_WAIT)
        break;

      return ret;
    }

    /* We read a complete packet. */
    if (server_con->thread->server->thread_count == 1)
    {
      /* Single threaded, run the command here. */
      ret= gearman_server_run_command(server_con,
                                      &(server_con->packet->packet));
      gearman_packet_free(&(server_con->packet->packet));
      gearman_server_packet_free(server_con->packet, server_con->thread, true);
      server_con->packet= NULL;
      if (ret != GEARMAN_SUCCESS)
        return ret;
    }
    else
    {
      /* Multi-threaded, queue for the processing thread to run. */
      gearman_server_proc_packet_add(server_con, server_con->packet);
      server_con->packet= NULL;
    }
  }

  return GEARMAN_SUCCESS;
}

static gearman_return_t _thread_packet_flush(gearman_server_con_st *server_con)
{
  gearman_return_t ret;

  /* Check to see if we've already tried to avoid excessive system calls. */
  if (server_con->con.events & POLLOUT)
    return GEARMAN_IO_WAIT;

  while (server_con->io_packet_list != NULL)
  {
    ret= gearman_con_send(&(server_con->con),
                          &(server_con->io_packet_list->packet),
                          server_con->io_packet_list->next == NULL ?
                          true : false);
    if (ret != GEARMAN_SUCCESS)
      return ret;

    if (server_con->io_packet_list->packet.command == GEARMAN_COMMAND_NOOP)
      server_con->noop_queued= false;

    gearman_server_io_packet_remove(server_con);
  }

  /* Clear the POLLOUT flag. */
  return gearman_con_set_events(&(server_con->con), POLLIN);
}

static gearman_return_t _proc_thread_start(gearman_server_st *server)
{
  pthread_attr_t attr;

  if (pthread_mutex_init(&(server->proc_lock), NULL) != 0)
    return GEARMAN_PTHREAD;

  if (pthread_cond_init(&(server->proc_cond), NULL) != 0)
    return GEARMAN_PTHREAD;

  if (pthread_attr_init(&attr) != 0)
    return GEARMAN_PTHREAD;

  if (pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM) != 0)
    return GEARMAN_PTHREAD;

  if (pthread_create(&(server->proc_id), &attr, _proc, server) != 0)
    return GEARMAN_PTHREAD;

  (void) pthread_attr_destroy(&attr);

  return GEARMAN_SUCCESS;
}

static void _proc_thread_kill(gearman_server_st *server)
{
  if (server->proc_shutdown)
    return;

  server->proc_shutdown= true;

  /* Signal proc thread to shutdown. */
  (void) pthread_mutex_lock(&(server->proc_lock));
  (void) pthread_cond_signal(&(server->proc_cond));
  (void) pthread_mutex_unlock(&(server->proc_lock));

  /* Wait for the proc thread to exit and then cleanup. */
  (void) pthread_join(server->proc_id, NULL);
  (void) pthread_cond_destroy(&(server->proc_cond));
  (void) pthread_mutex_destroy(&(server->proc_lock));
}

static void *_proc(void *data)
{
  gearman_server_st *server= (gearman_server_st *)data;
  gearman_server_thread_st *thread;
  gearman_server_con_st *con;
  gearman_server_packet_st *packet;

  while (1)
  {
    (void) pthread_mutex_lock(&(server->proc_lock));
    while (server->proc_wakeup == false)
    {
      if (server->proc_shutdown)
      {
        (void) pthread_mutex_unlock(&(server->proc_lock));
        return NULL;
      }

      (void) pthread_cond_wait(&(server->proc_cond), &(server->proc_lock));
    }
    server->proc_wakeup= false;
    (void) pthread_mutex_unlock(&(server->proc_lock));

    for (thread= server->thread_list; thread != NULL; thread= thread->next)
    {
      while ((con= gearman_server_con_proc_next(thread)) != NULL)
      {
        if (con->options & GEARMAN_SERVER_CON_DEAD)
        {
          gearman_server_con_free_workers(con);

          while (con->client_list != NULL)
            gearman_server_client_free(con->client_list);

          con->options|= GEARMAN_SERVER_CON_FREE;
          gearman_server_con_io_add(con);
          continue;
        }

        while (1)
        {
          packet= gearman_server_proc_packet_remove(con);
          if (packet == NULL)
            break;

          con->ret= gearman_server_run_command(con, &(packet->packet));
          gearman_packet_free(&(packet->packet));
          gearman_server_packet_free(packet, con->thread, false);
        }
      }
    }
  }
}

static void _log(gearman_st *gearman __attribute__ ((unused)), uint8_t verbose,
                 const char *line, void *fn_arg)
{
  gearman_server_thread_st *thread= (gearman_server_thread_st *)fn_arg;
  (*(thread->log_fn))(thread, verbose, line, thread->log_fn_arg);
}
