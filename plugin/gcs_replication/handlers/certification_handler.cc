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

#include "certification_handler.h"
#include "gcs_pipeline_interface.h"
#include "../observer_trans.h"
#include <gcs_replication.h>


Certification_handler::Certification_handler()
  :cert_module(NULL), seq_number(0)
{}

int
Certification_handler::initialize()
{
  DBUG_ENTER("Certification_handler::initialize");
  DBUG_ASSERT(cert_module == NULL);
  cert_module= new Certifier();
  DBUG_RETURN(0);
}

int
Certification_handler::terminate()
{
  DBUG_ENTER("Certification_handler::terminate");
  int error= 0;

  if(cert_module == NULL)
    DBUG_RETURN(error);

  error= cert_module->terminate();
  delete cert_module;
  cert_module= NULL;
  DBUG_RETURN(error);
}

int
Certification_handler::handle_action(PipelineAction* action)
{
  DBUG_ENTER("Certification_handler::handle_action");

  int error= 0;

  Plugin_handler_action action_type=
    (Plugin_handler_action)action->get_action_type();

  if (action_type == HANDLER_CERT_CONF_ACTION)
  {
    Handler_certifier_configuration_action* conf_action=
      (Handler_certifier_configuration_action*) action;

    error= cert_module->initialize(conf_action->get_last_delivered_gno());

    cluster_sidno= conf_action->get_cluster_sidno();
  }
  else if (action_type == HANDLER_GCS_INTERF_ACTION)
  {
    Handler_GCS_interfaces_action *gcs_intf_action=
      (Handler_GCS_interfaces_action*) action;

    cert_module->set_local_node_info(gcs_intf_action->get_local_info());
    cert_module->set_gcs_interfaces(gcs_intf_action->get_comm_interface(),
                                    gcs_intf_action->get_control_interface());
  }
  else if (action_type == HANDLER_CERT_DB_ACTION)
  {
    Handler_certifier_information_action *cert_inf_action=
      (Handler_certifier_information_action*) action;

    cert_module->set_certification_info(cert_inf_action->get_certification_db(),
                                        cert_inf_action->get_sequence_number());
  }
  else if (action_type == HANDLER_VIEW_CHANGE_ACTION)
  {
    View_change_pipeline_action *vc_action=
            (View_change_pipeline_action*) action;

    if (!vc_action->is_leaving())
    {
      cert_module->handle_view_change();
    }
  }

  if(error)
    DBUG_RETURN(error);

  DBUG_RETURN(next(action));
}

int
Certification_handler::handle_event(PipelineEvent *pevent, Continuation *cont)
{
  DBUG_ENTER("Certification_handler::handle_event");

  Log_event_type ev_type= pevent->get_event_type();
  switch (ev_type)
  {
    case TRANSACTION_CONTEXT_EVENT:
      DBUG_RETURN(certify(pevent, cont));
    case GTID_LOG_EVENT:
      DBUG_RETURN(inject_gtid(pevent, cont));
    case VIEW_CHANGE_EVENT:
      DBUG_RETURN(extract_certification_db(pevent, cont));
    default:
      next(pevent, cont);
      DBUG_RETURN(0);
  }
}

int
Certification_handler::certify(PipelineEvent *pevent, Continuation *cont)
{
  DBUG_ENTER("Certification_handler::certify");
  Log_event *event= NULL;
  pevent->get_LogEvent(&event);

  Transaction_context_log_event *tcle= (Transaction_context_log_event*) event;
  rpl_gno seq_number= cert_module->certify(tcle->get_snapshot_timestamp(),
                                           tcle->get_write_set());

  // FIXME: This needs to be improved before 0.2
  if (!strncmp(tcle->get_server_uuid(), server_uuid, UUID_LENGTH))
  {
    /*
      Local transaction.
      After a certification we need to wake up the waiting thread on the
      plugin to proceed with the transaction processing.
      Sequence number <= 0 means abort, so we need to pass a negative value to
      transaction context.
    */
    Transaction_termination_ctx transaction_termination_ctx;
    transaction_termination_ctx.m_thread_id= tcle->get_thread_id();
    if (seq_number > 0)
    {
      transaction_termination_ctx.m_rollback_transaction= FALSE;
      transaction_termination_ctx.m_generated_gtid= TRUE;
      transaction_termination_ctx.m_sidno= gcs_cluster_sidno;
      transaction_termination_ctx.m_seqno= seq_number;
    }
    else
    {
      transaction_termination_ctx.m_rollback_transaction= TRUE;
      transaction_termination_ctx.m_generated_gtid= FALSE;
      transaction_termination_ctx.m_sidno= -1;
      transaction_termination_ctx.m_seqno= -1;
    }

    if (set_transaction_ctx(transaction_termination_ctx))
    {
      log_message(MY_ERROR_LEVEL,
                  "Unable to update certification result on server side, thread_id: %lu",
                  tcle->get_thread_id());
      cont->signal(1,true);
      DBUG_RETURN(1);
    }

    if (certification_latch.releaseTicket(tcle->get_thread_id()))
    {
      log_message(MY_ERROR_LEVEL, "Failed to notify certification outcome");
      cont->signal(1,true);
      DBUG_RETURN(1);
    }

    //The pipeline ended for this transaction
    cont->signal(0,true);
  }
  else
  {
    if (!seq_number)
      //The transaction was not certified so discard it.
      cont->signal(0,true);
    else
    {
      set_seq_number(seq_number);
      next(pevent, cont);
    }
  }
  DBUG_RETURN(0);
}

int
Certification_handler::inject_gtid(PipelineEvent *pevent, Continuation *cont)
{
  DBUG_ENTER("Certification_handler::inject_gtid");
  Log_event *event= NULL;
  pevent->get_LogEvent(&event);
  Gtid_log_event *gle_old= (Gtid_log_event*)event;

  // Create new GTID event.
  rpl_gno seq_number = get_and_reset_seq_number();
  Gtid gtid= { cluster_sidno, seq_number };
  Gtid_specification spec= { GTID_GROUP, gtid };
  Gtid_log_event *gle= new Gtid_log_event(gle_old->server_id,
                                          gle_old->is_using_trans_cache(),
                                          spec);

  pevent->reset_pipeline_event();
  pevent->set_LogEvent(gle);

  next(pevent, cont);

  DBUG_RETURN(0);
}

int
Certification_handler::extract_certification_db(PipelineEvent *pevent,
                                                Continuation *cont)
{
  DBUG_ENTER("Certification_handler::extract_certification_db");
  Log_event *event= NULL;
  pevent->get_LogEvent(&event);
  View_change_log_event *vchange_event= (View_change_log_event*)event;

  rpl_gno sequence_number= 0;
  std::map<std::string, rpl_gno> cert_db;
  cert_module->get_certification_info(&cert_db, &sequence_number);

  vchange_event->set_certification_db_snapshot(&cert_db);
  vchange_event->set_seq_number(sequence_number);

  next(pevent, cont);
  DBUG_RETURN(0);
}

bool Certification_handler::is_unique()
{
  return true;
}

int Certification_handler::get_role()
{
  return CERTIFIER;
}

Certifier_interface*
Certification_handler::get_certifier()
{
  return cert_module;
}
