#include "core/round_trip_test_cases.h"

#include <vector>

namespace rom_nom_nom {

const std::vector<RoundTripTestCase>& GetSharedRoundTripTestCases() {
  static const auto* const k_cases = new std::vector<RoundTripTestCase>{
      RoundTripTestCase{
          .test_name = "ArithmeticAddTwo",
          .function_name = "AddTwo",
          .original_c = R"(
s32 AddTwo(s32 a, s32 b) {
    return a + b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticSubTwo",
          .function_name = "SubTwo",
          .original_c = R"(
s32 SubTwo(s32 a, s32 b) {
    return a - b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseAnd",
          .function_name = "BitwiseAnd",
          .original_c = R"(
s32 BitwiseAnd(s32 a, s32 b) {
    return a & b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseOr",
          .function_name = "BitwiseOr",
          .original_c = R"(
s32 BitwiseOr(s32 a, s32 b) {
    return a | b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseXor",
          .function_name = "BitwiseXor",
          .original_c = R"(
s32 BitwiseXor(s32 a, s32 b) {
    return a ^ b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseNot",
          .function_name = "BitwiseNot",
          .original_c = R"(
s32 BitwiseNot(s32 a) {
    return ~a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticShiftLeftImm",
          .function_name = "ShiftLeftImm",
          .original_c = R"(
s32 ShiftLeftImm(s32 a) {
    return a << 4;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticShiftRightArithmeticImm",
          .function_name = "ShiftRightArithmeticImm",
          .original_c = R"(
s32 ShiftRightArithmeticImm(s32 a) {
    return a >> 3;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticShiftRightLogicalImm",
          .function_name = "ShiftRightLogicalImm",
          .original_c = R"(
u32 ShiftRightLogicalImm(u32 a) {
    return a >> 3;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticShiftLeftVariable",
          .function_name = "ShiftLeftVariable",
          .original_c = R"(
s32 ShiftLeftVariable(s32 a, s32 b) {
    return a << b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticAddImmediate",
          .function_name = "AddImmediate",
          .original_c = R"(
s32 AddImmediate(s32 a) {
    return a + 1234;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticLargeImmediate",
          .function_name = "LargeImmediate",
          .original_c = R"(
u32 LargeImmediate(u32 a) {
    return a + 0x12345678;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticNegate",
          .function_name = "Negate",
          .original_c = R"(
s32 Negate(s32 a) {
    return -a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseAndNot",
          .function_name = "AndNot",
          .original_c = R"(
s32 AndNot(s32 a, s32 b) {
    return a & ~b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseXorImm",
          .function_name = "XorImm",
          .original_c = R"(
s32 XorImm(s32 a) {
    return a ^ 0xFF;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseAndImm",
          .function_name = "AndImm",
          .original_c = R"(
s32 AndImm(s32 a) {
    return a & 0x7F;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseOrImm",
          .function_name = "OrImm",
          .original_c = R"(
s32 OrImm(s32 a) {
    return a | 0x8000;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticShiftRightVar",
          .function_name = "ShiftRightVar",
          .original_c = R"(
s32 ShiftRightVar(s32 a, s32 b) {
    return a >> b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticShiftRightLogicalVar",
          .function_name = "ShiftRightLogicalVar",
          .original_c = R"(
u32 ShiftRightLogicalVar(u32 a, u32 b) {
    return a >> b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticSubImmediate",
          .function_name = "SubImmediate",
          .original_c = R"(
s32 SubImmediate(s32 a) {
    return a - 50;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticCompoundAdd",
          .function_name = "CompoundAdd",
          .original_c = R"(
s32 CompoundAdd(s32 a, s32 b, s32 c) {
    return a + b + c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticMultiOp",
          .function_name = "MultiOp",
          .original_c = R"(
s32 MultiOp(s32 a, s32 b, s32 c) {
    return (a + b) ^ c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticMaskLowSixteen",
          .function_name = "MaskLowSixteen",
          .original_c = R"(
u32 MaskLowSixteen(u32 a) {
    return a & 0xFFFF;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseNor",
          .function_name = "BitwiseNor",
          .original_c = R"(
s32 BitwiseNor(s32 a, s32 b) {
    return ~(a | b);
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticMulConstant",
          .function_name = "MulConstant",
          .original_c = R"(
s32 MulConstant(s32 a) {
    return a * 5;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticMulVar",
          .function_name = "MulVar",
          .original_c = R"(
s32 MulVar(s32 a, s32 b) {
    return a * b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticDivVar",
          .function_name = "DivVar",
          .original_c = R"(
s32 DivVar(s32 a, s32 b) {
    return a / b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticModVar",
          .function_name = "ModVar",
          .original_c = R"(
s32 ModVar(s32 a, s32 b) {
    return a % b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticDivUnsignedVar",
          .function_name = "DivUnsignedVar",
          .original_c = R"(
u32 DivUnsignedVar(u32 a, u32 b) {
    return a / b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticModUnsignedVar",
          .function_name = "ModUnsignedVar",
          .original_c = R"(
u32 ModUnsignedVar(u32 a, u32 b) {
    return a % b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticAddConstant",
          .function_name = "AddConstant",
          .original_c = R"(
s32 AddConstant(s32 a) {
    return a + 42;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseXorLargeImm",
          .function_name = "XorLargeImm",
          .original_c = R"(
u32 XorLargeImm(u32 a) {
    return a ^ 0x12345678;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseAndLargeImm",
          .function_name = "AndLargeImm",
          .original_c = R"(
u32 AndLargeImm(u32 a) {
    return a & 0x12345678;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitwiseOrLargeImm",
          .function_name = "OrLargeImm",
          .original_c = R"(
u32 OrLargeImm(u32 a) {
    return a | 0x12345678;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerLoadS8",
          .function_name = "LoadS8",
          .original_c = R"(
s32 LoadS8(s8* p) {
    return *p;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreS8",
          .function_name = "StoreS8",
          .original_c = R"(
void StoreS8(s8* p, s8 v) {
    *p = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerLoadU8",
          .function_name = "LoadU8",
          .original_c = R"(
u32 LoadU8(u8* p) {
    return *p;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreU8",
          .function_name = "StoreU8",
          .original_c = R"(
void StoreU8(u8* p, u8 v) {
    *p = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerLoadS16",
          .function_name = "LoadS16",
          .original_c = R"(
s32 LoadS16(s16* p) {
    return *p;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreS16",
          .function_name = "StoreS16",
          .original_c = R"(
void StoreS16(s16* p, s16 v) {
    *p = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerLoadU16",
          .function_name = "LoadU16",
          .original_c = R"(
u32 LoadU16(u16* p) {
    return *p;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreU16",
          .function_name = "StoreU16",
          .original_c = R"(
void StoreU16(u16* p, u16 v) {
    *p = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerLoadS32",
          .function_name = "LoadS32",
          .original_c = R"(
s32 LoadS32(s32* p) {
    return *p;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreS32",
          .function_name = "StoreS32",
          .original_c = R"(
void StoreS32(s32* p, s32 v) {
    *p = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerCastU8ToU32",
          .function_name = "CastU8ToU32",
          .original_c = R"(
u32 CastU8ToU32(u32 a) {
    return (u8)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerCastU16ToU32",
          .function_name = "CastU16ToU32",
          .original_c = R"(
u32 CastU16ToU32(u32 a) {
    return (u16)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerCastS16ToS32",
          .function_name = "CastS16ToS32",
          .original_c = R"(
s32 CastS16ToS32(s32 a) {
    return (s16)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerCastS8ToS32",
          .function_name = "CastS8ToS32",
          .original_c = R"(
s32 CastS8ToS32(s32 a) {
    return (s8)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerCastU8ToS32",
          .function_name = "CastU8ToS32",
          .original_c = R"(
s32 CastU8ToS32(s32 a) {
    return (u8)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerCastU16ToS32",
          .function_name = "CastU16ToS32",
          .original_c = R"(
s32 CastU16ToS32(s32 a) {
    return (u16)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerLoadOffsetS32",
          .function_name = "LoadOffsetS32",
          .original_c = R"(
s32 LoadOffsetS32(s32* p) {
    return p[4];
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreOffsetS32",
          .function_name = "StoreOffsetS32",
          .original_c = R"(
void StoreOffsetS32(s32* p, s32 v) {
    p[4] = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerLoadOffsetS16",
          .function_name = "LoadOffsetS16",
          .original_c = R"(
s32 LoadOffsetS16(s16* p) {
    return p[6];
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreOffsetS16",
          .function_name = "StoreOffsetS16",
          .original_c = R"(
void StoreOffsetS16(s16* p, s16 v) {
    p[6] = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerLoadOffsetU8",
          .function_name = "LoadOffsetU8",
          .original_c = R"(
u32 LoadOffsetU8(u8* p) {
    return p[5];
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreOffsetU8",
          .function_name = "StoreOffsetU8",
          .original_c = R"(
void StoreOffsetU8(u8* p, u8 v) {
    p[5] = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreZero",
          .function_name = "StoreZero",
          .original_c = R"(
void StoreZero(s32* p) {
    *p = 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IntegerStoreImmediate",
          .function_name = "StoreImmediate",
          .original_c = R"(
void StoreImmediate(s32* p) {
    *p = 42;
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerDereference",
          .function_name = "PointerDeref",
          .original_c = R"(
s32 PointerDeref(s32* p) {
    return *p;
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerArrayIndexConst",
          .function_name = "ArrayIndexConst",
          .original_c = R"(
s32 ArrayIndexConst(s32* arr) {
    return arr[3];
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerArrayIndexVar",
          .function_name = "ArrayIndexVar",
          .original_c = R"(
s32 ArrayIndexVar(s32* arr, s32 idx) {
    return arr[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerArrayWrite",
          .function_name = "ArrayWrite",
          .original_c = R"(
void ArrayWrite(s32* arr, s32 idx, s32 val) {
    arr[idx] = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "StructFieldAccess_GetEntityId",
          .function_name = "GetEntityId",
          .original_c = R"(
s32 GetEntityId(Entity* e) {
    return e->id;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 id;
    s16 x;
    s16 y;
    u32 flags;
} Entity;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "StructFieldAccess_GetEntityX",
          .function_name = "GetEntityX",
          .original_c = R"(
s32 GetEntityX(Entity* e) {
    return e->x;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 id;
    s16 x;
    s16 y;
    u32 flags;
} Entity;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "StructFieldAccess_SetEntityY",
          .function_name = "SetEntityY",
          .original_c = R"(
void SetEntityY(Entity* e, s16 y) {
    e->y = y;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 id;
    s16 x;
    s16 y;
    u32 flags;
} Entity;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "ArraySumFour",
          .function_name = "ArraySumFour",
          .original_c = R"(
s32 ArraySumFour(s32* arr) {
    return arr[0] + arr[1] + arr[2] + arr[3];
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArraySwapElements",
          .function_name = "ArraySwap",
          .original_c = R"(
void ArraySwap(s32* arr, s32 i, s32 j) {
    s32 tmp = arr[i];
    arr[i] = arr[j];
    arr[j] = tmp;
}
)",
      },
      RoundTripTestCase{
          .test_name = "StructNestedFieldAccess_GetActorPosX",
          .function_name = "GetActorPosX",
          .original_c = R"(
s16 GetActorPosX(Actor* a) {
    return a->pos.x;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s16 x;
    s16 y;
} Vec2s;

typedef struct {
    s32 id;
    Vec2s pos;
    u32 flags;
} Actor;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "StructNestedFieldAccess_SetActorPosY",
          .function_name = "SetActorPosY",
          .original_c = R"(
void SetActorPosY(Actor* a, s16 y) {
    a->pos.y = y;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s16 x;
    s16 y;
} Vec2s;

typedef struct {
    s32 id;
    Vec2s pos;
    u32 flags;
} Actor;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "StructBitwiseFlags_SetActorFlags",
          .function_name = "SetActorFlags",
          .original_c = R"(
void SetActorFlags(ActorFlags* a, u32 flag) {
    a->flags |= flag;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 id;
    u32 flags;
} ActorFlags;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "StructBitwiseFlags_ClearActorFlags",
          .function_name = "ClearActorFlags",
          .original_c = R"(
void ClearActorFlags(ActorFlags* a, u32 flag) {
    a->flags &= ~flag;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 id;
    u32 flags;
} ActorFlags;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "ArrayScaleElement",
          .function_name = "ArrayScaleElement",
          .original_c = R"(
void ArrayScaleElement(s32* arr, s32 idx, s32 scale) {
    arr[idx] *= scale;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArraySumTwo16",
          .function_name = "ArraySumTwo16",
          .original_c = R"(
s16 ArraySumTwo16(s16* arr) {
    return arr[0] + arr[1];
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArraySumTwoU8",
          .function_name = "ArraySumTwoU8",
          .original_c = R"(
u32 ArraySumTwoU8(u8* arr) {
    return arr[0] + arr[1];
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayWriteS16",
          .function_name = "ArrayWriteS16",
          .original_c = R"(
void ArrayWriteS16(s16* arr, s32 idx, s16 val) {
    arr[idx] = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayWriteU8",
          .function_name = "ArrayWriteU8",
          .original_c = R"(
void ArrayWriteU8(u8* arr, s32 idx, u8 val) {
    arr[idx] = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerAdvance",
          .function_name = "PointerAdvance",
          .original_c = R"(
s32* PointerAdvance(s32* ptr, s32 count) {
    return ptr + count;
}
)",
      },
      RoundTripTestCase{
          .test_name = "StructPointDistance",
          .function_name = "PointDistance",
          .original_c = R"(
s32 PointDistance(Point2D* p) {
    return p->x + p->y;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 x;
    s32 y;
} Point2D;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "StructPointCopy",
          .function_name = "PointCopy",
          .original_c = R"(
void PointCopy(Point2D* dst, Point2D* src) {
    dst->x = src->x;
    dst->y = src->y;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 x;
    s32 y;
} Point2D;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "BranchSignum",
          .function_name = "Signum",
          .original_c = R"(
s32 Signum(s32 a) {
    if (a > 0) {
        return 1;
    } else if (a < 0) {
        return -1;
    } else {
        return 0;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchIfElseReturnConst",
          .function_name = "IfElseReturnConst",
          .original_c = R"(
s32 IfElseReturnConst(s32 a) {
    if (a > 0) {
        return 10;
    } else {
        return 20;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchIfElseAssign",
          .function_name = "IfElseAssign",
          .original_c = R"(
s32 IfElseAssign(s32 a, s32 b) {
    s32 res;
    if (a > 10) {
        res = a + b;
    } else {
        res = a - b;
    }
    return res;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchEarlyReturnIfZero",
          .function_name = "EarlyReturnIfZero",
          .original_c = R"(
s32 EarlyReturnIfZero(s32 a, s32 b) {
    if (a == 0) {
        return b;
    }
    return a + b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchMaxTwo",
          .function_name = "MaxTwo",
          .original_c = R"(
s32 MaxTwo(s32 a, s32 b) {
    if (a > b) {
        return a;
    } else {
        return b;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchInRange",
          .function_name = "InRange",
          .original_c = R"(
s32 InRange(s32 a, s32 min, s32 max) {
    if (a >= min && a <= max) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchIsOutside",
          .function_name = "IsOutside",
          .original_c = R"(
s32 IsOutside(s32 a, s32 min, s32 max) {
    if (a < min || a > max) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchAbs",
          .function_name = "AbsVal",
          .original_c = R"(
s32 AbsVal(s32 a) {
    if (a < 0) {
        return -a;
    }
    return a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchMinTwo",
          .function_name = "MinTwo",
          .original_c = R"(
s32 MinTwo(s32 a, s32 b) {
    if (a < b) {
        return a;
    } else {
        return b;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchIsPositive",
          .function_name = "IsPositive",
          .original_c = R"(
s32 IsPositive(s32 a) {
    if (a > 0) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchIsNegative",
          .function_name = "IsNegative",
          .original_c = R"(
s32 IsNegative(s32 a) {
    if (a < 0) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchIsZero",
          .function_name = "IsZero",
          .original_c = R"(
s32 IsZero(s32 a) {
    if (a == 0) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchIsNotZero",
          .function_name = "IsNotZero",
          .original_c = R"(
s32 IsNotZero(s32 a) {
    if (a != 0) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareEqImm",
          .function_name = "CompareEqImm",
          .original_c = R"(
s32 CompareEqImm(s32 a) {
    if (a == 42) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareNeImm",
          .function_name = "CompareNeImm",
          .original_c = R"(
s32 CompareNeImm(s32 a) {
    if (a != 42) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareUnsigned",
          .function_name = "CompareUnsigned",
          .original_c = R"(
s32 CompareUnsigned(u32 a, u32 b) {
    if (a < b) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareUnsignedImm",
          .function_name = "CompareUnsignedImm",
          .original_c = R"(
s32 CompareUnsignedImm(u32 a) {
    if (a < 100u) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchLogicalAnd",
          .function_name = "LogicalAnd",
          .original_c = R"(
s32 LogicalAnd(s32 a, s32 b) {
    if (a != 0 && b != 0) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchLogicalOr",
          .function_name = "LogicalOr",
          .original_c = R"(
s32 LogicalOr(s32 a, s32 b) {
    if (a != 0 || b != 0) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareLess",
          .function_name = "CompareLess",
          .original_c = R"(
s32 CompareLess(s32 a, s32 b) {
    if (a < b) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareGeImm",
          .function_name = "CompareGeImm",
          .original_c = R"(
s32 CompareGeImm(s32 a) {
    if (a >= 10) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareLeImm",
          .function_name = "CompareLeImm",
          .original_c = R"(
s32 CompareLeImm(s32 a) {
    if (a <= 10) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareGtImm",
          .function_name = "CompareGtImm",
          .original_c = R"(
s32 CompareGtImm(s32 a) {
    if (a > 10) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchNestedIf",
          .function_name = "NestedIf",
          .original_c = R"(
s32 NestedIf(s32 a, s32 b) {
    if (a > 0) {
        if (b > 0) {
            return 1;
        }
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareGeZero",
          .function_name = "CompareGeZero",
          .original_c = R"(
s32 CompareGeZero(s32 a) {
    if (a >= 0) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareLeZero",
          .function_name = "CompareLeZero",
          .original_c = R"(
s32 CompareLeZero(s32 a) {
    if (a <= 0) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchTernary",
          .function_name = "TernaryOp",
          .original_c = R"(
s32 TernaryOp(s32 cond, s32 a, s32 b) {
    return cond ? a : b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchLogicalNor",
          .function_name = "LogicalNor",
          .original_c = R"(
s32 LogicalNor(s32 a, s32 b) {
    if (!a && !b) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchCompareGreater",
          .function_name = "CompareGreater",
          .original_c = R"(
s32 CompareGreater(s32 a, s32 b) {
    if (a > b) {
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchMaskAndBranchUseRegister",
          .function_name = "MaskBranchAndReuse",
          .original_c = R"(
s32 MaskBranchAndReuse(u32 a) {
    u32 masked = a & 0xFF;
    if (masked > 10) {
        return masked + 5;
    }
    return masked;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopDoWhileCountDown",
          .function_name = "DoWhileCountDown",
          .original_c = R"(
s32 DoWhileCountDown(s32 count) {
    s32 total = 0;
    do {
        total += count;
        count--;
    } while (count > 0);
    return total;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopWhileLoop",
          .function_name = "WhileLoop",
          .original_c = R"(
s32 WhileLoop(s32 count) {
    s32 total = 0;
    while (count > 0) {
        total += count;
        count--;
    }
    return total;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopForLoopSum",
          .function_name = "ForLoopSum",
          .original_c = R"(
s32 ForLoopSum(s32 arg0) {
    s32 v1;
    s32 arg1;
    arg1 = 0;
    for (v1 = 0; (v1 < arg0); v1++) {
        arg1 = (arg1 + v1);
    }
    return arg1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopArraySum",
          .function_name = "ArraySum",
          .original_c = R"(
s32 ArraySum(s32* arr, s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopCountUp",
          .function_name = "CountUp",
          .original_c = R"(
s32 CountUp(s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; i++) {
        sum += 2;
    }
    return sum;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopWhileCountDown",
          .function_name = "WhileCountDown",
          .original_c = R"(
s32 WhileCountDown(s32 n) {
    s32 r = 0;
    while (n > 0) {
        r += n;
        n--;
    }
    return r;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopDoWhileSum",
          .function_name = "DoWhileSum",
          .original_c = R"(
s32 DoWhileSum(s32 n) {
    s32 sum = 0;
    do {
        sum += n;
        n--;
    } while (n > 0);
    return sum;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopCopyArray",
          .function_name = "CopyArray",
          .original_c = R"(
void CopyArray(s32* dst, s32* src, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopArrayScale",
          .function_name = "ArrayScale",
          .original_c = R"(
void ArrayScale(s32* arr, s32 n, s32 scale) {
    s32 i;
    for (i = 0; i < n; i++) {
        arr[i] = arr[i] + scale;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallDirectOneArg",
          .function_name = "CallDirectOneArg",
          .original_c = R"(
extern s32 CalleeOne(s32 a);

s32 CallDirectOneArg(s32 a) {
    return CalleeOne(a);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallDirectTwoArgs",
          .function_name = "CallDirectTwoArgs",
          .original_c = R"(
extern s32 CalleeTwo(s32 a, s32 b);

s32 CallDirectTwoArgs(s32 a, s32 b) {
    return CalleeTwo(a, b);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallNonTailAdd",
          .function_name = "CallNonTailAdd",
          .original_c = R"(
extern s32 CalleeOne(s32 a);

s32 CallNonTailAdd(s32 a) {
    return CalleeOne(a) + 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallPreserveArgAcrossCall",
          .function_name = "CallPreserveArgAcrossCall",
          .original_c = R"(
extern s32 CalleeOne(s32 a);

s32 CallPreserveArgAcrossCall(s32 a) {
    return CalleeOne(a) + a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallDirectThreeArgs",
          .function_name = "CallDirectThreeArgs",
          .original_c = R"(
extern s32 CalleeThree(s32 a, s32 b, s32 c);

s32 CallDirectThreeArgs(s32 a, s32 b, s32 c) {
    return CalleeThree(a, b, c);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallDirectFourArgs",
          .function_name = "CallDirectFourArgs",
          .original_c = R"(
extern s32 CalleeFour(s32 a, s32 b, s32 c, s32 d);

s32 CallDirectFourArgs(s32 a, s32 b, s32 c, s32 d) {
    return CalleeFour(a, b, c, d);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallArgAdd",
          .function_name = "CallArgAdd",
          .original_c = R"(
extern s32 CalleeOne(s32 a);

s32 CallArgAdd(s32 a) {
    return CalleeOne(a + 5);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallPassConstant",
          .function_name = "CallPassConstant",
          .original_c = R"(
extern s32 CalleeOne(s32 a);

s32 CallPassConstant(void) {
    return CalleeOne(100);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallChain",
          .function_name = "CallChain",
          .original_c = R"(
extern s32 CalleeOne(s32 a);

s32 CallChain(s32 a) {
    return CalleeOne(CalleeOne(a));
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallVoidOneArg",
          .function_name = "CallVoidOneArg",
          .original_c = R"(
extern void CalleeVoid(s32 a);

void CallVoidOneArg(s32 a) {
    CalleeVoid(a);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallPassTwoConstants",
          .function_name = "PassTwoConstants",
          .original_c = R"(
extern s32 CalleeTwo(s32 a, s32 b);

s32 PassTwoConstants(void) {
    return CalleeTwo(10, 20);
}
)",
      },
      RoundTripTestCase{
          .test_name = "StackArrayWithCalls",
          .function_name = "StackArrayWithCalls",
          .original_c = R"(
extern s32 ProcessArray(s32* arr, s32 n);

s32 StackArrayWithCalls(s32 a, s32 b, s32 c, s32 d) {
    s32 buf[4];
    buf[0] = a;
    buf[1] = b;
    buf[2] = c;
    buf[3] = d;
    return ProcessArray(buf, 4);
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalReadS32",
          .function_name = "ReadGlobalS32",
          .original_c = R"(
extern s32 gGlobalS32;

s32 ReadGlobalS32(void) {
    return gGlobalS32;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalWriteS32",
          .function_name = "WriteGlobalS32",
          .original_c = R"(
extern s32 gGlobalS32;

void WriteGlobalS32(s32 v) {
    gGlobalS32 = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalReadU16",
          .function_name = "ReadGlobalU16",
          .original_c = R"(
extern u16 gGlobalU16;

u32 ReadGlobalU16(void) {
    return gGlobalU16;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalWriteS8",
          .function_name = "WriteGlobalS8",
          .original_c = R"(
extern s8 gGlobalS8;

void WriteGlobalS8(s8 v) {
    gGlobalS8 = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalReadS8",
          .function_name = "ReadGlobalS8",
          .original_c = R"(
extern s8 gGlobalS8;

s32 ReadGlobalS8(void) {
    return gGlobalS8;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalReadS16",
          .function_name = "ReadGlobalS16",
          .original_c = R"(
extern s16 gGlobalS16;

s32 ReadGlobalS16(void) {
    return gGlobalS16;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalReadU32",
          .function_name = "ReadGlobalU32",
          .original_c = R"(
extern u32 gGlobalU32;

u32 ReadGlobalU32(void) {
    return gGlobalU32;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalWriteU16",
          .function_name = "WriteGlobalU16",
          .original_c = R"(
extern u16 gGlobalU16;

void WriteGlobalU16(u16 v) {
    gGlobalU16 = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalWriteU32",
          .function_name = "WriteGlobalU32",
          .original_c = R"(
extern u32 gGlobalU32;

void WriteGlobalU32(u32 v) {
    gGlobalU32 = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalIncrementS32",
          .function_name = "GlobalIncrementS32",
          .original_c = R"(
extern s32 gGlobalS32;

void GlobalIncrementS32(void) {
    gGlobalS32++;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalAddAssignS32",
          .function_name = "GlobalAddAssignS32",
          .original_c = R"(
extern s32 gGlobalS32;

void GlobalAddAssignS32(s32 v) {
    gGlobalS32 += v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalSubAssignS32",
          .function_name = "GlobalSubAssignS32",
          .original_c = R"(
extern s32 gGlobalS32;

void GlobalSubAssignS32(s32 v) {
    gGlobalS32 -= v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalAndAssignU32",
          .function_name = "GlobalAndAssignU32",
          .original_c = R"(
extern u32 gGlobalU32;

void GlobalAndAssignU32(u32 mask) {
    gGlobalU32 &= mask;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalOrAssignU32",
          .function_name = "GlobalOrAssignU32",
          .original_c = R"(
extern u32 gGlobalU32;

void GlobalOrAssignU32(u32 mask) {
    gGlobalU32 |= mask;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalWriteZeroS32",
          .function_name = "WriteZeroS32",
          .original_c = R"(
extern s32 gGlobalS32;

void WriteZeroS32(void) {
    gGlobalS32 = 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalReadAddArg",
          .function_name = "ReadAddArg",
          .original_c = R"(
extern s32 gGlobalS32;

s32 ReadAddArg(s32 a) {
    return gGlobalS32 + a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatAdd",
          .function_name = "AddFloat",
          .original_c = R"(
f32 AddFloat(f32 a, f32 b) {
    return a + b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatSub",
          .function_name = "SubFloat",
          .original_c = R"(
f32 SubFloat(f32 a, f32 b) {
    return a - b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatMul",
          .function_name = "MulFloat",
          .original_c = R"(
f32 MulFloat(f32 a, f32 b) {
    return a * b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatDiv",
          .function_name = "DivFloat",
          .original_c = R"(
f32 DivFloat(f32 a, f32 b) {
    return a / b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatNeg",
          .function_name = "NegFloat",
          .original_c = R"(
f32 NegFloat(f32 a) {
    return -a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleAdd",
          .function_name = "AddDouble",
          .original_c = R"(
f64 AddDouble(f64 a, f64 b) {
    return a + b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleSub",
          .function_name = "SubDouble",
          .original_c = R"(
f64 SubDouble(f64 a, f64 b) {
    return a - b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleMul",
          .function_name = "MulDouble",
          .original_c = R"(
f64 MulDouble(f64 a, f64 b) {
    return a * b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatConvertIntToFloat",
          .function_name = "ConvertIntToFloat",
          .original_c = R"(
f32 ConvertIntToFloat(s32 a) {
    return (f32)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatConvertFloatToInt",
          .function_name = "ConvertFloatToInt",
          .original_c = R"(
s32 ConvertFloatToInt(f32 a) {
    return (s32)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatConvertFloatToDouble",
          .function_name = "ConvertFloatToDouble",
          .original_c = R"(
f64 ConvertFloatToDouble(f32 a) {
    return (f64)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatConvertDoubleToFloat",
          .function_name = "ConvertDoubleToFloat",
          .original_c = R"(
f32 ConvertDoubleToFloat(f64 a) {
    return (f32)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleDiv",
          .function_name = "DivDouble",
          .original_c = R"(
f64 DivDouble(f64 a, f64 b) {
    return a / b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleNeg",
          .function_name = "NegDouble",
          .original_c = R"(
f64 NegDouble(f64 a) {
    return -a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleConvertIntToDouble",
          .function_name = "ConvertIntToDouble",
          .original_c = R"(
f64 ConvertIntToDouble(s32 a) {
    return (f64)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleConvertDoubleToInt",
          .function_name = "ConvertDoubleToInt",
          .original_c = R"(
s32 ConvertDoubleToInt(f64 a) {
    return (s32)a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatLoad",
          .function_name = "LoadFloat",
          .original_c = R"(
f32 LoadFloat(f32* ptr) {
    return *ptr;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatStore",
          .function_name = "StoreFloat",
          .original_c = R"(
void StoreFloat(f32* ptr, f32 val) {
    *ptr = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleLoad",
          .function_name = "LoadDouble",
          .original_c = R"(
f64 LoadDouble(f64* ptr) {
    return *ptr;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleStore",
          .function_name = "StoreDouble",
          .original_c = R"(
void StoreDouble(f64 val, f64* ptr) {
    *ptr = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticAddThree",
          .function_name = "AddThree",
          .original_c = R"(
s32 AddThree(s32 a, s32 b, s32 c) {
    return a + b + c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticSubThree",
          .function_name = "SubThree",
          .original_c = R"(
s32 SubThree(s32 a, s32 b, s32 c) {
    return a - b - c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticAverageTwo",
          .function_name = "AverageTwo",
          .original_c = R"(
s32 AverageTwo(s32 a, s32 b) {
    return (a + b) >> 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticAverageUnsigned",
          .function_name = "AverageUnsigned",
          .original_c = R"(
u32 AverageUnsigned(u32 a, u32 b) {
    return (a + b) >> 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticMulAdd",
          .function_name = "MulAdd",
          .original_c = R"(
s32 MulAdd(s32 a, s32 b, s32 c) {
    return a * b + c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticMulSub",
          .function_name = "MulSub",
          .original_c = R"(
s32 MulSub(s32 a, s32 b, s32 c) {
    return a * b - c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticMaskAndShift",
          .function_name = "MaskAndShift",
          .original_c = R"(
u32 MaskAndShift(u32 a) {
    return (a & 0xFF00) >> 8;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticShiftAndMask",
          .function_name = "ShiftAndMask",
          .original_c = R"(
u32 ShiftAndMask(u32 a) {
    return (a >> 4) & 0x0F;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticNand",
          .function_name = "Nand",
          .original_c = R"(
u32 Nand(u32 a, u32 b) {
    return ~(a & b);
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticUnpackHighS16",
          .function_name = "UnpackHighS16",
          .original_c = R"(
s32 UnpackHighS16(u32 packed) {
    return (s32)(packed >> 16);
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticUnpackLowS16",
          .function_name = "UnpackLowS16",
          .original_c = R"(
s32 UnpackLowS16(u32 packed) {
    return (s16)(packed & 0xFFFF);
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticRotateLeftOne",
          .function_name = "RotateLeftOne",
          .original_c = R"(
u32 RotateLeftOne(u32 a) {
    return (a << 1) | (a >> 31);
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticIsOdd",
          .function_name = "IsOdd",
          .original_c = R"(
s32 IsOdd(s32 a) {
    return a & 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticClearLowestBit",
          .function_name = "ClearLowestBit",
          .original_c = R"(
u32 ClearLowestBit(u32 a) {
    return a & (a - 1);
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayAccessIndexedOffset",
          .function_name = "IndexedOffset",
          .original_c = R"(
s32 IndexedOffset(s32* arr, s32 i) {
    return arr[i + 2];
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayWriteIndexedOffset",
          .function_name = "WriteIndexedOffset",
          .original_c = R"(
void WriteIndexedOffset(s32* arr, s32 i, s32 v) {
    arr[i + 2] = v;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayReadS8Indexed",
          .function_name = "ReadS8Indexed",
          .original_c = R"(
s32 ReadS8Indexed(s8* arr, s32 i) {
    return arr[i];
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayReadS16Indexed",
          .function_name = "ReadS16Indexed",
          .original_c = R"(
s32 ReadS16Indexed(s16* arr, s32 i) {
    return arr[i];
}
)",
      },
      RoundTripTestCase{
          .test_name = "StructPoint3DNormSq",
          .function_name = "Point3DNormSq",
          .original_c = R"(
s32 Point3DNormSq(Point3D* p) {
    return p->x * p->x + p->y * p->y + p->z * p->z;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
} Point3D;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "StructPoint3DGetZ",
          .function_name = "Point3DGetZ",
          .original_c = R"(
s32 Point3DGetZ(Point3D* p) {
    return p->z;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
} Point3D;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "StructPoint3DSet",
          .function_name = "Point3DSet",
          .original_c = R"(
void Point3DSet(Point3D* p, s32 x, s32 y, s32 z) {
    p->x = x;
    p->y = y;
    p->z = z;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
} Point3D;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "BranchIfElseAssignDiff",
          .function_name = "IfElseAssignDiff",
          .original_c = R"(
s32 IfElseAssignDiff(s32 a, s32 b) {
    s32 res;
    if (a < b) {
        res = b - a;
    } else {
        res = a - b;
    }
    return res;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchMaxThree",
          .function_name = "MaxThree",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "BranchMinThree",
          .function_name = "MinThree",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "BranchThreeWayBranch",
          .function_name = "ThreeWayBranch",
          .original_c = R"(
s32 ThreeWayBranch(s32 a) {
    if (a > 0) {
        return 1;
    }
    if (a < 0) {
        return -1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchTestBitZero",
          .function_name = "TestBitZero",
          .original_c = R"(
s32 TestBitZero(u32 val) {
    if (val & 1) {
        return 10;
    }
    return 20;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchSelectPositive",
          .function_name = "SelectPositive",
          .original_c = R"(
s32 SelectPositive(s32 a, s32 fallback) {
    if (a <= 0) {
        return fallback;
    }
    return a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchSelectZero",
          .function_name = "SelectZero",
          .original_c = R"(
s32 SelectZero(s32 a, s32 fallback) {
    if (a == 0) {
        return fallback;
    }
    return a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopSumArrayS16",
          .function_name = "SumArrayS16",
          .original_c = R"(
s32 SumArrayS16(s16* arr, s32 n) {
    s32 sum = 0;
    s32 i;
    for (i = 0; i < n; i++) {
        sum += arr[i];
    }
    return sum;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopZeroArray",
          .function_name = "ZeroArray",
          .original_c = R"(
void ZeroArray(s32* arr, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        arr[i] = 0;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopDoWhileDecrement",
          .function_name = "DoWhileDecrement",
          .original_c = R"(
s32 DoWhileDecrement(s32 val) {
    s32 count = 0;
    do {
        count++;
        val -= 2;
    } while (val > 0);
    return count;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticBitClear",
          .function_name = "BitClear",
          .original_c = R"(
u32 BitClear(u32 val, u32 mask) {
    return val & ~mask;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallTwoArgsAdd",
          .function_name = "CallTwoArgsAdd",
          .original_c = R"(
extern s32 CalleeTwo(s32 a, s32 b);

s32 CallTwoArgsAdd(s32 a, s32 b) {
    return CalleeTwo(a + 1, b + 2);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallWithGlobal",
          .function_name = "CallWithGlobal",
          .original_c = R"(
extern s32 gGlobalS32;
extern s32 CalleeOne(s32 a);

s32 CallWithGlobal(void) {
    return CalleeOne(gGlobalS32);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallSetGlobal",
          .function_name = "CallSetGlobal",
          .original_c = R"(
extern s32 gGlobalS32;
extern s32 CalleeOne(s32 a);

void CallSetGlobal(s32 a) {
    gGlobalS32 = CalleeOne(a);
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallAddConstant",
          .function_name = "CallAddConstant",
          .original_c = R"(
extern s32 CalleeOne(s32 a);

s32 CallAddConstant(s32 a) {
    return CalleeOne(a) + 10;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalConditionalIncrement",
          .function_name = "GlobalConditionalIncrement",
          .original_c = R"(
extern s32 gGlobalS32;

void GlobalConditionalIncrement(s32 cond) {
    if (cond) {
        gGlobalS32++;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayRead",
          .function_name = "GlobalArrayRead",
          .original_c = R"(
extern s32 gGlobalArray[4];

s32 GlobalArrayRead(s32 idx) {
    return gGlobalArray[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayWrite",
          .function_name = "GlobalArrayWrite",
          .original_c = R"(
extern s32 gGlobalArray[4];

void GlobalArrayWrite(s32 idx, s32 val) {
    gGlobalArray[idx] = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatScale",
          .function_name = "FloatScale",
          .original_c = R"(
f32 FloatScale(f32 a, f32 scale) {
    return a * scale;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleScale",
          .function_name = "DoubleScale",
          .original_c = R"(
f64 DoubleScale(f64 a, f64 scale) {
    return a * scale;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleMulAdd",
          .function_name = "DoubleMulAdd",
          .original_c = R"(
f64 DoubleMulAdd(f64 a, f64 b, f64 c) {
    return a * b + c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleMulSub",
          .function_name = "DoubleMulSub",
          .original_c = R"(
f64 DoubleMulSub(f64 a, f64 b, f64 c) {
    return a * b - c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatAbs",
          .function_name = "FloatAbs",
          .original_c = R"(
f32 FloatAbs(f32 a) {
    if (a < 0.0f) {
        return -a;
    }
    return a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerChainedSelfAssign",
          .function_name = "ChainedSelfAssign",
          .original_c = R"(
void ChainedSelfAssign(s32 a, s32 b, s32 c) {
    s32 v0;
    v0 = (a | b);
    v0 = (v0 | 0xA0000000);
    *(s32*)v0 = c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "WriteScaledFloats",
          .function_name = "WriteScaledFloats",
          .original_c = R"(
void WriteScaledFloats(f32* out, f32 a, f32 b, f32 c) {
    out[0] = a + a;
    out[1] = b + b;
    out[2] = c + c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ScaleAndOffsetFloat",
          .function_name = "ScaleAndOffsetFloat",
          .original_c = R"(
f32 ScaleAndOffsetFloat(s32 mode, f32 x, f32 scale, f32 bias) {
    return (x * scale) + bias;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatToUintSimple",
          .function_name = "FloatToUintSimple",
          .original_c = R"(
u32 FloatToUintSimple(f32 val) {
    return (u32)val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleToUintSimple",
          .function_name = "DoubleToUintSimple",
          .original_c = R"(
u32 DoubleToUintSimple(f64 val) {
    return (u32)val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ConvertFloatAndDoubleToUint",
          .function_name = "ConvertFloatAndDoubleToUint",
          .original_c = R"(
void ConvertFloatAndDoubleToUint(f32 a, f64 b, u32* out) {
    out[0] = (u32)a;
    out[1] = (u32)b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ConditionalFloatToUint",
          .function_name = "ConditionalFloatToUint",
          .original_c = R"(
u32 ConditionalFloatToUint(f32 val, s32 cond) {
    u32 res = 0;
    if (cond) {
        res = (u32)val;
    }
    return res + 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SwitchWithSharedTail",
          .function_name = "SwitchWithSharedTail",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SwitchDecisionTreeFourCases",
          .function_name = "SwitchDecisionTreeFourCases",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayScaleMul5",
          .function_name = "ArrayScaleMul5",
          .original_c = R"(
void ArrayScaleMul5(s32* arr, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        arr[i] = arr[i] * 5;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayScaleMul9",
          .function_name = "ArrayScaleMul9",
          .original_c = R"(
void ArrayScaleMul9(s32* arr, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        arr[i] = arr[i] * 9;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayTransformMul5",
          .function_name = "ArrayTransformMul5",
          .original_c = R"(
void ArrayTransformMul5(s32* dst, s32* src, s32 n) {
    s32 i;
    for (i = 0; i < n; i++) {
        dst[i] = src[i] * 5;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkUnrolled4",
          .function_name = "PointerWalkUnrolled4",
          .original_c = R"(
void PointerWalkUnrolled4(s32* dst, s32* src, s32 count) {
    while (count > 0) {
        *dst++ = *src++ * 5;
        *dst++ = *src++ * 9;
        *dst++ = *src++ * 20;
        *dst++ = *src++ * 68;
        count--;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkUnrolled8",
          .function_name = "PointerWalkUnrolled8",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScaleAdd",
          .function_name = "PointerWalkScaleAdd",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScaleSub",
          .function_name = "PointerWalkScaleSub",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScaleBitwise",
          .function_name = "PointerWalkScaleBitwise",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkInPlaceScale6",
          .function_name = "PointerWalkInPlaceScale6",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ShiftAddMul5And9",
          .function_name = "ShiftAddMul5And9",
          .original_c = R"(
s32 ShiftAddMul5And9(s32 x, s32 y) {
    s32 a = x * 5;
    s32 b = y * 9;
    return a + b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ShiftAddMul20And68",
          .function_name = "ShiftAddMul20And68",
          .original_c = R"(
s32 ShiftAddMul20And68(s32 x, s32 y) {
    s32 a = x * 20;
    s32 b = y * 68;
    return a - b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ShiftAddMul7And3",
          .function_name = "ShiftAddMul7And3",
          .original_c = R"(
s32 ShiftAddMul7And3(s32 x, s32 y) {
    return (x * 7) + (y * 3);
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScale8",
          .function_name = "PointerWalkScale8",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScale10",
          .function_name = "PointerWalkScale10",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScale12",
          .function_name = "PointerWalkScale12",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScale16",
          .function_name = "PointerWalkScale16",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkInPlaceScale8",
          .function_name = "PointerWalkInPlaceScale8",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkInPlaceScale12",
          .function_name = "PointerWalkInPlaceScale12",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScale14",
          .function_name = "PointerWalkScale14",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkInPlaceScale10",
          .function_name = "PointerWalkInPlaceScale10",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkS16Scale6",
          .function_name = "PointerWalkS16Scale6",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkS16Scale8",
          .function_name = "PointerWalkS16Scale8",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScaleShift6",
          .function_name = "PointerWalkScaleShift6",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PointerWalkScaleShift8",
          .function_name = "PointerWalkScaleShift8",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "TransformArrayWithClamp",
          .function_name = "TransformArrayWithClamp",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "Vector3ProcessVertices",
          .function_name = "Vector3ProcessVertices",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "NestedLoopSum",
          .function_name = "NestedLoopSum",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "NestedLoopMatrixClamp",
          .function_name = "NestedLoopMatrixClamp",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PipelineProcess",
          .function_name = "PipelineProcess",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ArrayClampValues",
          .function_name = "ArrayClampValues",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LinearSearchFirstNegative",
          .function_name = "LinearSearchFirstNegative",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LinearSearchFirstMatchingTarget",
          .function_name = "LinearSearchFirstMatchingTarget",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ComputeChecksum32",
          .function_name = "ComputeChecksum32",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SubwordSpillSlotReadback",
          .function_name = "SubwordSpillSlotReadback",
          .original_c = R"(
void SubwordSpillSlotReadback(s32 flag, s32 value, u16* destination_one, u16* destination_two) {
    if (flag & 1) {
        *destination_one = (u16)value;
    }
    if (flag & 2) {
        *destination_two = (u16)value;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopWithUnsigned8BitCounter",
          .function_name = "LoopWithUnsigned8BitCounter",
          .original_c = R"(
u32 LoopWithUnsigned8BitCounter(const u32* array) {
    u8 counter = 0;
    u32 accumulator = 0;
    while (counter < 8) {
        accumulator += array[counter];
        counter++;
    }
    return accumulator;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoWhileLoopMultiStatementLatch",
          .function_name = "DoWhileLoopMultiStatementLatch",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "FloatToUintPrecededByFloatComputation",
          .function_name = "FloatToUintPrecededByFloatComputation",
          .original_c = R"(
u32 FloatToUintPrecededByFloatComputation(f32 first_float, f32 second_float, f32 scale_factor) {
    f32 computed_float = (first_float + second_float) * scale_factor;
    return (u32)computed_float;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SwitchCasesSharedIntermediateBeforeLatch",
          .function_name = "SwitchCasesSharedIntermediateBeforeLatch",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SixParameterCalleeWithStackArgs",
          .function_name = "SixParameterCalleeWithStackArgs",
          .original_c = R"(
s32 SixParameterCalleeWithStackArgs(s32 a, s32 b, s32 c, s32 d, s32 e, s32 f) {
    return (a + b) - (c + d) + (e * f);
}
)",
      },
      RoundTripTestCase{
          .test_name = "BitfieldShiftMaskAndBranch",
          .function_name = "BitfieldShiftMaskAndBranch",
          .original_c = R"(
s32 BitfieldShiftMaskAndBranch(const u16* flags_ptr, s32 fallback) {
    u16 raw = flags_ptr[2];
    u32 mode = (raw >> 3) & 3;
    if (mode == 1) {
        return (raw & 7) + 10;
    }
    return fallback;
}
)",
      },
      RoundTripTestCase{
          .test_name = "MultiCallConditionAndReturnFolding",
          .function_name = "MultiCallConditionAndReturnFolding",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "CallerPassingSixArgumentsOnStack",
          .function_name = "CallerPassingSixArgumentsOnStack",
          .original_c = R"(
extern s32 CombineSixValues(s32 a, s32 b, s32 c, s32 d, s32 e, s32 f);

s32 CallerPassingSixArgumentsOnStack(s32 x, s32 y) {
    return CombineSixValues(x, y, x + 1, y + 2, x + 3, y + 4);
}
)",
      },
      RoundTripTestCase{
          .test_name = "MultiCalleeSavedLoopAndAccumulate",
          .function_name = "MultiCalleeSavedLoopAndAccumulate",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "CalleeSavedInitializedBeforeMultipleCalls",
          .function_name = "CalleeSavedInitializedBeforeMultipleCalls",
          .original_c = R"(
extern s32 ComputeStep(s32 x);

s32 CalleeSavedInitializedBeforeMultipleCalls(s32 a, s32 b) {
    s32 first = a + 5;
    s32 second = b + 10;
    s32 r1 = ComputeStep(first + second);
    return r1 + first + second;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SwitchOnLoadedByteWithNonZeroLowerBound",
          .function_name = "SwitchOnLoadedByteWithNonZeroLowerBound",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MultiArgCallWithLoadedStructFields",
          .function_name = "MultiArgCallWithLoadedStructFields",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SignExtendedShortCallReturnInBranch",
          .function_name = "SignExtendedShortCallReturnInBranch",
          .original_c = R"(
extern s16 QueryShortStatus(s32 channel);
extern void NotifyComplete(s32 channel);

s32 SignExtendedShortCallReturnInBranch(s32 channel) {
    if (QueryShortStatus(channel) == 1) {
        NotifyComplete(channel);
        return 1;
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SequentialIfsReusingLoadedFieldAndCalls",
          .function_name = "SequentialIfsReusingLoadedFieldAndCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "StoreThroughIndexedStructPointerArray",
          .function_name = "StoreThroughIndexedStructPointerArray",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "FloatIntConvertWithInterleavedStores",
          .function_name = "FloatIntConvertWithInterleavedStores",
          .original_c = R"(
s32 FloatIntConvertWithInterleavedStores(s32 count, f32 scale, s32* out_meta, f32* out_float) {
    f32 fcount;
    f32 scaled;
    fcount = (f32)count;
    *out_meta = count + 1;
    scaled = fcount * scale;
    *out_float = scaled;
    return (s32)scaled;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatParametersPreservedAcrossCall",
          .function_name = "FloatParametersPreservedAcrossCall",
          .original_c = R"(
extern void NotifyMatrixUpdated(s32* out_flag);

f32 FloatParametersPreservedAcrossCall(s32* out_flag, f32 a, f32 b, f32 c) {
    f32 prod = a * b;
    NotifyMatrixUpdated(out_flag);
    return prod + c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CalleeSavedFloatPassedInGprAndStackAcrossCalls",
          .function_name = "CalleeSavedFloatPassedInGprAndStackAcrossCalls",
          .original_c = R"(
extern void ApplyVectorTransform(s32 slot, f32 x, f32 y, f32 z, f32 w);

void CalleeSavedFloatPassedInGprAndStackAcrossCalls(s32 slot, f32* vec) {
    f32 x = vec[0];
    f32 y = vec[1];
    ApplyVectorTransform(slot, x, y, x, y);
    ApplyVectorTransform(slot + 1, y, x, y, x);
}
)",
      },
      RoundTripTestCase{
          .test_name = "RepeatedSameArgPassedToMultiArgCall",
          .function_name = "RepeatedSameArgPassedToMultiArgCall",
          .original_c = R"(
extern s32 DispatchQuad(s32 cmd, s32 a, s32 b, s32 c);

s32 RepeatedSameArgPassedToMultiArgCall(s32 val) {
    return DispatchQuad(101, val, val, val);
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalScalarHalfwordAndByteUpdatesAroundCall",
          .function_name = "GlobalScalarHalfwordAndByteUpdatesAroundCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayReadU16",
          .function_name = "GlobalArrayReadU16",
          .original_c = R"(
extern u16 g_table_u16[16];

u16 GlobalArrayReadU16(s32 idx) {
    return g_table_u16[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayWriteU16",
          .function_name = "GlobalArrayWriteU16",
          .original_c = R"(
extern u16 g_table_u16[16];

void GlobalArrayWriteU16(s32 idx, u16 val) {
    g_table_u16[idx] = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayReadU8",
          .function_name = "GlobalArrayReadU8",
          .original_c = R"(
extern u8 g_table_u8[32];

u8 GlobalArrayReadU8(s32 idx) {
    return g_table_u8[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayWriteU8",
          .function_name = "GlobalArrayWriteU8",
          .original_c = R"(
extern u8 g_table_u8[32];

void GlobalArrayWriteU8(s32 idx, u8 val) {
    g_table_u8[idx] = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayReadS16",
          .function_name = "GlobalArrayReadS16",
          .original_c = R"(
extern s16 g_table_s16[16];

s16 GlobalArrayReadS16(s32 idx) {
    return g_table_s16[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayReadS8",
          .function_name = "GlobalArrayReadS8",
          .original_c = R"(
extern s8 g_table_s8[32];

s8 GlobalArrayReadS8(s32 idx) {
    return g_table_s8[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayReadFloat",
          .function_name = "GlobalArrayReadFloat",
          .original_c = R"(
extern f32 g_table_f32[16];

f32 GlobalArrayReadFloat(s32 idx) {
    return g_table_f32[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayWriteFloat",
          .function_name = "GlobalArrayWriteFloat",
          .original_c = R"(
extern f32 g_table_f32[16];

void GlobalArrayWriteFloat(s32 idx, f32 val) {
    g_table_f32[idx] = val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayReadModifyWriteAtIndex",
          .function_name = "GlobalArrayReadModifyWriteAtIndex",
          .original_c = R"(
extern s32 g_counters[16];

s32 GlobalArrayReadModifyWriteAtIndex(s32 idx, s32 delta) {
    g_counters[idx] += delta;
    return g_counters[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalPointerDereferenceLoadAndStore",
          .function_name = "GlobalPointerDereferenceLoadAndStore",
          .original_c = R"(
extern s32* g_active_buffer;

s32 GlobalPointerDereferenceLoadAndStore(s32 idx, s32 val) {
    s32 prev = g_active_buffer[idx];
    g_active_buffer[idx] = prev + val;
    return prev;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalArrayConditionalUpdateAroundCall",
          .function_name = "GlobalArrayConditionalUpdateAroundCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "GlobalParallelArraysReadAndUpdate",
          .function_name = "GlobalParallelArraysReadAndUpdate",
          .original_c = R"(
extern s32 g_slot_ids[8];
extern s32 g_slot_states[8];
extern s32 g_slot_timers[8];

s32 GlobalParallelArraysReadAndUpdate(s32 idx, s32 new_state) {
    g_slot_states[idx] = new_state;
    g_slot_timers[idx] = 0;
    return g_slot_ids[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "LinkedListSortedInsertVoid",
          .function_name = "LinkedListSortedInsertVoid",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 pri;
    struct ListNode* head_ref;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "LinkedListPopFrontReturnNode",
          .function_name = "LinkedListPopFrontReturnNode",
          .original_c = R"(
ListNode* LinkedListPopFrontReturnNode(ListNode** head_ptr) {
    ListNode* node = *head_ptr;
    *head_ptr = node->next;
    return node;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 value;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "LinkedListPushFrontVoid",
          .function_name = "LinkedListPushFrontVoid",
          .original_c = R"(
void LinkedListPushFrontVoid(ListNode** head_ptr, ListNode* node) {
    node->next = *head_ptr;
    *head_ptr = node;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 value;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "LinkedListSumValuesWhileNotNull",
          .function_name = "LinkedListSumValuesWhileNotNull",
          .original_c = R"(
s32 LinkedListSumValuesWhileNotNull(ListNode* curr) {
    s32 sum = 0;
    while (curr != 0) {
        sum += curr->value;
        curr = curr->next;
    }
    return sum;
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 value;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "LinkedListFindNodeByKey",
          .function_name = "LinkedListFindNodeByKey",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 key;
    s32 value;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "LinkedListClearFlagsVoidLoop",
          .function_name = "LinkedListClearFlagsVoidLoop",
          .original_c = R"(
void LinkedListClearFlagsVoidLoop(ListNode* curr) {
    while (curr != 0) {
        curr->flags = 0;
        curr = curr->next;
    }
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 flags;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "PointerLoopUnpackAndScaleToFloatVoid",
          .function_name = "PointerLoopUnpackAndScaleToFloatVoid",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "StackBufferTwoCallsWithThreeFloatGprParamsAndThreeStackParams",
          .function_name = "StackBufferTwoCallsWithThreeFloatGprParamsAndThreeStackParams",
          .original_c = R"(
extern void UnpackBuffer(f32* dst, s32* src);
extern void ProcessUnpacked(f32* buf, f32 x, f32 y, f32 z, s32 a, s32 b, s32 c);

void StackBufferTwoCallsWithThreeFloatGprParamsAndThreeStackParams(
    s32* src, f32 x, f32 y, f32 z, s32 a, s32 b, s32 c) {
    f32 buf[16];
    UnpackBuffer(buf, src);
    ProcessUnpacked(buf, x, y, z, a, b, c);
}
)",
      },
      RoundTripTestCase{
          .test_name = "PreserveTwoFloatGprParamsAcrossCall",
          .function_name = "PreserveTwoFloatGprParamsAcrossCall",
          .original_c = R"(
extern s32 PrepareTarget(s32 id);
extern void ApplyFloatPair(s32 handle, f32 x, f32 y, s32 extra);

void PreserveTwoFloatGprParamsAcrossCall(s32 id, f32 x, f32 y, s32 extra) {
    s32 handle = PrepareTarget(id);
    ApplyFloatPair(handle, x, y, extra);
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerArrayCopyWithStrideVoidLoop",
          .function_name = "PointerArrayCopyWithStrideVoidLoop",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LinkedListCountMatchingNodes",
          .function_name = "LinkedListCountMatchingNodes",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 type;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "LinkedListInvokeCallbackPerNode",
          .function_name = "LinkedListInvokeCallbackPerNode",
          .original_c = R"(
extern void OnVisitNode(s32 id, s32 ctx);

void LinkedListInvokeCallbackPerNode(ListNode* curr, s32 ctx) {
    while (curr != 0) {
        OnVisitNode(curr->id, ctx);
        curr = curr->next;
    }
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 id;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "LeafFunctionSavingCalleeSavedAtSpZero",
          .function_name = "LeafFunctionSavingCalleeSavedAtSpZero",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "StackLocalBufferWithMixedThreeCallSizes",
          .function_name = "StackLocalBufferWithMixedThreeCallSizes",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "CallWithSevenConstantAndStackArgsInsideBranch",
          .function_name = "CallWithSevenConstantAndStackArgsInsideBranch",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "CallWithSignExtendedHalfwordFifthArgInsideLoop",
          .function_name = "CallWithSignExtendedHalfwordFifthArgInsideLoop",
          .original_c = R"(
extern void SubmitRectFiveArgs(s32 handle, s32 x, s32 y, s32 w, s32 h);

void CallWithSignExtendedHalfwordFifthArgInsideLoop(s32 handle, s16* coords, s16* end) {
    while (coords != end) {
        s32 h = (s16)(coords[3] + 6);
        SubmitRectFiveArgs(handle, (s16)(coords[0] - 6), (s16)(coords[1] - 6), (s16)(coords[2] + 6), h);
        coords += 4;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatParamsAfterTwoPointerParamsWithCalls",
          .function_name = "FloatParamsAfterTwoPointerParamsWithCalls",
          .original_c = R"(
extern void ApplyForce(s32* dst, f32 fx, f32 fy);

void FloatParamsAfterTwoPointerParamsWithCalls(s32* dst, s32* src, f32 scale_x, f32 scale_y) {
    f32 x = (f32)src[0] * scale_x;
    f32 y = (f32)src[1] * scale_y;
    ApplyForce(dst, x, y);
}
)",
      },
      RoundTripTestCase{
          .test_name = "UnsignedByteLoadsConvertedToFloatStackArgs",
          .function_name = "UnsignedByteLoadsConvertedToFloatStackArgs",
          .original_c = R"(
extern void ComputePlane(s32* out_plane, f32 x0, f32 y0, f32 z0, f32 x1, f32 y1, f32 z1, f32 x2, f32 y2, f32 z2);

void UnsignedByteLoadsConvertedToFloatStackArgs(s32* out_plane, u8* pts) {
    ComputePlane(out_plane, (f32)pts[1], (f32)pts[2], (f32)pts[3], (f32)pts[4], (f32)pts[5], (f32)pts[6], (f32)pts[7], (f32)pts[8], (f32)pts[9]);
}
)",
      },
      RoundTripTestCase{
          .test_name = "VoidFunctionUpdatingPointerHeadWithoutReturn",
          .function_name = "VoidFunctionUpdatingPointerHeadWithoutReturn",
          .original_c = R"(
void VoidFunctionUpdatingPointerHeadWithoutReturn(ListNode** head) {
    ListNode* first = *head;
    if (first != 0) {
        *head = first->next;
    }
}
)",
          .options = {
              .extra_types_header = R"(
typedef struct ListNode {
    struct ListNode* next;
    s32 val;
} ListNode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "MultipleCallsWithFiveStackArgsInSwitchCases",
          .function_name = "MultipleCallsWithFiveStackArgsInSwitchCases",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "PreservePointerAcrossTwoVoidCallsBeforeStore",
          .function_name = "PreservePointerAcrossTwoVoidCallsBeforeStore",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
typedef struct EntityState {
    s32 flags;
    s32 timer;
    s32 mode;
} EntityState;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "SixParametersWithFourFloatStackAndRegisterArgs",
          .function_name = "SixParametersWithFourFloatStackAndRegisterArgs",
          .original_c = R"(
extern void WriteVec3(f32* dst, f32 x, f32 y, f32 z);

void SixParametersWithFourFloatStackAndRegisterArgs(f32* dst, s32 mode, f32 x0, f32 y0, f32 x1, f32 y1) {
    f32 dx = x1 - x0;
    f32 dy = y1 - y0;
    if (mode != 0) {
        WriteVec3(dst, dx, dy, dx + dy);
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallResultPassedToSecondCallAndStoredToGlobal",
          .function_name = "CallResultPassedToSecondCallAndStoredToGlobal",
          .original_c = R"(
extern s32 AllocateResource(s32 size);
extern void InitializeResource(s32 handle, s32 flags);

s32 CallResultPassedToSecondCallAndStoredToGlobal(s32 size, s32 flags) {
    s32 h = AllocateResource(size);
    gLastHandle = h;
    InitializeResource(h, flags);
    return h;
}
)",
          .options = {
              .extra_types_header = R"(
extern s32 gLastHandle;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "LoopCallingValidatorAndUpdatingStructFields",
          .function_name = "LoopCallingValidatorAndUpdatingStructFields",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
typedef struct SlotEntry {
    s32 active;
    s32 id;
    s32 score;
    s32 bonus;
} SlotEntry;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "SignedDivisionByHundredAndAccumulateFloat",
          .function_name = "SignedDivisionByHundredAndAccumulateFloat",
          .original_c = R"(
f32 SignedDivisionByHundredAndAccumulateFloat(const u8* values, s32 idx) {
    s32 a = (values[idx] * 25) / 100;
    s32 b = (values[idx + 7] * 25) / 100;
    return (f32)a + (f32)b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "NestedIfElseChainWithCallsAndSharedCleanup",
          .function_name = "NestedIfElseChainWithCallsAndSharedCleanup",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
extern s32 gCurrentPhase;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "MultipleStructPointerStoresAfterHelperCall",
          .function_name = "MultipleStructPointerStoresAfterHelperCall",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
typedef struct ActorTransform {
    s32 x;
    s32 y;
    s32 z;
    s32 vx;
    s32 vy;
    s32 vz;
} ActorTransform;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "FourFloatStackArgumentsToMatrixHelper",
          .function_name = "FourFloatStackArgumentsToMatrixHelper",
          .original_c = R"(
extern void BuildTransformEightArgs(s32* mtx, s32 flags, f32 rx, f32 ry, f32 rz, f32 tx, f32 ty, f32 tz);

void FourFloatStackArgumentsToMatrixHelper(s32* mtx, const f32* params, s32 flags) {
    BuildTransformEightArgs(mtx, flags, params[0], params[1], params[2], params[3], params[4], params[5]);
}
)",
      },
      RoundTripTestCase{
          .test_name = "NonZeroLoFloatConstantScaling",
          .function_name = "NonZeroLoFloatConstantScaling",
          .original_c = R"(
f32 NonZeroLoFloatConstantScaling(f32 x, f32 y) {
    return x * 0.2f + y * 0.1f;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatTruncationStoredDirectlyToMemory",
          .function_name = "FloatTruncationStoredDirectlyToMemory",
          .original_c = R"(
void FloatTruncationStoredDirectlyToMemory(s32* out, f32 a, f32 b, f32 c) {
    out[0] = (s32)(a * b + c);
    out[1] = (s32)(a - b);
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatStackParametersUsedInArithmeticAndStore",
          .function_name = "FloatStackParametersUsedInArithmeticAndStore",
          .original_c = R"(
void FloatStackParametersUsedInArithmeticAndStore(
    f32* out, f32 a, f32 b, f32 c, f32 d, f32 e, f32 f) {
    out[0] = a + d;
    out[1] = b + e;
    out[2] = c + f;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatStackParametersPreservedAcrossHelperCall",
          .function_name = "FloatStackParametersPreservedAcrossHelperCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "FloatGprParamsWithFirstFloatRegReusedForStatusReturn",
          .function_name = "FloatGprParamsWithFirstFloatRegReusedForStatusReturn",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "FloatGprParamsIncrementalUpdateWithStatusReturn",
          .function_name = "FloatGprParamsIncrementalUpdateWithStatusReturn",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
typedef struct VecActor {
    u16 flags;
    f32 x;
    f32 y;
    f32 z;
} VecActor;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "GlobalStoreBeforeFloatArgumentForwardingCall",
          .function_name = "GlobalStoreBeforeFloatArgumentForwardingCall",
          .original_c = R"(
extern u8 DispatchUniformScale(u16 id, f32 sx, f32 sy, f32 sz);

u8 GlobalStoreBeforeFloatArgumentForwardingCall(f32 scale) {
    gActiveMode = 2;
    return DispatchUniformScale(91, scale, scale, scale);
}
)",
          .options = {
              .extra_types_header = R"(
extern u8 gActiveMode;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "SixFloatStackParametersToMatrixInit",
          .function_name = "SixFloatStackParametersToMatrixInit",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "CalleeSavedFloatCarriersAcrossTwoSqrtCalls",
          .function_name = "CalleeSavedFloatCarriersAcrossTwoSqrtCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ContinuedFractionIntToFloatLoopSinglePrecision",
          .function_name = "ContinuedFractionIntToFloatLoopSinglePrecision",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ContinuedFractionIntToFloatLoopDoublePrecision",
          .function_name = "ContinuedFractionIntToFloatLoopDoublePrecision",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "DoublePrecisionConstantOffsetAdjustmentFromFloat",
          .function_name = "DoublePrecisionConstantOffsetAdjustmentFromFloat",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
extern f64 gHalfPiDouble;
extern f64 gPiDouble;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "ConditionalMultiFloatStackArgHelperCalls",
          .function_name = "ConditionalMultiFloatStackArgHelperCalls",
          .original_c = R"(
extern void SetupPerspective(s32* mtx, u16* norm, f32 fovy, f32 aspect, f32 near_p, f32 far_p, f32 scale);
extern void SetupLookAt(s32* mtx, f32 ex, f32 ey, f32 ez, f32 ax, f32 ay, f32 az);

void ConditionalMultiFloatStackArgHelperCalls(CameraRig* rig, s32* mtx, u16* norm) {
    if (rig->mode == 1) {
        SetupPerspective(mtx, norm, rig->fovy, rig->aspect, rig->near_p, rig->far_p, 1.0f);
    }
    SetupLookAt(mtx, rig->eye_x, rig->eye_y, rig->eye_z, rig->at_x, rig->at_y, rig->at_z);
}
)",
          .options = {
              .extra_types_header = R"(
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
)",
          },
      },
      RoundTripTestCase{
          .test_name = "PreserveFloatGprAndStackParamsInCalleeSavedAcrossCalls",
          .function_name = "PreserveFloatGprAndStackParamsInCalleeSavedAcrossCalls",
          .original_c = R"(
extern f32 EvalBasis(f32 u, f32 v);

void PreserveFloatGprAndStackParamsInCalleeSavedAcrossCalls(
    f32* out, f32 a, f32 b, f32 c, f32 d, f32 e) {
    f32 r0 = EvalBasis(a, b);
    f32 r1 = EvalBasis(c, d);
    out[0] = r0 + a * e;
    out[1] = r1 + b * d;
    out[2] = c + e;
}
)",
      },
      RoundTripTestCase{
          .test_name = "MultiStepShiftAddSubConstantMultiplications",
          .function_name = "MultiStepShiftAddSubConstantMultiplications",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "UnsignedConstantDivisionsAndModulos",
          .function_name = "UnsignedConstantDivisionsAndModulos",
          .original_c = R"(
void UnsignedConstantDivisionsAndModulos(u32* out, u32 a, u32 b) {
    out[0] = a / 255u;
    out[1] = b % 10u;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SignedConstantDivisionsAndModulos",
          .function_name = "SignedConstantDivisionsAndModulos",
          .original_c = R"(
void SignedConstantDivisionsAndModulos(s32* out, s32 a, s32 b) {
    out[0] = a / 100;
    out[1] = b % 6;
}
)",
      },
      RoundTripTestCase{
          .test_name = "UnsignedScaleDivideAndFloatAccumulate",
          .function_name = "UnsignedScaleDivideAndFloatAccumulate",
          .original_c = R"(
f32 UnsignedScaleDivideAndFloatAccumulate(u8* tbl, s32 idx, f32 base) {
    u32 d0 = (u32)(tbl[idx] * 100) / 255u;
    return base + (f32)d0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "UnalignedStructBlock8Copy",
          .function_name = "UnalignedStructBlock8Copy",
          .original_c = R"(
void UnalignedStructBlock8Copy(UnalignedBlock8* dst, UnalignedBlock8* src) {
    *dst = *src;
}
)",
      },
      RoundTripTestCase{
          .test_name = "UnalignedStructBlock10And20Copy",
          .function_name = "UnalignedStructBlock10And20Copy",
          .original_c = R"(
typedef struct { u8 b[10]; } CustomBlock10;
typedef struct { u8 b[20]; } CustomBlock20;
void UnalignedStructBlock10And20Copy(CustomBlock10* dst10, CustomBlock10* src10,
                                     CustomBlock20* dst20, CustomBlock20* src20) {
    *dst10 = *src10;
    *dst20 = *src20;
}
)",
      },
      RoundTripTestCase{
          .test_name = "UnalignedStructBlock16CopyLoop",
          .function_name = "UnalignedStructBlock16CopyLoop",
          .original_c = R"(
void UnalignedStructBlock16CopyLoop(UnalignedBlock16* dst, UnalignedBlock16* src, s32 count) {
    s32 i = 0;
    do {
        *dst = *src;
        src++;
        dst++;
        i++;
    } while (i < count);
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatStackArgsWithGlobalHalfwordStore",
          .function_name = "FloatStackArgsWithGlobalHalfwordStore",
          .original_c = R"(
extern u16 D_800E7A0E;
void sink10(s32 a0, s32 a1, s32 a2, s32 a3, f32 f4, f32 f5, f32 f6, f32 f7, s32 s8, s32 s9);
void FloatStackArgsWithGlobalHalfwordStore(s32 id, f32* p) {
    D_800E7A0E = (u16)id;
    sink10(id, 0, 1, 2, p[0], p[1], p[2], p[3], 10, 20);
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatStackArgsWithInterleavedGlobalStore",
          .function_name = "FloatStackArgsWithInterleavedGlobalStore",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ManyStackArgsFifteenParamCall",
          .function_name = "ManyStackArgsFifteenParamCall",
          .original_c = R"(
s32 sink15(s32 a0, s32 a1, s32 a2, s32 a3,
           f32 f4, f32 f5, f32 f6, f32 f7, f32 f8, f32 f9, f32 f10, f32 f11,
           s32 s12, s32 s13, s32 s14);
s32 ManyStackArgsFifteenParamCall(s32 a0, f32* p, s32 extra) {
    return sink15(a0, 1, 2, 3, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], extra, 10, 20);
}
)",
      },
      RoundTripTestCase{
          .test_name = "PassFirstParamAddressToCallee",
          .function_name = "PassFirstParamAddressToCallee",
          .original_c = R"(
s32 ext_take_ptr(s32* p);
s32 PassFirstParamAddressToCallee(s32 a, s32 b) {
    return ext_take_ptr(&a) + b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "PassSecondParamAddressAndReadModified",
          .function_name = "PassSecondParamAddressAndReadModified",
          .original_c = R"(
void ext_init_second(s32 a, s32* b_out);
s32 PassSecondParamAddressAndReadModified(s32 a, s32 b) {
    ext_init_second(a, &b);
    return b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "PassTwoParamAddressesToCallee",
          .function_name = "PassTwoParamAddressesToCallee",
          .original_c = R"(
s32 ext_swap_or_add(s32* a, s32* b);
s32 PassTwoParamAddressesToCallee(s32 a, s32 b) {
    ext_swap_or_add(&a, &b);
    return a + b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatStackArgsConditionalDispatch",
          .function_name = "FloatStackArgsConditionalDispatch",
          .original_c = R"(
void sink9a(s32 a0, s32 a1, s32 a2, s32 a3, f32 f4, f32 f5, f32 f6, f32 f7, s32 s8);
void sink9b(s32 a0, s32 a1, s32 a2, s32 a3, f32 f4, f32 f5, f32 f6, f32 f7, s32 s8);
void FloatStackArgsConditionalDispatch(s32 id, f32* p, s32 flag) {
    if (flag != 0) {
        sink9a(id, 1, 2, 3, p[0], p[1], p[2], p[3], flag);
    } else {
        sink9b(id, 4, 5, 6, p[4], p[5], p[6], p[7], 0);
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "NineCalleeSavedRegistersAcrossMultipleCalls",
          .function_name = "NineCalleeSavedRegistersAcrossMultipleCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LoopWithNineLiveCalleeSavedValuesAndCall",
          .function_name = "LoopWithNineLiveCalleeSavedValuesAndCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SignedDivisionBy184InLoopWithCalls",
          .function_name = "SignedDivisionBy184InLoopWithCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SignedDivisionWithBranchAndCall",
          .function_name = "SignedDivisionWithBranchAndCall",
          .original_c = R"(
s32 use_quotient(s32 q);

s32 SignedDivisionWithBranchAndCall(s32 x, s32 y) {
    s32 q = y / 100;
    if (q > x) {
        return use_quotient(q);
    }
    return q + y;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SignedDivisionBy60And3600StoreChain",
          .function_name = "SignedDivisionBy60And3600StoreChain",
          .original_c = R"(
void SignedDivisionBy60And3600StoreChain(s32 ticks, s32* out) {
    s32 min = ticks / 3600;
    s32 sec;
    out[0] = min;
    sec = ticks / 60;
    out[1] = sec - min * 60;
    out[2] = ticks - sec * 60;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SignedDivisionAndModuloBy240WithCall",
          .function_name = "SignedDivisionAndModuloBy240WithCall",
          .original_c = R"(
s32 combine_time(s32 q, s32 r);

s32 SignedDivisionAndModuloBy240WithCall(s32 frames, s32 offset) {
    s32 total = frames + offset;
    s32 q = total / 240;
    s32 r = total % 240;
    return combine_time(q, r);
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopHoistedByteStoresOnMatch",
          .function_name = "LoopHoistedByteStoresOnMatch",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LoopHoistedHalfwordConstantStore",
          .function_name = "LoopHoistedHalfwordConstantStore",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LoopHoistedBitwiseMaskAndCompare",
          .function_name = "LoopHoistedBitwiseMaskAndCompare",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ScratchFoldingBeforeGotoSharedExitLabel",
          .function_name = "ScratchFoldingBeforeGotoSharedExitLabel",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MultipleCallsAndStoresBeforeSharedReturnLabel",
          .function_name = "MultipleCallsAndStoresBeforeSharedReturnLabel",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LoopHoistedThreeWayStateTransition",
          .function_name = "LoopHoistedThreeWayStateTransition",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "NonLeafEntryCallFollowedByGotoBailoutAndScratchCompute",
          .function_name = "NonLeafEntryCallFollowedByGotoBailoutAndScratchCompute",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MultiBranchCallDispatchWithSharedGotoErrorAndScratch",
          .function_name = "MultiBranchCallDispatchWithSharedGotoErrorAndScratch",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MultiCalleeSavedWithEntryCallAndSharedGotoCleanup",
          .function_name = "MultiCalleeSavedWithEntryCallAndSharedGotoCleanup",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LoopWithMultipleHelperCallsAndScratchAccumulation",
          .function_name = "LoopWithMultipleHelperCallsAndScratchAccumulation",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ModifiedFirstParamWithExtraScratchArgsForHelperCall",
          .function_name = "ModifiedFirstParamWithExtraScratchArgsForHelperCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ThreeStepHelperCallAccumulationIntoReturnScratch",
          .function_name = "ThreeStepHelperCallAccumulationIntoReturnScratch",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "FourArgHelperCallInLoopWithDualScratchUpdates",
          .function_name = "FourArgHelperCallInLoopWithDualScratchUpdates",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ConditionalHelperCallWithPostCallArrayWriteback",
          .function_name = "ConditionalHelperCallWithPostCallArrayWriteback",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "DualHelperCallsWithIntermediateStructFieldUpdates",
          .function_name = "DualHelperCallsWithIntermediateStructFieldUpdates",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LoopWithEarlyExitHelperCallAndScratchWriteback",
          .function_name = "LoopWithEarlyExitHelperCallAndScratchWriteback",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SignedModuloByFiveInConditionalBranch",
          .function_name = "SignedModuloByFiveInConditionalBranch",
          .original_c = R"(
void SignedModuloByFiveInConditionalBranch(s32* p, s32* out) {
    s32 v = p[0];
    if (v > 0) {
        out[0] = v % 5;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "SignedDivisionByThreeInLoopArrayIndexing",
          .function_name = "SignedDivisionByThreeInLoopArrayIndexing",
          .original_c = R"(
s32 SignedDivisionByThreeInLoopArrayIndexing(s32* p, u16* table) {
    s32 idx = p[0];
    return table[idx / 3];
}
)",
      },
      RoundTripTestCase{
          .test_name = "CalleeSavedFramePointerRegisterPreservationAcrossCalls",
          .function_name = "CalleeSavedFramePointerRegisterPreservationAcrossCalls",
          .original_c = R"(
s32 step_work(s32 a, s32 b);

s32 CalleeSavedFramePointerRegisterPreservationAcrossCalls(s32* p, s32 n) {
    s32 a = step_work(p[0], n);
    s32 b = step_work(p[1], a);
    p[2] = a + b;
    return p[2] + n;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SingleParamCopiedToSavedAndReusedAcrossBranchCall",
          .function_name = "SingleParamCopiedToSavedAndReusedAcrossBranchCall",
          .original_c = R"(
void notify_value(s32 v);

s32 SingleParamCopiedToSavedAndReusedAcrossBranchCall(s32* ctx) {
    if (ctx[0] != 0) {
        notify_value(ctx[1] + 3);
    }
    return ctx[2];
}
)",
      },
      RoundTripTestCase{
          .test_name = "MultiCallReturnValueAccumulationWithBranchUpdates",
          .function_name = "MultiCallReturnValueAccumulationWithBranchUpdates",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "FloatBitcastExponentExtractBranch",
          .function_name = "FloatBitcastExponentExtractBranch",
          .original_c = R"(
f32 FloatBitcastExponentExtractBranch(f32 x, f32 fallback) {
    s32 bits = *(s32*)&x;
    if (((bits >> 22) & 511) < 310) {
        return x * 0.5f;
    }
    return fallback;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoublePolynomialHornerEvaluationWithParitySign",
          .function_name = "DoublePolynomialHornerEvaluationWithParitySign",
          .original_c = R"(
f32 DoublePolynomialHornerEvaluationWithParitySign(f64 x, f64 c1, f64 c2, s32 n) {
    f64 x2 = x * x;
    f64 poly = x + x * x2 * (c1 + x2 * c2);
    if ((n & 1) == 0) {
        return (f32)poly;
    }
    return -(f32)poly;
}
)",
      },
      RoundTripTestCase{
          .test_name = "TwoParamSaveBeforeZeroArgInitCall",
          .function_name = "TwoParamSaveBeforeZeroArgInitCall",
          .original_c = R"(
void init_subsystems(void);
s32 run_step(s32* buf, s32 mode);

s32 TwoParamSaveBeforeZeroArgInitCall(s32* buf, s32 mode) {
    init_subsystems();
    if (buf[0] != 0) {
        return run_step(buf, mode);
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "UnsignedShiftBitwiseOrBeforeSignedFloatCast",
          .function_name = "UnsignedShiftBitwiseOrBeforeSignedFloatCast",
          .original_c = R"(
f32 UnsignedShiftBitwiseOrBeforeSignedFloatCast(s32 a, s32 mask, u32 b) {
    s32 combined = (a & mask) | (b >> 16);
    return (f32)combined;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SwitchInsideIfBranchWithPostSwitchProcessing",
          .function_name = "SwitchInsideIfBranchWithPostSwitchProcessing",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MultiValueSparseEqualityDispatchToSingleAction",
          .function_name = "MultiValueSparseEqualityDispatchToSingleAction",
          .original_c = R"(
void trigger_event(s32 code);

void MultiValueSparseEqualityDispatchToSingleAction(s32 id) {
    if (id == 428 || id == 523 || id == 950 || id == 1050 || id == 1200 || id == 1250) {
        trigger_event(75);
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "ShortCircuitAndWithStructFieldLoadsAndUpdate",
          .function_name = "ShortCircuitAndWithStructFieldLoadsAndUpdate",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ShortCircuitOrWithStructFieldLoadsAndCall",
          .function_name = "ShortCircuitOrWithStructFieldLoadsAndCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "TwoCaseHighValueSwitchWithSharedTailAndDefaultExit",
          .function_name = "TwoCaseHighValueSwitchWithSharedTailAndDefaultExit",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "CalleeSavedSymbolAddressesAcrossMultipleCalls",
          .function_name = "CalleeSavedSymbolAddressesAcrossMultipleCalls",
          .original_c = R"(
extern s32 D_00DBA100;
extern s32 D_00DD38F0;
extern s32 D_00DD3910;

void submit_dma_chunk(s32 id, s32* a, s32* b, s32* c);

void CalleeSavedSymbolAddressesAcrossMultipleCalls(void) {
    submit_dma_chunk(1, &D_00DBA100, &D_00DD38F0, &D_00DD3910);
    submit_dma_chunk(2, &D_00DBA100, &D_00DD38F0, &D_00DD3910);
    submit_dma_chunk(3, &D_00DBA100, &D_00DD38F0, &D_00DD3910);
}
)",
      },
      RoundTripTestCase{
          .test_name = "TwoCaseHighValueSwitchWithSharedTailAndReturn",
          .function_name = "TwoCaseHighValueSwitchWithSharedTailAndReturn",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MaskedByteCallReturnStoredAndPassedAcrossCalls",
          .function_name = "MaskedByteCallReturnStoredAndPassedAcrossCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MaskedHalfwordCallReturnStoredAndPassedAcrossCalls",
          .function_name = "MaskedHalfwordCallReturnStoredAndPassedAcrossCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MaskedByteCallReturnStoredAndPassedAcrossThreeCalls",
          .function_name = "MaskedByteCallReturnStoredAndPassedAcrossThreeCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MaskedHalfwordCallReturnUsedInIndexedAddressCalculation",
          .function_name = "MaskedHalfwordCallReturnUsedInIndexedAddressCalculation",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MaskedCallReturnUsedInIndexedAddressCalculation",
          .function_name = "MaskedCallReturnUsedInIndexedAddressCalculation",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "RomSegmentSymbolAddressesPassedToCall",
          .function_name = "RomSegmentSymbolAddressesPassedToCall",
          .original_c = R"(
extern u8 rom_asset_start[];
extern u8 rom_asset_end[];
void dma_read_rom(s32 slot, u8* rom_start, u8* rom_end, s32 mode);

void RomSegmentSymbolAddressesPassedToCall(s32 slot, s32 mode) {
    dma_read_rom(slot, rom_asset_start, rom_asset_end, mode);
}
)",
      },
      RoundTripTestCase{
          .test_name = "RomSegmentSymbolAddressesStoredToOutgoingStackArgs",
          .function_name = "RomSegmentSymbolAddressesStoredToOutgoingStackArgs",
          .original_c = R"(
extern u8 rom_bank_a[];
extern u8 rom_bank_b[];
extern u8 rom_bank_c[];
void load_rom_banks(s32 slot, u8* a, s32 count, u8* b, u8* c, s32 flags);

void RomSegmentSymbolAddressesStoredToOutgoingStackArgs(s32 slot, s32 count, s32 flags) {
    load_rom_banks(slot, rom_bank_a, count, rom_bank_b, rom_bank_c, flags);
}
)",
      },
      RoundTripTestCase{
          .test_name = "RomSegmentSymbolAddressPassedAcrossBranches",
          .function_name = "RomSegmentSymbolAddressPassedAcrossBranches",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MultiCallHotTargetDispatcherWithBranches",
          .function_name = "MultiCallHotTargetDispatcherWithBranches",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "NineCalleeSavedRegistersAcrossCallsUsesFpS8",
          .function_name = "NineCalleeSavedRegistersAcrossCallsUsesFpS8",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "VoidFunctionWithIgnoredNonVoidCallOnOneBranchAndStoreTail",
          .function_name = "VoidFunctionWithIgnoredNonVoidCallOnOneBranchAndStoreTail",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "NonLeafTwoBranchReturnValuesInV0WithCalls",
          .function_name = "NonLeafTwoBranchReturnValuesInV0WithCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ModifiedParamWithLocalArgScratchUsedInSingleArgCall",
          .function_name = "ModifiedParamWithLocalArgScratchUsedInSingleArgCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "GprFloatParameterSavedInCalleeSavedFprWithLocalFloatArrayAndTwoArgCalls",
          .function_name = "GprFloatParameterSavedInCalleeSavedFprWithLocalFloatArrayAndTwoArgCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SubwordStackBufferAccessRightBelowSavedRegisters",
          .function_name = "SubwordStackBufferAccessRightBelowSavedRegisters",
          .original_c = R"(
s32 consume_bytes(u8* buf, s32 a, s32 b, s32 c, s32 d, s32 e);

s32 SubwordStackBufferAccessRightBelowSavedRegisters(s32 x, s32 y) {
    u8 buf[40];
    buf[0] = (u8)x;
    buf[39] = (u8)y;
    return consume_bytes(buf, x, y, x + 1, y + 1, x + y) + x;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SwitchJumpTableWithNonMonotonicCaseOrder",
          .function_name = "SwitchJumpTableWithNonMonotonicCaseOrder",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "EntryBlockDecisionTreeSwitchWithCalleeSavedRegisters",
          .function_name = "EntryBlockDecisionTreeSwitchWithCalleeSavedRegisters",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LeafWithModifiedParamAndMultipleCallerSavedScratch",
          .function_name = "LeafWithModifiedParamAndMultipleCallerSavedScratch",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "DuplicateRomSegmentSymbolsAcrossMultipleCallsAvoidCse",
          .function_name = "DuplicateRomSegmentSymbolsAcrossMultipleCallsAvoidCse",
          .original_c = R"(
extern s32 D_00DBA100;
extern s32 D_00DBA100__dup2;

void submit_rom_chunk(s32 id, s32* rom_addr);

void DuplicateRomSegmentSymbolsAcrossMultipleCallsAvoidCse(void) {
    submit_rom_chunk(1, &D_00DBA100);
    submit_rom_chunk(2, &D_00DBA100__dup2);
}
)",
      },
      RoundTripTestCase{
          .test_name = "O0FramePointerStructFieldAndCall",
          .function_name = "O0FramePointerStructFieldAndCall",
          .original_c = R"(
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
          .options = {
              .c_flags = {"-O0", "-mips2", "-mcpu=r4000"},
          },
      },
      RoundTripTestCase{
          .test_name = "O0FramePointerWhileLoopAndUnsignedByteStore",
          .function_name = "O0FramePointerWhileLoopAndUnsignedByteStore",
          .original_c = R"(
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
          .options = {
              .c_flags = {"-O0", "-mips2", "-mcpu=r4000"},
          },
      },
      RoundTripTestCase{
          .test_name = "NonLeafFloatScratchStoreBeforeHelperCall",
          .function_name = "NonLeafFloatScratchStoreBeforeHelperCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "MultiParamSavedToCalleeRegistersAndReusedAcrossCalls",
          .function_name = "MultiParamSavedToCalleeRegistersAndReusedAcrossCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "HalfwordBitmaskClearAndStoreAfterHelperCall",
          .function_name = "HalfwordBitmaskClearAndStoreAfterHelperCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ShortCircuitFloatAndStoreGlobal",
          .function_name = "ShortCircuitFloatAndStoreGlobal",
          .original_c = R"(
extern s32 g_ready_flag;

void ShortCircuitFloatAndStoreGlobal(f32 x, f32 y, f32 z) {
    if (x > 0.0f && y > 0.0f && z > 0.0f) {
        g_ready_flag = 0;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "ShortCircuitFloatOrStoreGlobal",
          .function_name = "ShortCircuitFloatOrStoreGlobal",
          .original_c = R"(
extern s32 g_error_flag;

void ShortCircuitFloatOrStoreGlobal(f32 x, f32 y, f32 z) {
    if (x < 0.0f || y < 0.0f || z < 0.0f) {
        g_error_flag = 0;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoublePrecisionCompareAndTruncateToInt",
          .function_name = "DoublePrecisionCompareAndTruncateToInt",
          .original_c = R"(
extern s32 g_clamped_int;

void DoublePrecisionCompareAndTruncateToInt(f64 a, f64 b) {
    if (a < b) {
        g_clamped_int = (s32)a;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoublePrecisionInA2A3AndStackParameters",
          .function_name = "DoublePrecisionInA2A3AndStackParameters",
          .original_c = R"(
f64 DoublePrecisionInA2A3AndStackParameters(s32 mode, f64 a, f64 b) {
    if (mode > 0) {
        return a + b;
    }
    return a - b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallerPassesDoubleInA2A3AndStackToHelper",
          .function_name = "CallerPassesDoubleInA2A3AndStackToHelper",
          .original_c = R"(
void combine_doubles(s32 mode, f64 a, f64 b);

void CallerPassesDoubleInA2A3AndStackToHelper(s32 mode, f64* values) {
    combine_doubles(mode, values[0], values[1]);
}
)",
      },
      RoundTripTestCase{
          .test_name = "ExtractFloatWordsFromDoubleOnStack",
          .function_name = "ExtractFloatWordsFromDoubleOnStack",
          .original_c = R"(
f32 ExtractFloatWordsFromDoubleOnStack(f64 x) {
    f64 local_d = x * 2.0;
    return *(f32*)&local_d + *(f32*)((u8*)&local_d + 4);
}
)",
      },
      RoundTripTestCase{
          .test_name = "StoreUintAsFloatAndDouble",
          .function_name = "StoreUintAsFloatAndDouble",
          .original_c = R"(
void StoreUintAsFloatAndDouble(u32 x, f32* out_f, f64* out_d) {
    *out_f = (f32)x;
    *out_d = (f64)x;
}
)",
      },
      RoundTripTestCase{
          .test_name = "TwoUintsToFloatAndDoubleRatio",
          .function_name = "TwoUintsToFloatAndDoubleRatio",
          .original_c = R"(
f64 TwoUintsToFloatAndDoubleRatio(u32 a, u32 b) {
    return (f64)a / (f64)(f32)b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "AddTwoUnsignedToDouble",
          .function_name = "AddTwoUnsignedToDouble",
          .original_c = R"(
f64 AddTwoUnsignedToDouble(u32 a, u32 b) {
    return (f64)a + (f64)b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ConvertTwoUnsignedToFloatAndStore",
          .function_name = "ConvertTwoUnsignedToFloatAndStore",
          .original_c = R"(
void ConvertTwoUnsignedToFloatAndStore(u32 a, u32 b, f32* out) {
    out[0] = (f32)a * 0.5f;
    out[1] = (f32)b * 0.25f;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ForLoopInclusiveUpperBoundStrideTwo",
          .function_name = "ForLoopInclusiveUpperBoundStrideTwo",
          .original_c = R"(
s32 ForLoopInclusiveUpperBoundStrideTwo(s32 limit) {
    s32 sum = 0;
    s32 i;
    for (i = 1; i <= limit; i += 2) {
        sum += (i ^ 3);
    }
    return sum;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ForLoopCountdownGreaterEqualOneStrideThree",
          .function_name = "ForLoopCountdownGreaterEqualOneStrideThree",
          .original_c = R"(
s32 ForLoopCountdownGreaterEqualOneStrideThree(s32 start) {
    s32 acc = 0;
    s32 i;
    for (i = start; i >= 1; i -= 3) {
        acc += (i * 2);
    }
    return acc;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LeafWithMultipleScratchAndCalleeSavedRegisters",
          .function_name = "LeafWithMultipleScratchAndCalleeSavedRegisters",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "LinkedListCursorInSecondParamWithFirstParamPreservedAcrossCall",
          .function_name = "LinkedListCursorInSecondParamWithFirstParamPreservedAcrossCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SixCaseSparseDecisionTreeSwitch",
          .function_name = "SixCaseSparseDecisionTreeSwitch",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "SecondIntParamSavedAndPassedWithThirdFloatGprParam",
          .function_name = "SecondIntParamSavedAndPassedWithThirdFloatGprParam",
          .original_c = R"(
s32 helper_pair(s32 a, s32 b);

f32 SecondIntParamSavedAndPassedWithThirdFloatGprParam(s32 a, s32 b, f32 c) {
    s32 r = helper_pair(a + 1, b);
    return (f32)(r + b) + c;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CallResultComparedWithSltiBeforeConditionalCallWithSavedArg",
          .function_name = "CallResultComparedWithSltiBeforeConditionalCallWithSavedArg",
          .original_c = R"(
s32 poll_level(s32 ch);
void apply_level(s32 ch, s32 level);

void CallResultComparedWithSltiBeforeConditionalCallWithSavedArg(s32 ch) {
    s32 lvl = poll_level(ch);
    if (lvl < 5) {
        apply_level(ch, lvl);
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "ThreeCaseSwitchLoadedSelectorWithCrossJumpedCall",
          .function_name = "ThreeCaseSwitchLoadedSelectorWithCrossJumpedCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "DoWhileLoopWithUnsigned16BitIndexArrayFill",
          .function_name = "DoWhileLoopWithUnsigned16BitIndexArrayFill",
          .original_c = R"(
void DoWhileLoopWithUnsigned16BitIndexArrayFill(s32* words, s32 fill_val) {
    u16 i = 0;
    do {
        words[i] = fill_val;
        i++;
    } while (i < 4);
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoWhileLoopWithConditionalNarrowCounterIncrement",
          .function_name = "DoWhileLoopWithConditionalNarrowCounterIncrement",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "TwoCaseSwitchWithPrologueDelaySlotAndStolenCaseConstant",
          .function_name = "TwoCaseSwitchWithPrologueDelaySlotAndStolenCaseConstant",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "NestedIfRangeChecksOnSameVariableWithoutFoldRangeTestMerge",
          .function_name = "NestedIfRangeChecksOnSameVariableWithoutFoldRangeTestMerge",
          .original_c = R"(
void apply_slot_action(s32 idx);

void NestedIfRangeChecksOnSameVariableWithoutFoldRangeTestMerge(s32 idx) {
    if (idx < 2) {
        if (idx >= 0) {
            apply_slot_action(idx);
        }
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoWhileLoopWithPostDecrementNarrowCounterAndCall",
          .function_name = "DoWhileLoopWithPostDecrementNarrowCounterAndCall",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "UnpinsS0CopyFromUnmodifiedParameterBeforeCallAndPostCallStore",
          .function_name = "UnpinsS0CopyFromUnmodifiedParameterBeforeCallAndPostCallStore",
          .original_c = R"(
s32 step_interp(s16* state);

void UnpinsS0CopyFromUnmodifiedParameterBeforeCallAndPostCallStore(s16* state) {
    s32 old_val = state[0];
    if (step_interp(state) != 0) {
        if (old_val == state[0]) {
            state[2] = 0;
        }
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "LeafWithUnusedLocalStackBuffer",
          .function_name = "LeafWithUnusedLocalStackBuffer",
          .original_c = R"(
void LeafWithUnusedLocalStackBuffer(s32* out, s32 a, s32 b) {
    u8 unused[16];
    out[0] = a + b;
    out[1] = a - b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoWhileLoopWithFourNarrowFloatToUintByteCallArgs",
          .function_name = "DoWhileLoopWithFourNarrowFloatToUintByteCallArgs",
          .original_c = R"(
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
          .options = {
              .extra_types_header = R"(
typedef struct ColorF {
    f32 r;
    f32 g;
    f32 b;
    f32 a;
} ColorF;
)",
          },
      },
      RoundTripTestCase{
          .test_name = "NineCalleeSavedRegistersPassedAsOutgoingStackArgsAcrossCalls",
          .function_name = "NineCalleeSavedRegistersPassedAsOutgoingStackArgsAcrossCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "CalleeSavedFloatWithTransientDoubleScaleAcrossCalls",
          .function_name = "CalleeSavedFloatWithTransientDoubleScaleAcrossCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "DecisionTreeSwitchWithCrossJumpedCallTailAndEmptyDefault",
          .function_name = "DecisionTreeSwitchWithCrossJumpedCallTailAndEmptyDefault",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "WhileCallLoopWithBranchLikelyExitByteClearAndReusedParamCounter",
          .function_name = "WhileCallLoopWithBranchLikelyExitByteClearAndReusedParamCounter",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "NarrowU16ParameterIndexedStructSetupAcrossCalls",
          .function_name = "NarrowU16ParameterIndexedStructSetupAcrossCalls",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ChainedFallthroughStolenComparisonDelaySlotsInNestedGuards",
          .function_name = "ChainedFallthroughStolenComparisonDelaySlotsInNestedGuards",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "BranchLikelyDiamondWithSharedCallTailAndEarlyReturn",
          .function_name = "BranchLikelyDiamondWithSharedCallTailAndEarlyReturn",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "ShortCircuitOrWithBitmaskAndStructFieldChecks",
          .function_name = "ShortCircuitOrWithBitmaskAndStructFieldChecks",
          .original_c = R"(
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
)",
      },
      RoundTripTestCase{
          .test_name = "DisjointBranchScratchAndNarrowParamAcrossCalls",
          .function_name = "DisjointBranchScratchAndNarrowParamAcrossCalls",
          .original_c = R"(
extern void notify_channel_event(s32 ch, s32 code);
extern void clear_channel_state(s32 ch);

s32 DisjointBranchScratchAndNarrowParamAcrossCalls(u16 ch, s32* table) {
    if (ch < 16) {
        s32 val = table[ch];
        if (val > 0) {
            notify_channel_event(ch, val + 1);
            return 1;
        }
        clear_channel_state(ch);
    }
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "NestedIfCascadeWithSharedDefaultReturnAndElseBranch",
          .function_name = "NestedIfCascadeWithSharedDefaultReturnAndElseBranch",
          .original_c = R"(
typedef struct {
    u16 flags;
    u16 pad;
    s32* table;
} ActorRecord;

extern s32 resolve_actor_anim(s32 val_a, s32 val_b);

s32 NestedIfCascadeWithSharedDefaultReturnAndElseBranch(ActorRecord* actors, u16 idx, u16 anim_id) {
    s32 result = 0;
    if (idx < 192) {
        ActorRecord* rec = &actors[idx];
        if ((rec->flags & 1) != 0) {
            s32* tbl = rec->table;
            if (tbl[anim_id] != tbl[anim_id + 1]) {
                result = resolve_actor_anim(tbl[anim_id], tbl[anim_id + 1]);
            }
        }
    }
    return result & 255;
}
)",
      },
      RoundTripTestCase{
          .test_name = "NestedDoWhileEarlyExitToOuterLoopAndPostLoopStep",
          .function_name = "NestedDoWhileEarlyExitToOuterLoopAndPostLoopStep",
          .original_c = R"(
typedef struct CallbackNode {
    struct CallbackNode* next;
    s32 (*callback)(s32);
} CallbackNode;

extern s32 wait_for_event(s32* out_event);
extern CallbackNode* g_callback_head;

void NestedDoWhileEarlyExitToOuterLoopAndPostLoopStep(s32* event_slot) {
    while (wait_for_event(event_slot) != 0) {
        CallbackNode* cur = g_callback_head;
        if (cur == 0) {
            continue;
        }
        do {
            if (cur->callback != 0) {
                if (cur->callback(*event_slot) != 0) {
                    break;
                }
            }
            cur = cur->next;
        } while (cur != 0);
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "NonLeafWithMultipleUnusedCallReturnsAndStructInit",
          .function_name = "NonLeafWithMultipleUnusedCallReturnsAndStructInit",
          .original_c = R"(
extern s32 init_entity_pos(u16 idx, s32 x, s32 y);
extern s32 init_entity_color(u16 idx, s32 r, s32 g);
extern s32 init_entity_mode(u16 idx, s32 mode);
extern s32 g_entity_x[64];
extern s32 g_entity_y[64];
extern u16 g_entity_flags[128];

s32 NonLeafWithMultipleUnusedCallReturnsAndStructInit(u16 idx, s32 x, s32 y) {
    s32 ok = 0;
    if (idx < 64) {
        if ((g_entity_flags[idx * 2] & 1) == 0) {
            g_entity_x[idx] = x;
            g_entity_y[idx] = y;
            g_entity_flags[idx * 2] = 1;
            init_entity_pos(idx, x, y);
            init_entity_color(idx, 255, 255);
            init_entity_mode(idx, 2);
            ok = 1;
        }
    }
    return ok;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ConsolidatedPipelineCoalescesParamAndSinksSharedReturnTail",
          .function_name = "ConsolidatedPipelineCoalescesParamAndSinksSharedReturnTail",
          .original_c = R"(
extern void apply_left_action(s32 base, s32 arg);
extern void apply_right_action(s32 base, s32 arg);

s32 ConsolidatedPipelineCoalescesParamAndSinksSharedReturnTail(s32 base, s32 mode, s32 arg) {
    if (mode != 0) {
        apply_left_action(base, arg);
    } else {
        apply_right_action(base, arg);
    }
    return base + 4;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SwitchWithMergedCasesAndCrossJumpedNestedIfCallTail",
          .function_name = "SwitchWithMergedCasesAndCrossJumpedNestedIfCallTail",
          .original_c = R"(
extern u8 check_slot_active(u8 slot);
extern void clear_slot_entry(u8 slot);
extern void reset_all_slots(void);
extern void dispatch_slot_event(s32 mode, s32 code);
extern u8 g_active_mode;
extern u8 g_active_slot;
extern u16 g_slot_flags;

void SwitchWithMergedCasesAndCrossJumpedNestedIfCallTail(void) {
    switch (g_active_mode) {
    case 0:
        if (check_slot_active(g_active_slot) == 0) {
            dispatch_slot_event(0, 10);
        } else {
            dispatch_slot_event(0, 429);
        }
        break;
    case 1:
        clear_slot_entry(g_active_slot);
        g_slot_flags = 0;
        reset_all_slots();
        break;
    case 2:
        if (check_slot_active(g_active_slot) != 0) {
            dispatch_slot_event(0, 429);
        }
        break;
    case 3:
        if (check_slot_active(g_active_slot) != 0) {
            dispatch_slot_event(0, 429);
        }
        break;
    default:
        g_slot_flags = 0;
        g_active_mode = 0;
        break;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "SwitchWithChainedSharedStoreTails",
          .function_name = "SwitchWithChainedSharedStoreTails",
          .original_c = R"(
extern u8 g_season_char_0;
extern u8 g_season_char_1;
extern u8 g_season_char_2;
extern u8 g_season_char_3;
extern u8 g_season_char_4;
extern u8 g_season_char_5;

void SwitchWithChainedSharedStoreTails(s32 season) {
    switch (season & 0xFF) {
    case 1:
        g_season_char_0 = 0xB3;
        g_season_char_1 = 0xCA;
        g_season_char_2 = 0xCC;
        g_season_char_3 = 0xC3;
        g_season_char_4 = 0xC8;
        g_season_char_5 = 0xC1;
        break;
    case 2:
        g_season_char_0 = 0xB3;
        g_season_char_1 = 0xCF;
        g_season_char_2 = 0xC7;
        g_season_char_3 = 0xC7;
        g_season_char_4 = 0xBF;
        g_season_char_5 = 0xCC;
        break;
    case 3:
        g_season_char_0 = 0xA1;
        g_season_char_1 = 0xCA;
        g_season_char_2 = 0xCE;
        g_season_char_3 = 0xCF;
        g_season_char_4 = 0xC7;
        g_season_char_5 = 0xC8;
        break;
    case 4:
        g_season_char_0 = 0xB7;
        g_season_char_1 = 0xC3;
        g_season_char_2 = 0xC8;
        g_season_char_3 = 0xCE;
        g_season_char_4 = 0xBF;
        g_season_char_5 = 0xCC;
        break;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "CoalescedParameterWithFpCalleeSavedRegisterAcrossCalls",
          .function_name = "CoalescedParameterWithFpCalleeSavedRegisterAcrossCalls",
          .original_c = R"(
extern s32 eval_pair(s32 a, s32 b);

s32 CoalescedParameterWithFpCalleeSavedRegisterAcrossCalls(s32 x, s32 y, s32 z, s32 w) {
    s32 a0 = x + y;
    s32 a1 = y + z;
    s32 a2 = z + w;
    s32 a3 = w + x;
    s32 a4 = x - y;
    s32 a5 = y - z;
    s32 r = eval_pair(a0, a1);
    r += eval_pair(x, a2);
    r += eval_pair(y, a3);
    r += eval_pair(z, a4);
    r += eval_pair(w, a5);
    return r + x + y + z + w + a0 + a1 + a2 + a3 + a4 + a5;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CalleeSavedDoubleInGprPairWithFpAcrossCalls",
          .function_name = "CalleeSavedDoubleInGprPairWithFpAcrossCalls",
          .original_c = R"(
extern f32 fallback_sqrtf(f32 x, s32 tag);
extern f64 g_unit_scale;

f32 CalleeSavedDoubleInGprPairWithFpAcrossCalls(f32 x, f32 y, s32 n) {
    register s32 temp_s0 asm("$16") = n + 1;
    register s32 temp_s1 asm("$17") = n + 2;
    register s32 temp_s2 asm("$18") = n + 3;
    register s32 temp_s3 asm("$19") = n + 4;
    register s32 temp_s4 asm("$20") = n + 5;
    register s32 temp_s5 asm("$21") = n + 6;
    register f64 temp_s6 asm("$22");
    s32 v8 = n + 7;
    register f32 f20 asm("$f20") = x + y;
    register f32 f22 asm("$f22") = x - y;
    register f32 f24 asm("$f24") = x * y;
    register f32 f26 asm("$f26") = f20 + f24;
    register f32 f28 asm("$f28") = f22 + f24;
    register f32 f30 asm("$f30") = f26 - f28;
    f32 r0 = fallback_sqrtf(f20, v8);
    f32 inv0;
    f32 r1;
    f32 inv1;
    temp_s6 = g_unit_scale;
    inv0 = (f32)(temp_s6 / (f64)r0);
    f20 = f20 * inv0 + f22 + f24 + f26 + f28 + f30;
    r1 = fallback_sqrtf(f20, temp_s0 + temp_s1 + temp_s2 + temp_s3 + temp_s4 + temp_s5 + v8);
    inv1 = (f32)(temp_s6 / (f64)r1);
    v8 = v8 + temp_s0;
    return f20 * inv1 + (f32)v8;
}
)",
      },
      RoundTripTestCase{
          .test_name = "PinnedCalleeSavedParamAndNarrowCastsAcrossCallWithBareDeref",
          .function_name = "PinnedCalleeSavedParamAndNarrowCastsAcrossCallWithBareDeref",
          .original_c = R"(
extern s32 prepare_slot_handle(s32 mode);
extern void notify_slot_pair(s32 a, s32 b);

s32 PinnedCalleeSavedParamAndNarrowCastsAcrossCallWithBareDeref(s32* node, s32 x, s32 y) {
    s32 r = prepare_slot_handle(1);
    node[1] = x;
    y = y & 2;
    *(s16*)(node + 2) = (s16)r;
    node[0] = r;
    if (y != 0) {
        notify_slot_pair(x, r);
    }
    return prepare_slot_handle(0);
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatExpressionPassedInSecondGprArgAfterHelperCall",
          .function_name = "FloatExpressionPassedInSecondGprArgAfterHelperCall",
          .original_c = R"(
extern void setup_channel(s32 ch);
extern s32 apply_channel_gain(s32 ch, f32 gain, s32 mode);

s32 FloatExpressionPassedInSecondGprArgAfterHelperCall(s32 ch, f32* gain_ptr, s32 mode) {
    setup_channel(ch + 1);
    return apply_channel_gain(ch, gain_ptr[0] * 0.5f, mode) + 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CrossJumpedDiamondStoreTailInThreadStartBranches",
          .function_name = "CrossJumpedDiamondStoreTailInThreadStartBranches",
          .original_c = R"(
typedef struct {
    s32 next;
    s32 prio;
    s32 queue;
    s32 pad;
    u16 state;
} ThreadNode;

extern ThreadNode* g_run_queue;
extern ThreadNode* g_active_thread;
extern s32 disable_ints(void);
extern void restore_ints(s32 mask);
extern void enqueue_thread(s32 queue, ThreadNode* t);
extern ThreadNode* pop_thread(s32 queue);
extern void dispatch_thread(void);

void CrossJumpedDiamondStoreTailInThreadStartBranches(ThreadNode* t) {
    s32 mask = disable_ints();
    switch (t->state) {
    case 1:
        t->state = 4;
        enqueue_thread((s32)&g_run_queue, t);
        break;
    case 8:
        if (t->queue == 0 || t->queue == (s32)&g_run_queue) {
            t->state = 2;
            enqueue_thread((s32)&g_run_queue, t);
        } else {
            t->state = 8;
            enqueue_thread(t->queue, t);
            enqueue_thread((s32)&g_run_queue, pop_thread(t->queue));
        }
        break;
    default:
        restore_ints(mask);
        return;
    }
    if (g_active_thread == 0) {
        dispatch_thread();
    }
    restore_ints(mask);
}
)",
      },
      RoundTripTestCase{
          .test_name = "TargetStolenShortCircuitDiamondStoreTail",
          .function_name = "TargetStolenShortCircuitDiamondStoreTail",
          .original_c = R"(
typedef struct {
    s32 mode;
    s32 submode;
    u16 state;
    u16 flags;
    s32 timer;
} BoxState;

void TargetStolenShortCircuitDiamondStoreTail(BoxState* box, s32 step) {
    if (box->mode == 1) {
        if (box->submode == 0 || box->timer <= 0) {
            box->state = 2;
            box->flags |= 4;
            box->timer = 0;
        } else {
            box->timer -= step;
        }
    } else if (box->mode == 2) {
        box->state = 4;
        box->flags |= 4;
        box->timer = 0;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "VariableShiftBitmaskCheckWithTableUpdate",
          .function_name = "VariableShiftBitmaskCheckWithTableUpdate",
          .original_c = R"(
extern u8 g_bit_table[64];

s32 VariableShiftBitmaskCheckWithTableUpdate(s32 idx) {
    s32 res = 0;
    if (idx < 256) {
        s32 byte_idx = idx >> 3;
        s32 mask = 1 << (idx & 7);
        if (g_bit_table[byte_idx] & mask) {
            res = 1;
        }
    }
    return res;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ShortCircuitOrGuardWithConflictingFallthroughDelayLoad",
          .function_name = "ShortCircuitOrGuardWithConflictingFallthroughDelayLoad",
          .original_c = R"(
typedef struct {
    u8 active;
    u8 paused;
    u16 count;
    s32 value;
} ChannelSlot;

void ShortCircuitOrGuardWithConflictingFallthroughDelayLoad(ChannelSlot* slot, s32 delta) {
    if (slot->active == 0 || slot->paused != 0) {
        return;
    }
    slot->count += 1;
    slot->value += delta;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ZeroFrameLeafBitcastFloatFromCallerHomedGprParams",
          .function_name = "ZeroFrameLeafBitcastFloatFromCallerHomedGprParams",
          .original_c = R"(
f32 ZeroFrameLeafBitcastFloatFromCallerHomedGprParams(
    f32 x, f32 y, s32 a_bits, s32 b_bits, s32 c_bits, s32 d_bits) {
    f32 b = *(f32*)&b_bits;
    if (b != 0.0f) {
        f32 a = *(f32*)&a_bits;
        f32 c = *(f32*)&c_bits;
        f32 d = *(f32*)&d_bits;
        return (-(a * x) - (c * y) - d) / b;
    }
    return 0.0f;
}
)",
      },
      RoundTripTestCase{
          .test_name = "LoopWithHeaderCounterStealingAndExitUse",
          .function_name = "LoopWithHeaderCounterStealingAndExitUse",
          .original_c = R"(
s32 LoopWithHeaderCounterStealingAndExitUse(s32* arr, s32 limit) {
    s32 count = 0;
    while (1) {
        count++;
        if (arr[count] == limit) {
            break;
        }
        if (arr[count] < 0) {
            return -1;
        }
    }
    return count;
}
)",
      },
      RoundTripTestCase{
          .test_name = "CrossJumpedNestedBranchesWithSharedStoreTail",
          .function_name = "CrossJumpedNestedBranchesWithSharedStoreTail",
          .original_c = R"(
typedef struct {
    s32 mode;
    s32 submode;
    s32 timer;
    s32 value;
    s32 dirty;
} ControllerState;

s32 CrossJumpedNestedBranchesWithSharedStoreTail(ControllerState* st, s32 step) {
    if (st->mode == 1) {
        if (st->submode == 0) {
            st->value += step;
            st->dirty = 1;
        } else {
            st->timer += 1;
        }
    } else if (st->mode == 2) {
        st->value -= step;
        st->dirty = 1;
    }
    return st->value;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SixArgCallWithDisjointStructFieldLoadsAndStore",
          .function_name = "SixArgCallWithDisjointStructFieldLoadsAndStore",
          .original_c = R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
    s32 w;
} Vec4i;

extern s32 SubmitSixArgs(s32 id, s32 a, s32 b, s32 c, s32 d, s32 e);

s32 SixArgCallWithDisjointStructFieldLoadsAndStore(s32 id, Vec4i* v) {
    v->x = id;
    return SubmitSixArgs(id, 1, 2, 3, v->y, v->z);
}
)",
      },
      RoundTripTestCase{
          .test_name = "PostLoopScratchWebReusedForPointerInitAndGlobalStore",
          .function_name = "PostLoopScratchWebReusedForPointerInitAndGlobalStore",
          .original_c = R"(
extern void InitHardwareSubsystem(s32 mode);
extern s32 g_tail_link;
extern s32 g_head_ptr;

void PostLoopScratchWebReusedForPointerInitAndGlobalStore(u8* table_base, s32 base_val, s32 step_val) {
    s32 i = 0;
    u8* cur = table_base;
    s32* tail;
    s32 first_addr;
    InitHardwareSubsystem(base_val);
    do {
        *(s32*)(cur + 0) = base_val + i;
        *(s32*)(cur + 4) = step_val;
        i++;
        cur += 8;
    } while ((u32)i < 8U);
    tail = &g_tail_link;
    first_addr = (s32)tail - 64;
    *tail = first_addr;
    g_head_ptr = first_addr;
}
)",
      },
      RoundTripTestCase{
          .test_name = "NestedLoopWithGlobalTableBaseAndSentinelComparison",
          .function_name = "NestedLoopWithGlobalTableBaseAndSentinelComparison",
          .original_c = R"(
extern u8 D_8016F500[];
extern u8 D_8018A728[];

void NestedLoopWithGlobalTableBaseAndSentinelComparison(void) {
    u8 row = 0;
    u8 col = 0;
    do {
        s32 offset = col * 20;
        u8* dst_base = D_8016F500 + offset;
        u8* src_base = D_8018A728 + offset;
        do {
            u8* dst = dst_base + row;
            u8* src = src_base + row;
            row++;
            *dst = *src;
        } while (row < 20);
        col++;
        row = 0;
    } while (col < 24);
}
)",
      },
      RoundTripTestCase{
          .test_name = "StoreFlagConditionalIncrementAndDecrementInLoop",
          .function_name = "StoreFlagConditionalIncrementAndDecrementInLoop",
          .original_c = R"(
s32 StoreFlagConditionalIncrementAndDecrementInLoop(s32* arr, s32 n, s32 target) {
    s32 score = 0;
    if (n > 0) {
        do {
            s32 val = *arr;
            if (val == target) {
                score++;
            }
            if (val < 0) {
                score--;
            }
            arr++;
            n--;
        } while (n != 0);
    }
    return score;
}
)",
      },
      RoundTripTestCase{
          .test_name = "SparseSwitchBinaryDecisionTreeWithFiveCasesAndDefault",
          .function_name = "SparseSwitchBinaryDecisionTreeWithFiveCasesAndDefault",
          .original_c = R"(
s32 SparseSwitchBinaryDecisionTreeWithFiveCasesAndDefault(s32 code, s32 base) {
    switch (code) {
        case 10:
            return base + 1;
        case 50:
            return base + 5;
        case 100:
            return base + 10;
        case 200:
            return base + 20;
        case 500:
            return base + 50;
        default:
            return base - 1;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "ArithmeticAndBitwise",
          .function_name = "ArithmeticAndBitwise",
          .original_c = R"(
int ArithmeticAndBitwise(int a, int b, int c) {
  int mult_res = a * b;
  int div_res = (b != 0) ? (a / b) : 0;
  int rem_res = (c != 0) ? (a % c) : 0;
  int bit_res = (a & 0x0FFF) ^ ((b | 0x1234) & ~c);
  int shift_res = (a << 3) + (b >> 2);
  return mult_res + div_res + rem_res + bit_res + shift_res - 42;
}
)",
      },
      RoundTripTestCase{
          .test_name = "ControlFlowLoops",
          .function_name = "ControlFlowLoops",
          .original_c = R"(
int ControlFlowLoops(int count, int limit) {
  int sum = 0;
  int i = 0;

  if (count <= 0 || limit <= 0) {
    return 0;
  }

  while (i < count) {
    if (i == limit) {
      break;
    }
    if ((i & 1) != 0) {
      sum += i * 3;
    } else {
      sum -= i / 2;
    }
    i++;
  }

  do {
    sum += count;
    count--;
  } while (count > limit);

  return sum;
}
)",
      },
      RoundTripTestCase{
          .test_name = "PointerAndMemoryAccess",
          .function_name = "PointerAndMemoryAccess",
          .original_c = R"(
int PointerAndMemoryAccess(struct Player* player, const int* offsets, int count) {
  int i;
  int accum = 0;

  if (!player || !offsets || count <= 0) {
    return -1;
  }

  player->x += 10;
  player->y -= 20;
  player->hp = (player->hp > 10) ? (short)(player->hp - 5) : (short)0;
  player->state = 1;
  player->flags |= 0x80;

  for (i = 0; i < count; i++) {
    accum += offsets[i];
  }

  return accum + player->x + player->y + player->hp;
}
)",
          .options = {
              .extra_types_header = R"(
struct Player {
  int x;
  int y;
  short hp;
  short max_hp;
  unsigned char state;
  unsigned char flags;
};
)",
          },
      },
      RoundTripTestCase{
          .test_name = "DenseSwitch",
          .function_name = "DenseSwitch",
          .original_c = R"(
int DenseSwitch(int val) {
  switch (val) {
    case 0:
      return 100;
    case 1:
      return 250;
    case 2:
      return 500;
    case 3:
      return 750;
    case 4:
      return 1000;
    case 5:
      return 2000;
    default:
      return -1;
  }
}
)",
      },
      RoundTripTestCase{
          .test_name = "FloatMath",
          .function_name = "FloatMath",
          .original_c = R"(
float FloatMath(float speed, float angle, float dt) {
  float vel = speed * angle;
  if (vel > 100.0f) {
    vel = 100.0f;
  } else if (vel < -100.0f) {
    vel = -100.0f;
  }
  return vel * dt + 0.5f;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DoubleMath",
          .function_name = "DoubleMath",
          .original_c = R"(
double DoubleMath(double speed, double angle, double dt) {
  double vel = speed * angle;
  if (vel > 1000.0) {
    vel = 1000.0;
  } else if (vel < -1000.0) {
    vel = -1000.0;
  }
  return vel * dt + 2.5;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchLikelyLoop",
          .function_name = "BranchLikelyLoop",
          .original_c = R"(
int BranchLikelyLoop(const int* arr, int count) {
  int total = 0;
  int i;
  for (i = 0; i < count; i++) {
    if (arr[i] > 0) {
      total += arr[i];
    }
  }
  return total;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedDelaySlotStealAndAnnullingLoadShift",
          .function_name = "GeneralizedDelaySlotStealAndAnnullingLoadShift",
          .original_c = R"(
s32 GeneralizedDelaySlotStealAndAnnullingLoadShift(s32* table, s32 idx, s32 flag, s32 fallback) {
    if (flag == 0 || idx < 0) {
        s32 shifted = fallback << 3;
        return table[0] + shifted;
    }
    return table[idx];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedCrossJumpMultiStatementSharedTail",
          .function_name = "GeneralizedCrossJumpMultiStatementSharedTail",
          .original_c = R"(
s32 GeneralizedCrossJumpMultiStatementSharedTail(s32 mode, s32* state, s32 x, s32 y) {
    if (mode > 0) {
        state[0] = x + 10;
        state[1] = y + 20;
        state[2] = x ^ y;
        return state[0] + state[1];
    } else {
        state[0] = x - 10;
        state[1] = y + 20;
        state[2] = x ^ y;
        return state[0] + state[1];
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedLongDistanceScheduledLoadFoldAcrossStores",
          .function_name = "GeneralizedLongDistanceScheduledLoadFoldAcrossStores",
          .original_c = R"(
void GeneralizedLongDistanceScheduledLoadFoldAcrossStores(s32* src, s32* dst) {
    s32 a = src[0];
    s32 b = src[1];
    dst[2] = 10;
    dst[3] = 20;
    dst[4] = 30;
    dst[5] = 40;
    dst[6] = 50;
    dst[0] = a + b;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedLeafScratchRegisterWebSplitAcrossBranches",
          .function_name = "GeneralizedLeafScratchRegisterWebSplitAcrossBranches",
          .original_c = R"(
s32 GeneralizedLeafScratchRegisterWebSplitAcrossBranches(s32* p, s32 sel, s32 x, s32 y) {
    if (sel != 0) {
        s32 t = (x * 5) + y;
        p[0] = t;
        p[1] = t ^ x;
    } else {
        s32 u = (y * 9) - x;
        p[2] = u;
        p[3] = u + y;
    }
    return p[0] + p[2];
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedNestedLoopInvariantAndStrideTwelveGiv",
          .function_name = "GeneralizedNestedLoopInvariantAndStrideTwelveGiv",
          .original_c = R"(
void GeneralizedNestedLoopInvariantAndStrideTwelveGiv(s32* grid, s32 cols) {
    s32 r = 0;
    do {
        s32 c;
        for (c = 0; c < cols; c++) {
            s32 idx = (c * 3) + r;
            grid[idx] = grid[idx] & 0x00FF00FF;
        }
        r += 24;
    } while (r < 96);
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedStoreFlagConditionalOnCalleeSavedAndArgVars",
          .function_name = "GeneralizedStoreFlagConditionalOnCalleeSavedAndArgVars",
          .original_c = R"(
extern s32 StepHook(s32 val);
s32 GeneralizedStoreFlagConditionalOnCalleeSavedAndArgVars(s32 a, s32 b, s32 c) {
    s32 saved = StepHook(a);
    if (b > 10) {
        saved++;
    }
    if (c < 0) {
        a--;
    }
    return saved + a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedCrossJumpTailWithFilledDelaySlots",
          .function_name = "GeneralizedCrossJumpTailWithFilledDelaySlots",
          .original_c = R"(
s32 GeneralizedCrossJumpTailWithFilledDelaySlots(s32 mode, s32 x, s32* out) {
    if (mode == 0) {
        out[0] = x + 1;
        out[1] = x + 2;
        out[2] = x + 3;
        return 1;
    }
    if (mode < 0) {
        return -1;
    }
    out[0] = x - 1;
    out[1] = x + 2;
    out[2] = x + 3;
    return 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedSparseSwitchWith32BitConstantsAndZeroPivot",
          .function_name = "GeneralizedSparseSwitchWith32BitConstantsAndZeroPivot",
          .original_c = R"(
s32 GeneralizedSparseSwitchWith32BitConstantsAndZeroPivot(s32 cmd, s32 val) {
    switch (cmd) {
        case -200:
            return val - 10;
        case -50:
            return val - 5;
        case 100:
            return val + 5;
        case 0x12345:
            return val + 100;
        default:
            return val;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedShiftAddMulAndReusedScratchStoreFlag",
          .function_name = "GeneralizedShiftAddMulAndReusedScratchStoreFlag",
          .original_c = R"(
s32 GeneralizedShiftAddMulAndReusedScratchStoreFlag(s32 a, s32 b, s32 c, s32* out) {
    s32 acc = (a * 6) + (b * 14);
    if (a > 5) {
        acc++;
    }
    if (b < 20) {
        acc--;
    }
    if (c == 0) {
        acc++;
    }
    out[0] = acc;
    return acc;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedDualGivForLoopWithTwoStrides",
          .function_name = "GeneralizedDualGivForLoopWithTwoStrides",
          .original_c = R"(
typedef struct {
    s32 x;
    s32 y;
    s32 z;
} Vec3i;
typedef struct {
    s32 a;
    s32 b;
    s32 c;
    s32 d;
    s32 e;
} Rec5i;
s32 GeneralizedDualGivForLoopWithTwoStrides(Vec3i* dst, Rec5i* src, s32 count) {
    s32 i;
    for (i = 0; i < count; i++) {
        dst[i].x = src[i].a + src[i].e;
        dst[i].y = src[i].b;
        dst[i].z = src[i].c;
    }
    return dst[0].x + src[0].a;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedDisjointOffsetLoadsAndStores",
          .function_name = "GeneralizedDisjointOffsetLoadsAndStores",
          .original_c = R"(
typedef struct {
    s16 a;
    s16 b;
    s32 c;
    s32 d;
} Packet;
s32 GeneralizedDisjointOffsetLoadsAndStores(Packet* p, s32 delta) {
    s32 old_c = p->c;
    p->a = (s16)(p->b + 1);
    p->d = old_c + delta;
    return p->d;
}
)",
      },
      RoundTripTestCase{
          .test_name = "GeneralizedNarrowU8ConditionalStepAndCall",
          .function_name = "GeneralizedNarrowU8ConditionalStepAndCall",
          .original_c = R"(
extern s32 NotifyByteStep(s32 x);
u8 GeneralizedNarrowU8ConditionalStepAndCall(u8 val, s32 cond, s32 extra) {
    NotifyByteStep(extra);
    if (cond != 0) {
        val = (u8)(val + 1);
    }
    return val;
}
)",
      },
      RoundTripTestCase{
          .test_name = "NormalizePathSlashesAndCaseWithIntraLoopTargetStolenJoin",
          .function_name = "NormalizePathSlashesAndCaseWithIntraLoopTargetStolenJoin",
          .original_c = R"(
void NormalizePathSlashesAndCaseWithIntraLoopTargetStolenJoin(u8* buf, s32 len) {
    s32 has_lower = 0;
    if (len > 0) {
        u8* p = buf;
        u8* end = buf + len;
        do {
            if ((u32)(*p - 97) < 26) {
                has_lower = 1;
                break;
            }
            p++;
        } while (p < end);
    }
    if (len > 0) {
        u8* end = buf + len;
        do {
            u8 c = *buf;
            if (c == 92) {
                *buf = 47;
            } else if (has_lower == 0) {
                if ((u32)(c - 65) < 26) {
                    *buf = (u8)(c + 32);
                }
            }
            buf++;
        } while (buf < end);
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "ShortCircuitIfElseWithStructScaleAndFloatCompare",
          .function_name = "ShortCircuitIfElseWithStructScaleAndFloatCompare",
          .original_c = R"(
typedef struct {
    s32 a;
    s32 b;
    u8 flags;
    u8 pad[3];
} SlotRec12;

extern SlotRec12 g_slot_recs[];
extern void HandleInactiveSlot(u8* actor, s32 mode);
extern void HandleActiveSlot(u8* actor, s32 mode);

void ShortCircuitIfElseWithStructScaleAndFloatCompare(u8* actor, s32 mode, f32 cur, f32 limit) {
    if (cur < limit || (g_slot_recs[actor[70]].flags & 1) == 0) {
        HandleInactiveSlot(actor, mode);
    } else {
        HandleActiveSlot(actor, mode);
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "SearchMaskBitOrLoopWithSharedEpilogueReturns",
          .function_name = "SearchMaskBitOrLoopWithSharedEpilogueReturns",
          .original_c = R"(
extern u32 g_bit_masks[32];

s32 SearchMaskBitOrLoopWithSharedEpilogueReturns(u32 mask, s32 mode) {
    s32 idx;
    if (mode != 0) {
        if ((mask & 1) != 0) {
            return 0;
        }
        if ((mask & 2) != 0) {
            return 1;
        }
        return -1;
    }
    for (idx = 0; idx < 32; idx++) {
        if ((mask & ~g_bit_masks[idx]) == 0) {
            return idx;
        }
    }
    return -1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "BranchesWithCrossJumpedStoreAndIfReturnTail",
          .function_name = "BranchesWithCrossJumpedStoreAndIfReturnTail",
          .original_c = R"(
extern s32 g_lock_state;
extern s32 ProcessPrimaryEntry(s32* entry, s32 tag, s32 mode);
extern s32 ProcessFallbackEntry(s32* entry, s32 tag, s32 mode);

s32 BranchesWithCrossJumpedStoreAndIfReturnTail(s32* table, s32 tag, s32 mode) {
    s32 res;
    if (mode != 0) {
        if (*table == tag) {
            res = ProcessPrimaryEntry(table, tag, mode);
            g_lock_state = 0;
            return res;
        }
        res = ProcessFallbackEntry(table, tag, mode);
    } else {
        res = ProcessFallbackEntry(table, tag, 0);
    }
    g_lock_state = 0;
    return res;
}
)",
      },
      RoundTripTestCase{
          .test_name = "IfElseSubtreesWithCrossJumpedConditionalStoreTail",
          .function_name = "IfElseSubtreesWithCrossJumpedConditionalStoreTail",
          .original_c = R"(
extern s32 CheckPrimarySlot(s32* slot, s32 mode);
extern s32 CheckSecondarySlot(s32* slot);

s32 IfElseSubtreesWithCrossJumpedConditionalStoreTail(s32* slot, s32 mode, s32 val) {
    if (mode != 0) {
        if (CheckPrimarySlot(slot, mode) == 0) {
            return -1;
        }
        slot[1] = val;
        slot[2] = 1;
        slot[3] = 0;
        return 0;
    } else if (val > 0) {
        if (CheckSecondarySlot(slot) == 0) {
            return -2;
        }
        slot[1] = val;
        slot[2] = 1;
        slot[3] = 0;
        return 0;
    }
    return 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf",
          .function_name = "DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf",
          .original_c = R"(
extern void StepNpcSlot(void);
extern s32 InitNpcScript(s32 id, s32 arg1, s32 arg2);
extern u8 g_npc_flags[40];

s32 DecisionTreeSwitchWithSharedCrossSplitCaseAndBoundedLeaf(s16* cmd) {
    s32 i;
    switch (cmd[0]) {
    case 1:
        StepNpcSlot();
        return 0;
    case 12288:
        InitNpcScript(cmd[2], 0, 1);
        return 1;
    case 12289:
        g_npc_flags[cmd[4]] = 3;
        InitNpcScript(cmd[2], 0, 1);
        return 1;
    case 2:
    case 12290:
        for (i = 0; i < 4; i++) {
            g_npc_flags[i * 10] = 4;
        }
        InitNpcScript(cmd[2], 0, 1);
        return 1;
    case 12291:
        for (i = 0; i < 4; i++) {
            g_npc_flags[i * 10] &= 0x7F;
        }
        InitNpcScript(cmd[2], 0, 1);
        return 1;
    default:
        return 0;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "ShortCircuitFloatBoundsWithIndexedFallbackTable",
          .function_name = "ShortCircuitFloatBoundsWithIndexedFallbackTable",
          .original_c = R"(
extern u16 g_cam_flags;
extern f32 g_cam_min_x;
extern f32 g_cam_min_y;
extern f32 g_cam_clamp_x;
extern f32 g_cam_inner_x;
extern f32 g_cam_table[76];

f32 ShortCircuitFloatBoundsWithIndexedFallbackTable(u16 mode, u16 idx, f32 val) {
    f32 result = 0.0f;
    if (mode == 0) {
        if (g_cam_flags & 1) {
            if (g_cam_min_x < val && g_cam_min_y < val) {
                if (val < g_cam_clamp_x) {
                    if (!(val < g_cam_inner_x)) {
                        result = g_cam_clamp_x;
                    }
                } else {
                    result = g_cam_clamp_x;
                }
            } else {
                result = g_cam_table[idx * 19];
            }
        }
    }
    return result;
}
)",
      },
      RoundTripTestCase{
          .test_name = "O0FramePointerByteFillAndWordPostIncrement",
          .function_name = "O0FramePointerByteFillAndWordPostIncrement",
          .original_c = R"(
s32 O0FramePointerByteFillAndWordPostIncrement(s32* dst, s32 val, s32 count) {
    s32 fill = (u8)val;
    s32* p = dst;
    while (count-- != 0) {
        *p++ = fill;
    }
    return fill;
}
)",
          .options = {.c_flags = {"-O0", "-mips2", "-mcpu=r4000", "-Wa,-g"}},
      },
      RoundTripTestCase{
          .test_name = "LoopWithNestedConditionAndMergedLatchIncrement",
          .function_name = "LoopWithNestedConditionAndMergedLatchIncrement",
          .original_c = R"(
extern void ResetVoiceChannel(s32 idx);

void LoopWithNestedConditionAndMergedLatchIncrement(s32* table, s32 target) {
    s32 i = 0;
    do {
        if (table[0] != 0) {
            if (table[1] == target) {
                ResetVoiceChannel(i);
                table[0] = 0;
            }
        }
        table += 4;
        i++;
    } while (i < 16);
}
)",
      },
      RoundTripTestCase{
          .test_name = "SpinWaitOnGlobalReadyFlag",
          .function_name = "SpinWaitOnGlobalReadyFlag",
          .original_c = R"(
extern volatile s32 g_ready_flag;
extern void ProcessReadyState(s32 arg);

void SpinWaitOnGlobalReadyFlag(s32 arg) {
    ProcessReadyState(arg);
    while (g_ready_flag != 0) {
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "CalleeSavedCallResultDereferencedAtZeroOffsetAcrossConditionalCall",
          .function_name = "CalleeSavedCallResultDereferencedAtZeroOffsetAcrossConditionalCall",
          .original_c = R"(
extern s32* AcquireSlotRecord(s32 id);
extern void NotifySlotChange(s32 mode);

s32 CalleeSavedCallResultDereferencedAtZeroOffsetAcrossConditionalCall(
    s32 id, s32 mode, s32 val) {
    s32* rec = AcquireSlotRecord(id + 1);
    if (rec == 0) {
        return 0;
    }
    if (mode != 0) {
        NotifySlotChange(mode);
    }
    *rec = val;
    rec[1] = id;
    return 1;
}
)",
      },
      RoundTripTestCase{
          .test_name = "StringCompareLoopEarlyReturnAndZeroTerminator",
          .function_name = "StringCompareLoopEarlyReturnAndZeroTerminator",
          .original_c = R"(
s32 StringCompareLoopEarlyReturnAndZeroTerminator(const u8* s1, const u8* s2) {
    u8 c1;
    u8 c2;
    do {
        c1 = *s1++;
        c2 = *s2++;
        if (c1 != c2) {
            return c1 - c2;
        }
    } while (c1 != 0);
    return 0;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DecimalDigitParseLoopWithPointerWriteback",
          .function_name = "DecimalDigitParseLoopWithPointerWriteback",
          .original_c = R"(
s32 DecimalDigitParseLoopWithPointerWriteback(const u8** str_ptr) {
    const u8* p = *str_ptr;
    u8 ch = *p;
    s32 acc = 0;
    while (ch != 0) {
        const u8* next = p + 1;
        *str_ptr = next;
        if ((u32)(ch - '0') >= 10u) {
            break;
        }
        acc = acc * 10 + (ch - '0');
        ch = *next;
        p = next;
    }
    return acc;
}
)",
      },
      RoundTripTestCase{
          .test_name = "DecisionTreeSwitchWithStoreCaseArms",
          .function_name = "DecisionTreeSwitchWithStoreCaseArms",
          .original_c = R"(
void DecisionTreeSwitchWithStoreCaseArms(s32* dst, s32 mode, s32 val) {
    switch (mode) {
        case 1:
            *dst = val;
            break;
        case 2:
            *dst = val + 1;
            break;
        case 4:
            *dst = 0;
            break;
        default:
            break;
    }
}
)",
      },
      RoundTripTestCase{
          .test_name = "InfiniteLoopWithCallSplitExitGuard",
          .function_name = "InfiniteLoopWithCallSplitExitGuard",
          .original_c = R"(
extern s32 QueryPrimaryOwner(void);
extern s32 QueryFallbackOwner(void);
extern void WaitForOwnerRelease(s32 mode);

void InfiniteLoopWithCallSplitExitGuard(s32 target_owner) {
    while (1) {
        if (QueryPrimaryOwner() != target_owner) {
            if (QueryFallbackOwner() != target_owner) {
                break;
            }
        }
        WaitForOwnerRelease(1);
    }
}
)",
      },
  };
  return *k_cases;
}

}  // namespace rom_nom_nom
