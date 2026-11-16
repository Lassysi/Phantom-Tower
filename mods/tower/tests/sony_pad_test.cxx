#include "../src/tower_sony_pad.h"
#include <cassert>
#include <cstdio>
int main() {
 using namespace tower::sony_pad;
 assert(Supported(0x054c,0x05c4) && Supported(0x054c,0x09cc));
 assert(!Supported(0x045e,0x028e) && !Supported(0x054c,0x0ce6));
 DIJOYSTATE2 s{}; s.rgdwPOV[0]=0xffffffff; s.lX=32768; s.lY=32768;
 assert(Map(s).buttons==0);
 s.rgbButtons[1]=0x80; s.rgbButtons[2]=0x80; s.rgbButtons[10]=0x80; s.rgbButtons[11]=0x80;
 auto m=Map(s); assert((m.buttons&0x30c0)==0x30c0 && m.chord);
 s.rgbButtons[11]=0; assert(!Map(s).chord);
 s={};s.rgdwPOV[0]=4500;s.lX=65535;s.lY=0;m=Map(s);
 assert((m.buttons&9)==9 && m.x==32767 && m.y==32767);
 s.rgdwPOV[0]=22500;assert((Map(s).buttons&6)==6);
 s.rgdwPOV[0]=31500;assert((Map(s).buttons&5)==5);
 std::printf("TOWER_SONY_PAD_OK (DS4 DirectInput mapping, no device)\n");
}
