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

#ifndef GCS_PIPELINE_INTERFACE_INCLUDED
#define	GCS_PIPELINE_INTERFACE_INCLUDED

#include "../gcs_plugin_utils.h"
#include "../gcs_member_info.h"
#include "gcs_communication_interface.h"
#include "gcs_control_interface.h"
#include <rpl_pipeline_interfaces.h>

/*
  @enum Event modifier
  Enumeration type for the different kinds of pipeline event modifiers.
*/
enum enum_event_modifier
{
  TRANSACTION_BEGIN= 1, //transaction start event
  TRANSACTION_END= 2,   //transaction end event
  UNMARKED_EVENT= 3,    //transaction regular event
};

/**
  @enum Handler role
  Enumeration type for the different roles of the used handlers.
*/
enum enum_handler_role
{
  EVENT_CATALOGER= 0,
  APPLIER= 1,
  CERTIFIER= 2,
  QUEUER= 3,
  ROLE_NUMBER= 4 //The number of roles
};

/**
  @enum Gcs_handler_action

  Enumeration of all actions sent to the plugin handlers.
*/
typedef enum enum_gcs_handler_actions {
  HANDLER_START_ACTION= 0,        // Action that signals the handlers to start
  HANDLER_STOP_ACTION= 1,         // Action that signals the handlers to stop
  HANDLER_APPLIER_CONF_ACTION= 2, // Configuration for applier handlers
  HANDLER_CERT_CONF_ACTION= 3,    // Configuration for certification handlers
  HANDLER_CERT_DB_ACTION= 4,      // Certification info for the certifier
  HANDLER_VIEW_CHANGE_ACTION= 5,  // Certification notification on view change
  HANDLER_GCS_INTERF_ACTION= 6,   // Action with GCS interfaces to be used.
  HANDLER_ACTION_NUMBER= 7        // The number of roles
} Plugin_handler_action;

/**
  @class Handler_start_action

  Action to signal the handler to start existing routines
*/
class Handler_start_action : public PipelineAction
{
public:
  Handler_start_action()
    :PipelineAction(HANDLER_START_ACTION)
  {}

  ~Handler_start_action() {}
};

/**
  @class Handler_stop_action

  Action to signal the handler to stop existing routines
*/
class Handler_stop_action : public PipelineAction
{
public:
  Handler_stop_action()
    :PipelineAction(HANDLER_STOP_ACTION)
  {}

  ~Handler_stop_action() {}
};

/**
  @class Handler_applier_configuration_action

  Action to configure existing applier handlers
*/
class Handler_applier_configuration_action : public PipelineAction
{
public:
  /**
   Configuration for applier handlers

   @param relay_log_name            the applier's relay log name
   @param relay_log_info_name       the applier's relay log index file name
   @param reset_logs                if a reset was executed in the server
   @param plugin_shutdown_timeout   the plugin's timeout for component shutdown
   @param cluster_sidno             the cluster configured sidno
  */
  Handler_applier_configuration_action(char *relay_log_name,
                                       char *relay_log_info_name,
                                       bool reset_logs,
                                       ulong plugin_shutdown_timeout,
                                       rpl_sidno cluster_sidno)
    :PipelineAction(HANDLER_APPLIER_CONF_ACTION),
    applier_relay_log_name(relay_log_name),
    applier_relay_log_info_name(relay_log_info_name), reset_logs(reset_logs),
    applier_shutdown_timeout(plugin_shutdown_timeout),
    cluster_sidno(cluster_sidno), initialization_conf(true), last_queued_gno(0)
    {
      DBUG_ASSERT(relay_log_name != NULL);
      DBUG_ASSERT(relay_log_info_name != NULL);
    }

  /**
   Configuration for applier handlers

   @param plugin_shutdown_timeout   the plugin's timeout for component shutdown
  */
  Handler_applier_configuration_action(ulong plugin_shutdown_timeout)
    :PipelineAction(HANDLER_APPLIER_CONF_ACTION), applier_relay_log_name(NULL),
    applier_relay_log_info_name(NULL),reset_logs(false),
    applier_shutdown_timeout(plugin_shutdown_timeout),
    cluster_sidno(0), initialization_conf(false), last_queued_gno(0)
  {
  }

  ~Handler_applier_configuration_action() {}

  /**
    @return the relay log info name
      @retval NULL    if not defined
      @retval !=NULL  if defined
   */
  char* get_applier_relay_log_info_name() {
    return applier_relay_log_info_name;
  }

  /**
    @return the relay log name
      @retval NULL    if not defined
      @retval !=NULL  if defined
   */
  char* get_applier_relay_log_name() {
    return applier_relay_log_name;
  }

  ulong get_applier_shutdown_timeout() {
    return applier_shutdown_timeout;
  }

  bool is_reset_logs_planned() {
    return reset_logs;
  }

