#include "menu_navigation.h"
#include <cstdio>
using namespace liquid::menu;
int main(){std::vector<MenuItem> items={{L"disabled",ItemKind::Command,false},{L"divider",ItemKind::Separator,true},{L"first",ItemKind::Command,true},{L"sub",ItemKind::Submenu,true},{L"last",ItemKind::Command,true}};
 if(NextSelectable(items,false,-1,1)!=2||NextSelectable(items,false,2,1)!=3||NextSelectable(items,false,4,1)!=2)return 1;
 if(NextSelectable(items,false,-1,-1)!=4||NextSelectable(items,false,2,-1)!=4)return 2;
 if(NextSelectable(items,true,-1,1)!=0||NextSelectable(items,true,0,1)!=3||NextSelectable(items,true,3,-1)!=0)return 3;
 std::vector<MenuItem> none={{L"x",ItemKind::Command,false},{L"-",ItemKind::Separator,true}};
 if(NextSelectable(none,false,-1,1)!=-1||NextSelectable(none,true,-1,1)!=0||NextSelectable(items,false,2,0)!=-1)return 4;
 std::puts("PASS keyboard selection skips disabled/separator, wraps, and supports Back");return 0;}
