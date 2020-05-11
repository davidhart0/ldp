#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <regex>

#include "../etymoncpp/include/postgres.h"
#include "../etymoncpp/include/util.h"
#include "anonymize.h"
#include "camelcase.h"
#include "dbtype.h"
#include "idmap.h"
#include "names.h"
#include "rapidjson/document.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/pointer.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/reader.h"
#include "rapidjson/stringbuffer.h"
#include "schema.h"
#include "stage_json.h"
#include "util.h"

//using namespace std;
namespace json = rapidjson;

constexpr json::ParseFlag pflags = json::kParseTrailingCommasFlag;

struct NameComparator {
    bool operator()(const json::Value::Member &lhs,
            const json::Value::Member &rhs) const {
        const char* s1 = lhs.name.GetString();
        const char* s2 = rhs.name.GetString();
        if (strcmp(s1, "id") == 0)
            return true;
        if (strcmp(s2, "id") == 0)
            return false;
        return (strcmp(s1, s2) < 0);
    }
};

bool looksLikeDateTime(const char* str)
{
    static regex dateTime(
            "\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}("
            "(\\.\\d{3}\\+\\d{4})|"
            "(Z)"
            ")"
            );
    return regex_match(str, dateTime);
}

// Collect statistics and anonimize data
void processJSONRecord(json::Document* root, json::Value* node,
        bool collectStats, bool anonymizeTable, const string& path,
        unsigned int depth, map<string,Counts>* stats)
{
    switch (node->GetType()) {
        case json::kNullType:
            if (collectStats && depth == 1)
                (*stats)[path.c_str() + 1].null++;
            break;
        case json::kTrueType:
        case json::kFalseType:
            if (anonymizeTable && possiblePersonalData(path.c_str())) {
                json::Pointer(path.c_str()).Set(*root, false);
            }
            if (collectStats && depth == 1)
                (*stats)[path.c_str() + 1].boolean++;
            break;
        case json::kNumberType:
            if (anonymizeTable && possiblePersonalData(path.c_str())) {
                json::Pointer(path.c_str()).Set(*root, 0);
            }
            if (collectStats && depth == 1) {
                (*stats)[path.c_str() + 1].number++;
                if (node->IsInt() || node->IsUint() || node->IsInt64() ||
                        node->IsUint64())
                    (*stats)[path.c_str() + 1].integer++;
                else
                    (*stats)[path.c_str() + 1].floating++;
            }
            break;
        case json::kStringType:
            if (anonymizeTable && possiblePersonalData(path.c_str())) {
                json::Pointer(path.c_str()).Set(*root, "");
            }
            if (collectStats && depth == 1) {
                (*stats)[path.c_str() + 1].string++;
                if (isUUID(node->GetString()))
                    (*stats)[path.c_str() + 1].uuid++;
                if (looksLikeDateTime(node->GetString()))
                    (*stats)[path.c_str() + 1].dateTime++;
            }
            break;
        case json::kArrayType:
            {
                int x = 0;
                for (json::Value::ValueIterator i = node->Begin();
                        i != node->End(); ++i) {
                    string newpath = path;
                    newpath += '/';
                    newpath += to_string(x);
                    processJSONRecord(root, i, collectStats, anonymizeTable,
                            newpath, depth + 1, stats);
                    x++;
                }
            }
            break;
        case json::kObjectType:
            sort(node->MemberBegin(), node->MemberEnd(), NameComparator());
            for (json::Value::MemberIterator i = node->MemberBegin();
                    i != node->MemberEnd(); ++i) {
                string newpath = path;
                newpath += '/';
                newpath += i->name.GetString();
                processJSONRecord(root, &(i->value), collectStats,
                        anonymizeTable, newpath, depth + 1, stats);
            }
            break;
        default:
            break;
    }
}

