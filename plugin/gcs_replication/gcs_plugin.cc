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

#include <pthread.h>
#include "gcs_plugin.h"
#include "observer_server_state.h"
#include "observer_trans.h"
#include <sql_class.h>                          // THD
#include <gcs_replication.h>
#include "gcs_event_handlers.h"
#include "gcs_binding_factory.h"
#include "gcs_corosync_control_interface.h"

using std::string;

static MYSQL_PLUGIN plugin_info_ptr;

/* configuration related: */

ulong gcs_protocol_opt;

const char *available_bindings_names[]= {"COROSYNC", (char *)0};

TYPELIB gcs_protocol_typelib=
{ array_elements(available_bindings_names) - 1, "", available_bindings_names, NULL };

//Plugin related
char gcs_replication_group[UUID_LENGTH+1];
char gcs_replication_boot;
rpl_sidno gcs_cluster_sidno;

//Applier module related
ulong handler_pipeline_type;

//Recovery module related
char gcs_recovery_user[USERNAME_LENGTH + 1];
char *gcs_recovery_user_pointer= NULL;

/**
  Dummy variable associated to the recovery password sysvar making it never
  accessible.
  The real value resides in the below field
*/
char *gcs_dummy_recovery_password= NULL;

//Invisible. After Recovery consumes it will be nullified
char gcs_recovery_password[MAX_PASSWORD_LENGTH + 1];

ulong gcs_recovery_retry_count= 0;

//Generic components variables
ulong gcs_components_stop_timeout= LONG_TIMEOUT;

//Certification latch
Wait_ticket<my_thread_id> certification_latch;

//GCS module variables
char *gcs_group_pointer= NULL;
Gcs_interface *gcs_module= NULL;
Gcs_plugin_events_handler* events_handler= NULL;
Gcs_plugin_leave_notifier* leave_notifier= NULL;

int gcs_communication_event_handle= 0;
int gcs_control_event_handler= 0;
int gcs_control_exchanged_data_handle= 0;

/* end of conf */

//The plugin running flag and lock
static pthread_mutex_t gcs_running_mutex;
static bool gcs_running;
bool wait_on_engine_initialization= false;

//The plugin applier
Applier_module *applier_module= NULL;
char applier_relay_log_name[] = "sql_applier";
char applier_relay_log_info_name[]= "sql_applier_relay_log.info";
//The plugin recovery module
Recovery_module *recovery_module= NULL;

//Application management information
Cluster_member_info_manager_interface *cluster_member_mgr= NULL;
Cluster_member_info* local_member_info= NULL;

/*
  Internal auxiliary functions signatures.
*/
static int check_group_name_string(const char *str);

static bool init_cluster_sidno();

static bool server_engine_initialized();

char* get_last_certified_transaction(char* buf, rpl_gno last_seq_num);

enum enum_node_state
map_protocol_node_state_to_server_node_state
                             (Cluster_member_info::Cluster_member_status
                                                               protocol_status);

enum enum_applier_status
map_node_applier_state_to_server_applier_status(Member_applier_state
                                                              applier_status);

/*
  Auxiliary public functions.
*/
bool is_gcs_rpl_running()
{
  return gcs_running;
}

int log_message(enum plugin_log_level level, const char *format, ...)
{
  va_list args;
  char buff[1024];

  va_start(args, format);
  my_vsnprintf(buff, sizeof(buff), format, args);
  va_end(args);
  return my_plugin_log_message(&plugin_info_ptr, level, buff);
}

/*
  Plugin interface.
*/
struct st_mysql_gcs_rpl gcs_rpl_descriptor=
{
  MYSQL_GCS_REPLICATION_INTERFACE_VERSION,
  get_gcs_stats_info,
  get_gcs_nodes_info,
  get_gcs_node_stat_info,
  get_gcs_nodes_number,
  gcs_rpl_start,
  gcs_rpl_stop,
  is_gcs_rpl_running
};

