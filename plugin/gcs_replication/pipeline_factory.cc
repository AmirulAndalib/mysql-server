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

#include "gcs_utils.h"
#include "pipeline_factory.h"
#include "handlers/event_cataloger.h"
#include "handlers/certification_handler.h"
#include "handlers/applier_sql_thread.h"


int get_pipeline(Handler_pipeline_type pipeline_type,
                 EventHandler** pipeline)
{
  DBUG_ENTER("get_pipeline(pipeline_type, pipeline)");

  Handler_id* handler_list= NULL;
  int num_handlers= get_pipeline_configuration(pipeline_type, &handler_list);
  DBUG_RETURN(configure_pipeline(pipeline, handler_list, num_handlers));
}

int get_pipeline_configuration(Handler_pipeline_type pipeline_type,
                               Handler_id** pipeline_conf)
{
  DBUG_ENTER("get_pipeline_configuration");
  /*
    When a new pipeline is defined the developer shall define here what are
    the handlers that belong to it and their order.
  */
  switch (pipeline_type)
  {
    case STANDARD_GCS_PIPELINE:
      (*pipeline_conf)= new Handler_id[3];
      (*pipeline_conf)[0]= CATALOGING_HANDLER;
      (*pipeline_conf)[1]= CERTIFICATION_HANDLER;
      (*pipeline_conf)[2]= SQL_THREAD_APPLICATION_HANDLER;
      DBUG_RETURN(3);
    default:
      sql_print_error("Unknown applier pipeline requested");
  }
  DBUG_RETURN(0);
}

int configure_pipeline(EventHandler** pipeline, Handler_id handler_list[],
                       int num_handlers)
{
  DBUG_ENTER("configure_pipeline(pipeline, handler_list[], num_handlers)");
  int error= 0;

  for (int i= 0; i < num_handlers; ++i)
  {
    EventHandler* handler= NULL;

    /*
      When a new handler is define the developer shall insert it here
    */
    switch (handler_list[i])
    {
      case CATALOGING_HANDLER:
        handler= new Event_cataloger();
        break;
      case CERTIFICATION_HANDLER:
        handler= new Certification_handler();
        break;
      case SQL_THREAD_APPLICATION_HANDLER:
        handler= new Applier_sql_thread();
        break;
      default:
        error= 1;
        sql_print_error("Unknown requested handler");
    }

    if (!handler)
    {
      if (!error) //not an unknown handler but a initialization error
      {
        sql_print_error("One of the applier handlers is null due to"
                        "an initialization error");
      }
      DBUG_RETURN(1);
    }

    /*
      TODO: This kind of tests don't belong here. We need a way to
      do this in a static way before initialization.
    */
    //Record the handler role if unique
    if (handler->is_unique())
    {
      for (int z= 0; z < i; ++z)
      {
        DBUG_EXECUTE_IF("double_unique_handler",
                        handler_list[z]= handler_list[i];);

        //Check to see if the handler was already used in this pipeline
        if (handler_list[i] == handler_list[z])
        {
          sql_print_error("An handler, marked as unique, is already in use.");
          DBUG_RETURN(1);
        }

        //check to see if no other handler has the same role
        EventHandler *handler_with_same_role= NULL;
        EventHandler::get_handler_by_role(*pipeline,handler->get_role(),
                                          &handler_with_same_role);
        if (handler_with_same_role != NULL)
        {
          sql_print_error("An handler role, that was marked as unique, \
            is already in use.");
          DBUG_RETURN(1);
        }

      }
    }

    if ((error= handler->initialize()))
    {
      sql_print_error("Error on handler initialization");
      DBUG_RETURN(error);
    }

    //Add the handler to the pipeline
    EventHandler::append_handler(pipeline,handler);
  }
  DBUG_RETURN(0);
}
