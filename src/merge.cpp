#include <stdexcept>

#include "merge.h"
#include "names.h"
#include "util.h"

void mergeTable(const options& opt, log* lg, const TableSchema& table,
        etymon::odbc_env* odbc, etymon::odbc_conn* conn, const dbtype& dbt)
{
    // Update history tables.

    string historyTable;
    historyTableName(table.tableName, &historyTable);

    string latestHistoryTable;
    latestHistoryTableName(table.tableName, &latestHistoryTable);

    string sql =
        "CREATE TEMPORARY TABLE\n"
        "    " + latestHistoryTable + "\n"
        "    AS\n"
        "SELECT id, data, tenant_id\n"
        "    FROM " + historyTable + " AS h1\n"
        "    WHERE NOT EXISTS\n"
        "      ( SELECT 1\n"
        "            FROM " + historyTable + " AS h2\n"
        "            WHERE h1.tenant_id = h2.tenant_id AND\n"
        "                  h1.id = h2.id AND\n"
        "                  h1.updated < h2.updated\n"
        "      );";
    lg->write(level::detail, "", "", sql, -1);
    conn->exec(sql);

    string loadingTable;
    loadingTableName(table.tableName, &loadingTable);

    sql =
        "INSERT INTO " + historyTable + "\n"
        "    (id, data, updated, tenant_id)\n"
        "SELECT s.id,\n"
        "       s.data,\n" +
        "       " + dbt.current_timestamp() + ",\n"
        "       s.tenant_id\n"
        "    FROM " + loadingTable + " AS s\n"
        "        LEFT JOIN " + latestHistoryTable + "\n"
        "            AS h\n"
        "            ON s.tenant_id = h.tenant_id AND\n"
        "               s.id = h.id\n"
        "    WHERE s.data IS NOT NULL AND\n"
        "          ( h.id IS NULL OR\n"
        "            (s.data)::VARCHAR <> (h.data)::VARCHAR );";
    lg->write(level::detail, "", "", sql, -1);
    conn->exec(sql);
}

void dropTable(const options& opt, log* lg, const string& tableName,
        etymon::odbc_conn* conn)
{
    string sql = "DROP TABLE IF EXISTS " + tableName + ";";
    lg->detail(sql);
    conn->exec(sql);
}

void placeTable(const options& opt, log* lg, const TableSchema& table,
        etymon::odbc_conn* conn)
{
    string loadingTable;
    loadingTableName(table.tableName, &loadingTable);
    string sql =
        "ALTER TABLE " + loadingTable + "\n"
        "    RENAME TO " + table.tableName + ";";
    lg->write(level::detail, "", "", sql, -1);
    conn->exec(sql);
}

