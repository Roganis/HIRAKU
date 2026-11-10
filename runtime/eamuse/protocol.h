// SPDX-License-Identifier: GPL-3.0-or-later
// e-AMUSE3, the e-AMUSEMENT protocol of 2003 (pop'n music 9 to 14, the e-AMUSEMENT GuitarFreaks
// and DrumMania 9th to 11th): the pieces every game shares. From the Python core's server
// (python-groundwork branch, runtime/python/easerver; its docs/EAMUSE3.md has the protocol).
//
// A call is an HTTP/1.0 POST whose body is XML, RC4-encrypted with key = MD5(the 6 bytes of
// "X-Eamuse-Info: 1-xxxxxxxx-xxxx" + a shared secret): <call srcid=.. model=..><MODULE
// method=.. .../></call>. The answer is <response><MODULE status="0" ...>...</MODULE></response>.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace hk::ea {

std::vector<uint8_t> md5(const std::vector<uint8_t> &msg);
std::vector<uint8_t> key(const std::string &info);   // "1-xxxxxxxx-xxxx"
void rc4(const std::vector<uint8_t> &key, std::string &data);
// an HTTP header's value from a message's head ("" if none)
std::string header(const std::string &head, const char *name);
// An HTTP message (request or answer) as text: its first line and its body in clear.
std::string clear(const std::string &message);

// One module element of a call: <MODULE method=.. ...>inner</MODULE>. A call can carry several
// (pop'n's posevent sends three at once); the answer has one element each, in order.
struct Request {
    std::string module, method;
    std::map<std::string, std::string> attrs;
    std::string inner;   // what is inside it, raw
};
struct Call {
    std::string srcid, model;
    std::vector<Request> requests;
};
bool parse(const std::string &xml, Call &c);
// name="value" pairs from the start of an element (after its name) to its '>' (the place after it);
// *self_closing: it ends "/>"
size_t read_attrs(const std::string &x, size_t at, std::map<std::string, std::string> &out, bool *self_closing);
// Every element inside some XML (a request's `inner`), in order, flat: its name and attributes
// (<card id="..."/> -> {"card", {{"id", "..."}}}).
struct Element {
    std::string name;
    std::map<std::string, std::string> attrs;
};
std::vector<Element> elements(const std::string &xml);

// Attribute helpers
std::string esc(const std::string &v);   // XML-escaped
std::string attr(const std::map<std::string, std::string> &m, const char *k);
long num(const std::map<std::string, std::string> &m, const char *k, int base = 10);
bool safe_name(const std::string &s);   // a card number or id fit for a file name
// A whole file name in a server's folder ("pdata-F14-<ref id>", "E13-rank-shop"): letters, digits,
// - _ and . only, no dot first. The two stores refuse any other, whatever a call's fields (a model, a
// card, an id) were put into it: nothing a cabinet sends names a file outside the folder
bool safe_file(const std::string &name);

// What a server keeps: plain files in a folder, records as lines of tab-separated key=value
// fields (values never hold tabs or newlines).
using Record = std::map<std::string, std::string>;
std::string to_line(const Record &r);
std::vector<Record> from_lines(const std::string &text);

class Store {
public:
    explicit Store(std::string dir) : dir_(std::move(dir)) {}   // "" keeps nothing
    std::string read(const std::string &name) const;
    void write(const std::string &name, const std::string &data) const;   // whole, atomically
private:
    std::string dir_;
};

}  // namespace hk::ea
