#include "drag_reorder.h"
#include <cstdio>
#include <string>
int main(){std::vector<bool> sep{false,false,false,true,false,false};auto r=dock::FindDragRegion(sep,0);if(r.first!=0||r.last!=2||dock::FindDragRegion(sep,3).Valid())return 1;
 std::vector<std::string> ids{"a","b","c","|","d","e"};if(!dock::ReorderWithinRegion(ids,0,2,r)||ids!=std::vector<std::string>{"b","c","a","|","d","e"})return 2;
 if(dock::ReorderWithinRegion(ids,2,4,r)||ids[3]!="|")return 3;if(!dock::ReorderWithinRegion(ids,2,0,r)||ids[0]!="a")return 4;
 auto second=dock::FindDragRegion(sep,4);if(second.first!=4||second.last!=5||!dock::ReorderWithinRegion(ids,4,5,second)||ids[4]!="e")return 5;
 std::vector<bool> multi{false,false,true,false,false,true,true};
 auto lp=dock::FindLaunchpadRegion(multi,0,6);if(lp.first!=0||lp.last!=4)return 6;
 std::vector<std::string> layout{"LP","a","|","b","c","|","Downloads"};
 if(!dock::ReorderWithinRegion(layout,0,4,lp)||layout!=std::vector<std::string>{"a","|","b","c","LP","|","Downloads"})return 7;
 if(dock::ReorderWithinRegion(layout,4,5,lp)||dock::FindLaunchpadRegion(multi,5,6).Valid())return 8;
 puts("PASS region boundaries, stable ID ordering, separator rejection, left/right moves");return 0;}
