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

#ifndef GCS_CERTIFIER
#define GCS_CERTIFIER

#include "../gcs_plugin_utils.h"
#include "gcs_replication.h"
#include <replication.h>
#include <log_event.h>
#include <applier_interfaces.h>
#include <map>
#include <string>
#include <list>
#include "gcs_certifier_stats_interface.h"

/**
  This class is a core component of the database state machine
  replication protocol. It implements conflict detection based
  on a certification procedure.

  Snapshot Isolation is based on assigning logical timestamp to optimistic
  transactions, i.e. the ones which successfully meet certification and
  are good to commit on all nodes in the group. This timestamp is a
  monotonically increasing counter, and is same across all nodes in the group.

  This timestamp is further used to update the certification Database, which
  maps the items in a transaction to the last optimistic Transaction Id
  that modified the particular item.
  The items here are extracted as part of the write-sets of a transaction.

  For the incoming transaction, if the items in its writeset are modified
  by any transaction which was optimistically certified to commit has a
  sequence number greater than the timestamp seen by the incoming transaction
  then it is not certified to commit. Otherwise, this transaction is marked
  certified and is later written to the Relay log of the participating node.

*/


typedef std::map<std::string, rpl_gno> cert_db;


class Certifier_broadcast_thread
{
public:
  Certifier_broadcast_thread();
  virtual ~Certifier_broadcast_thread();

  /**
    Initialize broadcast thread.

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int initialize();

  /**
    Terminate broadcast thread.

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int terminate();

  /**
    Broadcast thread worker method.
  */
  void dispatcher();

private:
  /**
    Period (in microseconds) between stable transactions set
    broadcast.
  */
  static const int BROADCAST_PERIOD= 60000000; // microseconds

  /**
    Thread control.
  */
  bool aborted;
  pthread_t broadcast_pthd;
  pthread_mutex_t broadcast_pthd_lock;
  pthread_cond_t broadcast_pthd_cond;
  bool broadcast_pthd_running;

  /**
    Broadcast local GTID_EXECUTED to group.

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int broadcast_gtid_executed();
};


class Certifier_interface : public Certifier_stats
{
public:
  virtual ~Certifier_interface() {}
  virtual void handle_view_change()= 0;
  virtual int handle_certifier_data(const char *data, uint len)= 0;
  virtual void get_certification_info(cert_db *cert_db, rpl_gno *seq_number)= 0;
  virtual void set_certification_info(std::map<std::string, rpl_gno> *cert_db,
                                      rpl_gno sequence_number)= 0;
};


class Certifier: public Certifier_interface
{
public:
  Certifier();
  virtual ~Certifier();

  /**
    Initialize certifier.

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int initialize();

  /**
    Terminate certifier.

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int terminate();

  /**
    Handle view changes on certifier.
   */
  virtual void handle_view_change();

  /**
    Queues the packet coming from the reader for future processing.

    @param[in]  data      the packet data
    @param[in]  len       the packet length

    @return the operation status
      @retval 0      OK
      @retval !=0    Error on queue
  */
  virtual int handle_certifier_data(const char *data, uint len);

  /**
    This member function SHALL certify the set of items against transactions
    that have already passed the certification test.

    @param snapshot_timestamp The incoming transaction snapshot timestamp.
    @param write_set          The incoming transaction write set.
    @returns >0               sequence number (positively certified);
              0               negatively certified;
             -1               error.
   */
  rpl_gno certify(rpl_gno snapshot_timestamp,
                  std::list<const char*> *write_set);

  /**
    Returns the transactions in stable set, that is, the set of transactions
    already applied on all group members.

    @returns                 transactions in stable set
   */
  Gtid_set* get_group_stable_transactions_set();

  /**
    Retrieves the current certification db and sequence number.

     @note if concurrent access is introduce to these variables,
     locking is needed in this method

     @param[out] cert_db     a pointer to retrieve the certification database
     @param[out] seq_number  a pointer to retrieve the sequence number
  */
  virtual void get_certification_info(cert_db *cert_db, rpl_gno *seq_number);

  /**
    Sets the certification db and sequence number according to the given values.

    @note if concurrent access is introduce to these variables,
    locking is needed in this method

    @param cert_db
    @param sequence_number
  */
  virtual void set_certification_info(std::map<std::string, rpl_gno> *cert_db,
                                      rpl_gno sequence_number);

  /**
    Get the number of postively certified transactions by the certifier
    */
  ulonglong get_positive_certified();

  /**
    Get method to retrieve the number of negatively certified transactions.
    */
  ulonglong get_negative_certified();

  /**
    Get method to retrieve the certification db size.
    */
  ulonglong get_cert_db_size();

  /**
    Get method to retrieve the last sequence number of the node.
    */
  rpl_gno get_last_sequence_number();

private:
  /**
    Is certifier initialized.
  */
  bool initialized;

  bool inline is_initialized()
  {
    return initialized;
  }

  /**
    Next sequence number.
  */
  rpl_gno next_seqno;

  /**
    Certification database.
  */
  cert_db item_to_seqno_map;

  ulonglong positive_cert;
  ulonglong negative_cert;

  mysql_mutex_t LOCK_certifier_map;
#ifdef HAVE_PSI_INTERFACE
  PSI_mutex_key key_LOCK_certifier_map;
#endif

  /**
    Stable set and garbage collector variables.
  */
  Sid_map *stable_sid_map;
  Gtid_set *stable_gtid_set;
  Synchronized_queue<Data_packet *> *incoming;

  /**
    Broadcast thread.
  */
  Certifier_broadcast_thread *broadcast_thread;

  /* Methods */
  cert_db::iterator begin();
  cert_db::iterator end();

  /**
    Adds an item from transaction writeset to the certification DB.
    @param[in]   item       item in the writeset to be added to the
                            Certification DB.
    @param[in]  seq_no      Sequence number of the incoming transaction
                            which modified the above mentioned item.
    @return
    @retval     False       successfully added to the map.
                True        otherwise.
  */
  bool add_item(const char* item, rpl_gno seq_no);

  /**
    Find the seq_no object corresponding to an item. Return if
    it exists, other wise return NULL;

    @param[in]  item          item for the seq_no object.
    @retval                   seq_no object if exists in the map.
                              Otherwise 0;
  */
  rpl_gno get_seqno(const char* item);

  /**
    This member function shall add transactions to the stable set

    @param gtid     The GTID set of the transactions to be added
                    to the stable set.
    @returns        False if adds successfully,
                    True otherwise.
   */
  bool set_group_stable_transactions_set(Gtid_set* executed_gtid_set);

  /**
    Computes intersection between all sets received, so that we
    have the already applied transactions on all servers.

    @return the operation status
      @retval 0      OK
      @retval !=0    Error
  */
  int stable_set_handle();

  /**
    Removes the intersection of the received transactions stable
    sets from certification database.
   */
  void garbage_collect();

  /**
    Clear incoming queue.
  */
  void clear_incoming();

  /**
    Returns last GNO for cluster UUID from applier relay log (already
    certified transactions).

    @return
      @retval  -1   Error
      @retval >=0   GNO value
  */
  rpl_gno get_last_delivered_gno();

  /**
    Update method to store the count of the positively and negatively
    certified transaction on a particular node.
    */
  void update_certified_transaction_count(bool result);
};

#endif /* GCS_CERTIFIER */
