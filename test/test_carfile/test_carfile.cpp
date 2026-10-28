#include <unity.h>
#include <string.h>
#include <ArduinoJson.h>
#include "cfg/CarFile.h"
#include "vehicle/DecodeTable.h"
#include "vehicle/Signals.h"
#include "proto/canbox/Protocol.h"

void setUp() {}
void tearDown() {}

// The car file (docs/design/rules.md, slice 1): every table by signal NAME, so
// a firmware that adds or reorders signals reads the same file, and a name it
// does not know is skipped, never taken for another signal (1.6.3's empty
// alert rows came back as fuel_pct because they were indexes).
using namespace Vehicle;
using namespace Canbox;

namespace
{
    bool sameRow(const DecodeRow& a, const DecodeRow& b)
    {
        return a.canId == b.canId && a.startByte == b.startByte && a.numBytes == b.numBytes &&
               a.shift == b.shift && a.mask == b.mask && a.mul == b.mul && a.div == b.div &&
               a.add == b.add && a.sig == b.sig && a.flags == b.flags && a.holdMs == b.holdMs;
    }
}

// The factory table, a constant and the owner's ✓ go out and come back the
// same, through JSON text.
void test_the_decode_table_round_trips_by_name()
{
    DecodeTable t;
    t.factory();
    TEST_ASSERT_TRUE(t.bindConst(SIG_BATT_V, 1250));
    TEST_ASSERT_TRUE(t.row.size() > 20);
    JsonDocument d;
    CarFile::putDecode(d.to<JsonObject>(), t);
    std::string text;
    serializeJson(d, text);
    // By name: the file says "speed", not 0.
    TEST_ASSERT_NOT_NULL(strstr(text.c_str(), "\"sig\":\"speed\""));
    JsonDocument back;
    TEST_ASSERT_FALSE(deserializeJson(back, text));
    DecodeTable u;
    uint16_t skipped = 9;
    TEST_ASSERT_NULL(CarFile::getDecode(back.as<JsonObjectConst>(), u, skipped));
    TEST_ASSERT_EQUAL_UINT16(0, skipped);
    TEST_ASSERT_EQUAL_UINT8(t.row.size(), u.row.size());
    TEST_ASSERT_EQUAL_UINT8(t.k.size(), u.k.size());
    for (uint8_t i = 0; i < t.row.size(); ++i) TEST_ASSERT_TRUE_MESSAGE(sameRow(t.row[i], u.row[i]), meta(t.row[i].sig).name);
    TEST_ASSERT_EQUAL_INT32(1250, u.k[u.constOf(SIG_BATT_V)].value);
}

// A name this firmware does not know (a newer image's signal, a typo) is
// skipped and counted; the rest reads.
void test_an_unknown_name_is_skipped_not_guessed()
{
    const char* f = R"({"decode":{"rows":[
        {"sig":"warp_drive","can":{"id":"0x123","byte":0,"len":1,"shift":0,"mask":"FF","mul":1,"div":1,"add":0}},
        {"sig":"speed","can":{"id":"0x354","byte":0,"len":2,"shift":0,"mask":"FFFF","mul":1,"div":1,"add":0}}],
        "consts":[{"sig":"nope","value":1}]}})";
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, f));
    DecodeTable t;
    uint16_t skipped = 0;
    TEST_ASSERT_NULL(CarFile::getDecode(d.as<JsonObjectConst>(), t, skipped));
    TEST_ASSERT_EQUAL_UINT16(2, skipped);
    TEST_ASSERT_EQUAL_UINT8(1, t.row.size());
    TEST_ASSERT_EQUAL_UINT8(SIG_SPEED, t.row[0].sig);
    TEST_ASSERT_EQUAL_UINT8(0, t.k.size());
}