bool get_gcs_stats_info(RPL_GCS_STATS_INFO *info)
{
  info->group_name= gcs_group_pointer;
  info->node_state= is_gcs_rpl_running();

  Gcs_view* view= NULL;
  Gcs_statistics_interface* stats_if= NULL;

  if(gcs_group_pointer != NULL)
  {
    string gcs_group_name(gcs_group_pointer);
    Gcs_group_identifier group_id(gcs_group_name);

    if(gcs_module != NULL)
    {
      Gcs_control_interface* ctrl_if= gcs_module->get_control_session(group_id);
      stats_if= gcs_module->get_statistics(group_id);

      if(ctrl_if != NULL)
      {
        view= ctrl_if->get_current_view();
      }
    }
  }

  if(view != NULL)
  {
    info->view_id= view->get_view_id()->get_view_id();
    info->number_of_nodes= view->get_members()->size();
  }
  else
  {
    info->view_id= 0;
    info->number_of_nodes= 0;
  }

  if(stats_if != NULL)
  {
    info->total_messages_sent= stats_if->get_total_messages_sent();
    info->total_bytes_sent= stats_if->get_total_bytes_sent();
    info->total_messages_received= stats_if->get_total_messages_received();
    info->total_bytes_received= stats_if->get_total_bytes_received();
    info->last_message_timestamp= stats_if->get_last_message_timestamp();
    info->min_message_length= stats_if->get_min_message_length();
    info->max_message_length= stats_if->get_max_message_length();
  }
  else
  {
    info->total_messages_sent= 0;
    info->total_bytes_sent= 0;
    info->total_messages_received= 0;
    info->total_bytes_received= 0;
    info->last_message_timestamp= 0;
    info->min_message_length= 0;
    info->max_message_length= 0;
  }

  return false;
}

bool get_gcs_nodes_info(uint index, RPL_GCS_NODES_INFO *info)
{
  /*
   This case means that the plugin has never been initialized...
   and one would not be able to extract information
   */
  if(cluster_member_mgr == NULL)
  {
    string empty_string("");

    info->group_name= gcs_group_pointer;
    info->node_id= my_strndup(
#ifdef HAVE_PSI_MEMORY_INTERFACE
                             PSI_NOT_INSTRUMENTED,
#endif
                             empty_string.c_str(),
                             empty_string.length(),
                             MYF(0));

    info->node_host= my_strndup(
#ifdef HAVE_PSI_MEMORY_INTERFACE
                             PSI_NOT_INSTRUMENTED,
#endif
                             empty_string.c_str(),
                             empty_string.length(),
                             MYF(0));

    info->node_port= 0;
    info->node_state= NODE_STATE_OFFLINE;

    return false;
  }

  Gcs_control_interface* ctrl_if= NULL;
  if(gcs_group_pointer != NULL)
  {
    info->group_name= gcs_group_pointer;

    string gcs_group_name(gcs_group_pointer);
    Gcs_group_identifier group_id(gcs_group_name);

    ctrl_if= gcs_module->get_control_session(group_id);
  }

  uint number_of_nodes= 0;
  if(ctrl_if != NULL)
  {
    Gcs_view* view= ctrl_if->get_current_view();
    if(view != NULL)
    {
      number_of_nodes= view->get_members()->size();
    }
  }
  else
  {
    number_of_nodes= 1;
  }

  if (index >= number_of_nodes) {
    if (index != 0) {
      // No nodes on view.
      return true;
    }
  }

  Cluster_member_info* node_info
                 = cluster_member_mgr->get_cluster_member_info_by_index(index);

  if(node_info == NULL) // The requested node is not managed...
  {
    return true;
  }

  // Get info from view.
    info->node_id= my_strndup(
#ifdef HAVE_PSI_MEMORY_INTERFACE
                             PSI_NOT_INSTRUMENTED,
#endif
                             node_info->get_uuid()->c_str(),
                             node_info->get_uuid()->length(),
                             MYF(0));

    info->node_host= my_strndup(
#ifdef HAVE_PSI_MEMORY_INTERFACE
                             PSI_NOT_INSTRUMENTED,
#endif
                             node_info->get_hostname()->c_str(),
                             node_info->get_hostname()->length(),
                             MYF(0));

  info->node_port= node_info->get_port();
  info->node_state=
      map_protocol_node_state_to_server_node_state(
          node_info->get_recovery_status());

  delete node_info;

  return false;
}

