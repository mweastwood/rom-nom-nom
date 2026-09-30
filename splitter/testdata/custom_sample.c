// Custom C test file exercising MIPS compiler edge cases for round-trip testing.

struct Player {
  int x;
  int y;
  short hp;
  short max_hp;
  unsigned char state;
  unsigned char flags;
};

int ArithmeticAndBitwise(int a, int b, int c) {
  int mult_res = a * b;
  int div_res = (b != 0) ? (a / b) : 0;
  int rem_res = (c != 0) ? (a % c) : 0;
  int bit_res = (a & 0x0FFF) ^ ((b | 0x1234) & ~c);
  int shift_res = (a << 3) + (b >> 2);
  return mult_res + div_res + rem_res + bit_res + shift_res - 42;
}

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

float FloatMath(float speed, float angle, float dt) {
  float vel = speed * angle;
  if (vel > 100.0f) {
    vel = 100.0f;
  } else if (vel < -100.0f) {
    vel = -100.0f;
  }
  return vel * dt + 0.5f;
}