// A bad row refuses the whole table (nothing half-read), and says where.
void test_a_bad_row_refuses_the_table()
{
    const char* f = R"({"decode":{"rows":[
        {"sig":"speed","can":{"id":"0x354","byte":9,"len":2,"shift":0,"mask":"FFFF","mul":1,"div":1,"add":0}}]}})";
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, f));
    DecodeTable t;
    uint16_t skipped = 0;
    TEST_ASSERT_EQUAL_STRING("can.byte: param", CarFile::getDecode(d.as<JsonObjectConst>(), t, skipped));
    TEST_ASSERT_EQUAL_UINT8(0, t.row.size());
    // No decode at all: the caller's factory table.
    JsonDocument e;
    e.to<JsonObject>();
    TEST_ASSERT_EQUAL_STRING("decode: missing", CarFile::getDecode(e.as<JsonObjectConst>(), t, skipped));
}

// Rows come back sorted by id, whatever order the file has (CanDecode needs it).
void test_rows_come_back_sorted_by_id()
{
    const char* f = R"({"decode":{"rows":[
        {"sig":"rpm","can":{"id":"0x500","byte":0,"len":2,"shift":0,"mask":"FFFF","mul":1,"div":1,"add":0}},
        {"sig":"speed","can":{"id":"0x354","byte":0,"len":2,"shift":0,"mask":"FFFF","mul":1,"div":1,"add":0}}]}})";
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, f));
    DecodeTable t;
    uint16_t skipped = 0;
    TEST_ASSERT_NULL(CarFile::getDecode(d.as<JsonObjectConst>(), t, skipped));
    TEST_ASSERT_EQUAL_HEX32(0x354, t.row[0].canId);
    TEST_ASSERT_EQUAL_HEX32(0x500, t.row[1].canId);
}

// The field map: per protocol, by field and source name; another protocol's
// entry is left as it is.
void test_the_field_map_round_trips_per_protocol()
{
    const Protocol& p = activeProtocol();
    FieldMap m;
    FieldRule a;
    a.field = SIG_RANGE; a.kind = FK_SIGNAL; a.src = SIG_RANGE_CALC; a.div = 2; a.add = 5;
    FieldRule b;
    b.field = SIG_ENG_TEMP; b.kind = FK_CONST; b.add = 70;
    FieldRule c;
    c.field = SIG_PRND; c.kind = FK_SIGNAL; c.src = SIG_PRND;   // "own"
    TEST_ASSERT_TRUE(m.set(a) && m.set(b) && m.set(c));
    JsonDocument d;
    JsonObject car = d.to<JsonObject>();
    car["hu"]["other"]["fields"].to<JsonArray>().add("kept");
    CarFile::putFields(car, p, m);
    std::string text;
    serializeJson(d, text);
    TEST_ASSERT_NOT_NULL(strstr(text.c_str(), "\"src\":\"range_calc\""));
    TEST_ASSERT_NOT_NULL(strstr(text.c_str(), "\"other\":{\"fields\":[\"kept\"]}"));
    JsonDocument back;
    TEST_ASSERT_FALSE(deserializeJson(back, text));
    FieldMap n;
    uint16_t skipped = 0;
    TEST_ASSERT_NULL(CarFile::getFields(back.as<JsonObjectConst>(), p, n, skipped));
    TEST_ASSERT_EQUAL_UINT8(3, n.count());
    const FieldRule* r = n.find(SIG_RANGE);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8(SIG_RANGE_CALC, r->src);
    TEST_ASSERT_EQUAL_INT16(2, r->div);
    TEST_ASSERT_EQUAL_INT32(5, r->add);
    TEST_ASSERT_EQUAL_UINT8(FK_CONST, n.find(SIG_ENG_TEMP)->kind);
    TEST_ASSERT_EQUAL_UINT8(SIG_PRND, n.find(SIG_PRND)->src);
    // An unknown field or source: skipped.
    const char* f = R"({"hu":{"raise112":{"fields":[{"field":"warp","from":"off"},
        {"field":"range","from":"sig","src":"warp"},{"field":"eng_temp","from":"off"}]}}})";
    JsonDocument e;
    TEST_ASSERT_FALSE(deserializeJson(e, f));
    TEST_ASSERT_NULL(CarFile::getFields(e.as<JsonObjectConst>(), p, n, skipped));
    TEST_ASSERT_EQUAL_UINT16(2, skipped);
    TEST_ASSERT_EQUAL_UINT8(1, n.count());
    // None for this protocol: an empty map.
    JsonDocument none;
    none.to<JsonObject>();
    TEST_ASSERT_NULL(CarFile::getFields(none.as<JsonObjectConst>(), p, n, skipped));
    TEST_ASSERT_EQUAL_UINT8(0, n.count());
}