/**
  * \brief  Main ETL processor for JSON data.
  *
  * This class handles most of the ETL processing for a FOLIO interface.
  * The large JSON files that have been retrieved from Okapi are
  * streamed in and parsed into individual JSON object records, in order
  * that only a single record needs to be held in memory at a time.
  * Several functions are performed during two passes over the data.  In
  * pass 1:  Statistics are collected on the data types, and a table
  * schema is generated based on the results.  In pass 2:  (i) Some data
  * are removed or altered as part of anonymization of personal data.
  * (ii) Each JSON object is normalized to enable later comparison with
  * historical data.  (iii) SQL insert statements are generated and
  * submitted to the database to stage the data for merging.
  */
class JSONHandler :
    public json::BaseReaderHandler<json::UTF8<>, JSONHandler> {
public:
    int pass;
    const Options& opt;
    Log* log;
    int level = 0;
    bool active = false;
    string record;
    const TableSchema& tableSchema;
    // Collection of statistics
    map<string,Counts>* stats;
    // Loading to database
    etymon::OdbcDbc* dbc;
    const DBType& dbt;
    IDMap* idmap;
    size_t recordCount = 0;
    string insertBuffer;
    JSONHandler(int pass, const Options& options, Log* log,
            const TableSchema& table, etymon::OdbcDbc* dbc, const DBType& dbt,
            IDMap* idmap, map<string,Counts>* statistics) :
        pass(pass), opt(options), log(log), tableSchema(table),
        stats(statistics), dbc(dbc), dbt(dbt), idmap(idmap) { }
    bool StartObject();
    bool EndObject(json::SizeType memberCount);
    bool StartArray();
    bool EndArray(json::SizeType elementCount);
    bool Key(const char* str, json::SizeType length, bool copy);
    bool String(const char* str, json::SizeType length, bool copy);
    bool Int(int i);
    bool Uint(unsigned u);
    bool Bool(bool b);
    bool Null();
    bool Int64(int64_t i);
    bool Uint64(uint64_t u);
    bool Double(double d);
};

bool JSONHandler::StartObject()
{
    if (level == 2) {
        record = '{';
    } else {
        if (level > 2)
            record += '{';
    }
    level++;
    return true;
}

static void beginInserts(const string& table, string* buffer)
{
    string loadingTable;
    loadingTableName(table, &loadingTable);
    *buffer = "INSERT INTO " + loadingTable + " VALUES ";
}

static void endInserts(const Options& opt, Log* log, const string& table,
        string* buffer, etymon::OdbcDbc* dbc)
{
    *buffer += ";\n";
    log->log(Level::detail, "", "", "Loading data for table: " + table, -1);
    dbc->execDirect(nullptr, *buffer);
    buffer->clear();
}

