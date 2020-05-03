// Old error printing functions

#ifndef LDP_UTIL_H
#define LDP_UTIL_H

#include <chrono>
#include <string>

#include "options.h"
#include "schema.h"

using namespace std;

enum class Print {
    error,
    warning,
    verbose,
    debug
};

void print(Print level, const Options& opt, const string& str);
void printSQL(Print level, const Options& opt, const string& sql);

void printSchema(FILE* stream, const Schema& schema);

#endif
