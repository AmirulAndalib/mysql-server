/* Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.

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

#include "gcs_plugin.h"
#include "observer_server_state.h"

/*
  DBMS lifecycle events observers.
*/
int gcs_before_handle_connection(Server_state_param *param)
{
  return 0;
}

int gcs_before_recovery(Server_state_param *param)
{
  return 0;
}

int gcs_after_engine_recovery(Server_state_param *param)
{
  /*
    The plugin was initialized on server start
    so only now we can start the applier
  */
  if (wait_on_engine_initialization)
  {
    int error= 0;
    wait_on_engine_initialization= false;

    if ((error= configure_and_start_applier()))
      return error;

    if ((error= configure_and_start_gcs()))
    {
      //terminate the before created pipeline
      log_message(MY_ERROR_LEVEL,
                  "Error on gcs initialization methods, killing the applier");
      applier->terminate_applier_thread();
      return error;
    }

    declare_plugin_running(); //All is OK
    return 0;
  }

  return 0;
}

int gcs_after_recovery(Server_state_param *param)
{
  return 0;
}

int gcs_before_server_shutdown(Server_state_param *param)
{
  return 0;
}

int gcs_after_server_shutdown(Server_state_param *param)
{
  return 0;
}

Server_state_observer server_state_observer = {
  sizeof(Server_state_observer),

  gcs_before_handle_connection, //before the client connect the node
  gcs_before_recovery,           //before_recovery
  gcs_after_engine_recovery,     //after engine recovery
  gcs_after_recovery,            //after_recovery
  gcs_before_server_shutdown,    //before shutdown
  gcs_after_server_shutdown,     //after shutdown
};
