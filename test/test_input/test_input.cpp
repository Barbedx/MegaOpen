#include <unity.h>
#include "input/MatrixScanner.h"
#include "input/KeyDebouncer.h"
#include "input/EncoderDecoder.h"

using namespace Input;

void setUp() {}
void tearDown() {}

// ---- fake matrix ------------------------------------------------------------
struct FakeMatrix {
    uint8_t rows[3] = {0, 0, 0};   // what each row reads when driven
    uint8_t driven = 0xFF;

    static void drive(uint8_t row, bool active, void* ctx) {
        auto* f = (FakeMatrix*)ctx;
        f->driven = active ? row : 0xFF;
    }
    static uint8_t read(void* ctx) {
        auto* f = (FakeMatrix*)ctx;
        return f->driven < 3 ? f->rows[f->driven] : 0;
    }
    void press(uint8_t pos) { rows[pos / 3] |= (uint8_t)(1u << (pos % 3)); }
    void release(uint8_t pos) { rows[pos / 3] &= (uint8_t)~(1u << (pos % 3)); }
};

void test_scan_maps_position_bits()
{
    FakeMatrix f; MatrixScanner s;
    s.begin(FakeMatrix::drive, FakeMatrix::read, nullptr, &f);

    f.press(4);                                   // r1c1
    TEST_ASSERT_EQUAL_UINT16(1u << 4, s.scan());

    f.press(0);
    TEST_ASSERT_EQUAL_UINT16((1u << 4) | 1u, s.scan());
}

void test_ghost_needs_two_shared_columns()
{
    // One shared column is unambiguous — and it happens constantly here, because
    // the encoder row is closed while a normal key is held. It must NOT be
    // rejected.
    uint8_t oneShared[3] = {0b001, 0b001, 0};
    TEST_ASSERT_FALSE(MatrixScanner::isGhosted(oneShared));

    uint8_t twoShared[3] = {0b011, 0b011, 0};
    TEST_ASSERT_TRUE(MatrixScanner::isGhosted(twoShared));
}

void test_ghosted_sweep_keeps_previous_state()
{
    FakeMatrix f; MatrixScanner s;
    s.begin(FakeMatrix::drive, FakeMatrix::read, nullptr, &f);
    f.press(0);
    TEST_ASSERT_EQUAL_UINT16(1u, s.scan());

    f.rows[0] = 0b011; f.rows[1] = 0b011;         // ambiguous
    TEST_ASSERT_EQUAL_UINT16(1u, s.scan());       // previous bitmap, not zero
    TEST_ASSERT_EQUAL_UINT32(1, s.ghosts());
}

// ---- debounce ---------------------------------------------------------------
static uint8_t pump(KeyDebouncer& d, uint16_t raw, uint32_t& t, uint32_t ms,
                    PosEvent* out, uint8_t maxOut)
{
    uint8_t total = 0;
    for (uint32_t i = 0; i < ms; ++i) {
        total += d.update(raw, t, out + total, (uint8_t)(maxOut - total));
        ++t;
    }
    return total;
}

void test_debounce_press_long_repeat()
{
    KeyDebouncer d;
    KeyDebouncer::Params p;              // 20 / 2 / 600 / 150
    d.configure(p, 0);
    uint32_t t = 1000;
    PosEvent ev[12];

    TEST_ASSERT_EQUAL_UINT8(0, pump(d, 1u << 3, t, 10, ev, 12));   // still settling
    uint8_t n = pump(d, 1u << 3, t, 15, ev, 12);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_EQUAL_UINT8(3, ev[0].pos);
    TEST_ASSERT_TRUE(ev[0].edge == Edge::Down);

    n = pump(d, 1u << 3, t, 600, ev, 12);        // long, then repeats
    TEST_ASSERT_TRUE(n >= 1);
    TEST_ASSERT_TRUE(ev[0].edge == Edge::Long);

    n = pump(d, 0, t, 30, ev, 12);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_TRUE(ev[0].edge == Edge::Up);
}

void test_fast_positions_settle_quicker_and_never_hold()
{
    KeyDebouncer d;
    KeyDebouncer::Params p;
    d.configure(p, 1u << 6);                      // position 6 is an encoder phase
    uint32_t t = 0;
    PosEvent ev[12];

    uint8_t n = pump(d, 1u << 6, t, 3, ev, 12);
    TEST_ASSERT_EQUAL_UINT8(1, n);
    TEST_ASSERT_TRUE(ev[0].edge == Edge::Down);

    n = pump(d, 1u << 6, t, 1200, ev, 12);        // held far past longMs
    TEST_ASSERT_EQUAL_UINT8(0, n);                // no Long, no Repeat for a phase
}

// ---- three-phase encoder ----------------------------------------------------
void test_encoder_direction_from_cyclic_order()
{
    EncoderDecoder e;
    e.setPhases(6, 7, 8);

    TEST_ASSERT_EQUAL_INT8(0, e.onDown(6));       // first contact only seeds
    TEST_ASSERT_EQUAL_INT8(-1, e.onDown(7));      // A->B
    TEST_ASSERT_EQUAL_INT8(-1, e.onDown(8));      // B->C, same way round
    TEST_ASSERT_EQUAL_INT8(-1, e.onDown(6));      // C->A wraps, still same way

    TEST_ASSERT_EQUAL_INT8(+1, e.onDown(8));      // A->C, the other way
    TEST_ASSERT_EQUAL_INT8(+1, e.onDown(7));
}

void test_encoder_ignores_repeat_and_foreign_positions()
{
    EncoderDecoder e;
    e.setPhases(6, 7, 8);
    e.onDown(6);
    TEST_ASSERT_EQUAL_INT8(0, e.onDown(6));       // same phase again: no step
    TEST_ASSERT_EQUAL_INT8(0, e.onDown(2));       // not a phase at all
    TEST_ASSERT_FALSE(e.isPhase(2));
    TEST_ASSERT_TRUE(e.isPhase(8));
}

void test_encoder_invert_flips_both_directions()
{
    EncoderDecoder e;
    e.setPhases(6, 7, 8);
    e.setInverted(true);
    e.onDown(6);
    TEST_ASSERT_EQUAL_INT8(+1, e.onDown(7));
    TEST_ASSERT_EQUAL_INT8(-1, e.onDown(6));
}

int main(int, char**)
{
    UNITY_BEGIN();
    RUN_TEST(test_scan_maps_position_bits);
    RUN_TEST(test_ghost_needs_two_shared_columns);
    RUN_TEST(test_ghosted_sweep_keeps_previous_state);
    RUN_TEST(test_debounce_press_long_repeat);
    RUN_TEST(test_fast_positions_settle_quicker_and_never_hold);
    RUN_TEST(test_encoder_direction_from_cyclic_order);
    RUN_TEST(test_encoder_ignores_repeat_and_foreign_positions);
    RUN_TEST(test_encoder_invert_flips_both_directions);
    return UNITY_END();
}
