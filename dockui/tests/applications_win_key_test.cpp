#include "../src/applications_win_key.h"

int main(){
    using dock::applications::WinTap;
    for(DWORD logo:{DWORD(VK_LWIN),DWORD(VK_RWIN)}){
        WinTap t;
        if(t.Update(logo,true,true,false) || t.Update(logo,true,true,false) || !t.Update(logo,false,true,false)) return 1;
        if(t.Update(logo,false,true,false))return 2;
        for(DWORD key:{DWORD('D'),DWORD('E'),DWORD(VK_TAB),DWORD(VK_LSHIFT)}){
            t.Update(logo,true,true,false);t.Update(key,true,true,false);t.Update(key,false,true,false);
            if(t.Update(logo,false,true,false))return 3;
        }
        t.Update(logo,true,true,true);if(t.Update(logo,false,true,false))return 4;
        t.Update(logo,true,false,false);if(t.Update(logo,false,true,false))return 5;
        t.Update(logo,true,true,false);if(t.Update(logo,false,false,false))return 6;
    }
    WinTap t;t.Update(VK_LWIN,true,true,false);t.Update(VK_RWIN,true,true,true);
    if(t.Update(VK_LWIN,false,true,false)||t.Update(VK_RWIN,false,true,false))return 7;
    return 0;
}