uint get_gcs_nodes_number()
{
  uint number_of_nodes= 0;

  if(gcs_group_pointer != NULL)
  {
    /*
      Even when node is disconnected from group there is the
      local node.
    */
    string gcs_group_name(gcs_group_pointer);
    Gcs_group_identifier group_id(gcs_group_name);

    Gcs_control_interface* ctrl_if= NULL;
    ctrl_if= gcs_module->get_control_session(group_id);

    Gcs_view* view= NULL;
    view= ctrl_if->get_current_view();

    if(view != NULL && ctrl_if != NULL && ctrl_if->belongs_to_group())
    {
      number_of_nodes= view->get_members()->size();
    }
  }

  return number_of_nodes == 0 ? 1 : number_of_nodes;
}

bool get_gcs_node_stat_info(RPL_GCS_NODE_STATS_INFO *info)
{
  //This means that the plugin never started
  if(cluster_member_mgr == NULL)
  {
    string empty_string("");
    info->node_id= empty_string.c_str();
  }
  else
  {
    char *hostname, *uuid;
    uint port;
    get_server_host_port_uuid(&hostname, &port, &uuid);

    info->node_id= uuid;
  }

  info->group_name= gcs_group_pointer;

  //Check if the gcs replication has started.
  if(applier_module)
  {
    Certification_handler *cert=
      applier_module->get_certification_handler();
    Certifier_interface *cert_module= cert->get_certifier();

    info->positively_certified= cert_module->get_positive_certified();
    info->negatively_certified= cert_module->get_negative_certified();
    info->transaction_certified= info->positively_certified +
                                 info->negatively_certified;
    info->certification_db_size= cert_module->get_cert_db_size();
    info->transaction_in_queue= applier_module->get_message_queue_size();

    Gtid_set* stable_gtid_set= cert_module->get_group_stable_transactions_set();

    if(stable_gtid_set)
      info->stable_set= stable_gtid_set->to_string();
    else
      info->stable_set= NULL;

    rpl_gno temp_seq_num= cert_module->get_last_sequence_number();
    char* buf= NULL;
    info->last_certified_transaction=
        get_last_certified_transaction(buf, temp_seq_num);

    info->applier_state=
        map_node_applier_state_to_server_applier_status(
            applier_module->get_applier_status());
  }
  else // gcs replication not running.
  {
    info->positively_certified= 0;
    info->negatively_certified= 0;
    info->transaction_certified= 0;
    info->certification_db_size= 0;
    info->stable_set= NULL;
    info->transaction_in_queue= 0;
    info->last_certified_transaction= NULL;
    info->applier_state=
        map_node_applier_state_to_server_applier_status(
            (Member_applier_state)APPLIER_STATE_OFF);
  }

  return false;
}

enum enum_node_state
map_protocol_node_state_to_server_node_state
                   (Cluster_member_info::Cluster_member_status protocol_status)
{
  switch(protocol_status)
  {
    case 1:  // MEMBER_ONLINE
      return NODE_STATE_ONLINE;
    case 2:  // MEMBER_OFFLINE
      return NODE_STATE_OFFLINE;
    case 3:  // MEMBER_IN_RECOVERY
      return NODE_STATE_RECOVERING;
    default:
      return NODE_STATE_OFFLINE;
  }
}

enum enum_applier_status
map_node_applier_state_to_server_applier_status(Member_applier_state
                                                                applier_status)
{
  switch(applier_status)
  {
    case 1:  // APPLIER_STATE_ON
      return APPLIER_STATE_RUNNING;
    case 2:  // APPLIER_STATE_OFF
      return APPLIER_STATE_STOP;
    default: // APPLIER_ERROR
      return APPLIER_STATE_ERROR;
  }
}

char* get_last_certified_transaction(char* buf, rpl_gno last_seq_num)
{
  if(last_seq_num > 0)
  {
    char seq_num[MAX_GNO_TEXT_LENGTH+1];
    char *last_cert_seq_num= my_safe_itoa(10, last_seq_num,
                                          &seq_num[sizeof(seq_num)- 1]);

    string group(gcs_group_pointer);
    group.append(":");
    group.append(last_cert_seq_num);
    buf= (char*)my_malloc(
#ifdef HAVE_PSI_MEMORY_INTERFACE
                               PSI_NOT_INSTRUMENTED,
#endif
                               Gtid::MAX_TEXT_LENGTH+1, MYF(0));

    memcpy(buf, group.c_str(), Gtid::MAX_TEXT_LENGTH+1);
  }
  return buf;
}