static void writeTuple(const Options& opt, Log* log, const DBType& dbt,
        IDMap* idmap, const TableSchema& table, const json::Document& doc,
        size_t* recordCount, string* insertBuffer)
{
    if (*recordCount > 0)
        *insertBuffer += ',';
    *insertBuffer += '(';

    //*insertBuffer += "DEFAULT,";

    const char* id = doc["id"].GetString();
    // sk
    string sk;
    idmap->makeSK(table.tableName, id, &sk);
    *insertBuffer += sk;
    *insertBuffer += ',';
    // id
    string idenc;
    dbt.encodeStringConst(id, &idenc);
    *insertBuffer += idenc;
    *insertBuffer += ',';

    string s;
    for (const auto& column : table.columns) {
        if (column.columnName == "id")
            continue;
        const char* sourceColumnName = column.sourceColumnName.c_str();
        if (doc.HasMember(sourceColumnName) == false) {
            if (column.columnType == ColumnType::id)
                *insertBuffer += "NULL,";
            *insertBuffer += "NULL,";
            continue;
        }
        const json::Value& jsonValue = doc[sourceColumnName];
        if (jsonValue.IsNull()) {
            if (column.columnType == ColumnType::id)
                *insertBuffer += "NULL,";
            *insertBuffer += "NULL,";
            continue;
        }
        switch (column.columnType) {
        case ColumnType::bigint:
            *insertBuffer += to_string(jsonValue.GetInt());
            break;
        case ColumnType::boolean:
            *insertBuffer += ( jsonValue.GetBool() ?  "TRUE" : "FALSE" );
            break;
        case ColumnType::numeric:
            *insertBuffer += to_string(jsonValue.GetDouble());
            break;
        case ColumnType::id:
            idmap->makeSK("", jsonValue.GetString(), &s);
            *insertBuffer += s;
            *insertBuffer += ",";
        case ColumnType::timestamptz:
        case ColumnType::varchar:
            dbt.encodeStringConst(jsonValue.GetString(), &s);
            // Check if varchar exceeds maximum string length (65535).
            if (s.length() >= 65535) {
                log->log(Level::warning, "", "",
                        "String length exceeds database limit:\n"
                        "    Table: " + table.tableName + "\n"
                        "    Column: " + column.columnName + "\n"
                        "    SK: " + sk + "\n"
                        "    ID: " + id + "\n"
                        "    Action: Value stored as NULL", -1);
                s = "NULL";
            }
            *insertBuffer += s;
            break;
        }
        *insertBuffer += ",";
    }

    string data;
    json::StringBuffer jsonText;
    json::PrettyWriter<json::StringBuffer> writer(jsonText);
    doc.Accept(writer);
    dbt.encodeStringConst(jsonText.GetString(), &data);
    // Check if pretty-printed JSON exceeds maximum string length (65535).
    if (data.length() > 65535) {
        // Formatted JSON object size exceeds database limit.  Try
        // compact-printed JSON.
        json::StringBuffer jsonText;
        json::Writer<json::StringBuffer> writer(jsonText);
        doc.Accept(writer);
        dbt.encodeStringConst(jsonText.GetString(), &data);
        if (data.length() > 65535) {
            log->log(Level::warning, "", "", 
                    "JSON object size exceeds database limit:\n"
                    "    Table: " + table.tableName + "\n"
                    "    SK: " + sk + "\n"
                    "    ID: " + id + "\n"
                    "    Action: Value of column \"data\" stored as NULL", -1);
            data = "NULL";
        }
    }

    //print(Print::warning, opt, "storing record as:\n" + data + "\n");

    *insertBuffer += data;
    //*insertBuffer += string(",") + dbt.currentTimestamp();
    *insertBuffer += ",1)";
    (*recordCount)++;
}

bool JSONHandler::EndObject(json::SizeType memberCount)
{
    if (level == 3) {

        record += '}';

        //if (opt.logLevel == Level::trace)
        //    fprintf(opt.err, 
        //            "New record parsed for \"%s\":\n%s\n",
        //            tableSchema.tableName.c_str(),
        //            record.c_str());

        // TODO Wrap buffer in a class.
        char* buffer = strdup(record.c_str());

        json::Document doc;
        doc.ParseInsitu<pflags>(buffer);

        string path;
        bool collectStats = (pass == 1);
        bool anonymizeTable;
        if (pass == 1) {
            anonymizeTable = false;
        } else {
            anonymizeTable =
                (strcmp(tableSchema.tableName.c_str(), "user_users") == 0);
        }
        // Collect statistics and anonymize data.
        processJSONRecord(&doc, &doc, collectStats, anonymizeTable, path, 0,
                stats);

        if (pass == 2) {

            //if (insertBuffer.length() > 16500000) {
            if (insertBuffer.length() > 10000000) {
                endInserts(opt, log, tableSchema.tableName, &insertBuffer, dbc);
                beginInserts(tableSchema.tableName, &insertBuffer);
                recordCount = 0;
            }

            writeTuple(opt, log, dbt, idmap, tableSchema, doc, &recordCount,
                    &insertBuffer);
        }

        free(buffer);

    } else {
        if (level > 3)
            record += "},";
    }
    level--;
    return true;
}

bool JSONHandler::StartArray()
{
    if (level == 1) {
        active = true;
        if (pass == 2)
            beginInserts(tableSchema.tableName, &insertBuffer);
    } else {
        if (level > 1)
            record += '[';
    }
    level++;
    return true;
}

