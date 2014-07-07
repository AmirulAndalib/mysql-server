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

#ifndef GCS_PLUGIN_INCLUDE
#define GCS_PLUGIN_INCLUDE

#include "gcs_plugin_utils.h" //defines HAVE_REPLICATION and MYSQL_SERVER flags
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "my_global.h"
#include <mysql/plugin.h>
#include <mysql/plugin_gcs_rpl.h>
#include <../include/mysql_com.h>
#include <mysqld.h>               // UUID_LENGTH
#include <rpl_gtid.h>             // rpl_sidno
#include "gcs_applier.h"
#include "gcs_recovery.h"


#include "gcs_interface.h"
#include "gcs_event_handlers.h"


/*
  Plugin errors
  TODO: If this goes into the server side, replace this with server errors.
*/
#define GCS_CONFIGURATION_ERROR 1
#define GCS_ALREADY_RUNNING 2
#define GCS_REPLICATION_APPLIER_INIT_ERROR 3
#define GCS_COMMUNICATION_LAYER_JOIN_ERROR 4
#define GCS_COMMUNICATION_LAYER_SESSION_ERROR 5

//Plugin variables
typedef st_mysql_sys_var SYS_VAR;
extern char gcs_replication_group[UUID_LENGTH+1];
extern rpl_sidno gcs_cluster_sidno;
extern char gcs_replication_boot;
extern bool wait_on_engine_initialization;
extern const char *available_bindings_names[];

//The modules
extern Gcs_interface *gcs_module;
extern Applier_module *applier_module;
extern Recovery_module *recovery_module;
extern Cluster_member_info_manager_interface *cluster_member_mgr;

//Auxiliary Functionality
extern Gcs_plugin_events_handler* events_handler;
extern Gcs_plugin_leave_notifier* leave_notifier;
extern Cluster_member_info* local_member_info;

/*
  These variables are handles to the registered event handlers in each GCS
  interface. To better understand the handle mechanism, please refer to
  the event handler registration mechanism in any GCS interface
 */
extern int gcs_communication_event_handle;
extern int gcs_control_event_handler;
extern int gcs_control_exchanged_data_handle;

//Appliers module variables
extern ulong handler_pipeline_type;
//GCS module variables
extern char *gcs_group_pointer;

//Plugin global methods
int configure_and_start_applier_module();
int configure_cluster_member_manager();
int terminate_applier_module();
int initialize_recovery_module();
int terminate_recovery_module();
int configure_and_start_gcs();
void declare_plugin_running();

//Plugin public methods
int gcs_replication_init(MYSQL_PLUGIN plugin_info);
int gcs_replication_deinit(void *p);
int gcs_rpl_start();
int gcs_rpl_stop();
bool is_gcs_rpl_running();
bool get_gcs_stats_info(RPL_GCS_STATS_INFO *info);
bool get_gcs_nodes_info(uint index, RPL_GCS_NODES_INFO *info);
bool get_gcs_node_stat_info(RPL_GCS_NODE_STATS_INFO *info);
uint get_gcs_nodes_number();

#endif /* GCS_PLUGIN_INCLUDE */