int gcs_rpl_start()
{
  Mutex_autolock a(&gcs_running_mutex);

  DBUG_ENTER("gcs_rpl_start");

  if (is_gcs_rpl_running())
    DBUG_RETURN(GCS_ALREADY_RUNNING);
  if (check_group_name_string(gcs_group_pointer))
    DBUG_RETURN(GCS_CONFIGURATION_ERROR);
  if (init_cluster_sidno())
    DBUG_RETURN(GCS_CONFIGURATION_ERROR);

  if(gcs_module->initialize())
  {
    DBUG_RETURN(GCS_CONFIGURATION_ERROR);
  }

  if (server_engine_initialized())
  {
    configure_cluster_member_manager();

    initialize_recovery_module();

    //we can only start the applier if the log has been initialized
    if (configure_and_start_applier_module())
      DBUG_RETURN(GCS_REPLICATION_APPLIER_INIT_ERROR);
  }
  else
  {
    wait_on_engine_initialization= true;
    DBUG_RETURN(0); //leave the decision for later
  }

  int error= 0;
  if ((error= configure_and_start_gcs()))
  {
    //terminate the before created pipeline
    log_message(MY_ERROR_LEVEL,
                "Error on gcs initialization methods, killing the applier");
    applier_module->terminate_applier_thread();
    DBUG_RETURN(error);
  }

  gcs_running= true;
  DBUG_RETURN(0); //All is OK
}

int configure_cluster_member_manager()
{
  //Retrieve local GCS information
  string group_id_str(gcs_group_pointer);
  Gcs_group_identifier group_id(group_id_str);
  Gcs_control_interface* gcs_ctrl= gcs_module->get_control_session(group_id);

  //Configure Cluster Member Manager
  char *hostname, *uuid;
  uint port;
  get_server_host_port_uuid(&hostname, &port, &uuid);

  if(local_member_info != NULL)
  {
    delete local_member_info;
  }

  local_member_info = new Cluster_member_info(hostname,
                                              port,
                                              uuid,
                                              gcs_ctrl->get_local_information(),
                                              Cluster_member_info::MEMBER_OFFLINE);

  //Create the membership info visible for the cluster
  if(cluster_member_mgr != NULL)
  {
    delete cluster_member_mgr;
  }

  cluster_member_mgr= new Cluster_member_info_manager(local_member_info);

  return 0;
}

int gcs_rpl_stop()
{
  Mutex_autolock a(&gcs_running_mutex);
  DBUG_ENTER("gcs_rpl_stop");

  if (!is_gcs_rpl_running())
    DBUG_RETURN(0);

  /* first leave all joined groups (currently one) */
  string gcs_group_name(gcs_group_pointer);
  Gcs_group_identifier group_id(gcs_group_name);

  Gcs_control_interface *ctrl_if=
                                gcs_module->get_control_session(group_id);
  Gcs_communication_interface *comm_if=
                                gcs_module->get_communication_session(group_id);

  leave_notifier->start_view_modification();

  if(ctrl_if->belongs_to_group())
  {
    if(ctrl_if->leave())
    {
      log_message(MY_WARNING_LEVEL,"Error leaving the group");
    }

    log_message(MY_INFORMATION_LEVEL, "going to wait for view modification");
    if(leave_notifier->wait_for_view_modification(10))
    {
      log_message(MY_WARNING_LEVEL,
                  "On shutdown there was a timeout receiving a view change."
                  "This can lead to a possible inconsistent state."
                  "Check the log for more details");
    }
  }

  //Unregister callbacks and destroy notifiers
  ctrl_if->remove_data_exchange_event_listener(gcs_control_event_handler);
  ctrl_if->remove_data_exchange_event_listener(gcs_control_exchanged_data_handle);
  comm_if->remove_event_listener(gcs_communication_event_handle);

  gcs_control_event_handler= 0;
  gcs_control_exchanged_data_handle= 0;
  gcs_communication_event_handle= 0;

  delete events_handler;
  delete leave_notifier;

  if(terminate_recovery_module())
  {
    //Do not trow an error since recovery is not vital, but warn either way
    log_message(MY_WARNING_LEVEL,
                "On shutdown there was a timeout on the recovery module "
                "termination. Check the log for more details");
  }

  /*
    The applier is only shutdown after the communication layer to avoid
    messages being delivered in the current view, but not applied
  */
  int error= 0;
  if((error= terminate_applier_module()))
    log_message(MY_ERROR_LEVEL,
                "On shutdown there was a timeout on the applier "
                "module termination.");

  /*
    Even if the applier did not terminate, let gcs_running be false
    as he can shutdown in the meanwhile.
  */

  gcs_module->finalize();

  gcs_running= false;
  DBUG_RETURN(error);
}