bool JSONHandler::EndArray(json::SizeType elementCount)
{
    if (level == 2) {
        active = false;
        if (recordCount > 0)
            if (pass == 2)
                endInserts(opt, log, tableSchema.tableName, &insertBuffer, dbc);
    } else {
        if (level > 2)
            record += "],";
    }
    level--;
    return true;
}

bool JSONHandler::Key(const char* str, json::SizeType length, bool copy)
{
    record += '\"';
    record += str;
    record += "\":";
    return true;
}

static void encodeJSON(const char* str, string* newstr)
{
    char buffer[8];
    const char *p = str;
    char c;
    while ( (c=*p) != '\0') {
        switch (c) {
            case '"':
                *newstr += "\\\"";
                break;
            case '\\':
                (*newstr) += "\\\\";
                break;
            case '\b':
                *newstr += "\\b";
                break;
            case '\f':
                *newstr += "\\f";
                break;
            case '\n':
                *newstr += "\\n";
                break;
            case '\r':
                *newstr += "\\r";
                break;
            case '\t':
                *newstr += "\\t";
                break;
            default:
                if (isprint(c)) {
                    *newstr += c;
                } else {
                    sprintf(buffer, "\\u%04X", (unsigned char) c);
                    *newstr += buffer;
                }
        }
        p++;
    }
}

bool JSONHandler::String(const char* str, json::SizeType length, bool copy)
{
    if (active && (level > 2) ) {
        record += '\"';
        string encStr;
        encodeJSON(str, &encStr);
        record += encStr;
        record += "\",";
    }
    return true;
}

bool JSONHandler::Int(int i)
{
    if ( active && (level > 2) ) {
        record += to_string(i);
        record += ',';
    }
    return true;
}

bool JSONHandler::Uint(unsigned u)
{
    if ( active && (level > 2) ) {
        record += to_string(u);
        record += ',';
    }
    return true;
}

bool JSONHandler::Int64(int64_t i)
{
    if ( active && (level > 2) ) {
        record += to_string(i);
        record += ',';
    }
    return true;
}

bool JSONHandler::Uint64(uint64_t u)
{
    if ( active && (level > 2) ) {
        record += to_string(u);
        record += ',';
    }
    return true;
}

bool JSONHandler::Double(double d)
{
    if ( active && (level > 2) ) {
        record += to_string(d);
        record += ',';
    }
    return true;
}

bool JSONHandler::Bool(bool b)
{
    if ( active && (level > 2) )
        record += b ? "true," : "false,";
    return true;

}

bool JSONHandler::Null()
{
    if ( active && (level > 2) )
        record += "null,";
    return true;
}

size_t readPageCount(const Options& opt, Log* log, const string& loadDir,
        const string& tableName)
{
    string filename = loadDir;
    etymon::join(&filename, tableName);
    filename += "_count.txt";
    if ( !(etymon::fileExists(filename)) ) {
        log->log(Level::warning, "", "", "File not found: " + filename, -1);
        return 0;
    }
    etymon::File f(filename, "r");
    size_t count;
    int r = fscanf(f.file, "%zu", &count);
    if (r < 1 || r == EOF)
        throw runtime_error("unable to read page count from " + filename);
    return count;
}

static void stagePage(const Options& opt, Log* log, int pass,
        const TableSchema& tableSchema, etymon::OdbcEnv* odbc,
        etymon::OdbcDbc* dbc, const DBType &dbt, map<string,Counts>* stats,
        const string& filename, char* readBuffer, size_t readBufferSize,
        IDMap* idmap)
{
    json::Reader reader;
    etymon::File f(filename, "r");
    json::FileReadStream is(f.file, readBuffer, readBufferSize);
    JSONHandler handler(pass, opt, log, tableSchema, dbc, dbt, idmap, stats);
    reader.Parse(is, handler);
}

static void composeDataFilePath(const string& loadDir,
        const TableSchema& table, const string& suffix, string* path)
{
    *path = loadDir;
    etymon::join(path, table.tableName);
    *path += suffix;
}

