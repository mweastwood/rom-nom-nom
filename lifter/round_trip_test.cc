#include "gtest/gtest.h"
#include "lifter/round_trip_test_util.h"

namespace rom_nom_nom {
namespace {

// -----------------------------------------------------------------------------
// 1. Basic Arithmetic & Bitwise Operations
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, ArithmeticAddTwo) {
  AssertRoundTripBitExact("AddTwo", R"(
s32 AddTwo(s32 a, s32 b) {
    return a + b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticSubTwo) {
  AssertRoundTripBitExact("SubTwo", R"(
s32 SubTwo(s32 a, s32 b) {
    return a - b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseAnd) {
  AssertRoundTripBitExact("BitwiseAnd", R"(
s32 BitwiseAnd(s32 a, s32 b) {
    return a & b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseOr) {
  AssertRoundTripBitExact("BitwiseOr", R"(
s32 BitwiseOr(s32 a, s32 b) {
    return a | b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseXor) {
  AssertRoundTripBitExact("BitwiseXor", R"(
s32 BitwiseXor(s32 a, s32 b) {
    return a ^ b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseNot) {
  AssertRoundTripBitExact("BitwiseNot", R"(
s32 BitwiseNot(s32 a) {
    return ~a;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftLeftImm) {
  AssertRoundTripBitExact("ShiftLeftImm", R"(
s32 ShiftLeftImm(s32 a) {
    return a << 4;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftRightArithmeticImm) {
  AssertRoundTripBitExact("ShiftRightArithmeticImm", R"(
s32 ShiftRightArithmeticImm(s32 a) {
    return a >> 3;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftRightLogicalImm) {
  AssertRoundTripBitExact("ShiftRightLogicalImm", R"(
u32 ShiftRightLogicalImm(u32 a) {
    return a >> 3;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftLeftVariable) {
  AssertRoundTripBitExact("ShiftLeftVariable", R"(
s32 ShiftLeftVariable(s32 a, s32 b) {
    return a << b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticAddImmediate) {
  AssertRoundTripBitExact("AddImmediate", R"(
s32 AddImmediate(s32 a) {
    return a + 1234;
}
)");
}

TEST_F(RoundTripTest, ArithmeticLargeImmediate) {
  AssertRoundTripBitExact("LargeImmediate", R"(
u32 LargeImmediate(u32 a) {
    return a + 0x12345678;
}
)");
}

TEST_F(RoundTripTest, ArithmeticNegate) {
  AssertRoundTripBitExact("Negate", R"(
s32 Negate(s32 a) {
    return -a;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseAndNot) {
  AssertRoundTripBitExact("AndNot", R"(
s32 AndNot(s32 a, s32 b) {
    return a & ~b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseXorImm) {
  AssertRoundTripBitExact("XorImm", R"(
s32 XorImm(s32 a) {
    return a ^ 0xFF;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseAndImm) {
  AssertRoundTripBitExact("AndImm", R"(
s32 AndImm(s32 a) {
    return a & 0x7F;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseOrImm) {
  AssertRoundTripBitExact("OrImm", R"(
s32 OrImm(s32 a) {
    return a | 0x8000;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftRightVar) {
  AssertRoundTripBitExact("ShiftRightVar", R"(
s32 ShiftRightVar(s32 a, s32 b) {
    return a >> b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftRightLogicalVar) {
  AssertRoundTripBitExact("ShiftRightLogicalVar", R"(
u32 ShiftRightLogicalVar(u32 a, u32 b) {
    return a >> b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticSubImmediate) {
  AssertRoundTripBitExact("SubImmediate", R"(
s32 SubImmediate(s32 a) {
    return a - 50;
}
)");
}

TEST_F(RoundTripTest, ArithmeticCompoundAdd) {
  AssertRoundTripBitExact("CompoundAdd", R"(
s32 CompoundAdd(s32 a, s32 b, s32 c) {
    return a + b + c;
}
)");
}

TEST_F(RoundTripTest, ArithmeticMultiOp) {
  AssertRoundTripBitExact("MultiOp", R"(
s32 MultiOp(s32 a, s32 b, s32 c) {
    return (a + b) ^ c;
}
)");
}

TEST_F(RoundTripTest, ArithmeticMaskLowSixteen) {
  AssertRoundTripBitExact("MaskLowSixteen", R"(
u32 MaskLowSixteen(u32 a) {
    return a & 0xFFFF;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseNor) {
  AssertRoundTripBitExact("BitwiseNor", R"(
s32 BitwiseNor(s32 a, s32 b) {
    return ~(a | b);
}
)");
}

TEST_F(RoundTripTest, ArithmeticMulConstant) {
  AssertRoundTripBitExact("MulConstant", R"(
s32 MulConstant(s32 a) {
    return a * 5;
}
)");
}

TEST_F(RoundTripTest, ArithmeticMulVar) {
  AssertRoundTripBitExact("MulVar", R"(
s32 MulVar(s32 a, s32 b) {
    return a * b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticDivVar) {
  AssertRoundTripBitExact("DivVar", R"(
s32 DivVar(s32 a, s32 b) {
    return a / b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticModVar) {
  AssertRoundTripBitExact("ModVar", R"(
s32 ModVar(s32 a, s32 b) {
    return a % b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticDivUnsignedVar) {
  AssertRoundTripBitExact("DivUnsignedVar", R"(
u32 DivUnsignedVar(u32 a, u32 b) {
    return a / b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticModUnsignedVar) {
  AssertRoundTripBitExact("ModUnsignedVar", R"(
u32 ModUnsignedVar(u32 a, u32 b) {
    return a % b;
}
)");
}

TEST_F(RoundTripTest, ArithmeticAddConstant) {
  AssertRoundTripBitExact("AddConstant", R"(
s32 AddConstant(s32 a) {
    return a + 42;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseXorLargeImm) {
  AssertRoundTripBitExact("XorLargeImm", R"(
u32 XorLargeImm(u32 a) {
    return a ^ 0x12345678;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseAndLargeImm) {
  AssertRoundTripBitExact("AndLargeImm", R"(
u32 AndLargeImm(u32 a) {
    return a & 0x12345678;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitwiseOrLargeImm) {
  AssertRoundTripBitExact("OrLargeImm", R"(
u32 OrLargeImm(u32 a) {
    return a | 0x12345678;
}
)");
}

// -----------------------------------------------------------------------------
// 2. Signed & Unsigned Integer Loads, Stores, and Casts
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, IntegerLoadS8) {
  AssertRoundTripBitExact("LoadS8", R"(
s32 LoadS8(s8* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreS8) {
  AssertRoundTripBitExact("StoreS8", R"(
void StoreS8(s8* p, s8 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadU8) {
  AssertRoundTripBitExact("LoadU8", R"(
u32 LoadU8(u8* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreU8) {
  AssertRoundTripBitExact("StoreU8", R"(
void StoreU8(u8* p, u8 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadS16) {
  AssertRoundTripBitExact("LoadS16", R"(
s32 LoadS16(s16* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreS16) {
  AssertRoundTripBitExact("StoreS16", R"(
void StoreS16(s16* p, s16 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadU16) {
  AssertRoundTripBitExact("LoadU16", R"(
u32 LoadU16(u16* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreU16) {
  AssertRoundTripBitExact("StoreU16", R"(
void StoreU16(u16* p, u16 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadS32) {
  AssertRoundTripBitExact("LoadS32", R"(
s32 LoadS32(s32* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreS32) {
  AssertRoundTripBitExact("StoreS32", R"(
void StoreS32(s32* p, s32 v) {
    *p = v;
}
)");
}

TEST_F(RoundTripTest, IntegerCastU8ToU32) {
  AssertRoundTripBitExact("CastU8ToU32", R"(
u32 CastU8ToU32(u32 a) {
    return (u8)a;
}
)");
}

TEST_F(RoundTripTest, IntegerCastU16ToU32) {
  AssertRoundTripBitExact("CastU16ToU32", R"(
u32 CastU16ToU32(u32 a) {
    return (u16)a;
}
)");
}

TEST_F(RoundTripTest, IntegerCastS16ToS32) {
  AssertRoundTripBitExact("CastS16ToS32", R"(
s32 CastS16ToS32(s32 a) {
    return (s16)a;
}
)");
}

TEST_F(RoundTripTest, IntegerCastS8ToS32) {
  AssertRoundTripBitExact("CastS8ToS32", R"(
s32 CastS8ToS32(s32 a) {
    return (s8)a;
}
)");
}

TEST_F(RoundTripTest, IntegerCastU8ToS32) {
  AssertRoundTripBitExact("CastU8ToS32", R"(
s32 CastU8ToS32(s32 a) {
    return (u8)a;
}
)");
}

TEST_F(RoundTripTest, IntegerCastU16ToS32) {
  AssertRoundTripBitExact("CastU16ToS32", R"(
s32 CastU16ToS32(s32 a) {
    return (u16)a;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadOffsetS32) {
  AssertRoundTripBitExact("LoadOffsetS32", R"(
s32 LoadOffsetS32(s32* p) {
    return p[4];
}
)");
}

TEST_F(RoundTripTest, IntegerStoreOffsetS32) {
  AssertRoundTripBitExact("StoreOffsetS32", R"(
void StoreOffsetS32(s32* p, s32 v) {
    p[4] = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadOffsetS16) {
  AssertRoundTripBitExact("LoadOffsetS16", R"(
s32 LoadOffsetS16(s16* p) {
    return p[6];
}
)");
}

TEST_F(RoundTripTest, IntegerStoreOffsetS16) {
  AssertRoundTripBitExact("StoreOffsetS16", R"(
void StoreOffsetS16(s16* p, s16 v) {
    p[6] = v;
}
)");
}

TEST_F(RoundTripTest, IntegerLoadOffsetU8) {
  AssertRoundTripBitExact("LoadOffsetU8", R"(
u32 LoadOffsetU8(u8* p) {
    return p[5];
}
)");
}

TEST_F(RoundTripTest, IntegerStoreOffsetU8) {
  AssertRoundTripBitExact("StoreOffsetU8", R"(
void StoreOffsetU8(u8* p, u8 v) {
    p[5] = v;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreZero) {
  AssertRoundTripBitExact("StoreZero", R"(
void StoreZero(s32* p) {
    *p = 0;
}
)");
}

TEST_F(RoundTripTest, IntegerStoreImmediate) {
  AssertRoundTripBitExact("StoreImmediate", R"(
void StoreImmediate(s32* p) {
    *p = 42;
}
)");
}

// -----------------------------------------------------------------------------
// 3. Pointer Dereferences, Struct Field Offsets, and Array Indexing
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, PointerDereference) {
  AssertRoundTripBitExact("PointerDeref", R"(
s32 PointerDeref(s32* p) {
    return *p;
}
)");
}

TEST_F(RoundTripTest, PointerArrayIndexConst) {
  AssertRoundTripBitExact("ArrayIndexConst", R"(
s32 ArrayIndexConst(s32* arr) {
    return arr[3];
}
)");
}

TEST_F(RoundTripTest, PointerArrayIndexVar) {
  AssertRoundTripBitExact("ArrayIndexVar", R"(
s32 ArrayIndexVar(s32* arr, s32 idx) {
    return arr[idx];
}
)");
}

TEST_F(RoundTripTest, PointerArrayWrite) {
  AssertRoundTripBitExact("ArrayWrite", R"(
void ArrayWrite(s32* arr, s32 idx, s32 val) {
    arr[idx] = val;
}
)");
}

TEST_F(RoundTripTest, StructFieldAccess) {
  std::string entity_def = R"(
typedef struct {
    s32 id;
    s16 x;
    s16 y;
    u32 flags;
} Entity;
)";
  RoundTripOptions opts;
  opts.extra_types_header = entity_def;

  AssertRoundTripBitExact("GetEntityId", R"(
s32 GetEntityId(Entity* e) {
    return e->id;
}
)",
                          opts);

  AssertRoundTripBitExact("GetEntityX", R"(
s32 GetEntityX(Entity* e) {
    return e->x;
}
)",
                          opts);

  AssertRoundTripBitExact("SetEntityY", R"(
void SetEntityY(Entity* e, s16 y) {
    e->y = y;
}
)",
                          opts);
}

TEST_F(RoundTripTest, ArraySumFour) {
  AssertRoundTripBitExact("ArraySumFour", R"(
s32 ArraySumFour(s32* arr) {
    return arr[0] + arr[1] + arr[2] + arr[3];
}
)");
}

TEST_F(RoundTripTest, ArraySwapElements) {
  AssertRoundTripBitExact("ArraySwap", R"(
void ArraySwap(s32* arr, s32 i, s32 j) {
    s32 tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
}
)");
}

TEST_F(RoundTripTest, StructNestedFieldAccess) {
  std::string actor_def = R"(
typedef struct {
    s16 x;
    s16 y;
} Vec2s;

typedef struct {
    s32 id;
    Vec2s pos;
    u32 flags;
} Actor;
)";
  RoundTripOptions opts;
  opts.extra_types_header = actor_def;

  AssertRoundTripBitExact("GetActorPosX", R"(
s16 GetActorPosX(Actor* a) {
    return a->pos.x;
}
)",
                          opts);

  AssertRoundTripBitExact("SetActorPosY", R"(
void SetActorPosY(Actor* a, s16 y) {
    a->pos.y = y;
}
)",
                          opts);
}

TEST_F(RoundTripTest, StructBitwiseFlags) {
  std::string actor_def = R"(
typedef struct {
    s32 id;
    u32 flags;
} ActorFlags;
)";
  RoundTripOptions opts;
  opts.extra_types_header = actor_def;

  AssertRoundTripBitExact("SetActorFlags", R"(
void SetActorFlags(ActorFlags* a, u32 flag) {
    a->flags |= flag;
}
)",
                          opts);

  AssertRoundTripBitExact("ClearActorFlags", R"(
void ClearActorFlags(ActorFlags* a, u32 flag) {
    a->flags &= ~flag;
}
)",
                          opts);
}

TEST_F(RoundTripTest, ArrayScaleElement) {
  AssertRoundTripBitExact("ArrayScaleElement", R"(
void ArrayScaleElement(s32* arr, s32 idx, s32 scale) {
    arr[idx] *= scale;
}
)");
}

TEST_F(RoundTripTest, ArraySumTwo16) {
  AssertRoundTripBitExact("ArraySumTwo16", R"(
s16 ArraySumTwo16(s16* arr) {
    return arr[0] + arr[1];
}
)");
}

TEST_F(RoundTripTest, ArraySumTwoU8) {
  AssertRoundTripBitExact("ArraySumTwoU8", R"(
u32 ArraySumTwoU8(u8* arr) {
    return arr[0] + arr[1];
}
)");
}

TEST_F(RoundTripTest, ArrayWriteS16) {
  AssertRoundTripBitExact("ArrayWriteS16", R"(
void ArrayWriteS16(s16* arr, s32 idx, s16 val) {
    arr[idx] = val;
}
)");
}

TEST_F(RoundTripTest, ArrayWriteU8) {
  AssertRoundTripBitExact("ArrayWriteU8", R"(
void ArrayWriteU8(u8* arr, s32 idx, u8 val) {
    arr[idx] = val;
}
)");
}

TEST_F(RoundTripTest, PointerAdvance) {
  AssertRoundTripBitExact("PointerAdvance", R"(
s32* PointerAdvance(s32* ptr, s32 count) {
    return ptr + count;
}
)");
}

TEST_F(RoundTripTest, StructPointDistance) {
  std::string point_def = R"(
typedef struct {
    s32 x;
    s32 y;
} Point2D;
)";
  RoundTripOptions opts;
  opts.extra_types_header = point_def;

  AssertRoundTripBitExact("PointDistance", R"(
s32 PointDistance(Point2D* p) {
    return p->x + p->y;
}
)",
                          opts);
}

TEST_F(RoundTripTest, StructPointCopy) {
  std::string point_def = R"(
typedef struct {
    s32 x;
    s32 y;
} Point2D;
)";
  RoundTripOptions opts;
  opts.extra_types_header = point_def;

  AssertRoundTripBitExact("PointCopy", R"(
void PointCopy(Point2D* dst, Point2D* src) {
    dst->x = src->x;
    dst->y = src->y;
}
)",
                          opts);
}

// -----------------------------------------------------------------------------
// 4. Simple and Nested If / If-Else Branches and Early Returns
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, BranchSignum) {
  AssertRoundTripBitExact("Signum", R"(
s32 Signum(s32 a) {
    if (a > 0) {
        return 1;
    } else if (a < 0) {
        return -1;
    } else {
        return 0;
    }
}
)");
}

TEST_F(RoundTripTest, BranchIfElseReturnConst) {
  AssertRoundTripBitExact("IfElseReturnConst", R"(
s32 IfElseReturnConst(s32 a) {
    if (a > 0) {
        return 10;
    } else {
        return 20;
    }
}
)");
}

TEST_F(RoundTripTest, BranchIfElseAssign) {
  AssertRoundTripBitExact("IfElseAssign", R"(
s32 IfElseAssign(s32 a, s32 b) {
    s32 res;
    if (a > 10) {
        res = a + b;
    } else {
        res = a - b;
    }
    return res;
}
)");
}

// In EarlyReturnIfZero, the lifter structures the control flow into a single return:
//   v0 = arg1; if (arg0 != 0) v0 = (arg0 + v0); return v0;
// When recompiled, GCC 2.7.2 emits commutative 'addu $v0, $v0, $a0' instead of
// original 'addu $v0, $a0, $v0'.
TEST_F(RoundTripTest, BranchEarlyReturnIfZero) {
  AssertRoundTripBitExact("EarlyReturnIfZero", R"(
s32 EarlyReturnIfZero(s32 a, s32 b) {
    if (a == 0) {
        return b;
    }
    return a + b;
}
)");
}

TEST_F(RoundTripTest, BranchMaxTwo) {
  AssertRoundTripBitExact("MaxTwo", R"(
s32 MaxTwo(s32 a, s32 b) {
    if (a > b) {
        return a;
    } else {
        return b;
    }
}
)");
}

TEST_F(RoundTripTest, BranchInRange) {
  AssertRoundTripBitExact("InRange", R"(
s32 InRange(s32 a, s32 min, s32 max) {
    if (a >= min && a <= max) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchIsOutside) {
  AssertRoundTripBitExact("IsOutside", R"(
s32 IsOutside(s32 a, s32 min, s32 max) {
    if (a < min || a > max) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchAbs) {
  AssertRoundTripBitExact("AbsVal", R"(
s32 AbsVal(s32 a) {
    if (a < 0) {
        return -a;
    }
    return a;
}
)");
}

TEST_F(RoundTripTest, BranchMinTwo) {
  AssertRoundTripBitExact("MinTwo", R"(
s32 MinTwo(s32 a, s32 b) {
    if (a < b) {
        return a;
    } else {
        return b;
    }
}
)");
}

TEST_F(RoundTripTest, BranchIsPositive) {
  AssertRoundTripBitExact("IsPositive", R"(
s32 IsPositive(s32 a) {
    if (a > 0) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchIsNegative) {
  AssertRoundTripBitExact("IsNegative", R"(
s32 IsNegative(s32 a) {
    if (a < 0) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchIsZero) {
  AssertRoundTripBitExact("IsZero", R"(
s32 IsZero(s32 a) {
    if (a == 0) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchIsNotZero) {
  AssertRoundTripBitExact("IsNotZero", R"(
s32 IsNotZero(s32 a) {
    if (a != 0) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareEqImm) {
  AssertRoundTripBitExact("CompareEqImm", R"(
s32 CompareEqImm(s32 a) {
    if (a == 42) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareNeImm) {
  AssertRoundTripBitExact("CompareNeImm", R"(
s32 CompareNeImm(s32 a) {
    if (a != 42) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareUnsigned) {
  AssertRoundTripBitExact("CompareUnsigned", R"(
s32 CompareUnsigned(u32 a, u32 b) {
    if (a < b) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareUnsignedImm) {
  AssertRoundTripBitExact("CompareUnsignedImm", R"(
s32 CompareUnsignedImm(u32 a) {
    if (a < 100u) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchLogicalAnd) {
  AssertRoundTripBitExact("LogicalAnd", R"(
s32 LogicalAnd(s32 a, s32 b) {
    if (a != 0 && b != 0) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchLogicalOr) {
  AssertRoundTripBitExact("LogicalOr", R"(
s32 LogicalOr(s32 a, s32 b) {
    if (a != 0 || b != 0) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareLess) {
  AssertRoundTripBitExact("CompareLess", R"(
s32 CompareLess(s32 a, s32 b) {
    if (a < b) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareGeImm) {
  AssertRoundTripBitExact("CompareGeImm", R"(
s32 CompareGeImm(s32 a) {
    if (a >= 10) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareLeImm) {
  AssertRoundTripBitExact("CompareLeImm", R"(
s32 CompareLeImm(s32 a) {
    if (a <= 10) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareGtImm) {
  AssertRoundTripBitExact("CompareGtImm", R"(
s32 CompareGtImm(s32 a) {
    if (a > 10) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchNestedIf) {
  AssertRoundTripBitExact("NestedIf", R"(
s32 NestedIf(s32 a, s32 b) {
    if (a > 0) {
        if (b > 0) {
            return 1;
        }
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareGeZero) {
  AssertRoundTripBitExact("CompareGeZero", R"(
s32 CompareGeZero(s32 a) {
    if (a >= 0) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareLeZero) {
  AssertRoundTripBitExact("CompareLeZero", R"(
s32 CompareLeZero(s32 a) {
    if (a <= 0) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchTernary) {
  AssertRoundTripBitExact("TernaryOp", R"(
s32 TernaryOp(s32 cond, s32 a, s32 b) {
    return cond ? a : b;
}
)");
}

TEST_F(RoundTripTest, BranchLogicalNor) {
  AssertRoundTripBitExact("LogicalNor", R"(
s32 LogicalNor(s32 a, s32 b) {
    if (!a && !b) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchCompareGreater) {
  AssertRoundTripBitExact("CompareGreater", R"(
s32 CompareGreater(s32 a, s32 b) {
    if (a > b) {
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchMaskAndBranchUseRegister) {
  AssertRoundTripBitExact("MaskBranchAndReuse", R"(
s32 MaskBranchAndReuse(u32 a) {
    u32 masked = a & 0xFF;
    if (masked > 10) {
        return masked + 5;
    }
    return masked;
}
)");
}

// -----------------------------------------------------------------------------
// 5. Loops (Do-While, While, For Loops)
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, LoopDoWhileCountDown) {
  AssertRoundTripBitExact("DoWhileCountDown", R"(
s32 DoWhileCountDown(s32 count) {
    s32 total = 0;
    do {
        total += count;
        count--;
    } while (count > 0);
    return total;
}
)");
}

TEST_F(RoundTripTest, LoopWhileLoop) {
  AssertRoundTripBitExact("WhileLoop", R"(
s32 WhileLoop(s32 count) {
    s32 total = 0;
    while (count > 0) {
        total += count;
        count--;
    }
    return total;
}
)");
}

TEST_F(RoundTripTest, LoopForLoopSum) {
  AssertRoundTripBitExact("ForLoopSum", R"(
s32 ForLoopSum(s32 arg0) {
    s32 v1;
    s32 arg1;
    arg1 = 0;
    for (v1 = 0; (v1 < arg0); v1++) {
        arg1 = (arg1 + v1);
    }
    return arg1;
}
)");
}

TEST_F(RoundTripTest, LoopArraySum) {
  AssertRoundTripBitExact("ArraySum", R"(
s32 ArraySum(s32* arr, s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}
)");
}

TEST_F(RoundTripTest, LoopCountUp) {
  AssertRoundTripBitExact("CountUp", R"(
s32 CountUp(s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; i++) {
        sum += 2;
    }
    return sum;
}
)");
}

TEST_F(RoundTripTest, LoopWhileCountDown) {
  AssertRoundTripBitExact("WhileCountDown", R"(
s32 WhileCountDown(s32 n) {
    s32 r = 0;
    while (n > 0) {
        r += n;
        n--;
    }
    return r;
}
)");
}

TEST_F(RoundTripTest, LoopDoWhileSum) {
  AssertRoundTripBitExact("DoWhileSum", R"(
s32 DoWhileSum(s32 n) {
    s32 sum = 0;
    do {
        sum += n;
        n--;
    } while (n > 0);
    return sum;
}
)");
}

TEST_F(RoundTripTest, LoopCopyArray) {
  AssertRoundTripBitExact("CopyArray", R"(
void CopyArray(s32* dst, s32* src, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}
)");
}

TEST_F(RoundTripTest, LoopArrayScale) {
  AssertRoundTripBitExact("ArrayScale", R"(
void ArrayScale(s32* arr, s32 n, s32 scale) {
    s32 i;
    for (i = 0; i < n; i++) {
        arr[i] = arr[i] + scale;
    }
}
)");
}

// -----------------------------------------------------------------------------
// 6. Function Calls (Leaf to Non-Leaf, Callee-Saved Registers)
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, CallDirectOneArg) {
  AssertRoundTripBitExact("CallDirectOneArg", R"(
extern s32 CalleeOne(s32 a);

s32 CallDirectOneArg(s32 a) {
    return CalleeOne(a);
}
)");
}

TEST_F(RoundTripTest, CallDirectTwoArgs) {
  AssertRoundTripBitExact("CallDirectTwoArgs", R"(
extern s32 CalleeTwo(s32 a, s32 b);

s32 CallDirectTwoArgs(s32 a, s32 b) {
    return CalleeTwo(a, b);
}
)");
}

TEST_F(RoundTripTest, CallNonTailAdd) {
  AssertRoundTripBitExact("CallNonTailAdd", R"(
extern s32 CalleeOne(s32 a);

s32 CallNonTailAdd(s32 a) {
    return CalleeOne(a) + 1;
}
)");
}

TEST_F(RoundTripTest, CallPreserveArgAcrossCall) {
  AssertRoundTripBitExact("CallPreserveArgAcrossCall", R"(
extern s32 CalleeOne(s32 a);

s32 CallPreserveArgAcrossCall(s32 a) {
    return CalleeOne(a) + a;
}
)");
}

TEST_F(RoundTripTest, CallDirectThreeArgs) {
  AssertRoundTripBitExact("CallDirectThreeArgs", R"(
extern s32 CalleeThree(s32 a, s32 b, s32 c);

s32 CallDirectThreeArgs(s32 a, s32 b, s32 c) {
    return CalleeThree(a, b, c);
}
)");
}

TEST_F(RoundTripTest, CallDirectFourArgs) {
  AssertRoundTripBitExact("CallDirectFourArgs", R"(
extern s32 CalleeFour(s32 a, s32 b, s32 c, s32 d);

s32 CallDirectFourArgs(s32 a, s32 b, s32 c, s32 d) {
    return CalleeFour(a, b, c, d);
}
)");
}

TEST_F(RoundTripTest, CallArgAdd) {
  AssertRoundTripBitExact("CallArgAdd", R"(
extern s32 CalleeOne(s32 a);

s32 CallArgAdd(s32 a) {
    return CalleeOne(a + 5);
}
)");
}

TEST_F(RoundTripTest, CallPassConstant) {
  AssertRoundTripBitExact("CallPassConstant", R"(
extern s32 CalleeOne(s32 a);

s32 CallPassConstant(void) {
    return CalleeOne(100);
}
)");
}

TEST_F(RoundTripTest, CallChain) {
  AssertRoundTripBitExact("CallChain", R"(
extern s32 CalleeOne(s32 a);

s32 CallChain(s32 a) {
    return CalleeOne(CalleeOne(a));
}
)");
}

TEST_F(RoundTripTest, CallVoidOneArg) {
  AssertRoundTripBitExact("CallVoidOneArg", R"(
extern void CalleeVoid(s32 a);

void CallVoidOneArg(s32 a) {
    CalleeVoid(a);
}
)");
}

TEST_F(RoundTripTest, CallPassTwoConstants) {
  AssertRoundTripBitExact("PassTwoConstants", R"(
extern s32 CalleeTwo(s32 a, s32 b);

s32 PassTwoConstants(void) {
    return CalleeTwo(10, 20);
}
)");
}

TEST_F(RoundTripTest, StackArrayWithCalls) {
  AssertRoundTripBitExact("StackArrayWithCalls", R"(
extern s32 ProcessArray(s32* arr, s32 n);

s32 StackArrayWithCalls(s32 a, s32 b, s32 c, s32 d) {
    s32 buf[4];
    buf[0] = a;
    buf[1] = b;
    buf[2] = c;
    buf[3] = d;
    return ProcessArray(buf, 4);
}
)");
}

// -----------------------------------------------------------------------------
// 7. Global Variable Reads & Writes
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, GlobalReadS32) {
  AssertRoundTripBitExact("ReadGlobalS32", R"(
extern s32 gGlobalS32;

s32 ReadGlobalS32(void) {
    return gGlobalS32;
}
)");
}

TEST_F(RoundTripTest, GlobalWriteS32) {
  AssertRoundTripBitExact("WriteGlobalS32", R"(
extern s32 gGlobalS32;

void WriteGlobalS32(s32 v) {
    gGlobalS32 = v;
}
)");
}

TEST_F(RoundTripTest, GlobalReadU16) {
  AssertRoundTripBitExact("ReadGlobalU16", R"(
extern u16 gGlobalU16;

u32 ReadGlobalU16(void) {
    return gGlobalU16;
}
)");
}

TEST_F(RoundTripTest, GlobalWriteS8) {
  AssertRoundTripBitExact("WriteGlobalS8", R"(
extern s8 gGlobalS8;

void WriteGlobalS8(s8 v) {
    gGlobalS8 = v;
}
)");
}

TEST_F(RoundTripTest, GlobalReadS8) {
  AssertRoundTripBitExact("ReadGlobalS8", R"(
extern s8 gGlobalS8;

s32 ReadGlobalS8(void) {
    return gGlobalS8;
}
)");
}

TEST_F(RoundTripTest, GlobalReadS16) {
  AssertRoundTripBitExact("ReadGlobalS16", R"(
extern s16 gGlobalS16;

s32 ReadGlobalS16(void) {
    return gGlobalS16;
}
)");
}

TEST_F(RoundTripTest, GlobalReadU32) {
  AssertRoundTripBitExact("ReadGlobalU32", R"(
extern u32 gGlobalU32;

u32 ReadGlobalU32(void) {
    return gGlobalU32;
}
)");
}

TEST_F(RoundTripTest, GlobalWriteU16) {
  AssertRoundTripBitExact("WriteGlobalU16", R"(
extern u16 gGlobalU16;

void WriteGlobalU16(u16 v) {
    gGlobalU16 = v;
}
)");
}

TEST_F(RoundTripTest, GlobalWriteU32) {
  AssertRoundTripBitExact("WriteGlobalU32", R"(
extern u32 gGlobalU32;

void WriteGlobalU32(u32 v) {
    gGlobalU32 = v;
}
)");
}

TEST_F(RoundTripTest, GlobalIncrementS32) {
  AssertRoundTripBitExact("GlobalIncrementS32", R"(
extern s32 gGlobalS32;

void GlobalIncrementS32(void) {
    gGlobalS32++;
}
)");
}

TEST_F(RoundTripTest, GlobalAddAssignS32) {
  AssertRoundTripBitExact("GlobalAddAssignS32", R"(
extern s32 gGlobalS32;

void GlobalAddAssignS32(s32 v) {
    gGlobalS32 += v;
}
)");
}

TEST_F(RoundTripTest, GlobalSubAssignS32) {
  AssertRoundTripBitExact("GlobalSubAssignS32", R"(
extern s32 gGlobalS32;

void GlobalSubAssignS32(s32 v) {
    gGlobalS32 -= v;
}
)");
}

TEST_F(RoundTripTest, GlobalAndAssignU32) {
  AssertRoundTripBitExact("GlobalAndAssignU32", R"(
extern u32 gGlobalU32;

void GlobalAndAssignU32(u32 mask) {
    gGlobalU32 &= mask;
}
)");
}

TEST_F(RoundTripTest, GlobalOrAssignU32) {
  AssertRoundTripBitExact("GlobalOrAssignU32", R"(
extern u32 gGlobalU32;

void GlobalOrAssignU32(u32 mask) {
    gGlobalU32 |= mask;
}
)");
}

TEST_F(RoundTripTest, GlobalWriteZeroS32) {
  AssertRoundTripBitExact("WriteZeroS32", R"(
extern s32 gGlobalS32;

void WriteZeroS32(void) {
    gGlobalS32 = 0;
}
)");
}

TEST_F(RoundTripTest, GlobalReadAddArg) {
  AssertRoundTripBitExact("ReadAddArg", R"(
extern s32 gGlobalS32;

s32 ReadAddArg(s32 a) {
    return gGlobalS32 + a;
}
)");
}

// -----------------------------------------------------------------------------
// 8. Floating-Point Arithmetic & Conversions
// -----------------------------------------------------------------------------

TEST_F(RoundTripTest, FloatAdd) {
  AssertRoundTripBitExact("AddFloat", R"(
f32 AddFloat(f32 a, f32 b) {
    return a + b;
}
)");
}

TEST_F(RoundTripTest, FloatSub) {
  AssertRoundTripBitExact("SubFloat", R"(
f32 SubFloat(f32 a, f32 b) {
    return a - b;
}
)");
}

TEST_F(RoundTripTest, FloatMul) {
  AssertRoundTripBitExact("MulFloat", R"(
f32 MulFloat(f32 a, f32 b) {
    return a * b;
}
)");
}

TEST_F(RoundTripTest, FloatDiv) {
  AssertRoundTripBitExact("DivFloat", R"(
f32 DivFloat(f32 a, f32 b) {
    return a / b;
}
)");
}

TEST_F(RoundTripTest, FloatNeg) {
  AssertRoundTripBitExact("NegFloat", R"(
f32 NegFloat(f32 a) {
    return -a;
}
)");
}

TEST_F(RoundTripTest, DoubleAdd) {
  AssertRoundTripBitExact("AddDouble", R"(
f64 AddDouble(f64 a, f64 b) {
    return a + b;
}
)");
}

TEST_F(RoundTripTest, DoubleSub) {
  AssertRoundTripBitExact("SubDouble", R"(
f64 SubDouble(f64 a, f64 b) {
    return a - b;
}
)");
}

TEST_F(RoundTripTest, DoubleMul) {
  AssertRoundTripBitExact("MulDouble", R"(
f64 MulDouble(f64 a, f64 b) {
    return a * b;
}
)");
}

TEST_F(RoundTripTest, FloatConvertIntToFloat) {
  AssertRoundTripBitExact("ConvertIntToFloat", R"(
f32 ConvertIntToFloat(s32 a) {
    return (f32)a;
}
)");
}

TEST_F(RoundTripTest, FloatConvertFloatToInt) {
  AssertRoundTripBitExact("ConvertFloatToInt", R"(
s32 ConvertFloatToInt(f32 a) {
    return (s32)a;
}
)");
}

TEST_F(RoundTripTest, FloatConvertFloatToDouble) {
  AssertRoundTripBitExact("ConvertFloatToDouble", R"(
f64 ConvertFloatToDouble(f32 a) {
    return (f64)a;
}
)");
}

TEST_F(RoundTripTest, FloatConvertDoubleToFloat) {
  AssertRoundTripBitExact("ConvertDoubleToFloat", R"(
f32 ConvertDoubleToFloat(f64 a) {
    return (f32)a;
}
)");
}

TEST_F(RoundTripTest, DoubleDiv) {
  AssertRoundTripBitExact("DivDouble", R"(
f64 DivDouble(f64 a, f64 b) {
    return a / b;
}
)");
}

TEST_F(RoundTripTest, DoubleNeg) {
  AssertRoundTripBitExact("NegDouble", R"(
f64 NegDouble(f64 a) {
    return -a;
}
)");
}

TEST_F(RoundTripTest, DoubleConvertIntToDouble) {
  AssertRoundTripBitExact("ConvertIntToDouble", R"(
f64 ConvertIntToDouble(s32 a) {
    return (f64)a;
}
)");
}

TEST_F(RoundTripTest, DoubleConvertDoubleToInt) {
  AssertRoundTripBitExact("ConvertDoubleToInt", R"(
s32 ConvertDoubleToInt(f64 a) {
    return (s32)a;
}
)");
}

TEST_F(RoundTripTest, FloatLoad) {
  AssertRoundTripBitExact("LoadFloat", R"(
f32 LoadFloat(f32* ptr) {
    return *ptr;
}
)");
}

TEST_F(RoundTripTest, FloatStore) {
  AssertRoundTripBitExact("StoreFloat", R"(
void StoreFloat(f32* ptr, f32 val) {
    *ptr = val;
}
)");
}

TEST_F(RoundTripTest, DoubleLoad) {
  AssertRoundTripBitExact("LoadDouble", R"(
f64 LoadDouble(f64* ptr) {
    return *ptr;
}
)");
}

TEST_F(RoundTripTest, DoubleStore) {
  AssertRoundTripBitExact("StoreDouble", R"(
void StoreDouble(f64 val, f64* ptr) {
    *ptr = val;
}
)");
}

TEST_F(RoundTripTest, ArithmeticAddThree) {
  AssertRoundTripBitExact("AddThree", R"(
s32 AddThree(s32 a, s32 b, s32 c) {
    return a + b + c;
}
)");
}

TEST_F(RoundTripTest, ArithmeticSubThree) {
  AssertRoundTripBitExact("SubThree", R"(
s32 SubThree(s32 a, s32 b, s32 c) {
    return a - b - c;
}
)");
}

TEST_F(RoundTripTest, ArithmeticAverageTwo) {
  AssertRoundTripBitExact("AverageTwo", R"(
s32 AverageTwo(s32 a, s32 b) {
    return (a + b) >> 1;
}
)");
}

TEST_F(RoundTripTest, ArithmeticAverageUnsigned) {
  AssertRoundTripBitExact("AverageUnsigned", R"(
u32 AverageUnsigned(u32 a, u32 b) {
    return (a + b) >> 1;
}
)");
}

TEST_F(RoundTripTest, ArithmeticMulAdd) {
  AssertRoundTripBitExact("MulAdd", R"(
s32 MulAdd(s32 a, s32 b, s32 c) {
    return a * b + c;
}
)");
}

TEST_F(RoundTripTest, ArithmeticMulSub) {
  AssertRoundTripBitExact("MulSub", R"(
s32 MulSub(s32 a, s32 b, s32 c) {
    return a * b - c;
}
)");
}

TEST_F(RoundTripTest, ArithmeticMaskAndShift) {
  AssertRoundTripBitExact("MaskAndShift", R"(
u32 MaskAndShift(u32 a) {
    return (a & 0xFF00) >> 8;
}
)");
}

TEST_F(RoundTripTest, ArithmeticShiftAndMask) {
  AssertRoundTripBitExact("ShiftAndMask", R"(
u32 ShiftAndMask(u32 a) {
    return (a >> 4) & 0x0F;
}
)");
}

TEST_F(RoundTripTest, ArithmeticNand) {
  AssertRoundTripBitExact("Nand", R"(
u32 Nand(u32 a, u32 b) {
    return ~(a & b);
}
)");
}

TEST_F(RoundTripTest, ArithmeticUnpackHighS16) {
  AssertRoundTripBitExact("UnpackHighS16", R"(
s32 UnpackHighS16(u32 packed) {
    return (s32)(packed >> 16);
}
)");
}

TEST_F(RoundTripTest, ArithmeticUnpackLowS16) {
  AssertRoundTripBitExact("UnpackLowS16", R"(
s32 UnpackLowS16(u32 packed) {
    return (s16)(packed & 0xFFFF);
}
)");
}

TEST_F(RoundTripTest, ArithmeticRotateLeftOne) {
  AssertRoundTripBitExact("RotateLeftOne", R"(
u32 RotateLeftOne(u32 a) {
    return (a << 1) | (a >> 31);
}
)");
}

TEST_F(RoundTripTest, ArithmeticIsOdd) {
  AssertRoundTripBitExact("IsOdd", R"(
s32 IsOdd(s32 a) {
    return a & 1;
}
)");
}

TEST_F(RoundTripTest, ArithmeticClearLowestBit) {
  AssertRoundTripBitExact("ClearLowestBit", R"(
u32 ClearLowestBit(u32 a) {
    return a & (a - 1);
}
)");
}

TEST_F(RoundTripTest, ArrayAccessIndexedOffset) {
  AssertRoundTripBitExact("IndexedOffset", R"(
s32 IndexedOffset(s32* arr, s32 i) {
    return arr[i + 2];
}
)");
}

TEST_F(RoundTripTest, ArrayWriteIndexedOffset) {
  AssertRoundTripBitExact("WriteIndexedOffset", R"(
void WriteIndexedOffset(s32* arr, s32 i, s32 v) {
    arr[i + 2] = v;
}
)");
}

TEST_F(RoundTripTest, ArrayReadS8Indexed) {
  AssertRoundTripBitExact("ReadS8Indexed", R"(
s32 ReadS8Indexed(s8* arr, s32 i) {
    return arr[i];
}
)");
}

TEST_F(RoundTripTest, ArrayReadS16Indexed) {
  AssertRoundTripBitExact("ReadS16Indexed", R"(
s32 ReadS16Indexed(s16* arr, s32 i) {
    return arr[i];
}
)");
}

TEST_F(RoundTripTest, StructPoint3DNormSq) {
  std::string point_def = R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
} Point3D;
)";
  RoundTripOptions opts;
  opts.extra_types_header = point_def;

  AssertRoundTripBitExact("Point3DNormSq", R"(
s32 Point3DNormSq(Point3D* p) {
    return p->x * p->x + p->y * p->y + p->z * p->z;
}
)",
                          opts);
}

TEST_F(RoundTripTest, StructPoint3DGetZ) {
  std::string point_def = R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
} Point3D;
)";
  RoundTripOptions opts;
  opts.extra_types_header = point_def;

  AssertRoundTripBitExact("Point3DGetZ", R"(
s32 Point3DGetZ(Point3D* p) {
    return p->z;
}
)",
                          opts);
}

TEST_F(RoundTripTest, StructPoint3DSet) {
  std::string point_def = R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
} Point3D;
)";
  RoundTripOptions opts;
  opts.extra_types_header = point_def;

  AssertRoundTripBitExact("Point3DSet", R"(
void Point3DSet(Point3D* p, s32 x, s32 y, s32 z) {
    p->x = x;
    p->y = y;
    p->z = z;
}
)",
                          opts);
}

TEST_F(RoundTripTest, BranchIfElseAssignDiff) {
  AssertRoundTripBitExact("IfElseAssignDiff", R"(
s32 IfElseAssignDiff(s32 a, s32 b) {
    s32 res;
    if (a < b) {
        res = b - a;
    } else {
        res = a - b;
    }
    return res;
}
)");
}

TEST_F(RoundTripTest, BranchMaxThree) {
  AssertRoundTripBitExact("MaxThree", R"(
s32 MaxThree(s32 a, s32 b, s32 c) {
    s32 m = a;
    if (b > m) {
        m = b;
    }
    if (c > m) {
        m = c;
    }
    return m;
}
)");
}

TEST_F(RoundTripTest, BranchMinThree) {
  AssertRoundTripBitExact("MinThree", R"(
s32 MinThree(s32 a, s32 b, s32 c) {
    s32 m = a;
    if (b < m) {
        m = b;
    }
    if (c < m) {
        m = c;
    }
    return m;
}
)");
}

TEST_F(RoundTripTest, BranchThreeWayBranch) {
  AssertRoundTripBitExact("ThreeWayBranch", R"(
s32 ThreeWayBranch(s32 a) {
    if (a > 0) {
        return 1;
    }
    if (a < 0) {
        return -1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, BranchTestBitZero) {
  AssertRoundTripBitExact("TestBitZero", R"(
s32 TestBitZero(u32 val) {
    if (val & 1) {
        return 10;
    }
    return 20;
}
)");
}

TEST_F(RoundTripTest, BranchSelectPositive) {
  AssertRoundTripBitExact("SelectPositive", R"(
s32 SelectPositive(s32 a, s32 fallback) {
    if (a <= 0) {
        return fallback;
    }
    return a;
}
)");
}

TEST_F(RoundTripTest, BranchSelectZero) {
  AssertRoundTripBitExact("SelectZero", R"(
s32 SelectZero(s32 a, s32 fallback) {
    if (a == 0) {
        return fallback;
    }
    return a;
}
)");
}

TEST_F(RoundTripTest, LoopSumArrayS16) {
  AssertRoundTripBitExact("SumArrayS16", R"(
s32 SumArrayS16(s16* arr, s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}
)");
}

TEST_F(RoundTripTest, LoopZeroArray) {
  AssertRoundTripBitExact("ZeroArray", R"(
void ZeroArray(s32* arr, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        arr[i] = 0;
    }
}
)");
}

TEST_F(RoundTripTest, LoopDoWhileDecrement) {
  AssertRoundTripBitExact("DoWhileDecrement", R"(
s32 DoWhileDecrement(s32 val) {
    s32 count = 0;
    do {
        count++;
        val -= 2;
    } while (val > 0);
    return count;
}
)");
}

TEST_F(RoundTripTest, ArithmeticBitClear) {
  AssertRoundTripBitExact("BitClear", R"(
u32 BitClear(u32 val, u32 mask) {
    return val & ~mask;
}
)");
}

TEST_F(RoundTripTest, CallTwoArgsAdd) {
  AssertRoundTripBitExact("CallTwoArgsAdd", R"(
extern s32 CalleeTwo(s32 a, s32 b);

s32 CallTwoArgsAdd(s32 a, s32 b) {
    return CalleeTwo(a + 1, b + 2);
}
)");
}

TEST_F(RoundTripTest, CallWithGlobal) {
  AssertRoundTripBitExact("CallWithGlobal", R"(
extern s32 gGlobalS32;
extern s32 CalleeOne(s32 a);

s32 CallWithGlobal(void) {
    return CalleeOne(gGlobalS32);
}
)");
}

TEST_F(RoundTripTest, CallSetGlobal) {
  AssertRoundTripBitExact("CallSetGlobal", R"(
extern s32 gGlobalS32;
extern s32 CalleeOne(s32 a);

void CallSetGlobal(s32 a) {
    gGlobalS32 = CalleeOne(a);
}
)");
}

TEST_F(RoundTripTest, CallAddConstant) {
  AssertRoundTripBitExact("CallAddConstant", R"(
extern s32 CalleeOne(s32 a);

s32 CallAddConstant(s32 a) {
    return CalleeOne(a) + 10;
}
)");
}

TEST_F(RoundTripTest, GlobalConditionalIncrement) {
  AssertRoundTripBitExact("GlobalConditionalIncrement", R"(
extern s32 gGlobalS32;

void GlobalConditionalIncrement(s32 cond) {
    if (cond) {
        gGlobalS32++;
    }
}
)");
}

TEST_F(RoundTripTest, GlobalArrayRead) {
  AssertRoundTripBitExact("GlobalArrayRead", R"(
extern s32 gGlobalArray[4];

s32 GlobalArrayRead(s32 idx) {
    return gGlobalArray[idx];
}
)");
}

TEST_F(RoundTripTest, GlobalArrayWrite) {
  AssertRoundTripBitExact("GlobalArrayWrite", R"(
extern s32 gGlobalArray[4];

void GlobalArrayWrite(s32 idx, s32 val) {
    gGlobalArray[idx] = val;
}
)");
}

TEST_F(RoundTripTest, FloatScale) {
  AssertRoundTripBitExact("FloatScale", R"(
f32 FloatScale(f32 a, f32 scale) {
    return a * scale;
}
)");
}

TEST_F(RoundTripTest, DoubleScale) {
  AssertRoundTripBitExact("DoubleScale", R"(
f64 DoubleScale(f64 a, f64 scale) {
    return a * scale;
}
)");
}

TEST_F(RoundTripTest, DoubleMulAdd) {
  AssertRoundTripBitExact("DoubleMulAdd", R"(
f64 DoubleMulAdd(f64 a, f64 b, f64 c) {
    return a * b + c;
}
)");
}

TEST_F(RoundTripTest, DoubleMulSub) {
  AssertRoundTripBitExact("DoubleMulSub", R"(
f64 DoubleMulSub(f64 a, f64 b, f64 c) {
    return a * b - c;
}
)");
}

TEST_F(RoundTripTest, FloatAbs) {
  AssertRoundTripBitExact("FloatAbs", R"(
f32 FloatAbs(f32 a) {
    if (a < 0.0f) {
        return -a;
    }
    return a;
}
)");
}

TEST_F(RoundTripTest, PointerChainedSelfAssign) {
  AssertRoundTripBitExact("ChainedSelfAssign", R"(
void ChainedSelfAssign(s32 a, s32 b, s32 c) {
    s32 v0;
    v0 = (a | b);
    v0 = (v0 | 0xA0000000);
    *(s32*)v0 = c;
}
)");
}

TEST_F(RoundTripTest, WriteScaledFloats) {
  AssertRoundTripBitExact("WriteScaledFloats", R"(
void WriteScaledFloats(f32* out, f32 a, f32 b, f32 c) {
    out[0] = a + a;
    out[1] = b + b;
    out[2] = c + c;
}
)");
}

TEST_F(RoundTripTest, ScaleAndOffsetFloat) {
  AssertRoundTripBitExact("ScaleAndOffsetFloat", R"(
f32 ScaleAndOffsetFloat(s32 mode, f32 x, f32 scale, f32 bias) {
    return (x * scale) + bias;
}
)");
}

TEST_F(RoundTripTest, FloatToUintSimple) {
  AssertRoundTripBitExact("FloatToUintSimple", R"(
u32 FloatToUintSimple(f32 val) {
    return (u32)val;
}
)");
}

TEST_F(RoundTripTest, DoubleToUintSimple) {
  AssertRoundTripBitExact("DoubleToUintSimple", R"(
u32 DoubleToUintSimple(f64 val) {
    return (u32)val;
}
)");
}

TEST_F(RoundTripTest, ConvertFloatAndDoubleToUint) {
  AssertRoundTripBitExact("ConvertFloatAndDoubleToUint", R"(
void ConvertFloatAndDoubleToUint(f32 a, f64 b, u32* out) {
    out[0] = (u32)a;
    out[1] = (u32)b;
}
)");
}

TEST_F(RoundTripTest, ConditionalFloatToUint) {
  AssertRoundTripBitExact("ConditionalFloatToUint", R"(
u32 ConditionalFloatToUint(f32 val, s32 cond) {
    u32 res = 0;
    if (cond) {
        res = (u32)val;
    }
    return res + 1;
}
)");
}

TEST_F(RoundTripTest, SwitchWithSharedTail) {
  AssertRoundTripBitExact("SwitchWithSharedTail", R"(
s32 SwitchWithSharedTail(s32 x) {
    s32 res;
    switch (x) {
        case 0:
            return 100;
        case 1:
            res = x + 10;
            break;
        case 2:
            res = x + 20;
            break;
        case 3:
            res = x + 30;
            break;
        case 4:
            res = x + 40;
            break;
        case 5:
            res = x + 50;
            break;
        default:
            res = -1;
            break;
    }
    return res * 2;
}
)");
}

TEST_F(RoundTripTest, SwitchDecisionTreeFourCases) {
  AssertRoundTripBitExact("SwitchDecisionTreeFourCases", R"(
s32 SwitchDecisionTreeFourCases(s32 x) {
    switch (x) {
        case 1:
            return 10;
        case 2:
            return 20;
        case 3:
            return 30;
        case 4:
            return 40;
        default:
            return 0;
    }
}
)");
}

TEST_F(RoundTripTest, ArrayScaleMul5) {
  AssertRoundTripBitExact("ArrayScaleMul5", R"(
void ArrayScaleMul5(s32* arr, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        arr[i] = arr[i] * 5;
    }
}
)");
}

TEST_F(RoundTripTest, ArrayScaleMul9) {
  AssertRoundTripBitExact("ArrayScaleMul9", R"(
void ArrayScaleMul9(s32* arr, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        arr[i] = arr[i] * 9;
    }
}
)");
}

TEST_F(RoundTripTest, ArrayTransformMul5) {
  AssertRoundTripBitExact("ArrayTransformMul5", R"(
void ArrayTransformMul5(s32* dst, s32* src, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i] * 5;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkUnrolled4) {
  AssertRoundTripBitExact("PointerWalkUnrolled4", R"(
void PointerWalkUnrolled4(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = *src++ * 5;
        *dst++ = *src++ * 9;
        *dst++ = *src++ * 20;
        *dst++ = *src++ * 68;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkUnrolled8) {
  AssertRoundTripBitExact("PointerWalkUnrolled8", R"(
void PointerWalkUnrolled8(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = *src++ * 5;
        *dst++ = *src++ * 9;
        *dst++ = *src++ * 20;
        *dst++ = *src++ * 68;
        *dst++ = *src++ * 7;
        *dst++ = *src++ * 3;
        *dst++ = *src++ * 5;
        *dst++ = *src++ * 9;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScaleAdd) {
  AssertRoundTripBitExact("PointerWalkScaleAdd", R"(
void PointerWalkScaleAdd(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) + 10;
        *dst++ = (*src++ * 9) - 20;
        *dst++ = (*src++ * 20) + 30;
        *dst++ = (*src++ * 68) - 40;
        *dst++ = (*src++ * 7) + 50;
        *dst++ = (*src++ * 3) - 60;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScaleSub) {
  AssertRoundTripBitExact("PointerWalkScaleSub", R"(
void PointerWalkScaleSub(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 3) + 15;
        *dst++ = (*src++ * 7) - 25;
        *dst++ = (*src++ * 5) + 35;
        *dst++ = (*src++ * 9) - 45;
        *dst++ = (*src++ * 20) + 55;
        *dst++ = (*src++ * 68) - 65;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScaleBitwise) {
  AssertRoundTripBitExact("PointerWalkScaleBitwise", R"(
void PointerWalkScaleBitwise(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) ^ 0xFF;
        *dst++ = (*src++ * 9) & 0x7FFF;
        *dst++ = (*src++ * 20) | 0x100;
        *dst++ = (*src++ * 68) ^ 0x5555;
        *dst++ = (*src++ * 7) & 0x3FFF;
        *dst++ = (*src++ * 3) | 0x80;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkInPlaceScale6) {
  AssertRoundTripBitExact("PointerWalkInPlaceScale6", R"(
void PointerWalkInPlaceScale6(s32* p, s32 count) {
    while (count > 0) {
        *p = (*p * 5) + 12; p++;
        *p = (*p * 9) - 34; p++;
        *p = (*p * 20) + 56; p++;
        *p = (*p * 68) - 78; p++;
        *p = (*p * 7) + 90; p++;
        *p = (*p * 3) - 12; p++;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, ShiftAddMul5And9) {
  AssertRoundTripBitExact("ShiftAddMul5And9", R"(
s32 ShiftAddMul5And9(s32 x, s32 y) {
    s32 a = x * 5;
    s32 b = y * 9;
    return a + b;
}
)");
}

TEST_F(RoundTripTest, ShiftAddMul20And68) {
  AssertRoundTripBitExact("ShiftAddMul20And68", R"(
s32 ShiftAddMul20And68(s32 x, s32 y) {
    s32 a = x * 20;
    s32 b = y * 68;
    return a - b;
}
)");
}

TEST_F(RoundTripTest, ShiftAddMul7And3) {
  AssertRoundTripBitExact("ShiftAddMul7And3", R"(
s32 ShiftAddMul7And3(s32 x, s32 y) {
    return (x * 7) + (y * 3);
}
)");
}

TEST_F(RoundTripTest, PointerWalkScale8) {
  AssertRoundTripBitExact("PointerWalkScale8", R"(
void PointerWalkScale8(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) + 10;
        *dst++ = (*src++ * 9) - 20;
        *dst++ = (*src++ * 20) + 30;
        *dst++ = (*src++ * 68) - 40;
        *dst++ = (*src++ * 7) + 50;
        *dst++ = (*src++ * 3) - 60;
        *dst++ = (*src++ * 5) + 70;
        *dst++ = (*src++ * 9) - 80;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScale10) {
  AssertRoundTripBitExact("PointerWalkScale10", R"(
void PointerWalkScale10(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) + 11;
        *dst++ = (*src++ * 9) - 22;
        *dst++ = (*src++ * 20) + 33;
        *dst++ = (*src++ * 68) - 44;
        *dst++ = (*src++ * 7) + 55;
        *dst++ = (*src++ * 3) - 66;
        *dst++ = (*src++ * 5) + 77;
        *dst++ = (*src++ * 9) - 88;
        *dst++ = (*src++ * 20) + 99;
        *dst++ = (*src++ * 68) - 100;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScale12) {
  AssertRoundTripBitExact("PointerWalkScale12", R"(
void PointerWalkScale12(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) + 1;
        *dst++ = (*src++ * 9) - 2;
        *dst++ = (*src++ * 20) + 3;
        *dst++ = (*src++ * 68) - 4;
        *dst++ = (*src++ * 7) + 5;
        *dst++ = (*src++ * 3) - 6;
        *dst++ = (*src++ * 5) + 7;
        *dst++ = (*src++ * 9) - 8;
        *dst++ = (*src++ * 20) + 9;
        *dst++ = (*src++ * 68) - 10;
        *dst++ = (*src++ * 7) + 11;
        *dst++ = (*src++ * 3) - 12;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScale16) {
  AssertRoundTripBitExact("PointerWalkScale16", R"(
void PointerWalkScale16(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) + 1;
        *dst++ = (*src++ * 9) - 2;
        *dst++ = (*src++ * 20) + 3;
        *dst++ = (*src++ * 68) - 4;
        *dst++ = (*src++ * 7) + 5;
        *dst++ = (*src++ * 3) - 6;
        *dst++ = (*src++ * 5) + 7;
        *dst++ = (*src++ * 9) - 8;
        *dst++ = (*src++ * 20) + 9;
        *dst++ = (*src++ * 68) - 10;
        *dst++ = (*src++ * 7) + 11;
        *dst++ = (*src++ * 3) - 12;
        *dst++ = (*src++ * 5) + 13;
        *dst++ = (*src++ * 9) - 14;
        *dst++ = (*src++ * 20) + 15;
        *dst++ = (*src++ * 68) - 16;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkInPlaceScale8) {
  AssertRoundTripBitExact("PointerWalkInPlaceScale8", R"(
void PointerWalkInPlaceScale8(s32* p, s32 count) {
    while (count > 0) {
        *p = (*p * 5) + 10; p++;
        *p = (*p * 9) - 20; p++;
        *p = (*p * 20) + 30; p++;
        *p = (*p * 68) - 40; p++;
        *p = (*p * 7) + 50; p++;
        *p = (*p * 3) - 60; p++;
        *p = (*p * 5) + 70; p++;
        *p = (*p * 9) - 80; p++;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkInPlaceScale12) {
  AssertRoundTripBitExact("PointerWalkInPlaceScale12", R"(
void PointerWalkInPlaceScale12(s32* p, s32 count) {
    while (count > 0) {
        *p = (*p * 5) + 1; p++;
        *p = (*p * 9) - 2; p++;
        *p = (*p * 20) + 3; p++;
        *p = (*p * 68) - 4; p++;
        *p = (*p * 7) + 5; p++;
        *p = (*p * 3) - 6; p++;
        *p = (*p * 5) + 7; p++;
        *p = (*p * 9) - 8; p++;
        *p = (*p * 20) + 9; p++;
        *p = (*p * 68) - 10; p++;
        *p = (*p * 7) + 11; p++;
        *p = (*p * 3) - 12; p++;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScale14) {
  AssertRoundTripBitExact("PointerWalkScale14", R"(
void PointerWalkScale14(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) + 1;
        *dst++ = (*src++ * 9) - 2;
        *dst++ = (*src++ * 20) + 3;
        *dst++ = (*src++ * 68) - 4;
        *dst++ = (*src++ * 7) + 5;
        *dst++ = (*src++ * 3) - 6;
        *dst++ = (*src++ * 5) + 7;
        *dst++ = (*src++ * 9) - 8;
        *dst++ = (*src++ * 20) + 9;
        *dst++ = (*src++ * 68) - 10;
        *dst++ = (*src++ * 7) + 11;
        *dst++ = (*src++ * 3) - 12;
        *dst++ = (*src++ * 5) + 13;
        *dst++ = (*src++ * 9) - 14;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkInPlaceScale10) {
  AssertRoundTripBitExact("PointerWalkInPlaceScale10", R"(
void PointerWalkInPlaceScale10(s32* p, s32 count) {
    while (count > 0) {
        *p = (*p * 5) + 11; p++;
        *p = (*p * 9) - 22; p++;
        *p = (*p * 20) + 33; p++;
        *p = (*p * 68) - 44; p++;
        *p = (*p * 7) + 55; p++;
        *p = (*p * 3) - 66; p++;
        *p = (*p * 5) + 77; p++;
        *p = (*p * 9) - 88; p++;
        *p = (*p * 20) + 99; p++;
        *p = (*p * 68) - 100; p++;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkS16Scale6) {
  AssertRoundTripBitExact("PointerWalkS16Scale6", R"(
void PointerWalkS16Scale6(s16* dst, s16* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) + 10;
        *dst++ = (*src++ * 9) - 20;
        *dst++ = (*src++ * 20) + 30;
        *dst++ = (*src++ * 68) - 40;
        *dst++ = (*src++ * 7) + 50;
        *dst++ = (*src++ * 3) - 60;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkS16Scale8) {
  AssertRoundTripBitExact("PointerWalkS16Scale8", R"(
void PointerWalkS16Scale8(s16* dst, s16* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) + 10;
        *dst++ = (*src++ * 9) - 20;
        *dst++ = (*src++ * 20) + 30;
        *dst++ = (*src++ * 68) - 40;
        *dst++ = (*src++ * 7) + 50;
        *dst++ = (*src++ * 3) - 60;
        *dst++ = (*src++ * 5) + 70;
        *dst++ = (*src++ * 9) - 80;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScaleShift6) {
  AssertRoundTripBitExact("PointerWalkScaleShift6", R"(
void PointerWalkScaleShift6(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) >> 2;
        *dst++ = (*src++ * 9) >> 3;
        *dst++ = (*src++ * 20) >> 4;
        *dst++ = (*src++ * 68) >> 5;
        *dst++ = (*src++ * 7) >> 2;
        *dst++ = (*src++ * 3) >> 1;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, PointerWalkScaleShift8) {
  AssertRoundTripBitExact("PointerWalkScaleShift8", R"(
void PointerWalkScaleShift8(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = (*src++ * 5) >> 2;
        *dst++ = (*src++ * 9) >> 3;
        *dst++ = (*src++ * 20) >> 4;
        *dst++ = (*src++ * 68) >> 5;
        *dst++ = (*src++ * 7) >> 2;
        *dst++ = (*src++ * 3) >> 1;
        *dst++ = (*src++ * 5) >> 3;
        *dst++ = (*src++ * 9) >> 2;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, TransformArrayWithClamp) {
  AssertRoundTripBitExact("TransformArrayWithClamp", R"(
void TransformArrayWithClamp(s32* dst, const s32* src, s32 count, s32 scale, s32 min_val, s32 max_val) {
    while (count > 0) {
        s32 val = (*src++ * scale) >> 8;
        if (val > max_val) {
            val = max_val;
        } else if (val < min_val) {
            val = min_val;
        }
        *dst++ = val;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, Vector3ProcessVertices) {
  AssertRoundTripBitExact("Vector3ProcessVertices", R"(
void Vector3ProcessVertices(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = ((*src++ * 5) >> 4) + 10;
        *dst++ = ((*src++ * 9) >> 4) - 20;
        *dst++ = ((*src++ * 17) >> 4) + 30;
        *dst++ = ((*src++ * 33) >> 5) - 40;
        *dst++ = ((*src++ * 65) >> 6) + 50;
        *dst++ = ((*src++ * 129) >> 7) - 60;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, NestedLoopSum) {
  AssertRoundTripBitExact("NestedLoopSum", R"(
s32 NestedLoopSum(s32* matrix, s32 rows, s32 cols) {
    s32 total = 0;
    while (rows > 0) {
        s32 c = cols;
        while (c > 0) {
            total += *matrix++;
            c--;
        }
        rows--;
    }
    return total;
}
)");
}

TEST_F(RoundTripTest, NestedLoopMatrixClamp) {
  AssertRoundTripBitExact("NestedLoopMatrixClamp", R"(
s32 NestedLoopMatrixClamp(s32* matrix, s32 rows, s32 cols, s32 limit) {
    s32 total = 0;
    while (rows > 0) {
        s32 c = cols;
        while (c > 0) {
            s32 v = *matrix++;
            if (v > limit) {
                total += limit;
            } else {
                total += v;
            }
            c--;
        }
        rows--;
    }
    return total;
}
)");
}

TEST_F(RoundTripTest, PipelineProcess) {
  AssertRoundTripBitExact("PipelineProcess", R"(
s32 PipelineProcess(s32* data, s32 count) {
    s32 total = 0;
    while (count > 0) {
        s32 v = *data++;
        if (v > 0) {
            total += v;
        } else {
            total -= v;
        }
        count--;
    }
    return total;
}
)");
}

TEST_F(RoundTripTest, ArrayClampValues) {
  AssertRoundTripBitExact("ArrayClampValues", R"(
void ArrayClampValues(s32* dst, s32* src, s32 count, s32 min_val, s32 max_val) {
    while (count > 0) {
        s32 v = *src++;
        if (v < min_val) {
            v = min_val;
        } else if (v > max_val) {
            v = max_val;
        }
        *dst++ = v;
        count--;
    }
}
)");
}

TEST_F(RoundTripTest, LinearSearchFirstNegative) {
  AssertRoundTripBitExact("LinearSearchFirstNegative", R"(
s32 LinearSearchFirstNegative(const s32* arr, s32 count) {
    s32 idx = 0;
    while (count > 0) {
        if (*arr < 0) {
            return idx;
        }
        arr++;
        idx++;
        count--;
    }
    return -1;
}
)");
}

TEST_F(RoundTripTest, LinearSearchFirstMatchingTarget) {
  AssertRoundTripBitExact("LinearSearchFirstMatchingTarget", R"(
s32 LinearSearchFirstMatchingTarget(const s32* arr, s32 count, s32 target) {
    s32 idx = 0;
    while (count > 0) {
        if (*arr == target) {
            return idx;
        }
        arr++;
        idx++;
        count--;
    }
    return -1;
}
)");
}

TEST_F(RoundTripTest, ComputeChecksum32) {
  AssertRoundTripBitExact("ComputeChecksum32", R"(
u32 ComputeChecksum32(const u32* data, s32 count) {
    u32 sum1 = 1;
    u32 sum2 = 0;
    while (count > 0) {
        sum1 += *data++;
        sum2 += sum1;
        count--;
    }
    return sum1 ^ sum2;
}
)");
}

TEST_F(RoundTripTest, SubwordSpillSlotReadback) {
  AssertRoundTripBitExact("SubwordSpillSlotReadback", R"(
void SubwordSpillSlotReadback(s32 flag, s32 value, u16* destination_one, u16* destination_two) {
    if (flag & 1) {
        *destination_one = (u16)value;
    }
    if (flag & 2) {
        *destination_two = (u16)value;
    }
}
)");
}

TEST_F(RoundTripTest, LoopWithUnsigned8BitCounter) {
  AssertRoundTripBitExact("LoopWithUnsigned8BitCounter", R"(
u32 LoopWithUnsigned8BitCounter(const u32* array) {
    u8 counter = 0;
    u32 accumulator = 0;
    while (counter < 8) {
        accumulator += array[counter];
        counter++;
    }
    return accumulator;
}
)");
}

TEST_F(RoundTripTest, DoWhileLoopMultiStatementLatch) {
  AssertRoundTripBitExact("DoWhileLoopMultiStatementLatch", R"(
s32 DoWhileLoopMultiStatementLatch(const s32* source, s32* destination, s32 count) {
    s32 total = 0;
    do {
        s32 value = *source++;
        *destination++ = value;
        total += value;
        count--;
    } while (count > 0);
    return total;
}
)");
}

TEST_F(RoundTripTest, FloatToUintPrecededByFloatComputation) {
  AssertRoundTripBitExact("FloatToUintPrecededByFloatComputation", R"(
u32 FloatToUintPrecededByFloatComputation(f32 first_float, f32 second_float, f32 scale_factor) {
    f32 computed_float = (first_float + second_float) * scale_factor;
    return (u32)computed_float;
}
)");
}

TEST_F(RoundTripTest, SwitchCasesSharedIntermediateBeforeLatch) {
  AssertRoundTripBitExact("SwitchCasesSharedIntermediateBeforeLatch", R"(
s32 SwitchCasesSharedIntermediateBeforeLatch(const s32* array, s32 selector, s32 count) {
    s32 total = 0;
    do {
        s32 value;
        switch (selector) {
            case 0:
                value = *array * 2;
                break;
            case 1:
                value = *array * 3;
                break;
            case 2:
                value = *array * 4;
                break;
            case 3:
                value = *array * 5;
                break;
            case 4:
                value = *array + 1;
                break;
            case 5:
                value = *array + 2;
                break;
            default:
                value = 0;
                break;
        }
        total += value;
        array++;
        count--;
    } while (count > 0);
    return total;
}
)");
}

TEST_F(RoundTripTest, SixParameterCalleeWithStackArgs) {
  AssertRoundTripBitExact("SixParameterCalleeWithStackArgs", R"(
s32 SixParameterCalleeWithStackArgs(s32 a, s32 b, s32 c, s32 d, s32 e, s32 f) {
    return (a + b) - (c + d) + (e * f);
}
)");
}

TEST_F(RoundTripTest, BitfieldShiftMaskAndBranch) {
  AssertRoundTripBitExact("BitfieldShiftMaskAndBranch", R"(
s32 BitfieldShiftMaskAndBranch(const u16* flags_ptr, s32 fallback) {
    u16 raw = flags_ptr[2];
    u32 mode = (raw >> 3) & 3;
    if (mode == 1) {
        return (raw & 7) + 10;
    }
    return fallback;
}
)");
}

TEST_F(RoundTripTest, MultiCallConditionAndReturnFolding) {
  AssertRoundTripBitExact("MultiCallConditionAndReturnFolding", R"(
extern s32 CheckPrimary(s32 x);
extern s32 CheckSecondary(s32 x);
extern s32 ComputeResult(s32 x);

s32 MultiCallConditionAndReturnFolding(s32 x) {
    if (CheckPrimary(x) != 0) {
        if (CheckSecondary(x + 1) != 0) {
            return ComputeResult(x);
        }
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, CallerPassingSixArgumentsOnStack) {
  AssertRoundTripBitExact("CallerPassingSixArgumentsOnStack", R"(
extern s32 CombineSixValues(s32 a, s32 b, s32 c, s32 d, s32 e, s32 f);

s32 CallerPassingSixArgumentsOnStack(s32 x, s32 y) {
    return CombineSixValues(x, y, x + 1, y + 2, x + 3, y + 4);
}
)");
}

TEST_F(RoundTripTest, MultiCalleeSavedLoopAndAccumulate) {
  AssertRoundTripBitExact("MultiCalleeSavedLoopAndAccumulate", R"(
extern s32 TransformElement(s32 val);

s32 MultiCalleeSavedLoopAndAccumulate(s32 start_val, s32 step_val, s32 limit) {
    s32 acc = 0;
    s32 cur = start_val;
    do {
        acc += TransformElement(cur);
        cur += step_val;
    } while (cur < limit);
    return acc;
}
)");
}

TEST_F(RoundTripTest, CalleeSavedInitializedBeforeMultipleCalls) {
  AssertRoundTripBitExact("CalleeSavedInitializedBeforeMultipleCalls", R"(
extern s32 ComputeStep(s32 x);

s32 CalleeSavedInitializedBeforeMultipleCalls(s32 a, s32 b) {
    s32 first = a + 5;
    s32 second = b + 10;
    s32 r1 = ComputeStep(first + second);
    return r1 + first + second;
}
)");
}

TEST_F(RoundTripTest, SwitchOnLoadedByteWithNonZeroLowerBound) {
  AssertRoundTripBitExact("SwitchOnLoadedByteWithNonZeroLowerBound", R"(
s32 SwitchOnLoadedByteWithNonZeroLowerBound(const u8* state_ptr, s32 base) {
    s32 res;
    switch (state_ptr[3]) {
        case 2:
            return 100;
        case 3:
            res = base + 20;
            break;
        case 4:
            res = base + 30;
            break;
        case 5:
            res = base + 40;
            break;
        case 6:
            res = base + 50;
            break;
        case 7:
            res = base + 60;
            break;
        default:
            res = -1;
            break;
    }
    return res * 2;
}
)");
}

TEST_F(RoundTripTest, MultiArgCallWithLoadedStructFields) {
  AssertRoundTripBitExact("MultiArgCallWithLoadedStructFields", R"(
typedef struct {
    s32 id;
    s32 x;
    s32 y;
    s32 flags;
} EntityState;

extern s32 UpdateEntityPosition(s32 id, s32 x, s32 y, s32 flags);

s32 MultiArgCallWithLoadedStructFields(EntityState* ent) {
    s32 res = UpdateEntityPosition(ent->id, ent->x, ent->y, ent->flags);
    ent->flags = res;
    return res;
}
)");
}

TEST_F(RoundTripTest, SignExtendedShortCallReturnInBranch) {
  AssertRoundTripBitExact("SignExtendedShortCallReturnInBranch", R"(
extern s16 QueryShortStatus(s32 channel);
extern void NotifyComplete(s32 channel);

s32 SignExtendedShortCallReturnInBranch(s32 channel) {
    if (QueryShortStatus(channel) == 1) {
        NotifyComplete(channel);
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, SequentialIfsReusingLoadedFieldAndCalls) {
  AssertRoundTripBitExact("SequentialIfsReusingLoadedFieldAndCalls", R"(
extern void TriggerEventA(s32 id, s32 param);
extern void TriggerEventB(s32 id, s32 param);

void SequentialIfsReusingLoadedFieldAndCalls(s32* state) {
    if (state[0] == 1) {
        TriggerEventA(state[1], 10);
    }
    if (state[2] == 2) {
        TriggerEventB(state[3], 20);
    }
}
)");
}

TEST_F(RoundTripTest, StoreThroughIndexedStructPointerArray) {
  AssertRoundTripBitExact("StoreThroughIndexedStructPointerArray", R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
} ActorSlot;

void StoreThroughIndexedStructPointerArray(ActorSlot** table, s32 new_x, s32 new_y, s32 new_z) {
    table[0]->x = new_x;
    table[1]->y = new_y;
    table[2]->z = new_z;
}
)");
}

TEST_F(RoundTripTest, FloatIntConvertWithInterleavedStores) {
  AssertRoundTripBitExact("FloatIntConvertWithInterleavedStores", R"(
s32 FloatIntConvertWithInterleavedStores(s32 count, f32 scale, s32* out_meta, f32* out_float) {
    f32 fcount;
    f32 scaled;
    fcount = (f32)count;
    *out_meta = count + 1;
    scaled = fcount * scale;
    *out_float = scaled;
    return (s32)scaled;
}
)");
}

TEST_F(RoundTripTest, FloatParametersPreservedAcrossCall) {
  AssertRoundTripBitExact("FloatParametersPreservedAcrossCall", R"(
extern void NotifyMatrixUpdated(s32* out_flag);

f32 FloatParametersPreservedAcrossCall(s32* out_flag, f32 a, f32 b, f32 c) {
    f32 prod = a * b;
    NotifyMatrixUpdated(out_flag);
    return prod + c;
}
)");
}

TEST_F(RoundTripTest, CalleeSavedFloatPassedInGprAndStackAcrossCalls) {
  AssertRoundTripBitExact("CalleeSavedFloatPassedInGprAndStackAcrossCalls", R"(
extern void ApplyVectorTransform(s32 slot, f32 x, f32 y, f32 z, f32 w);

void CalleeSavedFloatPassedInGprAndStackAcrossCalls(s32 slot, f32* vec) {
    f32 x = vec[0];
    f32 y = vec[1];
    ApplyVectorTransform(slot, x, y, x, y);
    ApplyVectorTransform(slot + 1, y, x, y, x);
}
)");
}

TEST_F(RoundTripTest, RepeatedSameArgPassedToMultiArgCall) {
  AssertRoundTripBitExact("RepeatedSameArgPassedToMultiArgCall", R"(
extern s32 DispatchQuad(s32 cmd, s32 a, s32 b, s32 c);

s32 RepeatedSameArgPassedToMultiArgCall(s32 val) {
    return DispatchQuad(101, val, val, val);
}
)");
}

TEST_F(RoundTripTest, GlobalScalarHalfwordAndByteUpdatesAroundCall) {
  AssertRoundTripBitExact("GlobalScalarHalfwordAndByteUpdatesAroundCall", R"(
extern u16 g_frame_counter;
extern u8 g_mode_flag;
extern void SyncSubsystem(s32 frame);

s32 GlobalScalarHalfwordAndByteUpdatesAroundCall(u16 next_val) {
    u16 cur = g_frame_counter;
    g_frame_counter = next_val;
    SyncSubsystem(cur);
    if (g_mode_flag != 0) {
        return g_frame_counter;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, GlobalArrayReadU16) {
  AssertRoundTripBitExact("GlobalArrayReadU16", R"(
extern u16 g_table_u16[16];

u16 GlobalArrayReadU16(s32 idx) {
    return g_table_u16[idx];
}
)");
}

TEST_F(RoundTripTest, GlobalArrayWriteU16) {
  AssertRoundTripBitExact("GlobalArrayWriteU16", R"(
extern u16 g_table_u16[16];

void GlobalArrayWriteU16(s32 idx, u16 val) {
    g_table_u16[idx] = val;
}
)");
}

TEST_F(RoundTripTest, GlobalArrayReadU8) {
  AssertRoundTripBitExact("GlobalArrayReadU8", R"(
extern u8 g_table_u8[32];

u8 GlobalArrayReadU8(s32 idx) {
    return g_table_u8[idx];
}
)");
}

TEST_F(RoundTripTest, GlobalArrayWriteU8) {
  AssertRoundTripBitExact("GlobalArrayWriteU8", R"(
extern u8 g_table_u8[32];

void GlobalArrayWriteU8(s32 idx, u8 val) {
    g_table_u8[idx] = val;
}
)");
}

TEST_F(RoundTripTest, GlobalArrayReadS16) {
  AssertRoundTripBitExact("GlobalArrayReadS16", R"(
extern s16 g_table_s16[16];

s16 GlobalArrayReadS16(s32 idx) {
    return g_table_s16[idx];
}
)");
}

TEST_F(RoundTripTest, GlobalArrayReadS8) {
  AssertRoundTripBitExact("GlobalArrayReadS8", R"(
extern s8 g_table_s8[32];

s8 GlobalArrayReadS8(s32 idx) {
    return g_table_s8[idx];
}
)");
}

TEST_F(RoundTripTest, GlobalArrayReadFloat) {
  AssertRoundTripBitExact("GlobalArrayReadFloat", R"(
extern f32 g_table_f32[16];

f32 GlobalArrayReadFloat(s32 idx) {
    return g_table_f32[idx];
}
)");
}

TEST_F(RoundTripTest, GlobalArrayWriteFloat) {
  AssertRoundTripBitExact("GlobalArrayWriteFloat", R"(
extern f32 g_table_f32[16];

void GlobalArrayWriteFloat(s32 idx, f32 val) {
    g_table_f32[idx] = val;
}
)");
}

TEST_F(RoundTripTest, GlobalArrayReadModifyWriteAtIndex) {
  AssertRoundTripBitExact("GlobalArrayReadModifyWriteAtIndex", R"(
extern s32 g_counters[16];

s32 GlobalArrayReadModifyWriteAtIndex(s32 idx, s32 delta) {
    g_counters[idx] += delta;
    return g_counters[idx];
}
)");
}

TEST_F(RoundTripTest, GlobalPointerDereferenceLoadAndStore) {
  AssertRoundTripBitExact("GlobalPointerDereferenceLoadAndStore", R"(
extern s32* g_active_buffer;

s32 GlobalPointerDereferenceLoadAndStore(s32 idx, s32 val) {
    s32 prev = g_active_buffer[idx];
    g_active_buffer[idx] = prev + val;
    return prev;
}
)");
}

TEST_F(RoundTripTest, GlobalArrayConditionalUpdateAroundCall) {
  AssertRoundTripBitExact("GlobalArrayConditionalUpdateAroundCall", R"(
extern s32 g_slot_status[16];
extern void NotifySlotChanged(s32 idx, s32 status);

s32 GlobalArrayConditionalUpdateAroundCall(s32 idx, s32 next_status) {
    if (g_slot_status[idx] != next_status) {
        g_slot_status[idx] = next_status;
        NotifySlotChanged(idx, next_status);
        return 1;
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, GlobalParallelArraysReadAndUpdate) {
  AssertRoundTripBitExact("GlobalParallelArraysReadAndUpdate", R"(
extern s32 g_slot_ids[8];
extern s32 g_slot_states[8];
extern s32 g_slot_timers[8];

s32 GlobalParallelArraysReadAndUpdate(s32 idx, s32 new_state) {
    g_slot_states[idx] = new_state;
    g_slot_timers[idx] = 0;
    return g_slot_ids[idx];
}
)");
}

// ============================================================================
// Category 28: Linked-List Traversal, Pointer Loops, and Multi-Argument Calls
// ============================================================================

TEST_F(RoundTripTest, LinkedListSortedInsertVoid) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 pri;
    struct ListNode* head_ref;
} ListNode;
)";
  AssertRoundTripBitExact("LinkedListSortedInsertVoid", R"(
void LinkedListSortedInsertVoid(ListNode* head, ListNode* node) {
    ListNode* prev = head;
    ListNode* curr = head->next;
    s32 pri = node->pri;
    while (curr->pri >= pri) {
        prev = curr;
        curr = curr->next;
    }
    node->next = prev->next;
    prev->next = node;
    node->head_ref = head;
}
)",
                          opts);
}

TEST_F(RoundTripTest, LinkedListPopFrontReturnNode) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 value;
} ListNode;
)";
  AssertRoundTripBitExact("LinkedListPopFrontReturnNode", R"(
ListNode* LinkedListPopFrontReturnNode(ListNode** head_ptr) {
    ListNode* node = *head_ptr;
    *head_ptr = node->next;
    return node;
}
)",
                          opts);
}

TEST_F(RoundTripTest, LinkedListPushFrontVoid) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 value;
} ListNode;
)";
  AssertRoundTripBitExact("LinkedListPushFrontVoid", R"(
void LinkedListPushFrontVoid(ListNode** head_ptr, ListNode* node) {
    node->next = *head_ptr;
    *head_ptr = node;
}
)",
                          opts);
}

TEST_F(RoundTripTest, LinkedListSumValuesWhileNotNull) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 value;
} ListNode;
)";
  AssertRoundTripBitExact("LinkedListSumValuesWhileNotNull", R"(
s32 LinkedListSumValuesWhileNotNull(ListNode* curr) {
    s32 sum = 0;
    while (curr != 0) {
        sum += curr->value;
        curr = curr->next;
    }
    return sum;
}
)",
                          opts);
}

TEST_F(RoundTripTest, LinkedListFindNodeByKey) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 key;
    s32 value;
} ListNode;
)";
  AssertRoundTripBitExact("LinkedListFindNodeByKey", R"(
ListNode* LinkedListFindNodeByKey(ListNode* curr, s32 key) {
    while (curr != 0) {
        if (curr->key == key) {
            return curr;
        }
        curr = curr->next;
    }
    return 0;
}
)",
                          opts);
}

TEST_F(RoundTripTest, LinkedListClearFlagsVoidLoop) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 flags;
} ListNode;
)";
  AssertRoundTripBitExact("LinkedListClearFlagsVoidLoop", R"(
void LinkedListClearFlagsVoidLoop(ListNode* curr) {
    while (curr != 0) {
        curr->flags = 0;
        curr = curr->next;
    }
}
)",
                          opts);
}

TEST_F(RoundTripTest, PointerLoopUnpackAndScaleToFloatVoid) {
  AssertRoundTripBitExact("PointerLoopUnpackAndScaleToFloatVoid", R"(
void PointerLoopUnpackAndScaleToFloatVoid(f32* dst, s32* src) {
    s32* end = src + 8;
    do {
        s32 w0 = src[0];
        s32 w1 = src[8];
        dst[0] = (f32)((w0 & 0xFFFF0000) | ((u32)w1 >> 16)) * 1.52587891e-05f;
        dst[1] = (f32)((w0 << 16) | (w1 & 0xFFFF)) * 1.52587891e-05f;
        src++;
        dst += 2;
    } while (src != end);
}
)");
}

TEST_F(RoundTripTest, StackBufferTwoCallsWithThreeFloatGprParamsAndThreeStackParams) {
  AssertRoundTripBitExact("StackBufferTwoCallsWithThreeFloatGprParamsAndThreeStackParams", R"(
extern void UnpackBuffer(f32* dst, s32* src);
extern void ProcessUnpacked(f32* buf, f32 x, f32 y, f32 z, s32 a, s32 b, s32 c);

void StackBufferTwoCallsWithThreeFloatGprParamsAndThreeStackParams(
    s32* src, f32 x, f32 y, f32 z, s32 a, s32 b, s32 c) {
    f32 buf[16];
    UnpackBuffer(buf, src);
    ProcessUnpacked(buf, x, y, z, a, b, c);
}
)");
}

TEST_F(RoundTripTest, PreserveTwoFloatGprParamsAcrossCall) {
  AssertRoundTripBitExact("PreserveTwoFloatGprParamsAcrossCall", R"(
extern s32 PrepareTarget(s32 id);
extern void ApplyFloatPair(s32 handle, f32 x, f32 y, s32 extra);

void PreserveTwoFloatGprParamsAcrossCall(s32 id, f32 x, f32 y, s32 extra) {
    s32 handle = PrepareTarget(id);
    ApplyFloatPair(handle, x, y, extra);
}
)");
}

TEST_F(RoundTripTest, PointerArrayCopyWithStrideVoidLoop) {
  AssertRoundTripBitExact("PointerArrayCopyWithStrideVoidLoop", R"(
void PointerArrayCopyWithStrideVoidLoop(s32* dst, s32* src, s32 count) {
    if (count > 0) {
        s32* end = src + count;
        do {
            *dst = *src;
            src++;
            dst += 2;
        } while (src != end);
    }
}
)");
}

TEST_F(RoundTripTest, LinkedListCountMatchingNodes) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 type;
} ListNode;
)";
  AssertRoundTripBitExact("LinkedListCountMatchingNodes", R"(
s32 LinkedListCountMatchingNodes(ListNode* curr, s32 target_type) {
    s32 count = 0;
    while (curr != 0) {
        if (curr->type == target_type) {
            count++;
        }
        curr = curr->next;
    }
    return count;
}
)",
                          opts);
}

TEST_F(RoundTripTest, LinkedListInvokeCallbackPerNode) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 id;
} ListNode;
)";
  AssertRoundTripBitExact("LinkedListInvokeCallbackPerNode", R"(
extern void OnVisitNode(s32 id, s32 ctx);

void LinkedListInvokeCallbackPerNode(ListNode* curr, s32 ctx) {
    while (curr != 0) {
        OnVisitNode(curr->id, ctx);
        curr = curr->next;
    }
}
)",
                          opts);
}

// ============================================================================
// Category 29: Leaf Saved Registers, Multi-Arg Stack Calls, and Mixed Float/Int
// ============================================================================

TEST_F(RoundTripTest, LeafFunctionSavingCalleeSavedAtSpZero) {
  AssertRoundTripBitExact("LeafFunctionSavingCalleeSavedAtSpZero", R"(
s32 LeafFunctionSavingCalleeSavedAtSpZero(s32* a, s32* b, s32* c, s32* d) {
    s32 sum = 0;
    s32 i = 0;
    while (i < 16) {
        s32 v0 = a[i];
        s32 v1 = b[i];
        s32 v2 = c[i];
        s32 v3 = d[i];
        s32 v4 = a[i + 1];
        s32 v5 = b[i + 1];
        s32 v6 = c[i + 1];
        s32 v7 = d[i + 1];
        sum += (v0 ^ v1) + (v2 ^ v3) + (v4 ^ v5) + (v6 ^ v7);
        i++;
    }
    return sum;
}
)");
}

TEST_F(RoundTripTest, StackLocalBufferWithMixedThreeCallSizes) {
  AssertRoundTripBitExact("StackLocalBufferWithMixedThreeCallSizes", R"(
extern void CallSevenArgs(s32 a0, s32* out_buf, s32 a2, s32 a3, f32 a4, f32 a5, f32 a6);
extern void CallTenArgs(s32 a0, s32 a1, s32 a2, s32 a3, f32 a4, f32 a5, f32 a6, f32 a7, f32 a8, f32 a9);
extern void CallTwelveArgs(s32 a0, s32 a1, s32 a2, s32 a3, f32 a4, f32 a5, f32 a6, f32 a7, f32 a8, f32 a9, s32 a10, s32 a11);

s32 StackLocalBufferWithMixedThreeCallSizes(s32* p, s32 mode) {
    s32 local_buf[2];
    CallSevenArgs(p[0], local_buf, p[1], p[2], *(f32*)&p[3], *(f32*)&p[4], *(f32*)&p[5]);
    if (mode == 0) {
        CallTenArgs(local_buf[0], p[6], p[7], p[8], *(f32*)&p[9], *(f32*)&p[10], *(f32*)&p[11], *(f32*)&p[12], *(f32*)&p[13], *(f32*)&p[14]);
    } else {
        CallTwelveArgs(local_buf[0], p[6], p[7], p[8], *(f32*)&p[9], *(f32*)&p[10], *(f32*)&p[11], *(f32*)&p[12], *(f32*)&p[13], *(f32*)&p[14], p[15], p[16]);
    }
    return local_buf[0];
}
)");
}

TEST_F(RoundTripTest, CallWithSevenConstantAndStackArgsInsideBranch) {
  AssertRoundTripBitExact("CallWithSevenConstantAndStackArgsInsideBranch", R"(
extern void InitConfigSevenArgs(s32 a0, s32 a1, s32 a2, s32 a3, s32 a4, s32 a5, s32 a6);
extern void PreStep(void);

void CallWithSevenConstantAndStackArgsInsideBranch(s32 flag, s8* state) {
    if (flag != 0) {
        PreStep();
        InitConfigSevenArgs(0, 8, 2, 64, 64, 256, 4096);
        state[0] = 0;
        state[1] = 0;
    }
}
)");
}

TEST_F(RoundTripTest, CallWithSignExtendedHalfwordFifthArgInsideLoop) {
  AssertRoundTripBitExact("CallWithSignExtendedHalfwordFifthArgInsideLoop", R"(
extern void SubmitRectFiveArgs(s32 handle, s32 x, s32 y, s32 w, s32 h);

void CallWithSignExtendedHalfwordFifthArgInsideLoop(s32 handle, s16* coords, s16* end) {
    while (coords != end) {
        s32 h = (s16)(coords[3] + 6);
        SubmitRectFiveArgs(handle, (s16)(coords[0] - 6), (s16)(coords[1] - 6), (s16)(coords[2] + 6), h);
        coords += 4;
    }
}
)");
}

TEST_F(RoundTripTest, FloatParamsAfterTwoPointerParamsWithCalls) {
  AssertRoundTripBitExact("FloatParamsAfterTwoPointerParamsWithCalls", R"(
extern void ApplyForce(s32* dst, f32 fx, f32 fy);

void FloatParamsAfterTwoPointerParamsWithCalls(s32* dst, s32* src, f32 scale_x, f32 scale_y) {
    f32 x = (f32)src[0] * scale_x;
    f32 y = (f32)src[1] * scale_y;
    ApplyForce(dst, x, y);
}
)");
}

TEST_F(RoundTripTest, UnsignedByteLoadsConvertedToFloatStackArgs) {
  AssertRoundTripBitExact("UnsignedByteLoadsConvertedToFloatStackArgs", R"(
extern void ComputePlane(s32* out_plane, f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2);

void UnsignedByteLoadsConvertedToFloatStackArgs(s32* out_plane, u8* pts) {
    ComputePlane(out_plane, (f32)pts[1], (f32)pts[2], (f32)pts[3], (f32)pts[4], (f32)pts[5], (f32)pts[6], (f32)pts[7], (f32)pts[8], (f32)pts[9]);
}
)");
}

TEST_F(RoundTripTest, VoidFunctionUpdatingPointerHeadWithoutReturn) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 val;
} ListNode;
)";
  AssertRoundTripBitExact("VoidFunctionUpdatingPointerHeadWithoutReturn", R"(
void VoidFunctionUpdatingPointerHeadWithoutReturn(ListNode** head) {
    ListNode* first = *head;
    if (first != 0) {
        *head = first->next;
    }
}
)",
                          opts);
}

TEST_F(RoundTripTest, MultipleCallsWithFiveStackArgsInSwitchCases) {
  AssertRoundTripBitExact("MultipleCallsWithFiveStackArgsInSwitchCases", R"(
extern s32 SetupLayerFiveArgs(s32 id, s32 a, s32 b, s32 addr, s32 extra);

s32 MultipleCallsWithFiveStackArgsInSwitchCases(s32 cmd) {
    s32 res = 0;
    if (cmd == 1) {
        res = SetupLayerFiveArgs(0, 14, 14, 0x1000, 0x2000);
    } else if (cmd == 2) {
        res = SetupLayerFiveArgs(0, 255, 255, 255, 255);
    } else if (cmd == 3) {
        res = SetupLayerFiveArgs(0, 0, 0, 0, 0);
    }
    return res;
}
)");
}

// ============================================================================
// Category 30: Multi-Call Callee-Saved Preservation, Float Stack Params & Stores
// ============================================================================

TEST_F(RoundTripTest, PreservePointerAcrossTwoVoidCallsBeforeStore) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct EntityState {
    s32 flags;
    s32 timer;
    s32 mode;
} EntityState;
)";
  AssertRoundTripBitExact("PreservePointerAcrossTwoVoidCallsBeforeStore", R"(
extern void PrepareEntity(EntityState* e);
extern void NotifySystem(s32 mode);

void PreservePointerAcrossTwoVoidCallsBeforeStore(EntityState* e, s32 new_mode) {
    if (e->flags != 0) {
        PrepareEntity(e);
        NotifySystem(new_mode);
        e->mode = new_mode;
        e->timer = 0;
    }
}
)",
                          opts);
}

TEST_F(RoundTripTest, SixParametersWithFourFloatStackAndRegisterArgs) {
  AssertRoundTripBitExact("SixParametersWithFourFloatStackAndRegisterArgs", R"(
extern void WriteVec3(f32* dst, f32 x, f32 y, f32 z);

void SixParametersWithFourFloatStackAndRegisterArgs(f32* dst, s32 mode, f32 x0, f32 y0, f32 x1, f32 y1) {
    f32 dx = x1 - x0;
    f32 dy = y1 - y0;
    if (mode != 0) {
        WriteVec3(dst, dx, dy, dx + dy);
    }
}
)");
}

TEST_F(RoundTripTest, CallResultPassedToSecondCallAndStoredToGlobal) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
extern s32 gLastHandle;
)";
  AssertRoundTripBitExact("CallResultPassedToSecondCallAndStoredToGlobal", R"(
extern s32 AllocateResource(s32 size);
extern void InitializeResource(s32 handle, s32 flags);

s32 CallResultPassedToSecondCallAndStoredToGlobal(s32 size, s32 flags) {
    s32 h = AllocateResource(size);
    gLastHandle = h;
    InitializeResource(h, flags);
    return h;
}
)",
                          opts);
}

TEST_F(RoundTripTest, LoopCallingValidatorAndUpdatingStructFields) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct SlotEntry {
    s32 active;
    s32 id;
    s32 score;
    s32 bonus;
} SlotEntry;
)";
  AssertRoundTripBitExact("LoopCallingValidatorAndUpdatingStructFields", R"(
extern s32 ComputeSlotBonus(s32 id, s32 score);

s32 LoopCallingValidatorAndUpdatingStructFields(SlotEntry* slots, s32 count) {
    s32 updated = 0;
    s32 i = 0;
    while (i < count) {
        if (slots[i].active != 0) {
            slots[i].bonus = ComputeSlotBonus(slots[i].id, slots[i].score);
            updated++;
        }
        i++;
    }
    return updated;
}
)",
                          opts);
}

TEST_F(RoundTripTest, SignedDivisionByHundredAndAccumulateFloat) {
  AssertRoundTripBitExact("SignedDivisionByHundredAndAccumulateFloat", R"(
f32 SignedDivisionByHundredAndAccumulateFloat(const u8* values, s32 idx) {
    s32 a = (values[idx] * 25) / 100;
    s32 b = (values[idx + 7] * 25) / 100;
    return (f32)a + (f32)b;
}
)");
}

TEST_F(RoundTripTest, NestedIfElseChainWithCallsAndSharedCleanup) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
extern s32 gCurrentPhase;
)";
  AssertRoundTripBitExact("NestedIfElseChainWithCallsAndSharedCleanup", R"(
extern s32 CheckConditionA(s32 x);
extern s32 CheckConditionB(s32 x);
extern void TriggerEventA(s32 a, s32 b);
extern void TriggerEventB(s32 a, s32 b);
extern void FinalizeStep(s32 status);

void NestedIfElseChainWithCallsAndSharedCleanup(s32 x, s32 y) {
    if (CheckConditionA(x) != 0) {
        TriggerEventA(x, 10);
        gCurrentPhase = 1;
    } else if (CheckConditionB(y) != 0) {
        TriggerEventB(y, 20);
        gCurrentPhase = 2;
    } else {
        gCurrentPhase = 0;
    }
    FinalizeStep(gCurrentPhase);
}
)",
                          opts);
}

TEST_F(RoundTripTest, MultipleStructPointerStoresAfterHelperCall) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ActorTransform {
    s32 x;
    s32 y;
    s32 z;
    s32 vx;
    s32 vy;
    s32 vz;
} ActorTransform;
)";
  AssertRoundTripBitExact("MultipleStructPointerStoresAfterHelperCall", R"(
extern s32 StepVelocity(s32 v, s32 accel);

void MultipleStructPointerStoresAfterHelperCall(ActorTransform* t, s32 ax, s32 ay, s32 az) {
    t->vx = StepVelocity(t->vx, ax);
    t->vy = StepVelocity(t->vy, ay);
    t->vz = StepVelocity(t->vz, az);
    t->x += t->vx;
    t->y += t->vy;
    t->z += t->vz;
}
)",
                          opts);
}

TEST_F(RoundTripTest, FourFloatStackArgumentsToMatrixHelper) {
  AssertRoundTripBitExact("FourFloatStackArgumentsToMatrixHelper", R"(
extern void BuildTransformEightArgs(s32* mtx, s32 flags, f32 rx, f32 ry, f32 rz, f32 tx, f32 ty, f32 tz);

void FourFloatStackArgumentsToMatrixHelper(s32* mtx, const f32* params, s32 flags) {
    BuildTransformEightArgs(mtx, flags, params[0], params[1], params[2], params[3], params[4], params[5]);
}
)");
}

// ============================================================================
// Category 31: Float Literals, Truncation to Memory, Float Stack Parameters,
//              and Reused Float-in-GPR Parameter Registers
// ============================================================================

TEST_F(RoundTripTest, NonZeroLoFloatConstantScaling) {
  AssertRoundTripBitExact("NonZeroLoFloatConstantScaling", R"(
f32 NonZeroLoFloatConstantScaling(f32 x, f32 y) {
    return x * 0.2f + y * 0.1f;
}
)");
}

TEST_F(RoundTripTest, FloatTruncationStoredDirectlyToMemory) {
  AssertRoundTripBitExact("FloatTruncationStoredDirectlyToMemory", R"(
void FloatTruncationStoredDirectlyToMemory(s32* out, f32 a, f32 b, f32 c) {
    out[0] = (s32)(a * b + c);
    out[1] = (s32)(a - b);
}
)");
}

TEST_F(RoundTripTest, FloatStackParametersUsedInArithmeticAndStore) {
  AssertRoundTripBitExact("FloatStackParametersUsedInArithmeticAndStore", R"(
void FloatStackParametersUsedInArithmeticAndStore(
    f32* out, f32 a, f32 b, f32 c, f32 d, f32 e, f32 f) {
    out[0] = a + d;
    out[1] = b + e;
    out[2] = c + f;
}
)");
}

TEST_F(RoundTripTest, FloatStackParametersPreservedAcrossHelperCall) {
  AssertRoundTripBitExact("FloatStackParametersPreservedAcrossHelperCall", R"(
extern void PrepareFloatOutput(f32* out);

void FloatStackParametersPreservedAcrossHelperCall(
    f32* out, f32 a, f32 b, f32 c, f32 d, f32 e) {
    f32 sum0 = a + d;
    f32 sum1 = b + e;
    PrepareFloatOutput(out);
    out[0] = sum0;
    out[1] = sum1;
    out[2] = c;
}
)");
}

TEST_F(RoundTripTest, FloatGprParamsWithFirstFloatRegReusedForStatusReturn) {
  AssertRoundTripBitExact("FloatGprParamsWithFirstFloatRegReusedForStatusReturn", R"(
extern f32 gVecActorX[64];
extern f32 gVecActorY[64];
extern f32 gVecActorZ[64];
extern u16 gVecActorFlags[64];

s32 FloatGprParamsWithFirstFloatRegReusedForStatusReturn(u16 index, f32 x, f32 y, f32 z) {
    s32 result = 0;
    if (index < 64) {
        if (gVecActorFlags[index] & 1) {
            gVecActorX[index] = x;
            gVecActorY[index] = y;
            gVecActorZ[index] = z;
            result = 1;
        }
    }
    return result;
}
)");
}

TEST_F(RoundTripTest, FloatGprParamsIncrementalUpdateWithStatusReturn) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct VecActor {
    u16 flags;
    f32 x;
    f32 y;
    f32 z;
} VecActor;
)";
  AssertRoundTripBitExact("FloatGprParamsIncrementalUpdateWithStatusReturn", R"(
s32 FloatGprParamsIncrementalUpdateWithStatusReturn(VecActor* a, f32 dx, f32 dy, f32 dz) {
    s32 result = 0;
    if (a != 0) {
        if (a->flags & 1) {
            a->x += dx;
            a->y += dy;
            a->z += dz;
            result = 1;
        }
    }
    return result;
}
)",
                          opts);
}

TEST_F(RoundTripTest, GlobalStoreBeforeFloatArgumentForwardingCall) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
extern u8 gActiveMode;
)";
  AssertRoundTripBitExact("GlobalStoreBeforeFloatArgumentForwardingCall", R"(
extern u8 DispatchUniformScale(u16 id, f32 sx, f32 sy, f32 sz);

u8 GlobalStoreBeforeFloatArgumentForwardingCall(f32 scale) {
    gActiveMode = 2;
    return DispatchUniformScale(91, scale, scale, scale);
}
)",
                          opts);
}

TEST_F(RoundTripTest, SixFloatStackParametersToMatrixInit) {
  AssertRoundTripBitExact("SixFloatStackParametersToMatrixInit", R"(
void SixFloatStackParametersToMatrixInit(
    f32* m, s32 mode, f32 a, f32 b, f32 c, f32 d, f32 e, f32 f, f32 g, f32 h) {
    m[0] = a * d + g;
    m[1] = b * e + h;
    m[2] = c * f;
    if (mode != 0) {
        m[3] = 1.0f;
    } else {
        m[3] = 0.0f;
    }
}
)");
}

// ============================================================================
// Category 32: Callee-Saved Float Carriers, Interleaved Int-to-Float Loops,
//              Double-Precision Scratch Folding & Multi-Arg Float Stack Calls
// ============================================================================

TEST_F(RoundTripTest, CalleeSavedFloatCarriersAcrossTwoSqrtCalls) {
  AssertRoundTripBitExact("CalleeSavedFloatCarriersAcrossTwoSqrtCalls", R"(
extern f32 HelperLength(f32 sq);

void CalleeSavedFloatCarriersAcrossTwoSqrtCalls(
    f32* out, f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1) {
    f32 dx = x1 - x0;
    f32 dy = y1 - y0;
    f32 dz = z1 - z0;
    f32 inv0 = HelperLength(dx * dx + dy * dy + dz * dz);
    f32 inv1;
    dx *= inv0;
    dy *= inv0;
    dz *= inv0;
    inv1 = HelperLength(dx * dx + dy * dy);
    out[0] = dx * inv1;
    out[1] = dy * inv1;
    out[2] = dz;
    out[3] = -(x0 * dx + y0 * dy + z0 * dz);
}
)");
}

TEST_F(RoundTripTest, ContinuedFractionIntToFloatLoopSinglePrecision) {
  AssertRoundTripBitExact("ContinuedFractionIntToFloatLoopSinglePrecision", R"(
f32 ContinuedFractionIntToFloatLoopSinglePrecision(f32 x) {
    f32 acc = 0.0f;
    s32 k = 12;
    s32 odd = 25;
    do {
        s32 sq = k * k;
        k -= 4;
        odd -= 8;
        acc = ((f32)sq * x * x) / ((f32)odd + acc);
    } while (k != 0);
    return x / (1.0f + acc);
}
)");
}

TEST_F(RoundTripTest, ContinuedFractionIntToFloatLoopDoublePrecision) {
  AssertRoundTripBitExact("ContinuedFractionIntToFloatLoopDoublePrecision", R"(
f64 ContinuedFractionIntToFloatLoopDoublePrecision(f64 x) {
    f64 acc = 0.0;
    s32 k = 12;
    s32 odd = 25;
    do {
        s32 sq = k * k;
        k -= 4;
        odd -= 8;
        acc = ((f64)sq * x * x) / ((f64)odd + acc);
    } while (k != 0);
    return x / (1.0 + acc);
}
)");
}

TEST_F(RoundTripTest, DoublePrecisionConstantOffsetAdjustmentFromFloat) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
extern f64 gHalfPiDouble;
extern f64 gPiDouble;
)";
  AssertRoundTripBitExact("DoublePrecisionConstantOffsetAdjustmentFromFloat", R"(
f32 DoublePrecisionConstantOffsetAdjustmentFromFloat(f32 val, f32 sign_ref) {
    if (val > 1.0f) {
        val = (f32)(gHalfPiDouble - (f64)val);
    }
    if (sign_ref < 0.0f) {
        val = (f32)(gPiDouble - (f64)val);
    }
    return val;
}
)",
                          opts);
}

TEST_F(RoundTripTest, ConditionalMultiFloatStackArgHelperCalls) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct CameraRig {
    s32 mode;
    f32 fovy;
    f32 aspect;
    f32 near_p;
    f32 far_p;
    f32 eye_x;
    f32 eye_y;
    f32 eye_z;
    f32 at_x;
    f32 at_y;
    f32 at_z;
} CameraRig;
)";
  AssertRoundTripBitExact("ConditionalMultiFloatStackArgHelperCalls", R"(
extern void SetupPerspective(s32* mtx, u16* norm, f32 fovy, f32 aspect, f32 near_p, f32 far_p, f32 scale);
extern void SetupLookAt(s32* mtx, f32 ex, f32 ey, f32 ez, f32 ax, f32 ay, f32 az);

void ConditionalMultiFloatStackArgHelperCalls(CameraRig* rig, s32* mtx, u16* norm) {
    if (rig->mode == 1) {
        SetupPerspective(mtx, norm, rig->fovy, rig->aspect, rig->near_p, rig->far_p, 1.0f);
    }
    SetupLookAt(mtx, rig->eye_x, rig->eye_y, rig->eye_z, rig->at_x, rig->at_y, rig->at_z);
}
)",
                          opts);
}

TEST_F(RoundTripTest, PreserveFloatGprAndStackParamsInCalleeSavedAcrossCalls) {
  AssertRoundTripBitExact("PreserveFloatGprAndStackParamsInCalleeSavedAcrossCalls", R"(
extern f32 EvalBasis(f32 u, f32 v);

void PreserveFloatGprAndStackParamsInCalleeSavedAcrossCalls(
    f32* out, f32 a, f32 b, f32 c, f32 d, f32 e) {
    f32 r0 = EvalBasis(a, b);
    f32 r1 = EvalBasis(c, d);
    out[0] = r0 + a * e;
    out[1] = r1 + b * d;
    out[2] = c + e;
}
)");
}

// ============================================================================
// Category 33: Multi-Step Shift-Add/Sub Multiplications, Reciprocal Constant
//              Divisions & Modulos, Unsigned-to-Float Conversion Diamonds &
//              Unaligned Struct Block Copies
// ============================================================================

TEST_F(RoundTripTest, MultiStepShiftAddSubConstantMultiplications) {
  AssertRoundTripBitExact("MultiStepShiftAddSubConstantMultiplications", R"(
void MultiStepShiftAddSubConstantMultiplications(s32* out, s32 x, s32 y, s32 z) {
    out[0] = x * 15;
    out[1] = y * 25;
    out[2] = z * 27;
    out[3] = x * 100;
    out[4] = y * 108;
    out[5] = z * 184;
    out[6] = x * 255;
    out[7] = y * 1000;
}
)");
}

TEST_F(RoundTripTest, UnsignedConstantDivisionsAndModulos) {
  AssertRoundTripBitExact("UnsignedConstantDivisionsAndModulos", R"(
void UnsignedConstantDivisionsAndModulos(u32* out, u32 a, u32 b) {
    out[0] = a / 255u;
    out[1] = b % 10u;
}
)");
}

TEST_F(RoundTripTest, SignedConstantDivisionsAndModulos) {
  AssertRoundTripBitExact("SignedConstantDivisionsAndModulos", R"(
void SignedConstantDivisionsAndModulos(s32* out, s32 a, s32 b) {
    out[0] = a / 100;
    out[1] = b % 6;
}
)");
}

TEST_F(RoundTripTest, UnsignedScaleDivideAndFloatAccumulate) {
  AssertRoundTripBitExact("UnsignedScaleDivideAndFloatAccumulate", R"(
f32 UnsignedScaleDivideAndFloatAccumulate(u8* tbl, s32 idx, f32 base) {
    u32 d0 = (u32)(tbl[idx] * 100) / 255u;
    return base + (f32)d0;
}
)");
}

TEST_F(RoundTripTest, UnalignedStructBlock8Copy) {
  AssertRoundTripBitExact("UnalignedStructBlock8Copy", R"(
void UnalignedStructBlock8Copy(UnalignedBlock8* dst, UnalignedBlock8* src) {
    *dst = *src;
}
)");
}

TEST_F(RoundTripTest, UnalignedStructBlock10And20Copy) {
  AssertRoundTripBitExact("UnalignedStructBlock10And20Copy", R"(
typedef struct { u8 b[10]; } CustomBlock10;
typedef struct { u8 b[20]; } CustomBlock20;
void UnalignedStructBlock10And20Copy(CustomBlock10* dst10, CustomBlock10* src10,
                                     CustomBlock20* dst20, CustomBlock20* src20) {
    *dst10 = *src10;
    *dst20 = *src20;
}
)");
}

TEST_F(RoundTripTest, UnalignedStructBlock16CopyLoop) {
  AssertRoundTripBitExact("UnalignedStructBlock16CopyLoop", R"(
void UnalignedStructBlock16CopyLoop(UnalignedBlock16* dst, UnalignedBlock16* src, s32 count) {
    s32 i = 0;
    do {
        *dst = *src;
        src++;
        dst++;
        i++;
    } while (i < count);
}
)");
}

// ============================================================================
// Category 34: Float Outgoing Stack Args & Caller-Homed Parameter Addresses
// ============================================================================

TEST_F(RoundTripTest, FloatStackArgsWithGlobalHalfwordStore) {
  AssertRoundTripBitExact("FloatStackArgsWithGlobalHalfwordStore", R"(
extern u16 D_800E7A0E;
void sink10(s32 a0, s32 a1, s32 a2, s32 a3, f32 f4, f32 f5, f32 f6, f32 f7, s32 s8, s32 s9);
void FloatStackArgsWithGlobalHalfwordStore(s32 id, f32* p) {
    D_800E7A0E = (u16)id;
    sink10(id, 0, 1, 2, p[0], p[1], p[2], p[3], 10, 20);
}
)");
}

TEST_F(RoundTripTest, FloatStackArgsWithInterleavedGlobalStore) {
  AssertRoundTripBitExact("FloatStackArgsWithInterleavedGlobalStore", R"(
extern u16 D_800E7A0E;
void sink8(s32 a0, s32 a1, s32 a2, s32 a3, f32 f4, f32 f5, f32 f6, f32 f7);
void sink10(s32 a0, s32 a1, s32 a2, s32 a3, f32 f4, f32 f5, f32 f6, f32 f7, s32 s8, s32 s9);
void FloatStackArgsWithInterleavedGlobalStore(s32 id, s32 p) {
    if (id > 0) {
        D_800E7A0E = (u16)id;
        sink10(id, 0, 1, 2, *(f32*)(p + 8), *(f32*)(p + 12), *(f32*)(p + 16), *(f32*)(p + 20), 10, 20);
    } else {
        sink8(id, 0, 1, 2, *(f32*)p, *(f32*)(p + 4), *(f32*)(p + 8), *(f32*)(p + 12));
    }
}
)");
}

TEST_F(RoundTripTest, ManyStackArgsFifteenParamCall) {
  AssertRoundTripBitExact("ManyStackArgsFifteenParamCall", R"(
s32 sink15(s32 a0, s32 a1, s32 a2, s32 a3,
           f32 f4, f32 f5, f32 f6, f32 f7, f32 f8, f32 f9, f32 f10, f32 f11,
           s32 s12, s32 s13, s32 s14);
s32 ManyStackArgsFifteenParamCall(s32 a0, f32* p, s32 extra) {
    return sink15(a0, 1, 2, 3, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], extra, 10, 20);
}
)");
}

TEST_F(RoundTripTest, PassFirstParamAddressToCallee) {
  AssertRoundTripBitExact("PassFirstParamAddressToCallee", R"(
s32 ext_take_ptr(s32* p);
s32 PassFirstParamAddressToCallee(s32 a, s32 b) {
    return ext_take_ptr(&a) + b;
}
)");
}

TEST_F(RoundTripTest, PassSecondParamAddressAndReadModified) {
  AssertRoundTripBitExact("PassSecondParamAddressAndReadModified", R"(
void ext_init_second(s32 a, s32* b_out);
s32 PassSecondParamAddressAndReadModified(s32 a, s32 b) {
    ext_init_second(a, &b);
    return b;
}
)");
}

TEST_F(RoundTripTest, PassTwoParamAddressesToCallee) {
  AssertRoundTripBitExact("PassTwoParamAddressesToCallee", R"(
s32 ext_swap_or_add(s32* a, s32* b);
s32 PassTwoParamAddressesToCallee(s32 a, s32 b) {
    ext_swap_or_add(&a, &b);
    return a + b;
}
)");
}

TEST_F(RoundTripTest, FloatStackArgsConditionalDispatch) {
  AssertRoundTripBitExact("FloatStackArgsConditionalDispatch", R"(
void sink9a(s32 a0, s32 a1, s32 a2, s32 a3, f32 f4, f32 f5, f32 f6, f32 f7, s32 s8);
void sink9b(s32 a0, s32 a1, s32 a2, s32 a3, f32 f4, f32 f5, f32 f6, f32 f7, s32 s8);
void FloatStackArgsConditionalDispatch(s32 id, f32* p, s32 flag) {
    if (flag != 0) {
        sink9a(id, 1, 2, 3, p[0], p[1], p[2], p[3], flag);
    } else {
        sink9b(id, 4, 5, 6, p[4], p[5], p[6], p[7], 0);
    }
}
)");
}

// ============================================================================
// Category 33: 9 Callee-Saved Registers ($s0..$s8 / fp), Loop-Hoisted Magic
// Division Multipliers in $s8, and Partially Folded Signed Division Idioms
// ============================================================================

TEST_F(RoundTripTest, NineCalleeSavedRegistersAcrossMultipleCalls) {
  AssertRoundTripBitExact("NineCalleeSavedRegistersAcrossMultipleCalls", R"(
s32 step_fn(s32 x);
void sink_nine(s32 a, s32 b, s32 c, s32 d);

s32 NineCalleeSavedRegistersAcrossMultipleCalls(s32* p, s32 a, s32 b, s32 c) {
    s32 s0 = p[0];
    s32 s1 = p[1];
    s32 s2 = p[2];
    s32 s3 = p[3];
    s32 s4 = p[4];
    s32 s5 = p[5];
    s32 s6 = p[6];
    s32 s7 = p[7];
    s32 v0 = step_fn(s0 + a);
    s32 v1 = step_fn(s1 + v0);
    sink_nine(s2 + v1, s3 + b, s4 + c, s5 - a);
    return s0 + s1 + s2 + s3 + s4 + s5 + s6 + s7 + p[8];
}
)");
}

TEST_F(RoundTripTest, LoopWithNineLiveCalleeSavedValuesAndCall) {
  AssertRoundTripBitExact("LoopWithNineLiveCalleeSavedValuesAndCall", R"(
s32 eval_slot(s32 idx, s32 val, s32 scale);

s32 LoopWithNineLiveCalleeSavedValuesAndCall(s32* arr, s32 count, s32 k0, s32 k1) {
    s32 a = arr[0];
    s32 b = arr[1];
    s32 c = arr[2];
    s32 d = arr[3];
    s32 sum = 0;
    s32 i = k0;
    while (i < count) {
        sum += eval_slot(i + a, i + b, i + c);
        i++;
    }
    return sum + a + b + c + d + k0 + k1;
}
)");
}

TEST_F(RoundTripTest, SignedDivisionBy184InLoopWithCalls) {
  AssertRoundTripBitExact("SignedDivisionBy184InLoopWithCalls", R"(
void process_pair(s32 q, s32 r);

void SignedDivisionBy184InLoopWithCalls(s32* data, s32 n) {
    s32 i = 0;
    if (n > 0) {
        s32* p = data;
        do {
            s32 v = *p;
            p++;
            i++;
            process_pair(v / 184, (v + 92) / 184);
        } while (i < n);
    }
}
)");
}

TEST_F(RoundTripTest, SignedDivisionWithBranchAndCall) {
  AssertRoundTripBitExact("SignedDivisionWithBranchAndCall", R"(
s32 use_quotient(s32 q);

s32 SignedDivisionWithBranchAndCall(s32 x, s32 y) {
    s32 q = y / 100;
    if (q > x) {
        return use_quotient(q);
    }
    return q + y;
}
)");
}

TEST_F(RoundTripTest, SignedDivisionBy60And3600StoreChain) {
  AssertRoundTripBitExact("SignedDivisionBy60And3600StoreChain", R"(
void SignedDivisionBy60And3600StoreChain(s32 ticks, s32* out) {
    s32 min = ticks / 3600;
    s32 sec;
    out[0] = min;
    sec = ticks / 60;
    out[1] = sec - min * 60;
    out[2] = ticks - sec * 60;
}
)");
}

TEST_F(RoundTripTest, SignedDivisionAndModuloBy240WithCall) {
  AssertRoundTripBitExact("SignedDivisionAndModuloBy240WithCall", R"(
s32 combine_time(s32 q, s32 r);

s32 SignedDivisionAndModuloBy240WithCall(s32 frames, s32 offset) {
    s32 total = frames + offset;
    s32 q = total / 240;
    s32 r = total % 240;
    return combine_time(q, r);
}
)");
}

// ============================================================================
// Category 34: Loop-Hoisted Constants & Liveness Across Goto Labels
// ============================================================================

TEST_F(RoundTripTest, LoopHoistedByteStoresOnMatch) {
  AssertRoundTripBitExact("LoopHoistedByteStoresOnMatch", R"(
void LoopHoistedByteStoresOnMatch(s8* p, s32 n) {
    s32 i = 0;
    if (n > 0) {
        do {
            s8 c = *p;
            if (c == 58) {
                *p = 47;
            } else if (c == 92) {
                *p = 46;
            }
            p++;
            i++;
        } while (i < n);
    }
}
)");
}

TEST_F(RoundTripTest, LoopHoistedHalfwordConstantStore) {
  AssertRoundTripBitExact("LoopHoistedHalfwordConstantStore", R"(
void LoopHoistedHalfwordConstantStore(s16* p, s32 count) {
    s32 i = 0;
    if (count > 0) {
        do {
            s16 v = *p;
            if (v == 100) {
                *p = 3;
            } else if (v == 200) {
                *p = 7;
            }
            p++;
            i++;
        } while (i < count);
    }
}
)");
}

TEST_F(RoundTripTest, LoopHoistedBitwiseMaskAndCompare) {
  AssertRoundTripBitExact("LoopHoistedBitwiseMaskAndCompare", R"(
s32 LoopHoistedBitwiseMaskAndCompare(u16* arr, s32 n) {
    s32 hits = 0;
    s32 i = 0;
    if (n > 0) {
        do {
            u16 v = *arr;
            if ((v & 7) == 5) {
                *arr = v | 8;
                hits++;
            }
            arr++;
            i++;
        } while (i < n);
    }
    return hits;
}
)");
}

TEST_F(RoundTripTest, ScratchFoldingBeforeGotoSharedExitLabel) {
  AssertRoundTripBitExact("ScratchFoldingBeforeGotoSharedExitLabel", R"(
void notify_error(s32 code);
s32 compute_step(s32 x);

s32 ScratchFoldingBeforeGotoSharedExitLabel(s32* p, s32 a, s32 b) {
    if (a < 0) {
        goto bail;
    }
    if (b < 0) {
        p[0] = a;
        goto bail;
    }
    p[1] = (a + b) << 2;
    p[2] = compute_step(a - b);
    return p[2];
bail:
    notify_error(a);
    return -1;
}
)");
}

TEST_F(RoundTripTest, MultipleCallsAndStoresBeforeSharedReturnLabel) {
  AssertRoundTripBitExact("MultipleCallsAndStoresBeforeSharedReturnLabel", R"(
s32 fetch_val(s32* dst, s32 mode);
void commit_val(s32* dst, s32 v);

s32 MultipleCallsAndStoresBeforeSharedReturnLabel(s32* dst, s32 mode) {
    if (mode <= 0) {
        goto done;
    }
    if (mode == 1) {
        s32 x = fetch_val(dst, 1);
        dst[0] = x + 10;
        goto done;
    }
    dst[1] = fetch_val(dst, mode);
    commit_val(dst, dst[1] * 3);
    return 1;
done:
    return 0;
}
)");
}

TEST_F(RoundTripTest, LoopHoistedThreeWayStateTransition) {
  AssertRoundTripBitExact("LoopHoistedThreeWayStateTransition", R"(
s32 LoopHoistedThreeWayStateTransition(u8* states, s32 n) {
    s32 changed = 0;
    s32 i = 0;
    if (n > 0) {
        do {
            u8 s = *states;
            if (s == 1) {
                *states = 2;
                changed++;
            } else if (s == 2) {
                *states = 3;
                changed++;
            } else if (s == 3) {
                *states = 0;
            }
            states++;
            i++;
        } while (i < n);
    }
    return changed;
}
)");
}

// ============================================================================
// Category 35: Caller-Saved Register Pinning Across Goto/Label Control Flow &
//              Multi-Branch Call Dispatch
// ============================================================================

TEST_F(RoundTripTest, NonLeafEntryCallFollowedByGotoBailoutAndScratchCompute) {
  AssertRoundTripBitExact("NonLeafEntryCallFollowedByGotoBailoutAndScratchCompute", R"(
s32 validate_pair(s32 a, s32 b);

s32 NonLeafEntryCallFollowedByGotoBailoutAndScratchCompute(s32* hdr, s32 scale) {
    s32 status = validate_pair(hdr[0], scale);
    if (status < 0) {
        goto fail;
    }
    if (status == 0) {
        goto fail;
    }
    hdr[1] = status + hdr[2];
    hdr[3] = hdr[1] ^ hdr[4];
    return hdr[3];
fail:
    hdr[0] = -1;
    return 0;
}
)");
}

TEST_F(RoundTripTest, MultiBranchCallDispatchWithSharedGotoErrorAndScratch) {
  AssertRoundTripBitExact("MultiBranchCallDispatchWithSharedGotoErrorAndScratch", R"(
s32 op_alpha(s32 a, s32 b);

s32 MultiBranchCallDispatchWithSharedGotoErrorAndScratch(s32* state, s32 kind) {
    s32 r = op_alpha(state[0], kind);
    if (r < 0) {
        goto err;
    }
    if (r == 0) {
        goto err;
    }
    state[1] = r - state[2];
    state[3] = state[1] + state[4];
    return state[3];
err:
    state[0] = -2;
    return -1;
}
)");
}

TEST_F(RoundTripTest, MultiCalleeSavedWithEntryCallAndSharedGotoCleanup) {
  AssertRoundTripBitExact("MultiCalleeSavedWithEntryCallAndSharedGotoCleanup", R"(
s32 step_value(s32 x, s32 y, s32 z);

s32 MultiCalleeSavedWithEntryCallAndSharedGotoCleanup(s32* p, s32 id, s32 mode) {
    s32 r = step_value(p[0], id, mode);
    if (r < 0) {
        goto cleanup;
    }
    if (r == 0) {
        goto cleanup;
    }
    p[1] = r + p[2];
    p[3] = p[1] ^ p[4];
    return p[3];
cleanup:
    p[0] = -1;
    return 0;
}
)");
}

TEST_F(RoundTripTest, LoopWithMultipleHelperCallsAndScratchAccumulation) {
  AssertRoundTripBitExact("LoopWithMultipleHelperCallsAndScratchAccumulation", R"(
s32 eval_item(s32 x, s32 idx);

s32 LoopWithMultipleHelperCallsAndScratchAccumulation(s32* items, s32 count) {
    s32 total = 0;
    while (count > 0) {
        s32 a = items[0];
        s32 b = items[1];
        s32 r = eval_item(a + 2, b - 1);
        total += (r ^ count);
        items += 2;
        count--;
    }
    return total;
}
)");
}

TEST_F(RoundTripTest, ModifiedFirstParamWithExtraScratchArgsForHelperCall) {
  AssertRoundTripBitExact("ModifiedFirstParamWithExtraScratchArgsForHelperCall", R"(
s32 transform_pair(s32 x, s32 y, s32 z);

s32 ModifiedFirstParamWithExtraScratchArgsForHelperCall(s32* buf, s32 count) {
    s32 total = 0;
    while (count > 0) {
        s32 a = buf[0];
        s32 b = buf[1];
        total += transform_pair(a + 1, b + 2, count);
        buf += 2;
        count--;
    }
    return total;
}
)");
}

// ============================================================================
// Category 36: Multi-Assignment Return Scratch & High-Arity Helper Call Loops
// ============================================================================

TEST_F(RoundTripTest, ThreeStepHelperCallAccumulationIntoReturnScratch) {
  AssertRoundTripBitExact("ThreeStepHelperCallAccumulationIntoReturnScratch", R"(
s32 compute_step(s32 a, s32 b);

s32 ThreeStepHelperCallAccumulationIntoReturnScratch(s32* p, s32 k) {
    s32 r = compute_step(p[0], k + 1);
    if (r < 0) {
        return -1;
    }
    p[1] = r + p[2];
    p[3] = p[1] ^ k;
    return p[3] + p[4];
}
)");
}

TEST_F(RoundTripTest, FourArgHelperCallInLoopWithDualScratchUpdates) {
  AssertRoundTripBitExact("FourArgHelperCallInLoopWithDualScratchUpdates", R"(
s32 blend_quad(s32 a, s32 b, s32 c, s32 d);

s32 FourArgHelperCallInLoopWithDualScratchUpdates(s32* arr, s32 n) {
    s32 sum = 0;
    while (n > 0) {
        s32 x = arr[0] + 4;
        s32 y = arr[1] - 4;
        sum += blend_quad(x, y, arr[2], n);
        arr += 3;
        n--;
    }
    return sum;
}
)");
}

TEST_F(RoundTripTest, ConditionalHelperCallWithPostCallArrayWriteback) {
  AssertRoundTripBitExact("ConditionalHelperCallWithPostCallArrayWriteback", R"(
s32 filter_entry(s32 val, s32 tag);

s32 ConditionalHelperCallWithPostCallArrayWriteback(s32* slot, s32 tag) {
    s32 v = filter_entry(slot[0], tag);
    if (v <= 0) {
        slot[0] = 0;
        return 0;
    }
    slot[1] = v + slot[2];
    slot[3] = slot[1] - tag;
    return slot[3];
}
)");
}

TEST_F(RoundTripTest, DualHelperCallsWithIntermediateStructFieldUpdates) {
  AssertRoundTripBitExact("DualHelperCallsWithIntermediateStructFieldUpdates", R"(
s32 stage_one(s32 a, s32 b);
s32 stage_two(s32 a, s32 b);

s32 DualHelperCallsWithIntermediateStructFieldUpdates(s32* ctx, s32 arg) {
    s32 a;
    s32 b;
    a = stage_one(ctx[0], arg + 1);
    ctx[1] = a;
    b = stage_two(a + ctx[2], arg);
    ctx[3] = b;
    return b + ctx[0];
}
)");
}

TEST_F(RoundTripTest, LoopWithEarlyExitHelperCallAndScratchWriteback) {
  AssertRoundTripBitExact("LoopWithEarlyExitHelperCallAndScratchWriteback", R"(
s32 check_slot(s32 val, s32 idx);

s32 LoopWithEarlyExitHelperCallAndScratchWriteback(s32* table, s32 len) {
    while (len > 0) {
        s32 res = check_slot(table[0], len);
        if (res != 0) {
            table[0] = res + len;
            return res;
        }
        table++;
        len--;
    }
    return 0;
}
)");
}

// ============================================================================
// Category 37: Global Indexed Int-to-Double Conversion & Modulo Subexpressions
// ============================================================================

TEST_F(RoundTripTest, SignedModuloByFiveInConditionalBranch) {
  AssertRoundTripBitExact("SignedModuloByFiveInConditionalBranch", R"(
void SignedModuloByFiveInConditionalBranch(s32* p, s32* out) {
    s32 v = p[0];
    if (v > 0) {
        out[0] = v % 5;
    }
}
)");
}

TEST_F(RoundTripTest, SignedDivisionByThreeInLoopArrayIndexing) {
  AssertRoundTripBitExact("SignedDivisionByThreeInLoopArrayIndexing", R"(
s32 SignedDivisionByThreeInLoopArrayIndexing(s32* p, u16* table) {
    s32 idx = p[0];
    return table[idx / 3];
}
)");
}

TEST_F(RoundTripTest, CalleeSavedFramePointerRegisterPreservationAcrossCalls) {
  AssertRoundTripBitExact("CalleeSavedFramePointerRegisterPreservationAcrossCalls", R"(
s32 step_work(s32 a, s32 b);

s32 CalleeSavedFramePointerRegisterPreservationAcrossCalls(s32* p, s32 n) {
    s32 a = step_work(p[0], n);
    s32 b = step_work(p[1], a);
    p[2] = a + b;
    return p[2] + n;
}
)");
}

TEST_F(RoundTripTest, SingleParamCopiedToSavedAndReusedAcrossBranchCall) {
  AssertRoundTripBitExact("SingleParamCopiedToSavedAndReusedAcrossBranchCall", R"(
void notify_value(s32 v);

s32 SingleParamCopiedToSavedAndReusedAcrossBranchCall(s32* ctx) {
    if (ctx[0] != 0) {
        notify_value(ctx[1] + 3);
    }
    return ctx[2];
}
)");
}

TEST_F(RoundTripTest, MultiCallReturnValueAccumulationWithBranchUpdates) {
  AssertRoundTripBitExact("MultiCallReturnValueAccumulationWithBranchUpdates", R"(
s32 query_state(s32 id, s32 mode);

s32 MultiCallReturnValueAccumulationWithBranchUpdates(s32* s, s32 id) {
    s32 a = query_state(id, s[0]);
    s[2] = a;
    if (a > 0) {
        s32 b = query_state(id + 1, s[1]);
        s[2] = a - b;
    }
    return s[2] + id;
}
)");
}

// ============================================================================
// Category 38: Float Bitcast, Unsigned Shift Before Float Cast & Stride Offsets
// ============================================================================

TEST_F(RoundTripTest, FloatBitcastExponentExtractBranch) {
  AssertRoundTripBitExact("FloatBitcastExponentExtractBranch", R"(
f32 FloatBitcastExponentExtractBranch(f32 x, f32 fallback) {
    s32 bits = *(s32*)&x;
    if (((bits >> 22) & 511) < 310) {
        return x * 0.5f;
    }
    return fallback;
}
)");
}

TEST_F(RoundTripTest, DoublePolynomialHornerEvaluationWithParitySign) {
  AssertRoundTripBitExact("DoublePolynomialHornerEvaluationWithParitySign", R"(
f32 DoublePolynomialHornerEvaluationWithParitySign(f64 x, f64 c1, f64 c2, s32 n) {
    f64 x2 = x * x;
    f64 poly = x + x * x2 * (c1 + x2 * c2);
    if ((n & 1) == 0) {
        return (f32)poly;
    }
    return -(f32)poly;
}
)");
}

TEST_F(RoundTripTest, TwoParamSaveBeforeZeroArgInitCall) {
  AssertRoundTripBitExact("TwoParamSaveBeforeZeroArgInitCall", R"(
void init_subsystems(void);
s32 run_step(s32* buf, s32 mode);

s32 TwoParamSaveBeforeZeroArgInitCall(s32* buf, s32 mode) {
    init_subsystems();
    if (buf[0] != 0) {
        return run_step(buf, mode);
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, UnsignedShiftBitwiseOrBeforeSignedFloatCast) {
  AssertRoundTripBitExact("UnsignedShiftBitwiseOrBeforeSignedFloatCast", R"(
f32 UnsignedShiftBitwiseOrBeforeSignedFloatCast(s32 a, s32 mask, u32 b) {
    s32 combined = (a & mask) | (b >> 16);
    return (f32)combined;
}
)");
}

TEST_F(RoundTripTest, SwitchInsideIfBranchWithPostSwitchProcessing) {
  AssertRoundTripBitExact("SwitchInsideIfBranchWithPostSwitchProcessing", R"(
s32 handle_a(void);
s32 handle_b(void);
s32 handle_c(void);
s32 handle_d(void);
s32 handle_e(void);
s32 handle_f(void);
void commit_id(s32 id);

s32 SwitchInsideIfBranchWithPostSwitchProcessing(s32 flags, s32 cmd) {
    s32 id = 65535;
    if ((flags & 1) == 0) {
        switch (cmd & 65535) {
        case 0:
            id = handle_a();
            break;
        case 1:
            id = handle_b();
            break;
        case 2:
            id = handle_c();
            break;
        case 3:
            id = handle_d();
            break;
        case 4:
            id = handle_e();
            break;
        case 5:
            id = handle_f();
            break;
        default:
            break;
        }
        if ((id & 65535) != 65535) {
            commit_id(id & 65535);
        }
    }
    return id & 65535;
}
)");
}

TEST_F(RoundTripTest, MultiValueSparseEqualityDispatchToSingleAction) {
  AssertRoundTripBitExact("MultiValueSparseEqualityDispatchToSingleAction", R"(
void trigger_event(s32 code);

void MultiValueSparseEqualityDispatchToSingleAction(s32 id) {
    if (id == 428 || id == 523 || id == 950 || id == 1050 || id == 1200 || id == 1250) {
        trigger_event(75);
    }
}
)");
}

// ============================================================================
// Category 39: Short-Circuit Delay-Slot Setup & Decision-Tree Switch Joins
// ============================================================================

TEST_F(RoundTripTest, ShortCircuitAndWithStructFieldLoadsAndUpdate) {
  AssertRoundTripBitExact("ShortCircuitAndWithStructFieldLoadsAndUpdate", R"(
typedef struct {
    s32 active;
    s32 state;
    s32 timer;
    s32 count;
} ActorSlot;

void tick_actor(ActorSlot* slot);

void ShortCircuitAndWithStructFieldLoadsAndUpdate(ActorSlot* slot) {
    if (slot->active != 0 && slot->state == 2 && slot->timer > 0) {
        slot->timer -= 1;
        tick_actor(slot);
    }
}
)");
}

TEST_F(RoundTripTest, ShortCircuitOrWithStructFieldLoadsAndCall) {
  AssertRoundTripBitExact("ShortCircuitOrWithStructFieldLoadsAndCall", R"(
typedef struct {
    s32 flags;
    s32 health;
    s32 timer;
} EntityState;

void reset_entity(EntityState* e);

void ShortCircuitOrWithStructFieldLoadsAndCall(EntityState* e) {
    if (e->flags == 0 || e->health <= 0 || e->timer < 0) {
        reset_entity(e);
    }
}
)");
}

TEST_F(RoundTripTest, TwoCaseHighValueSwitchWithSharedTailAndDefaultExit) {
  AssertRoundTripBitExact("TwoCaseHighValueSwitchWithSharedTailAndDefaultExit", R"(
void handle_mode_a(s32* p);
void handle_mode_b(s32* p);
void finalize_mode(s32* p);

void TwoCaseHighValueSwitchWithSharedTailAndDefaultExit(s32* p, s32 cmd) {
    if (cmd == 4097) {
        handle_mode_a(p);
        finalize_mode(p);
    } else if (cmd == 4098) {
        handle_mode_b(p);
        finalize_mode(p);
    }
}
)");
}

TEST_F(RoundTripTest, CalleeSavedSymbolAddressesAcrossMultipleCalls) {
  AssertRoundTripBitExact("CalleeSavedSymbolAddressesAcrossMultipleCalls", R"(
extern s32 D_00DBA100;
extern s32 D_00DD38F0;
extern s32 D_00DD3910;

void submit_dma_chunk(s32 id, s32* a, s32* b, s32* c);

void CalleeSavedSymbolAddressesAcrossMultipleCalls(void) {
    submit_dma_chunk(1, &D_00DBA100, &D_00DD38F0, &D_00DD3910);
    submit_dma_chunk(2, &D_00DBA100, &D_00DD38F0, &D_00DD3910);
    submit_dma_chunk(3, &D_00DBA100, &D_00DD38F0, &D_00DD3910);
}
)");
}

TEST_F(RoundTripTest, TwoCaseHighValueSwitchWithSharedTailAndReturn) {
  AssertRoundTripBitExact("TwoCaseHighValueSwitchWithSharedTailAndReturn", R"(
void handle_mode_a(s32* p);
void handle_mode_b(s32* p);
void finalize_mode(s32* p);

s32 TwoCaseHighValueSwitchWithSharedTailAndReturn(s32* p, s32 cmd) {
    if (cmd == 1025) {
        handle_mode_a(p);
        finalize_mode(p);
        return 1;
    } else if (cmd == 1026) {
        handle_mode_b(p);
        finalize_mode(p);
        return 1;
    }
    return 0;
}
)");
}

// ============================================================================
// Category 40: Masked Call Return Assignments & Multi-Call Scratch Pinning
// ============================================================================

TEST_F(RoundTripTest, MaskedByteCallReturnStoredAndPassedAcrossCalls) {
  AssertRoundTripBitExact("MaskedByteCallReturnStoredAndPassedAcrossCalls", R"(
u8 fetch_byte_id(s32 slot);
void apply_byte_offset(s32 id, s32* table, s32 len);

void MaskedByteCallReturnStoredAndPassedAcrossCalls(s32* table, s32 slot) {
    s32 a;
    s32 b;
    a = fetch_byte_id(slot) & 255;
    table[0] = a;
    apply_byte_offset(a * 4, table, 6);
    b = fetch_byte_id(slot + 1) & 255;
    table[1] = b;
    apply_byte_offset(b * 4, table, 6);
}
)");
}

TEST_F(RoundTripTest, MaskedHalfwordCallReturnStoredAndPassedAcrossCalls) {
  AssertRoundTripBitExact("MaskedHalfwordCallReturnStoredAndPassedAcrossCalls", R"(
u16 fetch_half_id(s32 slot);
void apply_half_offset(s32 id, s32* table, s32 len);

void MaskedHalfwordCallReturnStoredAndPassedAcrossCalls(s32* table, s32 slot) {
    s32 a;
    s32 b;
    a = fetch_half_id(slot) & 65535;
    table[0] = a;
    apply_half_offset(a * 8, table, 12);
    b = fetch_half_id(slot + 2) & 65535;
    table[1] = b;
    apply_half_offset(b * 8, table, 12);
}
)");
}

TEST_F(RoundTripTest, MaskedByteCallReturnStoredAndPassedAcrossThreeCalls) {
  AssertRoundTripBitExact("MaskedByteCallReturnStoredAndPassedAcrossThreeCalls", R"(
u8 fetch_byte_id(s32 slot);
void apply_byte_offset(s32 id, s32* table, s32 len);

void MaskedByteCallReturnStoredAndPassedAcrossThreeCalls(s32* table, s32 slot) {
    s32 a;
    s32 b;
    s32 c;
    a = fetch_byte_id(slot) & 255;
    table[0] = a;
    apply_byte_offset(a * 4, table, 6);
    b = fetch_byte_id(slot + 1) & 255;
    table[1] = b;
    apply_byte_offset(b * 4, table, 6);
    c = fetch_byte_id(slot + 2) & 255;
    table[2] = c;
    apply_byte_offset(c * 4, table, 6);
}
)");
}

TEST_F(RoundTripTest, MaskedHalfwordCallReturnUsedInIndexedAddressCalculation) {
  AssertRoundTripBitExact("MaskedHalfwordCallReturnUsedInIndexedAddressCalculation", R"(
u16 get_npc_half_slot(void);
s32 copy_npc_record(s32 dst_id, u8* src, s32 max_len);

s32 MaskedHalfwordCallReturnUsedInIndexedAddressCalculation(u8* base, s32 flag) {
    s32 idx;
    if (flag != 0) {
        idx = get_npc_half_slot() & 65535;
        return copy_npc_record(14, base + (idx * 32), 12);
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, MaskedCallReturnUsedInIndexedAddressCalculation) {
  AssertRoundTripBitExact("MaskedCallReturnUsedInIndexedAddressCalculation", R"(
u8 get_npc_slot(void);
s32 copy_npc_name(s32 dst_id, u8* src, s32 max_len);

s32 MaskedCallReturnUsedInIndexedAddressCalculation(u8* base, s32 flag) {
    s32 idx;
    if (flag != 0) {
        idx = get_npc_slot() & 255;
        return copy_npc_name(14, base + (idx * 16), 6);
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, RomSegmentSymbolAddressesPassedToCall) {
  AssertRoundTripBitExact("RomSegmentSymbolAddressesPassedToCall", R"(
extern u8 rom_asset_start[];
extern u8 rom_asset_end[];
void dma_read_rom(s32 slot, u8* rom_start, u8* rom_end, s32 mode);

void RomSegmentSymbolAddressesPassedToCall(s32 slot, s32 mode) {
    dma_read_rom(slot, rom_asset_start, rom_asset_end, mode);
}
)");
}

TEST_F(RoundTripTest, RomSegmentSymbolAddressesStoredToOutgoingStackArgs) {
  AssertRoundTripBitExact("RomSegmentSymbolAddressesStoredToOutgoingStackArgs", R"(
extern u8 rom_bank_a[];
extern u8 rom_bank_b[];
extern u8 rom_bank_c[];
void load_rom_banks(s32 slot, u8* a, s32 count, u8* b, u8* c, s32 flags);

void RomSegmentSymbolAddressesStoredToOutgoingStackArgs(s32 slot, s32 count, s32 flags) {
    load_rom_banks(slot, rom_bank_a, count, rom_bank_b, rom_bank_c, flags);
}
)");
}

TEST_F(RoundTripTest, RomSegmentSymbolAddressPassedAcrossBranches) {
  AssertRoundTripBitExact("RomSegmentSymbolAddressPassedAcrossBranches", R"(
extern u8 rom_sprite_table[];
extern u8 rom_palette_table[];
void setup_sprite_bank(s32 id, u8* table, u8* palette);
void clear_sprite_bank(u8* table);

void RomSegmentSymbolAddressPassedAcrossBranches(s32 id, s32 flag) {
    u8* tbl = rom_sprite_table;
    if (flag != 0) {
        clear_sprite_bank(tbl);
    }
    setup_sprite_bank(id, tbl, rom_palette_table);
}
)");
}

TEST_F(RoundTripTest, MultiCallHotTargetDispatcherWithBranches) {
  AssertRoundTripBitExact("MultiCallHotTargetDispatcherWithBranches", R"(
s32 actor_check_active(s32 slot);
void actor_set_position(s32 slot, s32 x, s32 y, s32 z);
void actor_clear_slot(s32 slot);

s32 MultiCallHotTargetDispatcherWithBranches(s32 slot, s32 x, s32 y, s32 z) {
    if (actor_check_active(slot) != 0) {
        actor_set_position(slot, x, y, z);
        return 1;
    }
    actor_clear_slot(slot);
    return 0;
}
)");
}

TEST_F(RoundTripTest, NineCalleeSavedRegistersAcrossCallsUsesFpS8) {
  AssertRoundTripBitExact("NineCalleeSavedRegistersAcrossCallsUsesFpS8", R"(
s32 step_channel(s32 x);

s32 NineCalleeSavedRegistersAcrossCallsUsesFpS8(s32* p) {
    s32 a = p[0];
    s32 b = p[1];
    s32 c = p[2];
    s32 d = p[3];
    s32 e = p[4];
    s32 f = p[5];
    s32 g = p[6];
    s32 h = p[7];
    s32 r = step_channel(a);
    r += step_channel(b + a);
    r += step_channel(c + b);
    r += step_channel(d + c);
    r += step_channel(e + d);
    r += step_channel(f + e);
    r += step_channel(g + f);
    r += step_channel(h + g);
    return r + a + b + c + d + e + f + g + h;
}
)");
}

TEST_F(RoundTripTest, VoidFunctionWithIgnoredNonVoidCallOnOneBranchAndStoreTail) {
  AssertRoundTripBitExact("VoidFunctionWithIgnoredNonVoidCallOnOneBranchAndStoreTail", R"(
extern u8 g_mode_state;
s32 set_loop_callback(s32 mode);
void play_mode_sfx(s32 sfx);

void VoidFunctionWithIgnoredNonVoidCallOnOneBranchAndStoreTail(s32 action) {
    if (g_mode_state != 0) {
        switch (action) {
            case 1:
                play_mode_sfx(10);
                set_loop_callback(4);
                break;
            case 2:
                g_mode_state = 2;
                play_mode_sfx(20);
                break;
        }
    }
}
)");
}

TEST_F(RoundTripTest, NonLeafTwoBranchReturnValuesInV0WithCalls) {
  AssertRoundTripBitExact("NonLeafTwoBranchReturnValuesInV0WithCalls", R"(
void notify_slot(s32 slot, s32 val);

s32 NonLeafTwoBranchReturnValuesInV0WithCalls(s32* p, s32 mode) {
    s32 a = p[0];
    s32 b = p[1];
    s32 res;
    notify_slot(a, b);
    if (mode > 0) {
        res = p[2] + a;
        p[4] = res;
    } else {
        res = p[3] - b;
        p[5] = res;
    }
    return res + 1;
}
)");
}

TEST_F(RoundTripTest, ModifiedParamWithLocalArgScratchUsedInSingleArgCall) {
  AssertRoundTripBitExact("ModifiedParamWithLocalArgScratchUsedInSingleArgCall", R"(
s32 eval_item_key(s32 key);

s32 ModifiedParamWithLocalArgScratchUsedInSingleArgCall(s32* p) {
    s32 a = p[0];
    s32 b = p[1];
    s32 c = p[2];
    s32 v;
    if (a > b) {
        v = a + c;
        p[3] = v;
        p[4] = v ^ 1;
        return eval_item_key(b + c) + 1;
    }
    v = b - c;
    p[3] = v;
    p[4] = v ^ 1;
    return eval_item_key(a - c) + 1;
}
)");
}

TEST_F(RoundTripTest, GprFloatParameterSavedInCalleeSavedFprWithLocalFloatArrayAndTwoArgCalls) {
  AssertRoundTripBitExact("GprFloatParameterSavedInCalleeSavedFprWithLocalFloatArrayAndTwoArgCalls",
                          R"(
void init_vec4(f32* v, s32 tag);

void GprFloatParameterSavedInCalleeSavedFprWithLocalFloatArrayAndTwoArgCalls(
    f32* out, f32 scale, f32 offset) {
    f32 buf_a[4];
    f32 buf_b[4];
    init_vec4(buf_a, 1);
    buf_a[0] = scale;
    buf_a[1] = offset;
    buf_a[2] = scale + offset;
    buf_a[3] = scale - offset;
    init_vec4(buf_b, 2);
    out[0] = buf_a[0] + buf_b[0] + scale;
    out[1] = buf_a[1] + buf_b[1] + offset;
}
)");
}

TEST_F(RoundTripTest, SubwordStackBufferAccessRightBelowSavedRegisters) {
  AssertRoundTripBitExact("SubwordStackBufferAccessRightBelowSavedRegisters", R"(
s32 consume_bytes(u8* buf, s32 a, s32 b, s32 c, s32 d, s32 e);

s32 SubwordStackBufferAccessRightBelowSavedRegisters(s32 x, s32 y) {
    u8 buf[40];
    buf[0] = (u8)x;
    buf[39] = (u8)y;
    return consume_bytes(buf, x, y, x + 1, y + 1, x + y) + x;
}
)");
}

TEST_F(RoundTripTest, SwitchJumpTableWithNonMonotonicCaseOrder) {
  AssertRoundTripBitExact("SwitchJumpTableWithNonMonotonicCaseOrder", R"(
void handle_step(s32 code, s32 val);

s32 SwitchJumpTableWithNonMonotonicCaseOrder(s32 cmd, s32 x) {
    s32 res = 0;
    switch (cmd) {
        case 5:
            handle_step(50, x + 5);
            res = x + 100;
            break;
        case 0:
            handle_step(10, x);
            res = x + 10;
            break;
        case 3:
            handle_step(30, x * 2);
            res = x + 30;
            break;
        case 1:
            handle_step(20, x - 1);
            res = x - 50;
            break;
        case 4:
            handle_step(40, x + 4);
            res = x + 40;
            break;
        case 2:
            handle_step(25, x ^ 3);
            res = x + 20;
            break;
        default:
            res = -1;
            break;
    }
    return res;
}
)");
}

TEST_F(RoundTripTest, EntryBlockDecisionTreeSwitchWithCalleeSavedRegisters) {
  AssertRoundTripBitExact("EntryBlockDecisionTreeSwitchWithCalleeSavedRegisters", R"(
s32 step_action(s32 a, s32 b);

s32 EntryBlockDecisionTreeSwitchWithCalleeSavedRegisters(s32 x, s32 y, s32 mode) {
    s32 res = 0;
    switch (mode) {
        case 1:
            res = step_action(x + 1, y) + x;
            break;
        case 2:
            res = step_action(x + 2, y) + y;
            break;
        case 3:
            res = step_action(x + 3, y) - x;
            break;
        case 4:
            res = step_action(x + 4, y) - y;
            break;
    }
    return res;
}
)");
}

TEST_F(RoundTripTest, LeafWithModifiedParamAndMultipleCallerSavedScratch) {
  AssertRoundTripBitExact("LeafWithModifiedParamAndMultipleCallerSavedScratch", R"(
s32 LeafWithModifiedParamAndMultipleCallerSavedScratch(s32* p, s32 n) {
    s32 sum = 0;
    while (n > 0) {
        s32 a = p[0];
        s32 b = p[1];
        if (a > b) {
            sum += (a - b) * 3;
        } else {
            sum += (b - a) * 2;
        }
        p += 2;
        n--;
    }
    return sum;
}
)");
}

TEST_F(RoundTripTest, DuplicateRomSegmentSymbolsAcrossMultipleCallsAvoidCse) {
  AssertRoundTripBitExact("DuplicateRomSegmentSymbolsAcrossMultipleCallsAvoidCse", R"(
extern s32 D_00DBA100;
extern s32 D_00DBA100__dup2;

void submit_rom_chunk(s32 id, s32* rom_addr);

void DuplicateRomSegmentSymbolsAcrossMultipleCallsAvoidCse(void) {
    submit_rom_chunk(1, &D_00DBA100);
    submit_rom_chunk(2, &D_00DBA100__dup2);
}
)");
}

TEST_F(RoundTripTest, O0FramePointerStructFieldAndCall) {
  RoundTripOptions opts;
  opts.c_flags = {"-O0", "-mips2", "-mcpu=r4000"};
  AssertRoundTripBitExact("O0FramePointerStructFieldAndCall", R"(
typedef struct {
    s32 id;
    s32 count;
    u16 flags;
} NodeState;

void notify_node(s32 id, s32 val);

void O0FramePointerStructFieldAndCall(NodeState* node, s32 delta) {
    node->count = node->count + delta;
    if (node->flags != 0) {
        notify_node(node->id, node->count);
    }
}
)",
                          opts);
}

TEST_F(RoundTripTest, O0FramePointerWhileLoopAndUnsignedByteStore) {
  RoundTripOptions opts;
  opts.c_flags = {"-O0", "-mips2", "-mcpu=r4000"};
  AssertRoundTripBitExact("O0FramePointerWhileLoopAndUnsignedByteStore", R"(
typedef struct {
    s32 count;
    u8 status;
    u8 limit;
    u16 mask;
} ChannelState;

void O0FramePointerWhileLoopAndUnsignedByteStore(ChannelState* ch, u8* buf) {
    s32 i = 0;
    while (i < ch->count) {
        *buf++ = 0;
        i = i + 1;
    }
    ch->mask = 65535;
    ch->status = 255;
    ch->limit = 127;
}
)",
                          opts);
}

TEST_F(RoundTripTest, NonLeafFloatScratchStoreBeforeHelperCall) {
  AssertRoundTripBitExact("NonLeafFloatScratchStoreBeforeHelperCall", R"(
typedef struct {
    f32 x;
    f32 y;
    f32 sum;
    s32 tag;
} VecNode;

void notify_vec_updated(VecNode* node);

void NonLeafFloatScratchStoreBeforeHelperCall(VecNode* node, f32 scale) {
    node->sum = (node->x + node->y) * scale;
    notify_vec_updated(node);
}
)");
}

TEST_F(RoundTripTest, MultiParamSavedToCalleeRegistersAndReusedAcrossCalls) {
  AssertRoundTripBitExact("MultiParamSavedToCalleeRegistersAndReusedAcrossCalls", R"(
typedef struct {
    u8* data;
    s32 len;
    s32 pos;
} StreamState;

void clear_buffer(u8* dst, s32 len);
s32 process_byte(u8* dst, s32 val);

s32 MultiParamSavedToCalleeRegistersAndReusedAcrossCalls(StreamState* st, u8* out) {
    s32 pos;
    s32 ch;
    clear_buffer(out, 300);
    pos = st->pos;
    if (pos < st->len) {
        st->pos = pos + 1;
        ch = st->data[pos];
    } else {
        ch = -1;
    }
    if (ch != -1) {
        return process_byte(out, ch);
    }
    return 0;
}
)");
}

TEST_F(RoundTripTest, HalfwordBitmaskClearAndStoreAfterHelperCall) {
  AssertRoundTripBitExact("HalfwordBitmaskClearAndStoreAfterHelperCall", R"(
typedef struct {
    u16 flags;
    u16 state;
    s32 pad[12];
} ActorSlot;

extern ActorSlot g_actors[16];
void reset_actor_slot(s32 idx);

void HalfwordBitmaskClearAndStoreAfterHelperCall(s32 idx) {
    reset_actor_slot(idx);
    g_actors[idx].flags &= 0xFFEF;
}
)");
}

TEST_F(RoundTripTest, ShortCircuitFloatAndStoreGlobal) {
  AssertRoundTripBitExact("ShortCircuitFloatAndStoreGlobal", R"(
extern s32 g_ready_flag;

void ShortCircuitFloatAndStoreGlobal(f32 x, f32 y, f32 z) {
    if (x > 0.0f && y > 0.0f && z > 0.0f) {
        g_ready_flag = 0;
    }
}
)");
}

TEST_F(RoundTripTest, ShortCircuitFloatOrStoreGlobal) {
  AssertRoundTripBitExact("ShortCircuitFloatOrStoreGlobal", R"(
extern s32 g_error_flag;

void ShortCircuitFloatOrStoreGlobal(f32 x, f32 y, f32 z) {
    if (x < 0.0f || y < 0.0f || z < 0.0f) {
        g_error_flag = 0;
    }
}
)");
}

TEST_F(RoundTripTest, DoublePrecisionCompareAndTruncateToInt) {
  AssertRoundTripBitExact("DoublePrecisionCompareAndTruncateToInt", R"(
extern s32 g_clamped_int;

void DoublePrecisionCompareAndTruncateToInt(f64 a, f64 b) {
    if (a < b) {
        g_clamped_int = (s32)a;
    }
}
)");
}

TEST_F(RoundTripTest, DoublePrecisionInA2A3AndStackParameters) {
  AssertRoundTripBitExact("DoublePrecisionInA2A3AndStackParameters", R"(
f64 DoublePrecisionInA2A3AndStackParameters(s32 mode, f64 a, f64 b) {
    if (mode > 0) {
        return a + b;
    }
    return a - b;
}
)");
}

TEST_F(RoundTripTest, CallerPassesDoubleInA2A3AndStackToHelper) {
  AssertRoundTripBitExact("CallerPassesDoubleInA2A3AndStackToHelper", R"(
void combine_doubles(s32 mode, f64 a, f64 b);

void CallerPassesDoubleInA2A3AndStackToHelper(s32 mode, f64* values) {
    combine_doubles(mode, values[0], values[1]);
}
)");
}

TEST_F(RoundTripTest, ExtractFloatWordsFromDoubleOnStack) {
  AssertRoundTripBitExact("ExtractFloatWordsFromDoubleOnStack", R"(
f32 ExtractFloatWordsFromDoubleOnStack(f64 x) {
    f64 local_d = x * 2.0;
    return *(f32*)&local_d + *(f32*)((u8*)&local_d + 4);
}
)");
}

TEST_F(RoundTripTest, StoreUintAsFloatAndDouble) {
  AssertRoundTripBitExact("StoreUintAsFloatAndDouble", R"(
void StoreUintAsFloatAndDouble(u32 x, f32* out_f, f64* out_d) {
    *out_f = (f32)x;
    *out_d = (f64)x;
}
)");
}

TEST_F(RoundTripTest, TwoUintsToFloatAndDoubleRatio) {
  AssertRoundTripBitExact("TwoUintsToFloatAndDoubleRatio", R"(
f64 TwoUintsToFloatAndDoubleRatio(u32 a, u32 b) {
    return (f64)a / (f64)(f32)b;
}
)");
}

TEST_F(RoundTripTest, AddTwoUnsignedToDouble) {
  AssertRoundTripBitExact("AddTwoUnsignedToDouble", R"(
f64 AddTwoUnsignedToDouble(u32 a, u32 b) {
    return (f64)a + (f64)b;
}
)");
}

TEST_F(RoundTripTest, ConvertTwoUnsignedToFloatAndStore) {
  AssertRoundTripBitExact("ConvertTwoUnsignedToFloatAndStore", R"(
void ConvertTwoUnsignedToFloatAndStore(u32 a, u32 b, f32* out) {
    out[0] = (f32)a * 0.5f;
    out[1] = (f32)b * 0.25f;
}
)");
}

TEST_F(RoundTripTest, ForLoopInclusiveUpperBoundStrideTwo) {
  AssertRoundTripBitExact("ForLoopInclusiveUpperBoundStrideTwo", R"(
s32 ForLoopInclusiveUpperBoundStrideTwo(s32 limit) {
    s32 sum = 0;
    s32 i;
    for (i = 1; i <= limit; i += 2) {
        sum += (i ^ 3);
    }
    return sum;
}
)");
}

TEST_F(RoundTripTest, ForLoopCountdownGreaterEqualOneStrideThree) {
  AssertRoundTripBitExact("ForLoopCountdownGreaterEqualOneStrideThree", R"(
s32 ForLoopCountdownGreaterEqualOneStrideThree(s32 start) {
    s32 acc = 0;
    s32 i;
    for (i = start; i >= 1; i -= 3) {
        acc += (i * 2);
    }
    return acc;
}
)");
}

TEST_F(RoundTripTest, LeafWithMultipleScratchAndCalleeSavedRegisters) {
  AssertRoundTripBitExact("LeafWithMultipleScratchAndCalleeSavedRegisters", R"(
s32 LeafWithMultipleScratchAndCalleeSavedRegisters(s32* p) {
    s32 x0 = p[0];
    s32 x1 = p[1];
    s32 x2 = p[2];
    s32 x3 = p[3];
    if (x0 > x1) {
        p[0] = x2 + x3;
        return x0 - x1;
    }
    p[1] = x0 ^ x2;
    return x1 + x3;
}
)");
}

TEST_F(RoundTripTest, LinkedListCursorInSecondParamWithFirstParamPreservedAcrossCall) {
  AssertRoundTripBitExact("LinkedListCursorInSecondParamWithFirstParamPreservedAcrossCall", R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 payload;
} ListNode;

void visit_payload(s32 ctx, s32 payload);

void LinkedListCursorInSecondParamWithFirstParamPreservedAcrossCall(s32 ctx, ListNode* cursor) {
    while (cursor != (ListNode*)0) {
        visit_payload(ctx, cursor->payload);
        cursor = cursor->next;
    }
}
)");
}

TEST_F(RoundTripTest, SixCaseSparseDecisionTreeSwitch) {
  AssertRoundTripBitExact("SixCaseSparseDecisionTreeSwitch", R"(
s32 step_op(s32 a, s32 b);

s32 SixCaseSparseDecisionTreeSwitch(s32 x, s32 y, s32 mode) {
    s32 res = 0;
    switch (mode) {
        case 10:
            res = step_op(x + 1, y) + x;
            break;
        case 20:
            res = step_op(x + 2, y) + y;
            break;
        case 30:
            res = step_op(x + 3, y) - x;
            break;
        case 40:
            res = step_op(x + 4, y) - y;
            break;
        case 50:
            res = step_op(x + 5, y) ^ x;
            break;
        case 60:
            res = step_op(x + 6, y) ^ y;
            break;
    }
    return res;
}
)");
}

TEST_F(RoundTripTest, SecondIntParamSavedAndPassedWithThirdFloatGprParam) {
  AssertRoundTripBitExact("SecondIntParamSavedAndPassedWithThirdFloatGprParam", R"(
s32 helper_pair(s32 a, s32 b);

f32 SecondIntParamSavedAndPassedWithThirdFloatGprParam(s32 a, s32 b, f32 c) {
    s32 r = helper_pair(a + 1, b);
    return (f32)(r + b) + c;
}
)");
}

TEST_F(RoundTripTest, CallResultComparedWithSltiBeforeConditionalCallWithSavedArg) {
  AssertRoundTripBitExact("CallResultComparedWithSltiBeforeConditionalCallWithSavedArg", R"(
s32 poll_level(s32 ch);
void apply_level(s32 ch, s32 level);

void CallResultComparedWithSltiBeforeConditionalCallWithSavedArg(s32 ch) {
    s32 lvl = poll_level(ch);
    if (lvl < 5) {
        apply_level(ch, lvl);
    }
}
)");
}

TEST_F(RoundTripTest, ThreeCaseSwitchLoadedSelectorWithCrossJumpedCall) {
  AssertRoundTripBitExact("ThreeCaseSwitchLoadedSelectorWithCrossJumpedCall", R"(
void dispatch_pair(s32 a, s32 b);

void ThreeCaseSwitchLoadedSelectorWithCrossJumpedCall(const s32* state_ptr) {
    switch (*state_ptr) {
        case 0:
            dispatch_pair(80, 104);
            break;
        case 1:
            dispatch_pair(88, 112);
            break;
        case 2:
            dispatch_pair(96, 120);
            break;
    }
}
)");
}

TEST_F(RoundTripTest, DoWhileLoopWithUnsigned16BitIndexArrayFill) {
  AssertRoundTripBitExact("DoWhileLoopWithUnsigned16BitIndexArrayFill", R"(
void DoWhileLoopWithUnsigned16BitIndexArrayFill(s32* words, s32 fill_val) {
    u16 i = 0;
    do {
        words[i] = fill_val;
        i++;
    } while (i < 4);
}
)");
}

TEST_F(RoundTripTest, DoWhileLoopWithConditionalNarrowCounterIncrement) {
  AssertRoundTripBitExact("DoWhileLoopWithConditionalNarrowCounterIncrement", R"(
s32 DoWhileLoopWithConditionalNarrowCounterIncrement(const u8* table, s32 target) {
    u8 count = 0;
    u8 i = 0;
    u8 key = (u8)target;
    do {
        const u8* row = table + (i * 48);
        if ((*(const u16*)(row + 16) & 1) != 0) {
            if (row[3] == key) {
                count++;
            }
        }
        i++;
    } while (i < 8);
    return count;
}
)");
}

TEST_F(RoundTripTest, TwoCaseSwitchWithPrologueDelaySlotAndStolenCaseConstant) {
  AssertRoundTripBitExact("TwoCaseSwitchWithPrologueDelaySlotAndStolenCaseConstant", R"(
void setup_tex_mode(s32 a, s32 b);

void TwoCaseSwitchWithPrologueDelaySlotAndStolenCaseConstant(s32* state, s32 val) {
    switch (state[0]) {
        case 0:
            state[1] = 0;
            setup_tex_mode(val, 10);
            break;
        case 1:
            state[1] = 2;
            setup_tex_mode(val, 20);
            break;
    }
}
)");
}

TEST_F(RoundTripTest, NestedIfRangeChecksOnSameVariableWithoutFoldRangeTestMerge) {
  AssertRoundTripBitExact("NestedIfRangeChecksOnSameVariableWithoutFoldRangeTestMerge", R"(
void apply_slot_action(s32 idx);

void NestedIfRangeChecksOnSameVariableWithoutFoldRangeTestMerge(s32 idx) {
    if (idx < 2) {
        if (idx >= 0) {
            apply_slot_action(idx);
        }
    }
}
)");
}

TEST_F(RoundTripTest, DoWhileLoopWithPostDecrementNarrowCounterAndCall) {
  AssertRoundTripBitExact("DoWhileLoopWithPostDecrementNarrowCounterAndCall", R"(
s32 update_layer_entry(s32 slot, s32 mode);

s32 DoWhileLoopWithPostDecrementNarrowCounterAndCall(u8* row, s32 mode) {
    s32 ret = 0;
    if ((*(u16*)(row + 12) & 1) != 0) {
        u8 count = row[10];
        s32 prev;
        do {
            update_layer_entry((u16)(*(u16*)(row + 8) + (u16)count), (u8)mode);
            prev = count;
            count--;
        } while (prev != 0);
        ret = 1;
    }
    return ret;
}
)");
}

TEST_F(RoundTripTest, UnpinsS0CopyFromUnmodifiedParameterBeforeCallAndPostCallStore) {
  AssertRoundTripBitExact("UnpinsS0CopyFromUnmodifiedParameterBeforeCallAndPostCallStore", R"(
s32 step_interp(s16* state);

void UnpinsS0CopyFromUnmodifiedParameterBeforeCallAndPostCallStore(s16* state) {
    s32 old_val = state[0];
    if (step_interp(state) != 0) {
        if (old_val == state[0]) {
            state[2] = 0;
        }
    }
}
)");
}

TEST_F(RoundTripTest, LeafWithUnusedLocalStackBuffer) {
  AssertRoundTripBitExact("LeafWithUnusedLocalStackBuffer", R"(
void LeafWithUnusedLocalStackBuffer(s32* out, s32 a, s32 b) {
    u8 unused[16];
    out[0] = a + b;
    out[1] = a - b;
}
)");
}

TEST_F(RoundTripTest, DoWhileLoopWithFourNarrowFloatToUintByteCallArgs) {
  RoundTripOptions opts;
  opts.extra_types_header = R"(
typedef struct ColorF {
    f32 r;
    f32 g;
    f32 b;
    f32 a;
} ColorF;
)";
  AssertRoundTripBitExact("DoWhileLoopWithFourNarrowFloatToUintByteCallArgs", R"(
void apply_rgba_slot(u8 r, u8 g, u8 b, u8 a);

void DoWhileLoopWithFourNarrowFloatToUintByteCallArgs(ColorF* slots) {
    u16 i = 0;
    do {
        apply_rgba_slot((u8)(u32)slots[i].r, (u8)(u32)slots[i].g,
                        (u8)(u32)slots[i].b, (u8)(u32)slots[i].a);
        i++;
    } while (i < 4);
}
)",
                          opts);
}

TEST_F(RoundTripTest, NineCalleeSavedRegistersPassedAsOutgoingStackArgsAcrossCalls) {
  AssertRoundTripBitExact("NineCalleeSavedRegistersPassedAsOutgoingStackArgsAcrossCalls", R"(
s32 init_layer_state(void);
void apply_layer_config(s32 a0, s32 a1, s32 a2, s32 a3, s32 a4, s32 a5,
                        s32 a6, s32 a7, s32 a8, s32 a9);

void NineCalleeSavedRegistersPassedAsOutgoingStackArgsAcrossCalls(s32* cfg) {
    s32 s0 = cfg[0];
    s32 s1 = cfg[1];
    s32 s2 = cfg[2];
    s32 s3 = cfg[3];
    s32 s4 = cfg[4];
    s32 s5 = cfg[5];
    s32 s6 = cfg[6];
    s32 s7 = cfg[7];
    s32 r = init_layer_state();
    apply_layer_config(r, s0, s1, s2, s3, s4, s5, s6, s7, r);
    apply_layer_config(r + 1, s0, s1, s2, s3, s4, s5, s6, s7, r);
}
)");
}

TEST_F(RoundTripTest, CalleeSavedFloatWithTransientDoubleScaleAcrossCalls) {
  AssertRoundTripBitExact("CalleeSavedFloatWithTransientDoubleScaleAcrossCalls", R"(
extern f64 g_scale_factor_d;
extern f32 EvalStepF(f32 u, f32 v);

void CalleeSavedFloatWithTransientDoubleScaleAcrossCalls(
    f32* out, f32 a, f32 b, f32 c) {
    f32 r0;
    f32 scaled_c;
    f32 r1;
    r0 = EvalStepF(a * 2.0f, b * 2.0f);
    scaled_c = (f32)((f64)c * g_scale_factor_d);
    r1 = EvalStepF(scaled_c, a);
    out[0] = r0 + scaled_c;
    out[1] = r1 + b;
}
)");
}

TEST_F(RoundTripTest, DecisionTreeSwitchWithCrossJumpedCallTailAndEmptyDefault) {
  AssertRoundTripBitExact("DecisionTreeSwitchWithCrossJumpedCallTailAndEmptyDefault", R"(
void notify_slot_event(s32 slot, s32 code);

void DecisionTreeSwitchWithCrossJumpedCallTailAndEmptyDefault(s32 mode, s32 slot) {
    switch (mode) {
        case 1:
            notify_slot_event(slot, 100);
            break;
        case 2:
            notify_slot_event(slot, 200);
            break;
    }
}
)");
}

TEST_F(RoundTripTest, WhileCallLoopWithBranchLikelyExitByteClearAndReusedParamCounter) {
  AssertRoundTripBitExact("WhileCallLoopWithBranchLikelyExitByteClearAndReusedParamCounter", R"(
typedef struct {
    s32 id;
    s32 pad;
    s32 flags;
    s32 mode;
} StreamReq;

typedef struct {
    u8 active;
    u8 pad[15];
    s32 total;
} StreamHandle;

void lock_stream_bus(s32 enable);
void reset_stream_bus(void);
StreamHandle* open_stream_handle(s32 id, s32 flags, s32 mode);
void report_stream_open_error(s32 id);
s32 read_stream_chunk(StreamHandle* h, s32* chunk_hdr);

s32 WhileCallLoopWithBranchLikelyExitByteClearAndReusedParamCounter(StreamReq* req) {
    StreamHandle* h;
    s32 count;
    s32 chunk[8];
    lock_stream_bus(1);
    reset_stream_bus();
    h = open_stream_handle(req->id, req->flags, req->mode);
    if (h == 0) {
        report_stream_open_error(req->id);
    }
    count = 0;
    while (read_stream_chunk(h, chunk) != 0) {
        count++;
        h->total = chunk[2];
    }
    h->active = 0;
    lock_stream_bus(0);
    return count;
}
)");
}

TEST_F(RoundTripTest, NarrowU16ParameterIndexedStructSetupAcrossCalls) {
  AssertRoundTripBitExact("NarrowU16ParameterIndexedStructSetupAcrossCalls", R"(
extern u16 g_slot_flags[];
extern s32 g_slot_base[];
extern void init_slot_pos(s32 idx, s32 x, s32 y);
extern void init_slot_color(s32 idx, s32 r, s32 g, s32 b);
extern void enable_slot_render(s32 idx, s32 mode);

s32 NarrowU16ParameterIndexedStructSetupAcrossCalls(u16 idx, s32 base_val) {
    s32 res = 0;
    if (idx < 192) {
        if ((g_slot_flags[idx * 78] & 1) == 0) {
            g_slot_base[idx * 39] = base_val;
            g_slot_flags[idx * 78] = 1;
            init_slot_pos(idx, 0, 0);
            init_slot_color(idx, 255, 255, 255);
            enable_slot_render(idx, 3);
            res = 1;
        }
    }
    return res;
}
)");
}

TEST_F(RoundTripTest, ChainedFallthroughStolenComparisonDelaySlotsInNestedGuards) {
  AssertRoundTripBitExact("ChainedFallthroughStolenComparisonDelaySlotsInNestedGuards", R"(
extern u16 g_actor_flags[];
extern void dispatch_actor_update(s32 idx, s32 state, s32 flags);

void ChainedFallthroughStolenComparisonDelaySlotsInNestedGuards(
    s32 idx, s32 state, s32 mode) {
    if ((u32)idx < 192) {
        u16 flags = g_actor_flags[idx];
        if ((flags & 1) != 0) {
            if ((flags & 2) == 0) {
                if (mode != 254) {
                    if (mode < 255) {
                        if (mode != 253) {
                            dispatch_actor_update(idx, state, flags);
                        }
                    }
                }
            }
        }
    }
}
)");
}

TEST_F(RoundTripTest, BranchLikelyDiamondWithSharedCallTailAndEarlyReturn) {
  AssertRoundTripBitExact("BranchLikelyDiamondWithSharedCallTailAndEarlyReturn", R"(
extern s32 eval_slot_cost(s32 id, s32 bonus);
extern void commit_slot_cost(s32 id, s32 cost);

s32 BranchLikelyDiamondWithSharedCallTailAndEarlyReturn(s32* dst, s32 kind) {
    if (kind <= 0) {
        goto done;
    }
    if (kind == 2) {
        s32 c = eval_slot_cost((s32)dst, 2);
        dst[0] = c + 20;
        goto done;
    }
    dst[1] = eval_slot_cost((s32)dst, kind);
    commit_slot_cost((s32)dst, dst[1] * 5);
    return 1;
done:
    return 0;
}
)");
}

TEST_F(RoundTripTest, ShortCircuitOrWithBitmaskAndStructFieldChecks) {
  AssertRoundTripBitExact("ShortCircuitOrWithBitmaskAndStructFieldChecks", R"(
typedef struct {
    u16 flags_a;
    u16 flags_b;
    s32 state;
    s32 timer;
} UnitSlot;

extern void deactivate_unit_slot(UnitSlot* u);

void ShortCircuitOrWithBitmaskAndStructFieldChecks(UnitSlot* u) {
    if ((u->flags_a & 4) != 0 || (u->flags_b & 8) != 0 || u->timer <= 0) {
        u->state = 0;
        deactivate_unit_slot(u);
    }
}
)");
}

}  // namespace
}  // namespace rom_nom_nom