int gcs_replication_init(MYSQL_PLUGIN plugin_info)
{
  pthread_mutex_init(&gcs_running_mutex, NULL);
  plugin_info_ptr= plugin_info;
  if (init_gcs_rpl())
  {
    log_message(MY_ERROR_LEVEL,
                "Failure on GCS Cluster handler initialization");
    return 1;
  }

  if(register_server_state_observer(&server_state_observer, (void *)plugin_info_ptr))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure in GCS cluster during registering the server state observers");
    return 1;
  }

  if (register_trans_observer(&trans_observer, (void *)plugin_info_ptr))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure in GCS cluster during registering the transactions state observers");
    return 1;
  }

  if ((gcs_module=
         Gcs_binding_factory::get_gcs_implementation
                              ((plugin_gcs_bindings)gcs_protocol_opt)) == NULL)
  {
    log_message(MY_ERROR_LEVEL, "Failure in GCS protocol initialization");
    return 1;
  };

  if (gcs_replication_boot && start_gcs_rpl())
    return 1;

  return 0;
}

int gcs_replication_deinit(void *p)
{
  if (cleanup_gcs_rpl())
    return 1;

  Gcs_binding_factory::cleanup_gcs_implementation
                                       ((plugin_gcs_bindings)gcs_protocol_opt);

  if(cluster_member_mgr != NULL)
  {
    delete cluster_member_mgr;
    cluster_member_mgr= NULL;
  }

  if(local_member_info != NULL)
  {
    delete local_member_info;
    local_member_info= NULL;
  }

  if (unregister_server_state_observer(&server_state_observer, p))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure in GCS cluster during unregistering the server state observers");
    return 1;
  }

  if (unregister_trans_observer(&trans_observer, p))
  {
    log_message(MY_ERROR_LEVEL,
                "Failure in GCS cluster during unregistering the transactions state observers");
    return 1;
  }

  log_message(MY_INFORMATION_LEVEL,
              "The observers in GCS cluster have been successfully unregistered");

  pthread_mutex_destroy(&gcs_running_mutex);

  return 0;
}

static void update_boot(MYSQL_THD thd, SYS_VAR *var, void *ptr, const void *val)
{
  DBUG_ENTER("update_boot");

  *(char *)ptr= *(char *)val;
  gcs_replication_boot= *((char *) ptr);

  DBUG_VOID_RETURN;
}

static bool init_cluster_sidno()
{
  DBUG_ENTER("init_cluster_sid");
  rpl_sid cluster_sid;

  if (cluster_sid.parse(gcs_group_pointer) != RETURN_STATUS_OK)
    DBUG_RETURN(true);

  gcs_cluster_sidno = get_sidno_from_global_sid_map(cluster_sid);
  if (gcs_cluster_sidno <= 0)
    DBUG_RETURN(true);

  DBUG_RETURN(false);
}

void declare_plugin_running()
{
  gcs_running= true;
}

