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

#ifndef GCS_RECOVERY_INCLUDE
#define GCS_RECOVERY_INCLUDE

#include "gcs_plugin_utils.h"
#include "gcs_applier.h"
#include <gcs_protocol.h>
#include <applier_interfaces.h>
#include <gcs_replication.h>
#include <rpl_rli.h>
#include <list>

using GCS::Member_set;
using GCS::Member;
using std::list;

class Recovery_module
{

public:

  Recovery_module(Applier_module_interface *applier,
                  GCS::Protocol *com_protocol);

  ~Recovery_module();

  void set_applier_module(Applier_module_interface *applier)
  {
    applier_module= applier;
  }

  /**
    Starts the recovery process, initializing the recovery thread.
    This method is designed to be as light as possible, as if it involved any
    major computation or wait process that would block the view change process
    delaying the cluster.

    @note this method only returns when the recovery thread is already running

    @param group_name       the joiner's group name
    @param view_id          the new view id
    @param cluster_members  the new view members

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int start_recovery(const string& group_name,
                                 ulonglong view_id,
                                 Member_set& cluster_members);

  /**
   Checks to see if the recovery IO/SQL thread is still running, probably caused
   by an timeout on shutdown.
   If the threads are still running, we try to stop them again.
   If not possible, an error is reported.

   @return are the threads stopped
      @retval 0      All is stopped.
      @retval !=0    Threads are still running
  */
  int check_recovery_thread_status();

  /**
    Recovery thread main execution method.

    Here, the donor is selected, the connection to the donor is established,
    and several safe keeping assurances are guaranteed, such as the applier
    being suspended.
  */
  int recovery_thread_handle();

  /**
    Stops the recovery process, shutting down the recovery thread.
    If the thread does not stop in a user designated time interval, a timeout
    is issued.

    @note this method only returns when the thread is stopped or on timeout

    @return the operation status
      @retval 0      OK
      @retval !=0    Timeout
  */
  int stop_recovery();

  /**
    This method executes what action to take whenever a node exists the cluster.
    It can for the joiner:
      If it exited, then terminate the recovery process.
      If the donor left, and the state transfer is still ongoing, then pick a
      new one and restart the transfer.

    @param left             the members who left the view
    @param cluster_members  the current view members (for donor selection)

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
   */
  int update_recovery_process(Member_set& left, Member_set& cluster_members);


  int donor_failover();

  //Methods for variable updates

  /**Sets the user for the account used when connecting to a donor.*/
  void set_recovery_donor_connection_user(const char *user)
  {
    (void) strncpy(donor_connection_user, user, strlen(user)+1);
  }

   /**Sets the password for the account used when connecting to a donor.*/
  void set_recovery_donor_connection_password(const char *pass)
  {
    (void) strncpy(donor_connection_password, pass, strlen(pass)+1);
  }

  /**Sets the number of times recovery tries to connect to a given donor*/
  void set_recovery_donor_retry_count(ulong retry_count)
  {
    max_connection_attempts_to_donors= retry_count;
  }

  /**
    Sets the recovery shutdown timeout.

    @param[in]  timeout      the timeout
  */
  void set_stop_wait_timeout (ulong timeout){
    stop_wait_timeout= timeout;
  }


private:

   /**
     Selects a donor among the cluster nodes.
     Being open to several implementations, for now this method simply picks
     the first non joining node in the list.

     @return operation statue
       @retval 0   Donor found
       @retval 1   N suitable donor found
   */
   bool select_donor();

  /**
    Establish a master/slave connection to the selected donor.

    @param failover  failover to another a donor,
                     so only the IO is important

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int establish_donor_connection(bool failover= false);

  /**
    Initializes the structures for the donor connection threads.

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int initialize_donor_connection();

  /**
   Initializes the connection parameters for the donor connection.

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
   */
  void initialize_connection_parameters();

  /**
    Starts the recovery slave threads to receive data from the donor.

    @param failover  failover to another a donor,
                     so only the IO thread needs a restart

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int start_recovery_donor_threads(bool failover= false);

  /**
    Terminates the connection to the donor

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int terminate_recovery_slave_threads();

  /**
    Terminates the connection to the donor

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  void set_recovery_thread_context();

  /**
    Cleans the recovery thread related options/structures.
  */
  void clean_recovery_thread_context();

  /**
    Starts a wait process until the node fulfills the necessary condition to be
    acknowledge as being online.
    As of now, this condition is met when the applier module's queue size drops
    below a certain margin.
  */
  void wait_for_applier_module_recovery();

  /**
    Sends a message throughout the cluster acknowledge the node as online.
  */
  void notify_cluster_recovery_end();

 //recovery thread variables
  pthread_t recovery_pthd;
#ifdef HAVE_PSI_INTERFACE
  PSI_thread_key key_thread_recovery;
#endif
  THD *recovery_thd;

  /** The plugin's communication protocol instance  */
  GCS::Protocol *communication_proto;
  /* The plugin's applier module interface*/
  Applier_module_interface *applier_module;

  /* The group to which the recovering node belongs*/
  string group_name;
  /* The associated view id for the current recovery session */
  ulonglong view_id;
  /* The cluster members during recovery */
  Member_set member_set;
  /* The selected donor node */
  Member selected_donor;
  /* Donors who recovery could not connect */
  list<string> rejected_donors;
  /* Retry count on donor connections*/
  long donor_connection_retry_count;

  /* Recovery running flag */
  bool recovery_running;
  /* Recovery abort flag */
  bool recovery_aborted;
  /*  Flag that signals when the donor transfered all it's data */
  bool donor_transfer_finished;
  /* Are we successfully connected to a donor*/
  bool connected_to_donor;

  //Recovery connection related structures
  /** Interface class to interact with the donor connection threads*/
  Replication_thread_api donor_connection_interface;
  /** User defined rep user to be use on donor connection*/
  char donor_connection_user[USERNAME_LENGTH + 1];
  /** User defined password to be use on donor connection*/
  char donor_connection_password[MAX_PASSWORD_LENGTH + 1];

  //run conditions and locks
  mysql_mutex_t run_lock;
  mysql_cond_t  run_cond;
#ifdef HAVE_PSI_INTERFACE
  PSI_mutex_key run_mutex_key;
  PSI_cond_key  run_cond_key;
#endif

  /* The lock for the recovery wait condition */
  mysql_mutex_t recovery_lock;
  /* The condition for the recovery wait */
  mysql_cond_t recovery_condition;
#ifdef HAVE_PSI_INTERFACE
  PSI_mutex_key recovery_mutex_key;
  PSI_cond_key recovery_cond_key;
#endif

  mysql_mutex_t donor_selection_lock;
#ifdef HAVE_PSI_INTERFACE
  PSI_mutex_key donor_selection_mutex_key;
#endif

  /* Recovery module's timeout on shutdown */
  ulong stop_wait_timeout;
  /* Recovery max number of retries due to failures*/
  long max_connection_attempts_to_donors;
};

#endif /* GCS_RECOVERY_INCLUDE */