static void createLoadingTable(const Options& opt, Log* log,
        const TableSchema& table, etymon::OdbcEnv* odbc, etymon::OdbcDbc* dbc,
        const DBType& dbt)
{
    string sequenceName = table.tableName + "_row_id_seq";
    string loadingTable;
    loadingTableName(table.tableName, &loadingTable);
    string sql;

    // Start auto-increment after max(row_id) to avoid reusing values.
    //int64_t autoIncStart = 1;  // Default to 1 if no data available.

    //{
    //    etymon::OdbcDbc dbc1(odbc, opt.db);
    //    sql = "SELECT max(row_id) FROM " + table.tableName + ";";
    //    log->log(Level::detail, "", "", sql, -1);
    //    try {
    //        etymon::OdbcStmt stmt(&dbc1);
    //        dbc1.execDirect(&stmt, sql);
    //        dbc1.fetch(&stmt);
    //        string maxRowId;
    //        dbc1.getData(&stmt, 1, &maxRowId);
    //        if (maxRowId != "NULL") {
    //            int64_t start = stol(maxRowId) + 1;
    //            if (start < 1000000000000000000)
    //                autoIncStart = start;
    //        }
    //    } catch (runtime_error& e) {}
    //}

    //dbt.renameSequence(sequenceName, sequenceName + "_old", &sql);
    //if (sql != "") {
    //    log->log(Level::detail, "", "", sql, -1);
    //    dbc->execDirect(nullptr, sql);
    //}

    //dbt.createSequence(sequenceName, autoIncStart, &sql);
    //if (sql != "") {
    //    log->log(Level::detail, "", "", sql, -1);
    //    dbc->execDirect(nullptr, sql);
    //}

    string rskeys;
    dbt.redshiftKeys("sk", "sk", &rskeys);
    //string autoInc;
    //dbt.autoIncrementType(autoIncStart, true, sequenceName, &autoInc);
    sql = "CREATE TABLE ";
    sql += loadingTable;
    sql += " (\n"
        //"    row_id " + autoInc + ",\n"
        "    sk BIGINT NOT NULL,\n"
        "    id VARCHAR(65535) NOT NULL,\n";
    string columnType;
    for (const auto& column : table.columns) {
        if (column.columnName != "id") {
            if (column.columnType == ColumnType::id) {
                sql += "    \"";
                sql += column.columnName;
                sql += "_sk\" BIGINT,\n";
            }
            sql += "    \"";
            sql += column.columnName;
            sql += "\" ";
            ColumnSchema::columnTypeToString(column.columnType, &columnType);
            sql += columnType;
            sql += ",\n";
        }
    }
    sql += string("    data ") + dbt.jsonType() + ",\n"
        //"    updated TIMESTAMPTZ NOT NULL,\n"
        //"    updated " + string(dbt.timestamp0()) + " NOT NULL,\n"
        "    tenant_id SMALLINT NOT NULL,\n"
        "    PRIMARY KEY (sk),\n"
        "    UNIQUE (id)\n"
        ")" + rskeys + ";";
    log->log(Level::detail, "", "", sql, -1);
    dbc->execDirect(nullptr, sql);

    //dbt.alterSequenceOwnedBy(sequenceName, loadingTable + ".row_id", &sql);
    //if (sql != "") {
    //    log->log(Level::detail, "", "", sql, -1);
    //    dbc->execDirect(nullptr, sql);
    //}

    // Add comment on table.
    if (table.moduleName != "mod-agreements") {
        sql = "COMMENT ON TABLE " + loadingTable + "\n"
            "    IS '";
        sql += table.sourcePath;
        sql += " in ";
        sql += table.moduleName;
        sql += ": ";
        sql += "https://dev.folio.org/reference/api/#";
        sql += table.moduleName;
        sql += "';";
        log->log(Level::detail, "", "",
                "Setting comment on table: " + table.tableName, -1);
        dbc->execDirect(nullptr, sql);
    }

    sql = "GRANT SELECT ON " + loadingTable + " TO " + opt.ldpUser + ";";
    log->logDetail(sql);
    dbc->execDirect(nullptr, sql);
}

