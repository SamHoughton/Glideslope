#pragma once
/*
Purpose: Parsing the web server's requests: the request line, a header by
name, Content-Length, and a query parameter. Pure functions over the raw
header text (at most 4 KB, read by WebConfig), so the native tests can feed
them malformed and random input.
*/
#include <Arduino.h>

namespace HttpRequest
{
struct Line
{
    String method, path, query;
};

// "GET /api/frame?z=1 HTTP/1.1\r\n...": false unless there is a method, a
// path starting with '/' and a protocol after it.
bool parseLine(const String &raw, Line &out);

// A header's value, case-insensitive name, trimmed; "" when absent.
String header(const String &raw, const char *name);

// Content-Length: its value, or -1 when absent, not a plain number, or
// larger than 100 MB.
long contentLength(const String &raw);

// A query parameter's value ("" when absent), matching whole keys only
// ("z" doesn't match "fz=1").
String qparam(const String &query, const char *key);
}
