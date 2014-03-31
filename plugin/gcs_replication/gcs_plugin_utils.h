/* Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#ifndef GCS_UTILS_INCLUDE
#define GCS_UTILS_INCLUDE

#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif
#ifndef HAVE_REPLICATION
#define HAVE_REPLICATION
#endif

#include <queue>
#include <string>
#include <my_global.h>
#include <my_sys.h>
#include <mysql/plugin.h>
#include <mysql/service_my_plugin_log.h>
#include <mysql/plugin_gcs_rpl.h>
#include <gcs_protocol.h>


int log_message(enum plugin_log_level level, const char *format, ...);


static void register_gcs_psi_keys(PSI_mutex_info gcs_mutexes[],
                                  int mutex_count,
                                  PSI_cond_info gcs_conds[],
                                  int cond_count);

template <typename T>
class Synchronized_queue
{
public:
  Synchronized_queue()
  {

    PSI_cond_info queue_conds[]=
    {
      { &key_cond, "COND_sync_queue_wait", 0}
    };

    PSI_mutex_info queue_mutexes[]=
    {
      { &key_mutex, "LOCK_sync_queue_wait", 0}
    };

    register_gcs_psi_keys(queue_mutexes, 1, queue_conds, 1);

    mysql_mutex_init(key_mutex, &lock, MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_cond, &cond, NULL);
  }

  bool empty()
  {
    bool res= true;
    mysql_mutex_lock(&lock);
    res= queue.empty();
    mysql_mutex_unlock(&lock);

    return res;
  }

  int push(const T& value)
  {
    mysql_mutex_lock(&lock);
    queue.push(value);
    mysql_mutex_unlock(&lock);
    mysql_cond_broadcast(&cond);
    return 0;
  }

  int pop(T* out)
  {
    *out= NULL;
    mysql_mutex_lock(&lock);
    while (queue.empty())
      mysql_cond_wait(&cond, &lock);
    *out= queue.front();
    queue.pop();
    mysql_mutex_unlock(&lock);

    return 0;
  }

  ulong size()
  {
    ulong qsize= 0;
    mysql_mutex_lock(&lock);
    qsize= queue.size();
    mysql_mutex_unlock(&lock);

    return qsize;
  }

private:
  mysql_mutex_t lock;
  mysql_cond_t cond;
  std::queue<T> queue;
#ifdef HAVE_PSI_INTERFACE
  PSI_mutex_key key_mutex;
  PSI_cond_key key_cond;
#endif
};


/*
  Register the psi keys for mutexes and conditions

  @param[in]  gcs_mutexes    PSI mutex info
  @param[in]  mutex_count    The number of elements in gcs_mutexes
  @param[in]  gcs_conds      PSI condition info
  @param[in]  cond_count     The number of elements in gcs_conds
*/
static void register_gcs_psi_keys(PSI_mutex_info gcs_mutexes[],
                                  int mutex_count,
                                  PSI_cond_info gcs_conds[],
                                  int cond_count)
{
  const char* category= "gcs";
  if (gcs_mutexes != NULL)
  {
    mysql_mutex_register(category, gcs_mutexes, mutex_count);
  }
  if (gcs_conds != NULL)
  {
    mysql_cond_register(category, gcs_conds, cond_count);
  }
}

class Mutex_autolock
{

public:
  Mutex_autolock(pthread_mutex_t *arg) : ptr_mutex(arg)
  {
    DBUG_ENTER("Mutex_autolock::Mutex_autolock");

    DBUG_ASSERT(arg != NULL);

    pthread_mutex_lock(ptr_mutex);
    DBUG_VOID_RETURN;
  }
  ~Mutex_autolock()
  {
      pthread_mutex_unlock(ptr_mutex);
  }

private:
  pthread_mutex_t *ptr_mutex;
  Mutex_autolock(Mutex_autolock const&); // no copies permitted
  void operator=(Mutex_autolock const&);
};

enum enum_node_state
map_protocol_node_state_to_server_node_state(GCS::Member_recovery_status protocol_status);

#endif /* GCS_UTILS_INCLUDE */