void stageTable(const Options& opt, Log* log, TableSchema* table,
        etymon::OdbcEnv* odbc, etymon::OdbcDbc* dbc, DBType* dbt,
        const string& loadDir)
{
    IDMap idmap(dbc, dbt, log);

    size_t pageCount = readPageCount(opt, log, loadDir, table->tableName);

    log->log(Level::detail, "", "",
            "Staging: " + table->tableName + ": page count: " +
            to_string(pageCount), -1);

    // TODO remove this and create the load table from merge.cpp after
    // pass 1
    //createStagingTable(opt, table->tableName, db);

    map<string,Counts> stats;
    char readBuffer[65536];

    for (int pass = 1; pass <= 2; pass++) {

        log->log(Level::detail, "", "",
                "Staging: " + table->tableName +
                (pass == 1 ?  ": analyze" : ": load"), -1);

        for (size_t page = 0; page < pageCount; page++) {
            string path;
            composeDataFilePath(loadDir, *table,
                    "_" + to_string(page) + ".json", &path);
            log->log(Level::detail, "", "",
                    "Staging: " + table->tableName +
                    (pass == 1 ?  ": analyze" : ": load") + ": page: " +
                    to_string(page), -1);
            stagePage(opt, log, pass, *table, odbc, dbc, *dbt, &stats, path,
                    readBuffer, sizeof readBuffer, &idmap);
        }

        if (opt.loadFromDir != "") {
            string path;
            composeDataFilePath(loadDir, *table, "_test.json", &path);
            if (etymon::fileExists(path)) {
                log->log(Level::detail, "", "",
                        "Staging: " + table->tableName +
                        (pass == 1 ?  ": analyze" : ": load") +
                        ": test file", -1);
                stagePage(opt, log, pass, *table, odbc, dbc, *dbt, &stats, path,
                        readBuffer, sizeof readBuffer, &idmap);
            }
        }

        if (pass == 1) {
            for (const auto& [field, counts] : stats) {
                log->log(Level::detail, "", "",
                        "Stats: in field: " + field, -1);
                log->log(Level::detail, "", "",
                        "Stats: string: " + to_string(counts.string), -1);
                log->log(Level::detail, "", "",
                        "Stats: datetime: " + to_string(counts.dateTime), -1);
                log->log(Level::detail, "", "",
                        "Stats: bool: " + to_string(counts.boolean), -1);
                log->log(Level::detail, "", "",
                        "Stats: number: " + to_string(counts.number), -1);
                log->log(Level::detail, "", "",
                        "Stats: int: " + to_string(counts.integer), -1);
                log->log(Level::detail, "", "",
                        "Stats: float: " + to_string(counts.floating), -1);
                log->log(Level::detail, "", "",
                        "Stats: null: " + to_string(counts.null), -1);
            }
        }

        if (pass == 1) {
            for (const auto& [field, counts] : stats) {
                ColumnSchema column;
                column.columnType = ColumnSchema::selectColumnType(counts);
                string typeStr;
                ColumnSchema::columnTypeToString(column.columnType, &typeStr);
                string newattr;
                decodeCamelCase(field.c_str(), &newattr);
                log->log(Level::detail, "", "",
                        string("Column: ") + newattr + string(" ") + typeStr,
                        -1);
                column.columnName = newattr;
                column.sourceColumnName = field;
                table->columns.push_back(column);
            }
            createLoadingTable(opt, log, *table, odbc, dbc, *dbt);
        }

    }

}

//void stageAll(const Options& opt, Schema* schema, etymon::Postgres* db,
//        const string& loadDir)
//{
//    print(Print::verbose, opt, "staging in target: " + opt.target);
//    for (auto& table : schema->tables) {
//        if (table.skip)
//            continue;
//        print(Print::verbose, opt, "staging table: " + table.tableName);
//        stageTable(opt, &table, db, loadDir);
//    }
//}