  rpl_sidno get_sidno() {
    return cluster_sidno;
  }

  void set_last_queued_gno(rpl_gno last_queued_gno) {
    this->last_queued_gno = last_queued_gno;
  }

  /**
   Gets the last known gno that was queued after being certified.
   It only returns a valid result if this action passed by a handler where such
   notion applies.

   @return the operation status
     @retval 0      No queued gtids exist in this context
     @retval != 0   The last queued gno
  */
  rpl_gno get_last_queued_gno() {
    return last_queued_gno;
  }

  /**
   Informs if this is a action with configurations for initialization or just
   timeout configurations.

   @return
     @retval true    if initialization action
     @retval false   if timeout configuration action
  */
  bool is_initialization_conf() {
    return initialization_conf;
  }

private:
  /*The applier's relay log name*/
  char *applier_relay_log_name;
  /*The applier's relay log index file name */
  char *applier_relay_log_info_name;
  /*If a reset was executed in the server */
  bool reset_logs;
  /*The plugin's timeout for component shutdown set in the applier*/
  ulong applier_shutdown_timeout;
  /*The configured cluster sidno*/
  rpl_sidno cluster_sidno;

  //Internal fields

  /*If this is a initialization packet or not*/
  bool initialization_conf;

  //Returned data

  /*The last queued gtid in the appliers relay log*/
  rpl_gno last_queued_gno;
};

/**
  @class Handler_certifier_configuration_action

  Action to configure existing certification handlers
*/
class Handler_certifier_configuration_action : public PipelineAction
{
public:
  /**
   Configuration for certification handlers

   @param last_delivered_gno   the last certified gno in the plugin
  */
  Handler_certifier_configuration_action(rpl_gno last_delivered_gno,
                                         rpl_sidno cluster_sidno)
   :PipelineAction(HANDLER_CERT_CONF_ACTION),
   last_delivered_gno(last_delivered_gno), cluster_sidno(cluster_sidno)
  {}

  rpl_gno get_last_delivered_gno() {
    return last_delivered_gno;
  }

  rpl_sidno get_cluster_sidno() {
    return cluster_sidno;
  }

private:
  rpl_gno last_delivered_gno;
  rpl_sidno cluster_sidno;
};

/**
  @class Handler_certifier_information_action

  Action that carries a certification database and sequence number to be
  applied on certification handlers.
*/
class Handler_certifier_information_action : public PipelineAction
{
public:
  /**
    Create an action to communicate certification info to a certifier

    @param cert_db     A certification database
    @param seq_number  A sequence number associated to the database
  */
  Handler_certifier_information_action(std::map<std::string,rpl_gno>* cert_db,
                                       rpl_gno seq_number)
   :PipelineAction(HANDLER_CERT_DB_ACTION),
   certification_db(cert_db), sequence_number(seq_number)
  {}

  std::map<std::string, rpl_gno>* get_certification_db() {
    return certification_db;
  }

  rpl_gno get_sequence_number() {
    return sequence_number;
  }

private:
  std::map<std::string,rpl_gno>* certification_db;
  rpl_gno sequence_number;
};


/**
  @class View_change_pipeline_action

  Action to signal any interested handler that a VC happened
*/
class View_change_pipeline_action : public PipelineAction
{
public:
  /**
    Creates an action to inform handler of a View Change

    @param is_leaving informs if the node is leaving
  */
  View_change_pipeline_action(bool is_leaving)
    :PipelineAction(HANDLER_VIEW_CHANGE_ACTION), leaving(is_leaving)
  {}

  bool is_leaving() {
    return leaving;
  }

private:
  /*If this node is leaving the cluster on this view change*/
  bool leaving;
};

/**
  @class Handler_GCS_interfaces_action

  Action that carries GCS interfaces to be used on handlers that need them.
*/
class Handler_GCS_interfaces_action : public PipelineAction
{
public:
  /**
    An action that contains group communication related interfaces to
    be used on interested handlers.

    @param local_info GCS info on the local node
    @param comm_if    GCS communication interface
    @param ctrl_if    GCS control interface
  */
  Handler_GCS_interfaces_action(Cluster_member_info *local_info,
                                Gcs_communication_interface *comm_if,
                                Gcs_control_interface *ctrl_if)
   :PipelineAction(HANDLER_GCS_INTERF_ACTION),
   local_info(local_info), communication_interface(comm_if),
   control_interface(ctrl_if)
  {}

  Gcs_communication_interface* get_comm_interface() {
    return communication_interface;
  }

  Gcs_control_interface* get_control_interface() {
    return control_interface;
  }

  Cluster_member_info* get_local_info() {
    return local_info;
  }

private:
  Cluster_member_info *local_info;
  Gcs_communication_interface *communication_interface;
  Gcs_control_interface *control_interface;
};

#endif	/* GCS_PIPELINE_INTERFACE_INCLUDED */