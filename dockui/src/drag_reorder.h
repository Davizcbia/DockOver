#pragma once
#include <algorithm>
#include <cstddef>
#include <vector>
namespace dock {
struct DragRegion { int first=-1,last=-1; bool Valid() const { return first>=0&&last>=first; } };
inline DragRegion FindDragRegion(const std::vector<bool>& separators,int index){
    if(index<0||static_cast<size_t>(index)>=separators.size()||separators[index])return {};
    int first=index,last=index;while(first>0&&!separators[first-1])--first;
    while(last+1<static_cast<int>(separators.size())&&!separators[last+1])++last;return {first,last};
}
// A system Launchpad may move across application separators, but never beyond the last separator.
inline DragRegion FindLaunchpadRegion(const std::vector<bool>& separators,int index,int downloadsIndex){
    if(index<0||downloadsIndex<0||static_cast<size_t>(downloadsIndex)>separators.size()||index>=downloadsIndex)return {};
    int lastSeparator=-1;for(int i=0;i<downloadsIndex;++i)if(separators[i])lastSeparator=i;
    const int last=lastSeparator>=0?lastSeparator-1:downloadsIndex-1;
    return index<=last?DragRegion{0,last}:DragRegion{};
}
template<class T> bool ReorderWithinRegion(std::vector<T>& values,int from,int to,DragRegion region){
    if(!region.Valid()||from<region.first||from>region.last||to<region.first||to>region.last||region.last>=static_cast<int>(values.size()))return false;
    if(from<to)std::rotate(values.begin()+from,values.begin()+from+1,values.begin()+to+1);
    else if(from>to)std::rotate(values.begin()+to,values.begin()+from,values.begin()+from+1);
    return true;
}
}