// One rule, the API's shape: auto clears, a bad source or divisor is refused.
void test_a_rule_reads_like_the_api_sends_it()
{
    FieldRule r;
    bool clear = false;
    JsonDocument d;
    TEST_ASSERT_FALSE(deserializeJson(d, R"({"from":"auto"})"));
    TEST_ASSERT_NULL(CarFile::ruleFrom(d.as<JsonObjectConst>(), SIG_RANGE, r, clear));
    TEST_ASSERT_TRUE(clear);
    TEST_ASSERT_FALSE(deserializeJson(d, R"({"from":"sig","src":"nope"})"));
    TEST_ASSERT_EQUAL_STRING("src: param", CarFile::ruleFrom(d.as<JsonObjectConst>(), SIG_RANGE, r, clear));
    TEST_ASSERT_FALSE(deserializeJson(d, R"({"from":"sig","src":"range_calc","div":0})"));
    TEST_ASSERT_EQUAL_STRING("div: param", CarFile::ruleFrom(d.as<JsonObjectConst>(), SIG_RANGE, r, clear));
    TEST_ASSERT_FALSE(deserializeJson(d, R"({"from":"maybe"})"));
    TEST_ASSERT_EQUAL_STRING("from: param", CarFile::ruleFrom(d.as<JsonObjectConst>(), SIG_RANGE, r, clear));
    char b[160];
    FieldRule k;
    k.field = SIG_ENG_TEMP; k.kind = FK_CONST; k.add = 70;
    CarFile::ruleJson(b, sizeof(b), k);
    TEST_ASSERT_EQUAL_STRING("{\"from\":\"k\",\"src\":\"\",\"mul\":1,\"div\":1,\"add\":70}", b);
}

// 1.6.x's NVS blob (indexes) reads into the table CarStore then writes by name.
void test_the_old_nvs_blob_carries_into_the_file()
{
    DecodeTable t;
    t.factory();
    TEST_ASSERT_TRUE(t.bindConst(SIG_BATT_V, 1300));
    uint8_t blob[DecodeTable::BLOB_MAX];
    const size_t n = t.pack(blob);
    DecodeTable u;
    TEST_ASSERT_TRUE(u.unpack(blob, n));
    JsonDocument d;
    CarFile::putDecode(d.to<JsonObject>(), u);
    DecodeTable w;
    uint16_t skipped = 0;
    TEST_ASSERT_NULL(CarFile::getDecode(d.as<JsonObjectConst>(), w, skipped));
    TEST_ASSERT_EQUAL_UINT8(t.row.size(), w.row.size());
    TEST_ASSERT_EQUAL_INT32(1300, w.k[w.constOf(SIG_BATT_V)].value);
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_decode_table_round_trips_by_name);
    RUN_TEST(test_an_unknown_name_is_skipped_not_guessed);
    RUN_TEST(test_a_bad_row_refuses_the_table);
    RUN_TEST(test_rows_come_back_sorted_by_id);
    RUN_TEST(test_the_field_map_round_trips_per_protocol);
    RUN_TEST(test_a_rule_reads_like_the_api_sends_it);
    RUN_TEST(test_the_old_nvs_blob_carries_into_the_file);
    return UNITY_END();
}
