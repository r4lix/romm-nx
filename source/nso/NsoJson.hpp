#pragma once

#include <cstddef>
#include <string>
#include <vector>

// Structural JSON helpers for editing files romm-nx does not own.
//
// The SNES Online database (`lclassics.titlesdb`) and the localized string
// table (`strings.lng`) are written by Nintendo's tooling and by CaVE, and
// carry fields romm-nx has no model for (`mouse_type`, `startup_state`,
// `hidden_countries`, `repo_type`, ...). Round-tripping them through a
// deserialize/serialize pair would silently drop everything unmodelled, so
// instead every edit here is a *byte-span splice*: existing bytes are never
// re-encoded, only new members are inserted and, when reinstalling the same
// ROM, one member's value is replaced in place.
//
// This is not a general-purpose parser: it validates structure, enumerates
// object members with their byte spans, and escapes strings. Values are handed
// back as raw slices of the document. It never throws (the project builds with
// -fno-exceptions) — every entry point reports failure by return value.
namespace romm::nso::json {

    struct Member {
        std::string key;
        size_t key_start = 0;    // index of the opening quote of the key
        size_t value_start = 0;  // first byte of the value
        size_t value_end = 0;    // one past the last byte of the value
    };

    // Full structural validation of a whole document (objects, arrays,
    // strings, numbers, literals, nesting). `error` gets a human-readable
    // reason with a byte offset when validation fails.
    bool Validate(const std::string& text, std::string& error);

    // First non-whitespace byte, or npos when the document is blank.
    size_t SkipWhitespace(const std::string& text, size_t pos);

    // Given `pos` at the first byte of a JSON value, returns one past its last
    // byte, or npos if the value is malformed. Handles all six JSON types.
    size_t SkipValue(const std::string& text, size_t pos);

    // Enumerates the members of the object whose '{' is at `obj_start`.
    // Returns false if that is not a well-formed object.
    bool ScanObject(const std::string& text, size_t obj_start, std::vector<Member>& out);

    // Locates a member of the document's root object. Returns false when the
    // root is not an object or the key is absent.
    bool FindRootMember(const std::string& text, const std::string& key, Member& out);

    // Finds one member inside an already-scanned list.
    const Member* FindMember(const std::vector<Member>& members, const std::string& key);

    // Inserts `"key":value` into the object whose '{' is at `obj_start`,
    // immediately before its closing brace, adding a separating comma when the
    // object is not empty. Returns the new document, or false on malformed
    // input. Existing bytes are copied verbatim.
    bool InsertMember(const std::string& text, size_t obj_start,
                      const std::string& key, const std::string& value_json,
                      std::string& out, std::string& error);

    // Replaces the value of an existing member (span from ScanObject) with
    // `value_json`, leaving every other byte untouched.
    std::string ReplaceValue(const std::string& text, const Member& member, const std::string& value_json);

    // Escapes a UTF-8 string for embedding as a JSON string *including* the
    // surrounding quotes. Non-ASCII bytes are passed through unchanged, which
    // is what the existing database does (its "©" is literal UTF-8, not ©).
    std::string QuoteString(const std::string& value);

    // Decodes a JSON string literal (starting at its opening quote) into its
    // UTF-8 value. Returns false when the literal is malformed.
    bool DecodeString(const std::string& text, size_t quote_pos, std::string& out);

}
