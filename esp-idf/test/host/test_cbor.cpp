/* Host test for the RRC CBOR codec against encodings made by cbor2.
 *
 *   g++ -std=c++17 -I../../src test_cbor.cpp ../../src/rrc_cbor.cpp -o /tmp/test_cbor && /tmp/test_cbor
 */
#include "rrc_cbor.h"
#include "vectors.h"

#include <cstdio>
#include <cstring>

using rrccbor::Value;
using rrccbor::Writer;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

template <size_t N>
static bool same(const std::vector<uint8_t>& a, const uint8_t (&b)[N])
{
    return a.size() == N && std::memcmp(a.data(), b, N) == 0;
}

int main()
{
    /* Encoding a MSG envelope the way rrc.cpp does matches cbor2 byte for byte. */
    {
        Writer w;
        w.map(8);
        w.uint(0); w.uint(1);
        w.uint(1); w.uint(20);
        uint8_t id[8]; std::memset(id, 1, 8);
        w.uint(2); w.bytes(id, 8);
        w.uint(3); w.uint(5);
        uint8_t src[16]; std::memset(src, 0xaa, 16);
        w.uint(4); w.bytes(src, 16);
        w.uint(5); w.text("#test");
        w.uint(6); w.text("h\xc3\xa9llo w\xc3\xb6rld");
        w.uint(7); w.text("rop");
        CHECK(same(w.out, vec_msg));
    }
    /* The announce map, text keys. */
    {
        Writer w;
        w.map(3);
        w.text("proto"); w.text("rrc");
        w.text("v"); w.uint(1);
        w.text("hub"); w.text("spangap test hub");
        CHECK(same(w.out, vec_announce));
    }
    /* Decode WELCOME and walk to the limits. */
    {
        Value v;
        CHECK(rrccbor::decode(vec_welcome, sizeof vec_welcome, v));
        CHECK(v.isMap() && v.mapSize() == 6);
        CHECK(v.at(1) && v.at(1)->u == 2);
        CHECK(v.at(2) && v.at(2)->isBytes() && v.at(2)->s.size() == 8);
        CHECK(v.at(3) && v.at(3)->u == 1730419200123ull);
        const Value* body = v.at(6);
        CHECK(body && body->isMap());
        CHECK(body->at(0) && body->at(0)->s == "spangap test hub");
        const Value* caps = body->at(2);
        CHECK(caps && caps->at(2) && caps->at(2)->isBool() && caps->at(2)->b);
        const Value* lim = body->at(3);
        CHECK(lim && lim->at(2) && lim->at(2)->u == 350);
        CHECK(lim && lim->at(4) && lim->at(4)->u == 240);
    }
    /* JOINED with a member list. */
    {
        Value v;
        CHECK(rrccbor::decode(vec_joined, sizeof vec_joined, v));
        const Value* b = v.at(6);
        CHECK(b && b->isArray() && b->items.size() == 2 && b->items[1].s.size() == 16);
        CHECK(v.at(3)->u == 70000);
    }
    /* Text-keyed announce. */
    {
        Value v;
        CHECK(rrccbor::decode(vec_announce, sizeof vec_announce, v));
        CHECK(v.atText("proto") && v.atText("proto")->s == "rrc");
        CHECK(v.atText("hub") && v.atText("hub")->s == "spangap test hub");
        CHECK(v.atText("v") && v.atText("v")->u == 1);
    }
    /* Nested arrays, null, false, a negative and a 64-bit integer. */
    {
        Value v;
        CHECK(rrccbor::decode(vec_nested, sizeof vec_nested, v));
        const Value* a = v.at(0);
        CHECK(a && a->items.size() == 4);
        CHECK(a->items[2].kind == Value::NINT && a->items[2].u == 4);
        CHECK(a->items[3].u == (1ull << 40));
        CHECK(a->items[1].items[1].items[1].kind == Value::NUL);
    }
    /* Malformed input is refused, not over-read. */
    {
        Value v;
        CHECK(!rrccbor::decode(vec_msg, sizeof vec_msg - 1, v));
        uint8_t huge[] = { 0x5b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        CHECK(!rrccbor::decode(huge, sizeof huge, v));
        uint8_t bigmap[] = { 0xba, 0x7f, 0xff, 0xff, 0xff };
        CHECK(!rrccbor::decode(bigmap, sizeof bigmap, v));
        uint8_t trailing[] = { 0x01, 0x02 };
        CHECK(!rrccbor::decode(trailing, sizeof trailing, v));
        uint8_t deep[40];
        std::memset(deep, 0x81, sizeof deep);
        deep[39] = 0x00;
        CHECK(!rrccbor::decode(deep, sizeof deep, v));
    }
    std::printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
