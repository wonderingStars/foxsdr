// CSV text, strict and bounded: the reader for every CSV the tuning lane takes from outside (a marker file, a station database) and the writer for
// every CSV it hands out (a marker export).
//
// READ (RFC 4180 with the usual tolerances): fields separated by `delimiter` (default ','); a field may be quoted with ", a quote inside is "";
// quoted fields may hold the delimiter and line breaks; records end at LF or CRLF; a UTF-8 byte-order mark at the start is dropped; a last line without a
// line break counts; a completely empty line is skipped. Refused, with the line number: a NUL byte, a quote opening in the middle of an unquoted
// field, text after a closing quote, a quote never closed, more than limits.maxCols fields in a record, more than limits.maxRows records, a field
// longer than limits.maxCell, input longer than limits.maxBytes. Rows may have different lengths (the caller decides whether that matters).
//
// WRITE: csvField() quotes when the field holds the delimiter, a quote, CR or LF; with `guard` set a field that starts with = + - @ TAB or CR gets a
// leading ' so a spreadsheet does not run it as a formula (the usual guard against "CSV injection"). The guard changes the text; a reader that wants
// the original calls csvUnguard().
//
// Pure, no file, thread safe (no state).
//
// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cascade::core::tuning {

struct CsvLimits {
    std::size_t maxBytes = 20u * 1024u * 1024u;
    std::size_t maxRows = 200000;
    std::size_t maxCols = 64;
    std::size_t maxCell = 4096;
};

inline bool parseCsv(std::string_view text, std::vector<std::vector<std::string>>& rows, const CsvLimits& lim, std::string& err, char delimiter = ',') {
    rows.clear();
    err.clear();
    auto fail = [&](std::size_t line, const char* m) {
        err = "line " + std::to_string(line) + ": " + m;
        rows.clear();
        return false;
    };
    if (text.size() > lim.maxBytes) { return fail(1, "the file is too large"); }
    if (delimiter == '"' || delimiter == '\n' || delimiter == '\r' || delimiter == '\0') { return fail(1, "bad delimiter"); }
    std::size_t i = 0;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) { i = 3; }
    std::vector<std::string> rec;
    std::string field;
    std::size_t line = 1;
    bool inQuotes = false, afterQuote = false, fieldStarted = false, recStarted = false;
    auto endField = [&]() -> bool {
        if (field.size() > lim.maxCell) { return false; }
        rec.push_back(std::move(field));
        field.clear();
        fieldStarted = afterQuote = false;
        return rec.size() <= lim.maxCols;
    };
    auto endRecord = [&]() -> int {  // 0 ok, 1 too wide or a cell too long, 2 too many rows
        if (!recStarted && !fieldStarted && rec.empty() && field.empty()) { return 0; }  // an empty line
        if (!endField()) { return 1; }
        if (rows.size() >= lim.maxRows) { return 2; }
        rows.push_back(std::move(rec));
        rec.clear();
        recStarted = false;
        return 0;
    };
    for (; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\0') { return fail(line, "a NUL byte"); }
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') { field.push_back('"'); ++i; }
                else { inQuotes = false; afterQuote = true; }
            } else {
                if (c == '\n') { ++line; }
                field.push_back(c);
                if (field.size() > lim.maxCell) { return fail(line, "a field is too long"); }
            }
            continue;
        }
        if (c == '"') {
            if (fieldStarted || afterQuote) { return fail(line, "a quote inside an unquoted field"); }
            inQuotes = true;
            fieldStarted = true;
            recStarted = true;
            continue;
        }
        if (c == delimiter) {
            recStarted = true;
            if (!endField()) { return fail(line, "too many fields or a field is too long"); }
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n') { ++i; }
            const int e = endRecord();
            if (e == 1) { return fail(line, "too many fields or a field is too long"); }
            if (e == 2) { return fail(line, "too many rows"); }
            ++line;
            continue;
        }
        if (afterQuote) { return fail(line, "text after a closing quote"); }
        field.push_back(c);
        fieldStarted = true;
        recStarted = true;
        if (field.size() > lim.maxCell) { return fail(line, "a field is too long"); }
    }
    if (inQuotes) { return fail(line, "a quote is never closed"); }
    const int e = endRecord();
    if (e == 1) { return fail(line, "too many fields or a field is too long"); }
    if (e == 2) { return fail(line, "too many rows"); }
    return true;
}

inline std::string csvField(const std::string& s, bool guard = true, char delimiter = ',') {
    std::string v = s;
    if (guard && !v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@' || v[0] == '\t' || v[0] == '\r')) { v.insert(v.begin(), '\''); }
    const bool quote = v.find(delimiter) != std::string::npos || v.find('"') != std::string::npos || v.find('\n') != std::string::npos || v.find('\r') != std::string::npos;
    if (!quote) { return v; }
    std::string o = "\"";
    for (const char c : v) {
        if (c == '"') { o.push_back('"'); }
        o.push_back(c);
    }
    o.push_back('"');
    return o;
}

// Undoes the guard: one leading ' followed by = + - @ TAB or CR.
inline std::string csvUnguard(const std::string& s) {
    if (s.size() >= 2 && s[0] == '\'' && (s[1] == '=' || s[1] == '+' || s[1] == '-' || s[1] == '@' || s[1] == '\t' || s[1] == '\r')) { return s.substr(1); }
    return s;
}

}  // namespace cascade::core::tuning