int configure_and_start_applier_module()
{
  DBUG_ENTER("configure_and_start_applier");

  int error= 0;

  //The applier did not stop properly or suffered a configuration error
  if (applier_module != NULL)
  {
    if ((error= applier_module->is_running())) //it is still running?
    {
      log_message(MY_ERROR_LEVEL,
                  "Cannot start the applier as a previous shutdown is still "
                  "running: The thread will stop once its task is complete.");
      DBUG_RETURN(error);
    }
    else
    {
      //clean a possible existent pipeline
      applier_module->terminate_applier_pipeline();
      //delete it and create from scratch
      delete applier_module;
    }
  }

  applier_module= new Applier_module();

  recovery_module->set_applier_module(applier_module);

  //For now, only defined pipelines are accepted.
  error=
    applier_module->setup_applier_module((Handler_pipeline_type)handler_pipeline_type,
                                         gcs_components_stop_timeout);
  if (error)
  {
    //Delete the possible existing pipeline
    applier_module->terminate_applier_pipeline();
    DBUG_RETURN(error);
  }

  if ((error= applier_module->initialize_applier_thread()))
  {
    log_message(MY_ERROR_LEVEL, "Unable to initialize the plugin applier module!");
    //clean a possible existent pipeline
    applier_module->terminate_applier_pipeline();
    delete applier_module;
    applier_module= NULL;
  }
  else
    log_message(MY_INFORMATION_LEVEL,
                "Event applier module successfully initialized!");

  DBUG_RETURN(error);
}

int terminate_applier_module()
{

  int error= 0;
  if (applier_module != NULL)
  {
    if (!applier_module->terminate_applier_thread()) //all goes fine
    {
      delete applier_module;
      applier_module= NULL;
    }
    else
    {
      error= ER_STOP_GCS_APPLIER_THREAD_TIMEOUT;
    }
  }
  return error;
}

int configure_and_start_gcs()
{
  //Create data to be exchanged here...
  string group_id_str(gcs_group_pointer);
  Gcs_group_identifier group_id(group_id_str);
  Gcs_control_interface* gcs_ctrl= gcs_module->get_control_session(group_id);

  gcs_ctrl
        ->set_exchangeable_data(cluster_member_mgr->get_exchangeable_format());

  leave_notifier= new Gcs_plugin_leave_notifier();
  events_handler= new Gcs_plugin_events_handler(applier_module,
                                                recovery_module,
                                                cluster_member_mgr,
                                                local_member_info,
                                                leave_notifier);

  gcs_control_event_handler= gcs_ctrl->add_event_listener(events_handler);
  gcs_control_exchanged_data_handle
                   = gcs_ctrl->add_data_exchange_event_listener(events_handler);

  //Set interfaces for Certifier
  Gcs_communication_interface *comm_if=
                            gcs_module->get_communication_session(group_id);

  gcs_communication_event_handle= comm_if->add_event_listener(events_handler);

  applier_module->get_certification_handler()->get_certifier()
                                       ->set_gcs_interfaces(comm_if, gcs_ctrl);
  applier_module->get_certification_handler()->get_certifier()
                                       ->set_local_node_info(local_member_info);

  if (gcs_ctrl->join())
  {
    return GCS_COMMUNICATION_LAYER_JOIN_ERROR;
  }

  return 0;
}

int initialize_recovery_module()
{
  string group_id_str(gcs_group_pointer);
  Gcs_group_identifier group_id(group_id_str);

  Gcs_communication_interface *comm_if=
                            gcs_module->get_communication_session(group_id);
  Gcs_control_interface *ctrl_if=
                            gcs_module->get_control_session(group_id);

  recovery_module = new Recovery_module(applier_module, comm_if,
                                        ctrl_if, local_member_info,
                                        cluster_member_mgr);

  recovery_module
              ->set_recovery_donor_connection_user(gcs_recovery_user_pointer);
  recovery_module
              ->set_recovery_donor_connection_password(&gcs_recovery_password[0]);
  return 0;
}

int terminate_recovery_module()
{
  int error= 0;
  if(recovery_module != NULL)
  {
    error = recovery_module->stop_recovery();

    delete recovery_module;

    recovery_module= NULL;
  }
  return error;
}

static bool server_engine_initialized(){
  return is_server_engine_ready();
}

