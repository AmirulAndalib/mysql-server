/*
      Copyright (c) 2014, Oracle and/or its affiliates. All rights reserved.

      This program is free software; you can redistribute it and/or modify
      it under the terms of the GNU General Public License as published by
      the Free Software Foundation; version 2 of the License.

      This program is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
      GNU General Public License for more details.

      You should have received a copy of the GNU General Public License
      along with this program; if not, write to the Free Software
      Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file storage/perfschema/table_replication_connection_nodes.cc
  Table replication_connection_nodes (implementation).
*/

#define HAVE_REPLICATION

#include "my_global.h"
#include "sql_priv.h"
#include "table_replication_connection_nodes.h"
#include "pfs_instr_class.h"
#include "pfs_instr.h"
#include "rpl_slave.h"
#include "rpl_info.h"
#include "rpl_rli.h"
#include "rpl_mi.h"
#include "sql_parse.h"
#include "gcs_replication.h"
#include "log.h"

THR_LOCK table_replication_connection_nodes::m_table_lock;

/* Numbers in varchar count utf8 characters. */
static const TABLE_FIELD_TYPE field_types[]=
{
  {
    {C_STRING_WITH_LEN("GROUP_NAME")},
    {C_STRING_WITH_LEN("varchar(36)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("NODE_ID")},
    {C_STRING_WITH_LEN("char(60)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("NODE_HOST")},
    {C_STRING_WITH_LEN("char(60)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("NODE_PORT")},
    {C_STRING_WITH_LEN("int(11)")},
    {NULL, 0}
  },
  {
    {C_STRING_WITH_LEN("NODE_STATE")},
    {C_STRING_WITH_LEN("enum('ONLINE','OFFLINE','RECOVERING')")},
    {NULL, 0}
  }
};

TABLE_FIELD_DEF
table_replication_connection_nodes::m_field_def=
{ 5, field_types };

PFS_engine_table_share
table_replication_connection_nodes::m_share=
{
  { C_STRING_WITH_LEN("replication_connection_nodes") },
  &pfs_readonly_acl,
  &table_replication_connection_nodes::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  table_replication_connection_nodes::get_row_count,
  sizeof(PFS_simple_index), /* ref length */
  &m_table_lock,
  &m_field_def,
  false /* checked */
};

PFS_engine_table* table_replication_connection_nodes::create(void)
{
  return new table_replication_connection_nodes();
}

table_replication_connection_nodes::table_replication_connection_nodes()
  : PFS_engine_table(&m_share, &m_pos),
    m_row_exists(false), m_pos(0), m_next_pos(0)
{}

table_replication_connection_nodes::~table_replication_connection_nodes()
{}

void table_replication_connection_nodes::reset_position(void)
{
  m_pos.m_index= 0;
  m_next_pos.m_index= 0;
}

ha_rows table_replication_connection_nodes::get_row_count()
{
  return get_gcs_nodes_stats_number();
}

int table_replication_connection_nodes::rnd_next(void)
{
  if (!is_gcs_plugin_loaded())
    return HA_ERR_END_OF_FILE;

  for (m_pos.set_at(&m_next_pos);
       m_pos.m_index < get_row_count();
       m_pos.next())
  {
    make_row(m_pos.m_index);
    m_next_pos.set_after(&m_pos);
    return 0;
  }

  return HA_ERR_END_OF_FILE;
}

int table_replication_connection_nodes::rnd_pos(const void *pos)
{
  if (!is_gcs_plugin_loaded())
    return HA_ERR_END_OF_FILE;

  set_position(pos);
  DBUG_ASSERT(m_pos.m_index < get_row_count());
  make_row(m_pos.m_index);

  return 0;
}

void table_replication_connection_nodes::make_row(uint index)
{
  DBUG_ENTER("table_replication_connection_nodes::make_row");
  m_row_exists= false;

  RPL_GCS_NODES_INFO* gcs_info;
  if(!(gcs_info= (RPL_GCS_NODES_INFO*)my_malloc(PSI_NOT_INSTRUMENTED,
                                                sizeof(RPL_GCS_NODES_INFO),
                                                MYF(MY_WME))))
  {
    sql_print_error("Unable to allocate memory on"
                    " table_replication_connection_nodes::make_row");
    DBUG_VOID_RETURN;
  }

  gcs_info->node_host= NULL;
  gcs_info->node_id= NULL;

  bool stats_not_available= get_gcs_nodes_stats(index, gcs_info);
  if (stats_not_available)
    DBUG_PRINT("info", ("GCS stats not available!"));
  else
  {
    char* gcs_replication_group= gcs_info->group_name;
    if (gcs_replication_group)
    {
      memcpy(m_row.group_name, gcs_replication_group, UUID_LENGTH);
      m_row.is_group_name_null= false;
    }
    else
      m_row.is_group_name_null= true;

    m_row.node_id_length= strlen(gcs_info->node_id);
    memcpy(m_row.node_id, gcs_info->node_id, m_row.node_id_length);

    m_row.node_host_length= strlen(gcs_info->node_host);
    memcpy(m_row.node_host, gcs_info->node_host, m_row.node_host_length);
    m_row.node_port= gcs_info->node_port;

    m_row.node_state= gcs_info->node_state;

    m_row_exists= true;
  }

  if(gcs_info->node_host != NULL)
    my_free((char*)gcs_info->node_host);

  if(gcs_info->node_id != NULL)
    my_free((char*)gcs_info->node_id);

  my_free(gcs_info);
  DBUG_VOID_RETURN;
}


int table_replication_connection_nodes::read_row_values(TABLE *table,
                                                         unsigned char *buf,
                                                         Field **fields,
                                                         bool read_all)
{
  Field *f;

  if (unlikely(! m_row_exists))
    return HA_ERR_RECORD_DELETED;

  DBUG_ASSERT(table->s->null_bytes == 0);
  buf[0]= 0;

  for (; (f= *fields) ; fields++)
  {
    if (read_all || bitmap_is_set(table->read_set, f->field_index))
    {
      switch(f->field_index)
      {
      case 0: /** group_name */
        if (!m_row.is_group_name_null)
          set_field_varchar_utf8(f, m_row.group_name, UUID_LENGTH);
        else
          f->set_null();
        break;
      case 1: /** node_id */
        set_field_char_utf8(f, m_row.node_id, m_row.node_id_length);
        break;
      case 2: /** node_host */
        set_field_char_utf8(f, m_row.node_host, m_row.node_host_length);
        break;
      case 3: /** node_port */
        set_field_ulong(f, m_row.node_port);
        break;
      case 4: /** node_state */
        set_field_enum(f, m_row.node_state);
        break;
      default:
        DBUG_ASSERT(false);
      }
    }
  }
  return 0;
}
