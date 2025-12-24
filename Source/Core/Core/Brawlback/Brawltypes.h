#pragma once
#include "include/brawlback-common/ExiStructures.h"

enum PADButtonBits : u16
{
  Start = 0x1000,
  Y = 0x800,
  X = 0x400,
  B = 0x200,
  A = 0x100,
  L = 0x40,
  RButton = 0x20,
  Z = 0x10,
  UpDPad = 0x8,
  DownDPad = 0x4,
  RightDPad = 0x2,
  LeftDPad = 0x1
};


#pragma pack(push, 2)
union PADButtons
{
  unsigned int bits;
  /*struct
  {
    unsigned _none : 3;
    unsigned Start : 1;
    unsigned Y : 1;
    unsigned X : 1;
    unsigned B : 1;
    unsigned A : 1;
    unsigned _none2 : 1;
    unsigned L : 1;
    unsigned R : 1;
    unsigned Z : 1;
    unsigned UpDPad : 1;
    unsigned DownDPad : 1;
    unsigned RightDPad : 1;
    unsigned LeftDPad : 1;
  };*/
};
#pragma pack(pop)


#pragma pack(push, 4)
struct gfPadGamecube
{
  unsigned int _buttons;
  unsigned int buttons;
  unsigned int holdButtons;
  unsigned int rapidFireButtons;
  unsigned int releasedButtons;
  unsigned int newPressedButtons;
  char _spacer2[0x18];
  // 0x30
  //  TODO: calculate new offset
  signed char stickX;
  signed char stickY;
  signed char cStickX;
  signed char cStickY;
  unsigned char LAnalogue;
  unsigned char RAnalogue;
  char _spacer5[0x2];

  // 0x38
  // 0xFF if not connected, else 0
  bool isNotConnected;

  char _spacer4[0x3];

  // 0x3C
  int type;

  gfPadGamecube() {
      _buttons = 0;
      buttons = 0;
      stickX = 0;
      stickY = 0;
      cStickX = 0;
      cStickY = 0;
      LAnalogue = 0;
      RAnalogue = 0;
      isNotConnected = 0;
      type = 0;
  }
};
#pragma pack(pop)