static int check_group_name_string(const char *str)
{
  DBUG_ENTER("check_group_name_string");

  if (!str)
  {
    log_message(MY_ERROR_LEVEL, "The group name option is mandatory");
    DBUG_RETURN(1);
  }
  if (!Uuid::is_valid(str))
  {
    log_message(MY_ERROR_LEVEL, "The group name '%s' is not a valid UUID", str);
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}

static int check_group_name(MYSQL_THD thd, SYS_VAR *var, void* prt,
                            struct st_mysql_value *value)
{
  DBUG_ENTER("check_group_name");

  char buff[NAME_CHAR_LEN];
  const char *str;

  //safe_mutex_assert_owner(&gcs_running_mutex);
  if (is_gcs_rpl_running())
  {
    log_message(MY_ERROR_LEVEL,
                "The group name cannot be changed when cluster is running");
    DBUG_RETURN(1);
  }

  int length= sizeof(buff);
  str= value->val_str(value, buff, &length);
  if (check_group_name_string(str))
    DBUG_RETURN(1);

  *(const char**)prt= str;
  DBUG_RETURN(0);
}

static void update_group_name(MYSQL_THD thd, SYS_VAR *var, void *ptr, const
                              void *val)
{
  DBUG_ENTER("update_group_name");

  const char *newGroup= *(const char**)val;
  strncpy(gcs_replication_group, newGroup, UUID_LENGTH);
  gcs_group_pointer= &gcs_replication_group[0];

  DBUG_VOID_RETURN;
}

//Recovery module's module variable update/validate methods

static int check_recovery_con_user(MYSQL_THD thd, SYS_VAR *var, void* ptr,
                                   struct st_mysql_value *value)
{
  DBUG_ENTER("check_recovery_con_user");

  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;

  int length= sizeof(buff);
  str= value->val_str(value, buff, &length);

  if(strlen(str) > USERNAME_LENGTH)
  {
    log_message(MY_ERROR_LEVEL,
                "The given user name for recovery donor connection is to big");
    DBUG_RETURN(1);
  }

  *(const char**)ptr= str;
  DBUG_RETURN(0);
}


static void update_recovery_con_user(MYSQL_THD thd, SYS_VAR *var, void *ptr,
                                     const void *value)
{
  DBUG_ENTER("update_recovery_con_user");

  const char *new_user= *(const char**)value;
  strncpy(gcs_recovery_user, new_user, strlen(new_user)+1);
  gcs_recovery_user_pointer= &gcs_recovery_user[0];
  if (recovery_module != NULL)
  {
    recovery_module->set_recovery_donor_connection_user(new_user);
  }
  DBUG_VOID_RETURN;
}

static int check_recovery_con_password(MYSQL_THD thd, SYS_VAR *var, void* ptr,
                                       struct st_mysql_value *value)
{
  DBUG_ENTER("check_recovery_con_password");

  char buff[STRING_BUFFER_USUAL_SIZE];
  const char *str;

  int length= sizeof(buff);
  str= value->val_str(value, buff, &length);

  if(strlen(str) > MAX_PASSWORD_LENGTH)
  {
    log_message(MY_ERROR_LEVEL,
                "The given password for recovery donor connection is to big");
    DBUG_RETURN(1);
  }

  strncpy(gcs_recovery_password, str, strlen(str)+1);
  if (recovery_module != NULL)
  {
    recovery_module->set_recovery_donor_connection_password(str);
  }

  DBUG_RETURN(0);
}

static void update_recovery_con_password(MYSQL_THD thd, SYS_VAR *var, void *ptr,
                                         const void *value)
{
  DBUG_ENTER("update_recovery_con_password");
  DBUG_VOID_RETURN;
}

static void update_recovery_retry_count(MYSQL_THD thd, SYS_VAR *var, void *ptr,
                                        const void *value)
{
  DBUG_ENTER("update_recovery_retry_count");

  ulong in_val= *static_cast<const ulong*>(value);
  gcs_recovery_retry_count= in_val;

  if (recovery_module != NULL)
  {
    recovery_module->set_recovery_donor_retry_count(gcs_recovery_retry_count);
  }

  DBUG_VOID_RETURN;
}

//Component timeout update method

static void update_component_timeout(MYSQL_THD thd, SYS_VAR *var, void *ptr,
                                     const void *value)
{
  DBUG_ENTER("update_component_timeout");

  ulong in_val= *static_cast<const ulong*>(value);
  gcs_components_stop_timeout= in_val;

  if (applier_module != NULL)
  {
    applier_module->set_stop_wait_timeout(gcs_components_stop_timeout);
  }
  recovery_module->set_stop_wait_timeout(gcs_components_stop_timeout);

  DBUG_VOID_RETURN;
}

//Base plugin variables

static MYSQL_SYSVAR_BOOL(start_on_boot, gcs_replication_boot,
  PLUGIN_VAR_OPCMDARG,
  "Whether this server should start the group or not during bootstrap.",
  NULL,
  update_boot,
  0);

static MYSQL_SYSVAR_STR(group_name, gcs_group_pointer,
  PLUGIN_VAR_OPCMDARG,
  "The cluster name this server has joined.",
  check_group_name,
  update_group_name,
  NULL);

static const char* pipeline_names[]= { "STANDARD", NullS};

static TYPELIB pipeline_name_typelib_t= {
         array_elements(pipeline_names) - 1,
         "pipeline_name_typelib_t",
         pipeline_names,
         NULL
 };

//Applier module variables

static MYSQL_SYSVAR_ENUM(pipeline_type_var, handler_pipeline_type,
   PLUGIN_VAR_OPCMDARG,
   "pipeline types"
   "possible values are STANDARD",
   NULL, NULL, STANDARD_GCS_PIPELINE, &pipeline_name_typelib_t);

//GCS module variables

static MYSQL_SYSVAR_ENUM(gcs_protocol, gcs_protocol_opt,
  PLUGIN_VAR_OPCMDARG,
  "The name of GCS protocol to us.",
  NULL,
  NULL,
  COROSYNC,
  &gcs_protocol_typelib);

//Recovery module variables

static MYSQL_SYSVAR_STR(
  recovery_user,                              /* name */
  gcs_recovery_user_pointer,                  /* var */
  PLUGIN_VAR_OPCMDARG,                        /* optional var */
  "The user name of the account that recovery uses for the donor connection",
  check_recovery_con_user,                                       /* check func*/
  update_recovery_con_user,                   /* update func*/
  "root");                                    /* default*/

static MYSQL_SYSVAR_STR(
  recovery_password,                          /* name */
  gcs_dummy_recovery_password,                /* var */
  PLUGIN_VAR_OPCMDARG,                        /* optional var */
  "The password of the account that recovery uses for the donor connection",
  check_recovery_con_password,                /* check func*/
  update_recovery_con_password,               /* update func*/
  "");                                        /* default*/

static MYSQL_SYSVAR_ULONG(
  recovery_retry_count,              /* name */
  gcs_recovery_retry_count,          /* var */
  PLUGIN_VAR_OPCMDARG,               /* optional var */
  "The number of times that the joiner tries to connect to the donor before giving up.",
  NULL,                              /* check func. */
  update_recovery_retry_count,       /* update func. */
  0,                                 /* default */
  0,                                 /* min */
  LONG_TIMEOUT,                      /* max */
  0                                  /* block */
);

//Generic timeout setting

static MYSQL_SYSVAR_ULONG(
  components_stop_timeout,           /* name */
  gcs_components_stop_timeout,       /* var */
  PLUGIN_VAR_OPCMDARG,               /* optional var */
  "Timeout in seconds that the plugin waits for each of the components when shutting down.",
  NULL,                              /* check func. */
  update_component_timeout,          /* update func. */
  LONG_TIMEOUT,                      /* default */
  2,                                 /* min */
  LONG_TIMEOUT,                      /* max */
  0                                  /* block */
);

static SYS_VAR* gcs_system_vars[]= {
  MYSQL_SYSVAR(group_name),
  MYSQL_SYSVAR(start_on_boot),
  MYSQL_SYSVAR(pipeline_type_var),
  MYSQL_SYSVAR(gcs_protocol),
  MYSQL_SYSVAR(recovery_user),
  MYSQL_SYSVAR(recovery_password),
  MYSQL_SYSVAR(recovery_retry_count),
  MYSQL_SYSVAR(components_stop_timeout),
  NULL,
};

mysql_declare_plugin(gcs_repl_plugin)
{
  MYSQL_GCS_RPL_PLUGIN,
  &gcs_rpl_descriptor,
  "gcs_replication_plugin",
  "ORACLE",
  "GCS replication plugin",
  PLUGIN_LICENSE_GPL,
  gcs_replication_init,   /* Plugin Init */
  gcs_replication_deinit, /* Plugin Deinit */
  0x0100,                 /* 1.0 Plugin version*/
  NULL,                   /* status variables */
  gcs_system_vars,        /* system variables */
  NULL,                   /* config options */
  0,                      /* flags */
}
mysql_declare_plugin_end;
